/* Correctness-check engines (implementation).  See check.h.

   Strategy: for every case the implementation runs on arena A and the
   byte-wise oracle on an identical twin arena B (same initial content,
   same pointers).  The whole arenas are then compared byte by byte, so
   we detect wrong results *and* writes anywhere outside the intended
   destination range.  Deterministic fills guarantee the source and
   destination regions differ, so a no-op implementation cannot pass by
   accident.

   Guard pages: the arenas have a PROT_NONE page right after them, so a
   read/write past the end faults; the crash reporter in bench_common.c
   then identifies the implementation.
 */

#include "check.h"

#include <stdio.h>
#include <string.h>

static unsigned long total_cases;
static unsigned long total_fails;

/* Every interesting length up to the arena limit.  */
static const size_t lens_write[] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 17, 23, 24, 31, 32, 33,
  47, 48, 63, 64, 65, 95, 96, 127, 128, 129, 191, 192, 255, 256, 257,
  383, 384, 511, 512, 513, 767, 768, 1023, 1024, 1025, 1535, 2047,
  2048, 2049, 3071, 4095, 4096, 4097, 6143, 8191, 8192, 8193, 12288,
  16384, 16385, 24576, 32768, 32769, 49152, 65535
};

static const int aligns[] = { 0, 1, 2, 3, 7, 8, 15, 16, 31, 63 };

static const int cvals[] = { 0x00, 0x01, 0x12, 0x7f, 0x80, 0xff };

/* Fill both arenas with fresh identical deterministic content.  Twins
   start from the same state; the implementation runs on A and the oracle
   on B, so the whole-arena comparison is meaningful.  */
static void
fresh_fill (unsigned char *a, unsigned char *b, size_t n,
	    unsigned long case_no, unsigned long seed)
{
  unsigned long s = seed + case_no * 2654435761u;
  mb_fill (a, n, s);
  mb_fill (b, n, s);
}

static void
report_fail (const char *what, const char *impl, const char *where,
	     size_t off)
{
  fprintf (stderr, "  FAIL %s: impl %-12s %s (byte %zu)\n",
	   what, impl, where, off);
  total_fails++;
}

static void
report_impl (const char *name)
{
  fprintf (stderr, "  testing %-16s", name);
}

static void
report_impl_end (int failed, unsigned long fails)
{
  if (!failed)
    fprintf (stderr, "  OK\n");
  else
    fprintf (stderr, "  FAILED (%lu case(s))\n", fails);
}

/* First differing byte between two regions; (size_t)-1 if identical.  */
static size_t
first_diff (const unsigned char *a, const unsigned char *b, size_t n)
{
  for (size_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return i;
  return (size_t) -1;
}

/* ------------------------------------------------------------------ */
/* Write-type engine: memcpy / memmove / memset                        */
/* ------------------------------------------------------------------ */

int
mb_check_write_run (const char *what, int is_memmove, const mb_opts_t *o,
		    mb_wimpl_t impl_wrapper, mb_woracle_t oracle)
{
  const size_t arena = mb_buf1.size;
  unsigned char *ar = mb_buf1.base;	/* implementation arena	*/
  unsigned char *rf = mb_buf2.base;	/* oracle arena		*/
  unsigned long fails = 0;
  unsigned long case_no = 0;

  printf ("Checking %s against byte-wise oracle...\n", what);
  fflush (stdout);

  for (int im = 0; im < mb_impl_count (); im++)
    {
      const mb_impl_t *impl = mb_impl_get (im);
      unsigned long impl_fails = 0;
      report_impl (impl->name);

      for (size_t li = 0; li < sizeof lens_write / sizeof lens_write[0];
	   li++)
	{
	  size_t len = lens_write[li];
	  if (o->max_len != 0 && len > o->max_len)
	    break;

	  if (is_memmove)
	    {
	      /* memmove: dst/src within the same arena, with overlap at
		 several distances in both directions.  */
	      if (len != 0 && len > arena - 256)
		continue;
	      for (size_t ai = 0; ai < (len <= 512 ? 3 : 1); ai++)
		{
		  int base = aligns[ai];
		  size_t S = ((arena - len) / 2 + base) & ~(size_t) 15;
		  if (S + len > arena - 1)
		    S = arena - len - 1;
		  for (size_t di = 0; di < 11; di++)
		    {
		      long delta = 0;
		      size_t half = len > 1 ? len / 2 : 1;
		      switch (di)
			{
			case 0: delta = 0; break;
			case 1: delta = 1; break;
			case 2: delta = 2; break;
			case 3: delta = 7; break;
			case 4: delta = 31; break;
			case 5: delta = 127; break;
			case 6: delta = (long) half; break;
			case 7: delta = (long) len; break;
			case 8: delta = (long) len + 17; break;
			case 9: delta = -(long) len; break;
			case 10: delta = -((long) len + 17); break;
			}
		      if (len == 0 && delta != 0)
			continue;
		      long long D = (long long) S + delta;
		      if (D < 0 || (unsigned long long) D + len > arena)
			continue;
		      case_no++;
		      total_cases++;
		      fresh_fill (ar, rf, arena, case_no, o->seed);

		      mb_current_impl = impl->name;
		      impl_wrapper (impl, ar + D, ar + S, 0, len);
		      oracle (rf + D, rf + S, 0, len);

		      size_t off = first_diff (ar, rf, arena);
		      if (off != (size_t) -1)
			{
			  char where[160];
			  snprintf (where, sizeof where,
				    "len=%zu delta=%ld S=%zu D=%lld",
				    len, delta, S, D);
			  report_fail (what, impl->name, where, off);
			  impl_fails++;
			}
		    }
		}
	      continue;
	    }

	  /* memcpy / memset: dst and src (memcpy only) are two disjoint
	     regions inside each arena, so a full-arena comparison is
	     meaningful.  */
	  if (2 * len + 512 > arena)
	    continue;
	  size_t nA = (len <= 512) ? 10 : (len <= 16384 ? 4 : 2);
	  for (size_t ai = 0; ai < nA; ai++)
	    {
	      int a1 = aligns[ai];
	      int a2 = aligns[(ai * 5 + 3) % 10];
	      int c = 0;
	      if (what[0] == 'm' && what[2] == 'm' && what[3] == 's')
		c = cvals[ai % 6];
	      case_no++;
	      total_cases++;
	      fresh_fill (ar, rf, arena, case_no, o->seed);

	      /* memcpy: dst at a1, src on the far side of the arena.
		 memset: dst at a1, c is the fill byte (src unused).  */
	      mb_current_impl = impl->name;
	      impl_wrapper (impl, ar + a1, ar + arena / 2 + a2, c, len);
	      oracle (rf + a1, rf + arena / 2 + a2, c, len);

	      size_t off = first_diff (ar, rf, arena);
	      if (off != (size_t) -1)
		{
		  char where[160];
		  snprintf (where, sizeof where,
			    "len=%zu align1=%d align2=%d char=0x%02x",
			    len, a1, a2, c & 0xff);
		  report_fail (what, impl->name, where, off);
		  impl_fails++;
		}

	      /* Guard-tail placement: destination ends exactly at the end
		 of the mapping, so over-writes past len fault.  */
	      if (len >= 16 && what[0] == 'm' && what[2] == 'm'
		  && what[3] == 's')
		{
		  case_no++;
		  total_cases++;
		  fresh_fill (ar, rf, arena, case_no, o->seed);
		  mb_current_impl = impl->name;
		  impl_wrapper (impl, ar + (arena - len), ar + arena / 2,
				c, len);
		  oracle (rf + (arena - len), rf + arena / 2, c, len);
		  off = first_diff (ar, rf, arena);
		  if (off != (size_t) -1)
		    {
		      char where[160];
		      snprintf (where, sizeof where,
				"len=%zu guard-tail char=0x%02x",
				len, c & 0xff);
		      report_fail (what, impl->name, where, off);
		      impl_fails++;
		    }
		}
	    }
	}
      report_impl_end (impl_fails != 0, impl_fails);
      fails += impl_fails;
    }

  printf ("%s check: %lu case(s) per implementation, %lu failure(s).\n",
	  what, total_cases, fails);
  return (int) fails;
}

/* ------------------------------------------------------------------ */
/* Memcmp engine                                                       */
/* ------------------------------------------------------------------ */

int
mb_check_memcmp_run (const char *what, const mb_opts_t *o,
		     mb_cimpl_t impl_wrapper, mb_coracle_t oracle)
{
  const size_t arena = mb_buf1.size;
  unsigned char *b1 = mb_buf1.base;
  unsigned char *b2 = mb_buf2.base;
  unsigned long fails = 0;
  unsigned long case_no = 0;

  static const size_t lens[] = {
    0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127,
    128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 2047,
    2048, 2049, 4095, 4096, 4097, 8191, 8192, 16383, 16384, 32767,
    32768, 65535
  };

  printf ("Checking %s against byte-wise oracle...\n", what);
  fflush (stdout);

  for (int im = 0; im < mb_impl_count (); im++)
    {
      const mb_impl_t *impl = mb_impl_get (im);
      unsigned long impl_fails = 0;
      report_impl (impl->name);

      for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++)
	{
	  size_t len = lens[li];
	  if (o->max_len != 0 && len > o->max_len)
	    break;
	  if (len > arena - 256)
	    continue;

	  for (size_t ai = 0; ai < 10; ai++)
	    {
	      int al = aligns[ai];
	      int al2 = aligns[(ai * 3 + 1) % 10];
	      if (len + al > arena - 128 || len + al2 > arena - 128)
		continue;

	      for (int mode = 0; mode < 5; mode++)
		{
		  if (len == 0 && mode != 0)
		    continue;
		  case_no++;
		  total_cases++;
		  unsigned long s = o->seed + case_no * 2654435761u;
		  mb_fill (b1, arena, s);
		  mb_fill (b2, arena, s ^ 0x11111111UL);

		  unsigned char *x = b1 + al;
		  unsigned char *y = b2 + al2;
		  size_t pos;
		  switch (mode)
		    {
		    case 0: pos = (size_t) -1; break;
		    case 1: pos = len / 2; break;
		    case 2: pos = len - 1; break;
		    case 3: pos = 0; break;
		    default: pos = (s >> 8) % len; break;
		    }
		  if (pos != (size_t) -1)
		    y[pos] = (unsigned char) (x[pos] + ((s & 1) ? 1 : 0x31));

		  mb_current_impl = impl->name;
		  int got = impl_wrapper (impl, x, y, len);
		  int ref = oracle (x, y, len);

		  int gs = (got > 0) - (got < 0);
		  int rs = (ref > 0) - (ref < 0);
		  if (gs != rs)
		    {
		      char where[160];
		      snprintf (where, sizeof where,
				"len=%zu align1=%d align2=%d mode=%d "
				"(got=%d ref=%d)",
				len, al, al2, mode, got, ref);
		      report_fail (what, impl->name, where, 0);
		      impl_fails++;
		    }
		}
	    }
	}
      report_impl_end (impl_fails != 0, impl_fails);
      fails += impl_fails;
    }

  printf ("%s check: %lu case(s) per implementation, %lu failure(s).\n",
	  what, total_cases, fails);
  return (int) fails;
}

/* Benchmark memcpy implementations (glibc benchtests methodology).

   Measures every registered implementation (libc, generic C, and
   dlopen'ed user implementations) over the glibc alignment/length
   matrix for memcpy and prints glibc-compatible JSON on stdout.
   --check instead verifies correctness against a byte-wise oracle.

   Requires: buffers allocated via mb_buffers_init() with the
   guard-page scheme; timing via CNTVCT_EL0 on aarch64.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench_drv.h"
#include "check.h"
#include "generic_ref.h"
#include "matrix.h"

typedef void *(*proto_t) (void *, const void *, size_t);

#define FN_SYMBOL "memcpy"

static const mb_opts_t *g_o;
static size_t real_page;	/* OS page size (alignment mask base) */

static void
warm_impl (const mb_impl_t *impl, char *dst, const char *src, size_t len,
	   size_t iters)
{
  proto_t f = (proto_t) impl->fn;
  size_t warm = mb_warmup_iters (iters);

  mb_current_impl = impl->name;
  for (size_t i = 0; i < warm; ++i)
    f (dst, src, len);
  mb_current_impl = NULL;
}

static double
measure_impl (const mb_impl_t *impl, char *dst, const char *src, size_t len,
	      size_t iters)
{
  proto_t f = (proto_t) impl->fn;

  mb_current_impl = impl->name;
  uint64_t t0 = mb_counter_read ();
  for (size_t i = 0; i < iters; ++i)
    f (dst, src, len);
  uint64_t t1 = mb_counter_read ();
  mb_current_impl = NULL;

  double ns = (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
  return ns / (double) iters;
}

static void
do_test (json_ctx_t *json_ctx, size_t align1, size_t align2, size_t len,
	 int both_ways)
{
  size_t i, j, repeats;
  char *s1, *s2;

  if (g_o->max_len != 0 && len > g_o->max_len)
    return;
  align1 &= (real_page - 1);
  if (align1 + len >= mb_page_size)
    return;
  align2 &= (real_page - 1);
  if (align2 + len >= mb_page_size)
    return;

  s1 = (char *) (mb_buf1.base + align1);
  s2 = (char *) (mb_buf2.base + align2);

  size_t iters = mb_pick_iters (g_o, len == 0 ? 1 : len);
  size_t reps = g_o->repeat > 0 ? (size_t) g_o->repeat : 1;

  for (repeats = both_ways ? 2 : 1; repeats; --repeats)
    {
      for (i = 0, j = 1; i < len; i++, j += 23)
	s1[i] = (char) j;

      /* Warm up once, then take REPS measurements per implementation.  */
      for (int k = 0; k < mb_impl_count (); k++)
	warm_impl (mb_impl_get (k), s2, s1, len, iters);

      for (size_t r = 0; r < reps; r++)
	{
	  json_element_object_begin (json_ctx);
	  json_attr_uint (json_ctx, "length", len);
	  json_attr_uint (json_ctx, "align1", align1);
	  json_attr_uint (json_ctx, "align2", align2);
	  json_attr_uint (json_ctx, "dst > src", (s2 > s1));
	  if (reps > 1)
	    json_attr_uint (json_ctx, "run", r);
	  json_array_begin (json_ctx, "timings");

	  for (int k = 0; k < mb_impl_count (); k++)
	    json_element_double (json_ctx,
				 measure_impl (mb_impl_get (k), s2, s1, len,
					       iters));

	  json_array_end (json_ctx);
	  json_element_object_end (json_ctx);
	}

      /* Swap buffers to test dst < src and dst > src.  */
      s1 = (char *) (mb_buf2.base + align1);
      s2 = (char *) (mb_buf1.base + align2);
    }
}

/* Extra "large" cases (like glibc bench-memcpy-large), enabled when the
   user raises --max-len beyond the default matrix coverage.  */
/* Run the loaded matrix: every case is one do_test() call.  */
static void
run_matrix (json_ctx_t *json_ctx, const mb_matrix_t *m)
{
  for (size_t k = 0; k < m->ncases; k++)
    {
      const mb_case_t *c = &m->cases[k];
      do_test (json_ctx, (size_t) c->a1, (size_t) c->a2, c->len, c->both);
    }
}

/* --- correctness mode ---------------------------------------------- */

static void
check_wimpl (const mb_impl_t *impl, void *dst, const void *src, int c,
	     size_t len)
{
  (void) c;
  ((proto_t) impl->fn) (dst, src, len);
}

static void
check_woracle (void *dst, const void *src, int c, size_t len)
{
  (void) c;
  mb_oracle_memcpy (dst, src, len);
}

int
main (int argc, char **argv)
{
  static mb_opts_t o;
  g_o = &o;
  mb_opts_defaults (&o);

  mb_install_crash_reporter ();
  mb_opts_parse (&o, argc, argv, FN_SYMBOL, NULL);

  if (mb_register_impls (&o, FN_SYMBOL, (mb_fn_t) mb_ref_memcpy) < 0)
    return 1;

  real_page = (size_t) sysconf (_SC_PAGESIZE);
  if (real_page == (size_t) -1)
    real_page = 4096;

  /* Test matrix: a --matrix profile or the compiled-in glibc
     defaults (small, plus the large matrix when --max-len is above
     128 KiB).  */
  mb_matrix_t mx;
  mb_matrix_init (&mx);
  if (!o.check)
    {
      int rc = (o.matrix != NULL)
        ? mb_matrix_load (o.matrix, FN_SYMBOL, &mx)
        : mb_matrix_load_default (FN_SYMBOL, o.max_len > (1u << 17), &mx);
      if (rc != 0)
        {
          fprintf (stderr, "error: %s\n", mb_matrix_err ());
          return 1;
        }
    }

  size_t want = o.max_len != 0 ? o.max_len : (MB_MIN_PAGE_SIZE - 1);
  if (!o.check && o.matrix != NULL
      && (size_t) mb_matrix_max_len (&mx) > want)
    want = (size_t) mb_matrix_max_len (&mx);
  if (mb_buffers_init (MB_MIN_PAGE_SIZE, want) == 0)
    {
      fprintf (stderr, "error: cannot allocate benchmark buffers\n");
      return 1;
    }

  if (o.check)
    return mb_check_write_run ("memcpy", 0, &o, check_wimpl, check_woracle) ? 1 : 0;

  if (getenv ("MB_NO_WARMUP") == NULL)
    mb_warmup (0);

  json_ctx_t json_ctx;
  json_init (&json_ctx, 0, stdout);
  json_document_begin (&json_ctx);
  json_attr_string (&json_ctx, "timing_type", MB_TIMING_TYPE);
  json_attr_object_begin (&json_ctx, "functions");
  json_attr_object_begin (&json_ctx, FN_SYMBOL);
  json_attr_string (&json_ctx, "bench-variant", "default");

  json_array_begin (&json_ctx, "ifuncs");
  for (int k = 0; k < mb_impl_count (); k++)
    json_element_string (&json_ctx, mb_impl_get (k)->name);
  json_array_end (&json_ctx);

  json_array_begin (&json_ctx, "results");
  run_matrix (&json_ctx, &mx);
  json_array_end (&json_ctx);

  json_attr_object_end (&json_ctx);
  json_attr_object_end (&json_ctx);
  json_document_end (&json_ctx);
  putchar ('\n');

  mb_matrix_free (&mx);
  mb_buffers_free ();
  return 0;
}

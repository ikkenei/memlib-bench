/* Benchmark memmove implementations (glibc benchtests methodology).

   As in glibc bench-memmove, source and destination live in the SAME
   buffer at controlled offsets, so the measured cases include genuine
   overlapping moves in both directions.  Prints glibc-compatible JSON.
   --check verifies correctness (incl. forward/backward overlap) against
   a byte-wise oracle.
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

#define FN_SYMBOL "memmove"
#define MIN_PAGE_SIZE (131072)

static const mb_opts_t *g_o;
static size_t real_page;
static size_t half_page;

static void
do_one_test (json_ctx_t *json_ctx, const mb_impl_t *impl, char *dst,
	     char *src, size_t len, size_t iters)
{
  proto_t f = (proto_t) impl->fn;
  size_t warm = iters / 64;
  if (warm < 8)
    warm = iters / 8;
  if (warm < 1)
    warm = 1;

  mb_current_impl = impl->name;
  for (size_t i = 0; i < warm; ++i)
    f (dst, src, len);

  uint64_t t0 = mb_counter_read ();
  for (size_t i = 0; i < iters; ++i)
    f (dst, src, len);
  uint64_t t1 = mb_counter_read ();
  mb_current_impl = NULL;

  double ns = (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
  json_element_double (json_ctx, ns / (double) iters);
}

static void
do_test (json_ctx_t *json_ctx, size_t align1, size_t align2, size_t len)
{
  size_t i, j;

  if (g_o->max_len != 0 && len > g_o->max_len)
    return;
  align1 &= (real_page - 1);
  if (align1 + len >= mb_page_size)
    return;
  align2 &= (real_page - 1);
  if (align2 + len >= mb_page_size)
    return;

  char *s1 = (char *) (mb_buf2.base + align1);	/* src	*/
  char *s2 = (char *) (mb_buf2.base + align2);	/* dst	*/

  for (i = 0, j = 1; i < len; i++, j += 23)
    s1[i] = (char) j;

  size_t iters = mb_pick_iters (g_o, len == 0 ? 1 : len);

  json_element_object_begin (json_ctx);
  json_attr_uint (json_ctx, "length", len);
  json_attr_uint (json_ctx, "align1", align1);
  json_attr_uint (json_ctx, "align2", align2);
  json_array_begin (json_ctx, "timings");

  for (int k = 0; k < mb_impl_count (); k++)
    do_one_test (json_ctx, mb_impl_get (k), s2, s1, len, iters);

  json_array_end (json_ctx);
  json_element_object_end (json_ctx);
}

static void
run_large (json_ctx_t *json_ctx)
{
  if (g_o->max_len <= (1u << 17))
    return;
  for (size_t l = (1u << 17); l <= g_o->max_len; l <<= 1)
    {
      do_test (json_ctx, 0, 64, l + 7);
      do_test (json_ctx, 0, 3, l + 15);
      do_test (json_ctx, 3, 0, l + 31);
      do_test (json_ctx, 3, 7, l + 63);
      do_test (json_ctx, 9, 5, l + 127);
    }
}

static void
run_matrix (json_ctx_t *json_ctx, const mb_matrix_t *m)
{
  for (size_t il = 0; il < m->nsizes; il++)
    {
      size_t len = (size_t) m->sizes[il];

      /* src and dst are the same offset list: run every unordered pair
         in both directions (dst after src / dst before src).  */
      if (m->nsrc == m->ndst)
        {
          int equal = 1;
          for (size_t k = 0; k < m->nsrc; k++)
            if (m->src[k] != m->dst[k])
              equal = 0;
          if (equal)
            {
              for (size_t i = 0; i < m->nsrc; i++)
                for (size_t j = i + 1; j < m->nsrc; j++)
                  {
                    if (m->src[i] == m->src[j])
                      continue;
                    do_test (json_ctx, (size_t) m->src[i],
                             (size_t) m->src[j], len);
                    do_test (json_ctx, (size_t) m->src[j],
                             (size_t) m->src[i], len);
                  }
              continue;
            }
        }

      /* Independent src/dst lists: directed pairs; both=1 adds the
         reversed direction as well.  */
      for (size_t i = 0; i < m->nsrc; i++)
        for (size_t j = 0; j < m->ndst; j++)
          {
            if (m->src[i] == m->dst[j])
              continue;
            do_test (json_ctx, (size_t) m->src[i], (size_t) m->dst[j], len);
            if (m->both)
              do_test (json_ctx, (size_t) m->dst[j], (size_t) m->src[i], len);
          }
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
  mb_oracle_memmove (dst, src, len);
}

int
main (int argc, char **argv)
{
  static mb_opts_t o;
  g_o = &o;
  mb_opts_defaults (&o);

  mb_install_crash_reporter ();

  mb_opts_parse (&o, argc, argv, FN_SYMBOL, NULL);

  if (mb_register_impls (&o, FN_SYMBOL, (mb_fn_t) mb_ref_memmove) < 0)
    return 1;

  real_page = (size_t) sysconf (_SC_PAGESIZE);
  if (real_page == (size_t) -1)
    real_page = 4096;
  half_page = real_page / 2;

  size_t want = o.max_len != 0 ? o.max_len : (MIN_PAGE_SIZE - 1);
  mb_matrix_t mx;
  mb_matrix_init (&mx);
  int have_mx = 0;
  if (!o.check && o.matrix != NULL)
    {
      if (mb_matrix_load (o.matrix, FN_SYMBOL, &mx) != 0)
        {
          fprintf (stderr, "error: %s\n", mb_matrix_err ());
          return 1;
        }
      have_mx = 1;
      if ((size_t) mb_matrix_max_size (&mx) > want)
        want = (size_t) mb_matrix_max_size (&mx);
    }
  if (have_mx && mx.nsrc == 0 && mx.ndst == 0)
    {
      fprintf (stderr, "error: [memmove] section must define a 'src' "
               "and/or 'dst' offset list\n");
      return 1;
    }
  if (mb_buffers_init (MIN_PAGE_SIZE, want) == 0)
    {
      fprintf (stderr, "error: cannot allocate benchmark buffers\n");
      return 1;
    }

  if (o.check)
    return mb_check_write_run ("memmove", 1, &o, check_wimpl,
			       check_woracle) ? 1 : 0;

  if (getenv ("MB_NO_WARMUP") == NULL)
    mb_warmup (0);

  json_ctx_t json_ctx;
  json_init (&json_ctx, 0, stdout);
  json_document_begin (&json_ctx);
  json_attr_string (&json_ctx, "timing_type", MB_TIMING_TYPE);
  json_attr_object_begin (&json_ctx, "functions");
  json_attr_object_begin (&json_ctx, "memmove");
  json_attr_string (&json_ctx, "bench-variant", "default");

  json_array_begin (&json_ctx, "ifuncs");
  for (int k = 0; k < mb_impl_count (); k++)
    json_element_string (&json_ctx, mb_impl_get (k)->name);
  json_array_end (&json_ctx);

  json_array_begin (&json_ctx, "results");

  if (have_mx)
    run_matrix (&json_ctx, &mx);
  else
    {
      size_t i;
  for (i = 0; i < 14; ++i)
    {
      do_test (&json_ctx, 0, 32, 1u << i);
      do_test (&json_ctx, 32, 0, 1u << i);
      do_test (&json_ctx, 0, i, 1u << i);
      do_test (&json_ctx, i, 0, 1u << i);
    }

  for (i = 0; i < 32; ++i)
    {
      do_test (&json_ctx, 0, 32, i);
      do_test (&json_ctx, 32, 0, i);
      do_test (&json_ctx, 0, i, i);
      do_test (&json_ctx, i, 0, i);
    }

  for (i = 3; i < 32; ++i)
    {
      if ((i & (i - 1)) == 0)
	continue;
      do_test (&json_ctx, 0, 32, 16 * i);
      do_test (&json_ctx, 32, 0, 16 * i);
      do_test (&json_ctx, 0, i, 16 * i);
      do_test (&json_ctx, i, 0, 16 * i);
    }

  for (i = 32; i < 64; ++i)
    {
      do_test (&json_ctx, 0, 0, 32 * i);
      do_test (&json_ctx, i, 0, 32 * i);
      do_test (&json_ctx, 0, i, 32 * i);
      do_test (&json_ctx, i, i, 32 * i);
    }

  for (i = 0; i <= 48; ++i)
    {
      do_test (&json_ctx, 0, 0, 2048 + 64 * i);
      do_test (&json_ctx, i, 0, 2048 + 64 * i);
      do_test (&json_ctx, 0, i, 2048 + 64 * i);
      do_test (&json_ctx, i, i, 2048 + 64 * i);
      do_test (&json_ctx, half_page, 0, 2048 + 64 * i);
      do_test (&json_ctx, 0, half_page, 2048 + 64 * i);
      do_test (&json_ctx, half_page + i, 0, 2048 + 64 * i);
      do_test (&json_ctx, i, half_page, 2048 + 64 * i);
      do_test (&json_ctx, half_page, i, 2048 + 64 * i);
      do_test (&json_ctx, 0, half_page + i, 2048 + 64 * i);
      do_test (&json_ctx, half_page + i, i, 2048 + 64 * i);
      do_test (&json_ctx, i, half_page + i, 2048 + 64 * i);
    }

      run_large (&json_ctx);
    }

  json_array_end (&json_ctx);
  json_attr_object_end (&json_ctx);
  json_attr_object_end (&json_ctx);
  json_document_end (&json_ctx);
  putchar ('\n');

  mb_matrix_free (&mx);
  mb_buffers_free ();
  return 0;
}

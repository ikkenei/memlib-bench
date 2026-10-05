/* Benchmark memcmp implementations (glibc benchtests methodology).  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench_drv.h"
#include "check.h"
#include "generic_ref.h"
#include "matrix.h"

typedef int (*proto_t) (const void *, const void *, size_t);

#define FN_SYMBOL "memcmp"
#define MIN_PAGE_SIZE (131072)

static const mb_opts_t *g_o;

static void
warm_impl (const mb_impl_t *impl, const char *s1, const char *s2, size_t len,
	   size_t iters)
{
  proto_t f = (proto_t) impl->fn;
  size_t warm = mb_warmup_iters (iters);

  mb_current_impl = impl->name;
  for (size_t i = 0; i < warm; ++i)
    f (s1, s2, len);
  mb_current_impl = NULL;
}

static double
measure_impl (const mb_impl_t *impl, const char *s1, const char *s2,
	      size_t len, size_t iters)
{
  proto_t f = (proto_t) impl->fn;

  mb_current_impl = impl->name;
  uint64_t t0 = mb_counter_read ();
  for (size_t i = 0; i < iters; ++i)
    f (s1, s2, len);
  uint64_t t1 = mb_counter_read ();
  mb_current_impl = NULL;

  double ns = (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
  return ns / (double) iters;
}

static void
do_test (json_ctx_t *json_ctx, size_t align1, size_t align2, size_t len,
	 int exp_result)
{
  size_t i;
  char *s1, *s2;

  if (g_o->max_len != 0 && len > g_o->max_len)
    return;
  align1 &= 4095;
  if (align1 + len + 1 >= mb_page_size)
    return;
  align2 &= 4095;
  if (align2 + len + 1 >= mb_page_size)
    return;

  s1 = (char *) (mb_buf1.base + align1);
  s2 = (char *) (mb_buf2.base + align2);

  for (i = 0; i < len; i++)
    s1[i] = s2[i] = (char) (1 + (23 * i) % 255);

  if (len)
    {
      s1[len] = (char) align1;
      s2[len] = (char) align2;
      s2[len - 1] -= (char) exp_result;
    }

  size_t iters = mb_pick_iters (g_o, len == 0 ? 1 : len);
  size_t reps = g_o->repeat > 0 ? (size_t) g_o->repeat : 1;

  for (int k = 0; k < mb_impl_count (); k++)
    warm_impl (mb_impl_get (k), s1, s2, len, iters);

  for (size_t r = 0; r < reps; r++)
    {
      json_element_object_begin (json_ctx);
      json_attr_uint (json_ctx, "length", len);
      json_attr_uint (json_ctx, "align1", align1);
      json_attr_uint (json_ctx, "align2", align2);
      json_attr_int (json_ctx, "result", exp_result);
      if (reps > 1)
	json_attr_uint (json_ctx, "run", r);
      json_array_begin (json_ctx, "timings");

      for (int k = 0; k < mb_impl_count (); k++)
	json_element_double (json_ctx,
			     measure_impl (mb_impl_get (k), s1, s2, len, iters));

      json_array_end (json_ctx);
      json_element_object_end (json_ctx);
    }
}

static void
run_large (json_ctx_t *json_ctx)
{
  if (g_o->max_len <= (1u << 17))
    return;
  for (size_t l = (1u << 17); l <= g_o->max_len; l <<= 1)
    {
      do_test (json_ctx, 0, 0, l, 0);
      do_test (json_ctx, 0, 0, l, 1);
      do_test (json_ctx, 63, 0, l, 0);
      do_test (json_ctx, 0, 63, l, -1);
      do_test (json_ctx, 3, 5, l, 0);
    }
}

static void
run_matrix (json_ctx_t *json_ctx, const mb_matrix_t *m)
{
  for (size_t il = 0; il < m->nsizes; il++)
    {
      size_t len = (size_t) m->sizes[il];
      for (size_t is = 0; is < m->nsrc; is++)
        for (size_t id = 0; id < m->ndst; id++)
          for (size_t ir = 0; ir < m->ndiff; ir++)
            do_test (json_ctx, (size_t) m->src[is], (size_t) m->dst[id],
                     len, (int) m->diff[ir]);
    }
}


/* --- correctness mode ---------------------------------------------- */

static int
check_cimpl (const mb_impl_t *impl, const void *a, const void *b,
	     size_t n)
{
  return ((proto_t) impl->fn) (a, b, n);
}

static int
check_coracle (const void *a, const void *b, size_t n)
{
  return mb_oracle_memcmp (a, b, n);
}

int
main (int argc, char **argv)
{
  static mb_opts_t o;
  g_o = &o;
  mb_opts_defaults (&o);

  mb_install_crash_reporter ();

  mb_opts_parse (&o, argc, argv, FN_SYMBOL, NULL);

  if (mb_register_impls (&o, FN_SYMBOL, (mb_fn_t) mb_ref_memcmp) < 0)
    return 1;

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
  if (have_mx && (mx.nsrc == 0 || mx.ndst == 0 || mx.ndiff == 0))
    {
      fprintf (stderr, "error: [memcmp] section must define 'src', 'dst' "
               "and 'diff' lists\n");
      return 1;
    }
  if (mb_buffers_init (MIN_PAGE_SIZE, want) == 0)
    {
      fprintf (stderr, "error: cannot allocate benchmark buffers\n");
      return 1;
    }

  if (o.check)
    return mb_check_memcmp_run ("memcmp", &o, check_cimpl, check_coracle)
	   ? 1 : 0;

  if (getenv ("MB_NO_WARMUP") == NULL)
    mb_warmup (0);

  json_ctx_t json_ctx;
  json_init (&json_ctx, 0, stdout);
  json_document_begin (&json_ctx);
  json_attr_string (&json_ctx, "timing_type", MB_TIMING_TYPE);
  json_attr_object_begin (&json_ctx, "functions");
  json_attr_object_begin (&json_ctx, "memcmp");
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
  for (i = 0; i < 32; ++i)
    {
      do_test (&json_ctx, i, i, i, 0);
      do_test (&json_ctx, i, i, i, 1);
      do_test (&json_ctx, i, i, i, -1);
    }

  for (i = 0; i < 32; ++i)
    {
      do_test (&json_ctx, 0, 0, i, 0);
      do_test (&json_ctx, 0, 0, i, 1);
      do_test (&json_ctx, 0, 0, i, -1);
      do_test (&json_ctx, 4096 - i, 0, i, 0);
      do_test (&json_ctx, 4096 - i, 0, i, 1);
      do_test (&json_ctx, 4096 - i, 0, i, -1);
    }

  for (i = 33; i < 385; i += 32)
    {
      do_test (&json_ctx, 0, 0, i, 0);
      do_test (&json_ctx, 0, 0, i, 1);
      do_test (&json_ctx, 0, 0, i, -1);
      do_test (&json_ctx, i, 0, i, 0);
      do_test (&json_ctx, 0, i, i, 1);
      do_test (&json_ctx, i, i, i, -1);
    }

  for (i = 1; i < 10; ++i)
    {
      do_test (&json_ctx, 0, 0, 2u << i, 0);
      do_test (&json_ctx, 0, 0, 2u << i, 1);
      do_test (&json_ctx, 0, 0, 2u << i, -1);
      do_test (&json_ctx, 8 - i, 2 * i, 16u << i, 0);
      do_test (&json_ctx, 0, 0, 16u << i, 0);
      do_test (&json_ctx, 0, 0, 16u << i, 1);
      do_test (&json_ctx, 0, 0, 16u << i, -1);
      do_test (&json_ctx, i, 0, 2u << i, 0);
      do_test (&json_ctx, 0, i, 2u << i, 1);
      do_test (&json_ctx, i, i, 2u << i, -1);
      do_test (&json_ctx, i, 0, 16u << i, 0);
      do_test (&json_ctx, 0, i, 16u << i, 1);
      do_test (&json_ctx, i, i, 16u << i, -1);
    }

  for (i = 1; i < 10; ++i)
    {
      do_test (&json_ctx, i, 2 * i, 8u << i, 0);
      do_test (&json_ctx, i, 2 * i, 8u << i, 1);
      do_test (&json_ctx, i, 2 * i, 8u << i, -1);
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

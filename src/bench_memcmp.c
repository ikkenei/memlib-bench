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

/* Run the loaded matrix: every case is one do_test() call.  */
static void
run_matrix (json_ctx_t *json_ctx, const mb_matrix_t *m)
{
  for (size_t k = 0; k < m->ncases; k++)
    {
      const mb_case_t *c = &m->cases[k];
      do_test (json_ctx, (size_t) c->a1, (size_t) c->a2, c->len, c->result);
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
    return mb_check_memcmp_run ("memcmp", &o, check_cimpl, check_coracle) ? 1 : 0;

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

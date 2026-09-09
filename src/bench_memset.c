/* Benchmark memset implementations (glibc benchtests methodology).  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench_drv.h"
#include "check.h"
#include "generic_ref.h"
#include "matrix.h"

typedef void *(*proto_t) (void *, int, size_t);

#define FN_SYMBOL "memset"
#define MIN_PAGE_SIZE (131072)

static const mb_opts_t *g_o;

static void
do_one_test (json_ctx_t *json_ctx, const mb_impl_t *impl, char *s, int c,
	     size_t len, size_t iters)
{
  proto_t f = (proto_t) impl->fn;
  size_t warm = iters / 64;
  if (warm < 8)
    warm = iters / 8;
  if (warm < 1)
    warm = 1;

  mb_current_impl = impl->name;
  for (size_t i = 0; i < warm; ++i)
    f (s, c, len);

  uint64_t t0 = mb_counter_read ();
  for (size_t i = 0; i < iters; ++i)
    f (s, c, len);
  uint64_t t1 = mb_counter_read ();
  mb_current_impl = NULL;

  double ns = (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
  json_element_double (json_ctx, ns / (double) iters);
}

static void
do_test (json_ctx_t *json_ctx, size_t align, int c, size_t len)
{
  if (g_o->max_len != 0 && len > g_o->max_len)
    return;
  align &= 4095;
  if (align + len > mb_page_size)
    return;

  size_t iters = mb_pick_iters (g_o, len == 0 ? 1 : len);

  json_element_object_begin (json_ctx);
  json_attr_uint (json_ctx, "length", len);
  json_attr_uint (json_ctx, "alignment", align);
  json_attr_int (json_ctx, "char", c);
  json_array_begin (json_ctx, "timings");

  for (int k = 0; k < mb_impl_count (); k++)
    do_one_test (json_ctx, mb_impl_get (k), (char *) mb_buf1.base + align,
		 c, len, iters);

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
      do_test (json_ctx, 0, 0, l);
      do_test (json_ctx, 3, 0, l + 1);
      do_test (json_ctx, 0, -1, l);	/* c = 0xff	*/
      do_test (json_ctx, 63, 0x80, l - 1);
      do_test (json_ctx, 0, 0x41, l + 3);
    }
}

static void
run_matrix (json_ctx_t *json_ctx, const mb_matrix_t *m)
{
  for (size_t il = 0; il < m->nsizes; il++)
    {
      size_t len = (size_t) m->sizes[il];
      for (size_t ia = 0; ia < m->nalign; ia++)
        for (size_t ic = 0; ic < m->nfill; ic++)
          do_test (json_ctx, (size_t) m->align[ia], (int) m->fill[ic], len);
    }
}


/* --- correctness mode ---------------------------------------------- */

static void
check_wimpl (const mb_impl_t *impl, void *dst, const void *src, int c,
	     size_t len)
{
  (void) src;
  ((proto_t) impl->fn) (dst, c, len);
}

static void
check_woracle (void *dst, const void *src, int c, size_t len)
{
  (void) src;
  mb_oracle_memset (dst, c, len);
}

int
main (int argc, char **argv)
{
  static mb_opts_t o;
  g_o = &o;
  mb_opts_defaults (&o);

  mb_install_crash_reporter ();

  mb_opts_parse (&o, argc, argv, FN_SYMBOL, NULL);

  if (mb_register_impls (&o, FN_SYMBOL, (mb_fn_t) mb_ref_memset) < 0)
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
  if (have_mx && (mx.nalign == 0 || mx.nfill == 0))
    {
      fprintf (stderr, "error: [memset] section must define 'align' and "
               "'fill' lists\n");
      return 1;
    }
  if (mb_buffers_init (MIN_PAGE_SIZE, want) == 0)
    {
      fprintf (stderr, "error: cannot allocate benchmark buffers\n");
      return 1;
    }

  if (o.check)
    return mb_check_write_run ("memset", 0, &o, check_wimpl,
			       check_woracle) ? 1 : 0;

  if (getenv ("MB_NO_WARMUP") == NULL)
    mb_warmup (0);

  json_ctx_t json_ctx;
  json_init (&json_ctx, 0, stdout);
  json_document_begin (&json_ctx);
  json_attr_string (&json_ctx, "timing_type", MB_TIMING_TYPE);
  json_attr_object_begin (&json_ctx, "functions");
  json_attr_object_begin (&json_ctx, "memset");
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
      int c = 0;

  for (c = -65; c <= 130; c += 65)
    {
      for (i = 0; i < 18; ++i)
	do_test (&json_ctx, 0, c, 1u << i);
      for (i = 0; i < 64; ++i)
	{
	  do_test (&json_ctx, i, c, i);
	  do_test (&json_ctx, 4096 - i, c, i);
	  do_test (&json_ctx, 4095, c, i);
	  if (i & (i - 1))
	    do_test (&json_ctx, 0, c, i);
	}
      for (i = 32; i < 1024; i += 32)
	{
	  do_test (&json_ctx, 0, c, i);
	  do_test (&json_ctx, i, c, i);
	}
      do_test (&json_ctx, 1, c, 14);
      do_test (&json_ctx, 3, c, 1024);
      do_test (&json_ctx, 4, c, 64);
      do_test (&json_ctx, 2, c, 25);
    }
  /* Trailing glibc block: long multiples of 32 bytes.  */
  for (i = 33; i <= 256; i += 4)
    {
      do_test (&json_ctx, 0, c, 32 * i);
      do_test (&json_ctx, i, c, 32 * i);
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

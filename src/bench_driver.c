/* Shared driver core (implementation).  See bench_driver.h. */

#include "bench_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const mb_opts_t *mb_opts;
size_t mb_real_page = 4096;

/* ------------------------------------------------------------------ */
/* Warm-up and measurement                                            */
/*                                                                    */
/* One loop per call signature keeps the measured call direct (no     */
/* extra indirection inside the timed loop).                          */
/* ------------------------------------------------------------------ */

static void
warm_up (const mb_func_t *f, const mb_impl_t *impl, const mb_pointers_t *p,
	 size_t iters)
{
  size_t warm = mb_warmup_iters (iters);

  mb_current_impl = impl->name;
  if (f->sig == MB_SIG_FILL)
    {
      void *(*fn) (void *, int, size_t)
	= (void *(*)(void *, int, size_t)) impl->fn;
      for (size_t i = 0; i < warm; i++)
	fn (p->dst, p->c, p->len);
    }
  else if (f->sig == MB_SIG_CMP)
    {
      int (*fn) (const void *, const void *, size_t)
	= (int (*)(const void *, const void *, size_t)) impl->fn;
      for (size_t i = 0; i < warm; i++)
	fn (p->dst, p->src, p->len);
    }
  else
    {
      void *(*fn) (void *, const void *, size_t)
	= (void *(*)(void *, const void *, size_t)) impl->fn;
      for (size_t i = 0; i < warm; i++)
	fn (p->dst, p->src, p->len);
    }
  mb_current_impl = NULL;
}

static double
measure (const mb_func_t *f, const mb_impl_t *impl, const mb_pointers_t *p,
	 size_t iters)
{
  uint64_t t0 = 0, t1 = 0;

  mb_current_impl = impl->name;
  if (f->sig == MB_SIG_FILL)
    {
      void *(*fn) (void *, int, size_t)
	= (void *(*)(void *, int, size_t)) impl->fn;
      t0 = mb_counter_read ();
      for (size_t i = 0; i < iters; i++)
	fn (p->dst, p->c, p->len);
      t1 = mb_counter_read ();
    }
  else if (f->sig == MB_SIG_CMP)
    {
      int (*fn) (const void *, const void *, size_t)
	= (int (*)(const void *, const void *, size_t)) impl->fn;
      t0 = mb_counter_read ();
      for (size_t i = 0; i < iters; i++)
	fn (p->dst, p->src, p->len);
      t1 = mb_counter_read ();
    }
  else
    {
      void *(*fn) (void *, const void *, size_t)
	= (void *(*)(void *, const void *, size_t)) impl->fn;
      t0 = mb_counter_read ();
      for (size_t i = 0; i < iters; i++)
	fn (p->dst, p->src, p->len);
      t1 = mb_counter_read ();
    }
  mb_current_impl = NULL;

  double ns = (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
  return ns / (double) iters;
}

/* ------------------------------------------------------------------ */
/* Matrix execution                                                   */
/* ------------------------------------------------------------------ */

static void
run_matrix (json_ctx_t *ctx, const mb_matrix_t *m, const mb_func_t *f)
{
  mb_pointers_t p;
  size_t reps = mb_opts->repeat > 0 ? (size_t) mb_opts->repeat : 1;

  for (size_t k = 0; k < m->ncases; k++)
    {
      const mb_case_t *c = &m->cases[k];
      int ndir = f->prepare (c, 0, &p);

      if (ndir <= 0)
	continue;
      size_t iters = mb_pick_iters (mb_opts, c->len == 0 ? 1 : c->len);

      for (int dir = 0; dir < ndir; dir++)
	{
	  if (dir > 0 && f->prepare (c, dir, &p) <= 0)
	    break;

	  /* Warm up every implementation once, then measure each of them
	     "reps" times.  */
	  for (int i = 0; i < mb_impl_count (); i++)
	    warm_up (f, mb_impl_get (i), &p, iters);

	  for (size_t r = 0; r < reps; r++)
	    {
	      json_element_object_begin (ctx);
	      f->attrs (ctx, c, &p);
	      if (reps > 1)
		json_attr_uint (ctx, "run", r);
	      json_array_begin (ctx, "timings");

	      for (int i = 0; i < mb_impl_count (); i++)
		json_element_double (ctx,
				     measure (f, mb_impl_get (i), &p,
					      iters));

	      json_array_end (ctx);
	      json_element_object_end (ctx);
	    }
	}
    }
}

/* ------------------------------------------------------------------ */
/* Correctness mode                                                   */
/* ------------------------------------------------------------------ */

static void
check_wimpl_copy (const mb_impl_t *impl, void *dst, const void *src, int c,
		  size_t len)
{
  (void) c;
  ((void *(*)(void *, const void *, size_t)) impl->fn) (dst, src, len);
}

static void
check_wimpl_fill (const mb_impl_t *impl, void *dst, const void *src, int c,
		  size_t len)
{
  (void) src;
  ((void *(*)(void *, int, size_t)) impl->fn) (dst, c, len);
}

static int
check_cimpl_cmp (const mb_impl_t *impl, const void *a, const void *b,
		 size_t len)
{
  return ((int (*)(const void *, const void *, size_t)) impl->fn) (a, b, len);
}

static int
run_check (const mb_func_t *f, const mb_opts_t *o)
{
  if (f->sig == MB_SIG_CMP)
    return mb_check_memcmp_run (f->name, o, check_cimpl_cmp, f->c_oracle);
  mb_wimpl_t wrap = (f->sig == MB_SIG_FILL) ? check_wimpl_fill
					    : check_wimpl_copy;
  return mb_check_write_run (f->name, f->sig == MB_SIG_MOVE, o, wrap,
			     f->w_oracle);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int
mb_driver_main (const mb_func_t *func, int argc, char **argv)
{
  static mb_opts_t o;
  mb_opts = &o;
  mb_opts_defaults (&o);

  mb_install_crash_reporter ();
  mb_opts_parse (&o, argc, argv, func->name, NULL);

  if (mb_register_impls (&o, func->name, func->generic_ref) < 0)
    return 1;

  long os_page = sysconf (_SC_PAGESIZE);
  mb_real_page = os_page > 0 ? (size_t) os_page : 4096;

  /* Test matrix: a --matrix profile or the compiled-in glibc defaults
     (small, plus the large matrix when --max-len is above 128 KiB).  */
  mb_matrix_t mx;
  mb_matrix_init (&mx);
  if (!o.check)
    {
      int rc = (o.matrix != NULL)
	? mb_matrix_load (o.matrix, func->name, &mx)
	: mb_matrix_load_default (func->name, o.max_len > (1u << 17), &mx);
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
    return run_check (func, &o) ? 1 : 0;

  if (getenv ("MB_NO_WARMUP") == NULL)
    mb_warmup (0);

  json_ctx_t ctx;
  json_init (&ctx, 0, stdout);
  json_document_begin (&ctx);
  json_attr_string (&ctx, "timing_type", MB_TIMING_TYPE);
  json_attr_object_begin (&ctx, "functions");
  json_attr_object_begin (&ctx, func->name);
  json_attr_string (&ctx, "bench-variant", "default");

  json_array_begin (&ctx, "ifuncs");
  for (int i = 0; i < mb_impl_count (); i++)
    json_element_string (&ctx, mb_impl_get (i)->name);
  json_array_end (&ctx);

  json_array_begin (&ctx, "results");
  run_matrix (&ctx, &mx, func);
  json_array_end (&ctx);

  json_attr_object_end (&ctx);
  json_attr_object_end (&ctx);
  json_document_end (&ctx);
  putchar ('\n');

  mb_matrix_free (&mx);
  mb_buffers_free ();
  return 0;
}

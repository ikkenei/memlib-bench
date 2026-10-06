/* Shared driver core (implementation).  See bench_driver.h. */

#include "bench_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const mb_opts_t *mb_opts;
size_t mb_real_page = 4096;

/* Hard limit for a single precision-mode sample (like llvm-libc's
   max_iterations).  */
#define MB_PRECISION_MAX_ITERS ((size_t) 10000000)

/* ------------------------------------------------------------------ */
/* Warm-up and measurement                                            */
/*                                                                    */
/* One loop per call signature keeps the measured call direct (no     */
/* extra indirection inside the timed loop).  The "hot" mode measures */
/* one call repeated; the batch modes iterate an array of prepared    */
/* calls with randomized parameters.                                  */
/* ------------------------------------------------------------------ */

static inline size_t
chunk (size_t remaining, size_t batch)
{
  return remaining < batch ? remaining : batch;
}

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

/* Total time (ns) of ITERS calls on one fixed set of pointers.  */
static double
measure_one_ns (const mb_func_t *f, const mb_impl_t *impl,
		const mb_pointers_t *p, size_t iters)
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

  return (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
}

/* ------------------------------------------------------------------ */
/* Randomized batches (--measure offsets / mixed)                     */
/* ------------------------------------------------------------------ */

typedef struct
{
  mb_pointers_t *p;
  size_t n;
} mb_batch_t;

static uint64_t rng_state;

static uint64_t
rng_next (void)
{
  uint64_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  rng_state = x;
  return x;
}

static uint64_t
rng_below (uint64_t n)
{
  return n > 0 ? rng_next () % n : 0;
}

static void
batch_free (mb_batch_t *b)
{
  free (b->p);
  b->p = NULL;
  b->n = 0;
}

/* Build a batch of COUNT prepared calls.  Sizes are drawn uniformly from
   SIZES (all equal for the "offsets" mode) and the offsets are randomly
   placed within the page.  C provides the function-specific parameters
   (fill byte, expected result).  */
static int
batch_build (const mb_func_t *f, const mb_case_t *c, const size_t *sizes,
	     size_t nsizes, size_t count, mb_batch_t *b)
{
  unsigned long mask = mb_real_page > 0 ? (unsigned long) (mb_real_page - 1)
					: 4095ul;
  long long delta = (long long) c->a2 - (long long) c->a1;

  b->p = malloc (count * sizeof *b->p);
  if (b->p == NULL)
    return -1;
  b->n = count;

  for (size_t i = 0; i < count; i++)
    {
      size_t len = sizes[rng_below (nsizes)];
      int ok = 0;

      for (int tries = 0; tries < 16 && !ok; tries++)
	{
	  mb_case_t t = *c;
	  t.len = len;
	  t.both = 0;			/* batch modes run one direction */

	  if (f->sig == MB_SIG_MOVE)
	    {
	      /* Keep the overlap distance of the matrix case.  */
	      long long lo = delta < 0 ? -delta : 0;
	      long long hi = (long long) mask - (delta > 0 ? delta : 0);
	      if (hi < lo)
		{
		  t.a1 = c->a1;
		  t.a2 = c->a2;
		}
	      else
		{
		  long long base = lo + (long long) rng_below ((uint64_t)
							       (hi - lo + 1));
		  t.a1 = base;
		  t.a2 = base + delta;
		}
	    }
	  else
	    {
	      t.a1 = (long) rng_below (mask + 1);
	      t.a2 = (long) rng_below (mask + 1);
	    }
	  ok = f->prepare (&t, 0, &b->p[i]) > 0;
	}

      if (!ok)
	{
	  mb_case_t t = *c;
	  t.both = 0;
	  if (f->prepare (&t, 0, &b->p[i]) <= 0)
	    {
	      batch_free (b);
	      return -1;
	    }
	}
    }

  if (getenv ("MB_DEBUG_BATCH") != NULL)
    fprintf (stderr, "[memlib] batch: n=%zu first=(len=%zu, off=%.0f/%.0f)\n",
	     b->n, c->len,
	     (double) (size_t) (char *) b->p[0].dst - (double) (size_t) (char *) b->p[0].src,
	     (double) b->p[0].len);
  return 0;
}

static void
warm_batch (const mb_func_t *f, const mb_impl_t *impl, const mb_batch_t *b,
	    size_t iters)
{
  size_t warm = mb_warmup_iters (iters);

  mb_current_impl = impl->name;
  if (f->sig == MB_SIG_FILL)
    {
      void *(*fn) (void *, int, size_t)
	= (void *(*)(void *, int, size_t)) impl->fn;
      for (size_t done = 0; done < warm;)
	{
	  size_t k = chunk (warm - done, b->n);
	  for (size_t j = 0; j < k; j++)
	  fn (b->p[j].dst, b->p[j].c, b->p[j].len);
	  done += k;
	}
    }
  else if (f->sig == MB_SIG_CMP)
    {
      int (*fn) (const void *, const void *, size_t)
	= (int (*)(const void *, const void *, size_t)) impl->fn;
      for (size_t done = 0; done < warm;)
	{
	  size_t k = chunk (warm - done, b->n);
	  for (size_t j = 0; j < k; j++)
	  fn (b->p[j].dst, b->p[j].src, b->p[j].len);
	  done += k;
	}
    }
  else
    {
      void *(*fn) (void *, const void *, size_t)
	= (void *(*)(void *, const void *, size_t)) impl->fn;
      for (size_t done = 0; done < warm;)
	{
	  size_t k = chunk (warm - done, b->n);
	  for (size_t j = 0; j < k; j++)
	  fn (b->p[j].dst, b->p[j].src, b->p[j].len);
	  done += k;
	}
    }
  mb_current_impl = NULL;
}

/* Total time (ns) of ITERS calls cycling through the batch.  */
static double
measure_batch_ns (const mb_func_t *f, const mb_impl_t *impl,
		  const mb_batch_t *b, size_t iters)
{
  uint64_t t0 = 0, t1 = 0;

  mb_current_impl = impl->name;
  if (f->sig == MB_SIG_FILL)
    {
      void *(*fn) (void *, int, size_t)
	= (void *(*)(void *, int, size_t)) impl->fn;
      t0 = mb_counter_read ();
      for (size_t done = 0; done < iters;)
	{
	  size_t k = chunk (iters - done, b->n);
	  for (size_t j = 0; j < k; j++)
	  fn (b->p[j].dst, b->p[j].c, b->p[j].len);
	  done += k;
	}
      t1 = mb_counter_read ();
    }
  else if (f->sig == MB_SIG_CMP)
    {
      int (*fn) (const void *, const void *, size_t)
	= (int (*)(const void *, const void *, size_t)) impl->fn;
      t0 = mb_counter_read ();
      for (size_t done = 0; done < iters;)
	{
	  size_t k = chunk (iters - done, b->n);
	  for (size_t j = 0; j < k; j++)
	  fn (b->p[j].dst, b->p[j].src, b->p[j].len);
	  done += k;
	}
      t1 = mb_counter_read ();
    }
  else
    {
      void *(*fn) (void *, const void *, size_t)
	= (void *(*)(void *, const void *, size_t)) impl->fn;
      t0 = mb_counter_read ();
      for (size_t done = 0; done < iters;)
	{
	  size_t k = chunk (iters - done, b->n);
	  for (size_t j = 0; j < k; j++)
	  fn (b->p[j].dst, b->p[j].src, b->p[j].len);
	  done += k;
	}
      t1 = mb_counter_read ();
    }
  mb_current_impl = NULL;

  return (double) mb_counter_diff_ns (t0, t1, mb_counter_freq ());
}

/* ------------------------------------------------------------------ */
/* Iteration control                                                  */
/* ------------------------------------------------------------------ */

/* One measurement (ns per call) with the configured iteration policy.
   When USBATCH, B is used, otherwise the single pointer P.  */
static double
measure_impl (const mb_func_t *f, const mb_impl_t *impl,
	      const mb_pointers_t *p, const mb_batch_t *b, int usebatch,
	      size_t iters)
{
  if (mb_opts->iters_mode == MB_ITERS_BUDGET)
    {
      double ns = usebatch ? measure_batch_ns (f, impl, b, iters)
			   : measure_one_ns (f, impl, p, iters);
      return ns / (double) iters;
    }

  /* Precision mode: grow the iteration count until the running estimate
     settles within --epsilon (like llvm-libc's stopping rule).  */
  size_t cur = (size_t) mb_opts->initial_iters;
  double total_ns = 0.0;
  size_t total_calls = 0, samples = 0;
  double prev = 0.0;

  for (;;)
    {
      double ns = usebatch ? measure_batch_ns (f, impl, b, cur)
			   : measure_one_ns (f, impl, p, cur);
      total_ns += ns;
      total_calls += cur;
      samples++;

      double est = total_ns / (double) total_calls;
      double change = 1.0;
      if (prev > 0.0)
	{
	  change = est / prev - 1.0;
	  if (change < 0.0)
	    change = -change;
	}
      prev = est;

      if (total_ns >= mb_opts->min_duration * 1e9
	  && samples >= (size_t) mb_opts->min_samples
	  && change < mb_opts->epsilon)
	break;
      if (samples >= (size_t) mb_opts->max_samples
	  || total_ns >= mb_opts->max_duration * 1e9
	  || cur >= MB_PRECISION_MAX_ITERS)
	break;

      size_t next = (size_t) (cur * mb_opts->scaling);
      cur = next > cur ? next : cur + 1;
    }

  if (getenv ("MB_DEBUG_PRECISION") != NULL)
    fprintf (stderr, "[memlib] precision: %-16s %zu samples, %zu calls, "
	     "%.3f ns/call\n", impl->name, samples, total_calls,
	     total_ns / (double) total_calls);

  return total_ns / (double) total_calls;
}

/* ------------------------------------------------------------------ */
/* Matrix execution                                                   */
/* ------------------------------------------------------------------ */

/* Emit the JSON object of one randomized batch.  RUN < 0 omits the
   repetition attribute.  */
static void
emit_batch (json_ctx_t *ctx, const mb_func_t *f, const mb_case_t *c,
	    const mb_batch_t *b, size_t length, const char *sizes,
	    long run, size_t iters)
{
  json_element_object_begin (ctx);
  json_attr_uint (ctx, "length", length);
  json_attr_string (ctx, "offsets", "random");
  json_attr_uint (ctx, "batch", b->n);
  if (sizes != NULL)
    json_attr_string (ctx, "sizes", sizes);
  if (f->attrs_batch != NULL)
    f->attrs_batch (ctx, c);
  if (run >= 0)
    json_attr_uint (ctx, "run", (uint64_t) run);
  json_array_begin (ctx, "timings");
  for (int i = 0; i < mb_impl_count (); i++)
    json_element_double (ctx,
			 measure_impl (f, mb_impl_get (i), NULL, b, 1,
				       iters));
  json_array_end (ctx);
  json_element_object_end (ctx);
}

/* One batch of COUNT random offsets for the fixed-size case C.  */
static void
run_offsets_mode (json_ctx_t *ctx, const mb_matrix_t *m, const mb_func_t *f,
		  size_t reps)
{
  size_t count = (size_t) mb_opts->batch;
  mb_pointers_t probe;

  for (size_t k = 0; k < m->ncases; k++)
    {
      const mb_case_t *c = &m->cases[k];
      if (f->prepare (c, 0, &probe) <= 0)	/* bounds and --max-len */
	continue;

      size_t iters = mb_pick_iters (mb_opts, c->len == 0 ? 1 : c->len);
      for (size_t r = 0; r < reps; r++)
	{
	  size_t sizes[1] = { c->len };
	  mb_batch_t b;
	  if (batch_build (f, c, sizes, 1, count, &b) != 0)
	    continue;
	  for (int i = 0; i < mb_impl_count (); i++)
	    warm_batch (f, mb_impl_get (i), &b, iters);
	  emit_batch (ctx, f, c, &b, c->len, NULL,
		      reps > 1 ? (long) r : -1, iters);
	  batch_free (&b);
	}
    }
}

/* One group of mixed-mode cases: the function-specific parameters plus
   the pool of sizes seen in the matrix.  */
struct mixed_group
{
  long key;			/* fill byte or expected result		*/
  mb_case_t rep;		/* representative case			*/
  size_t *pool;
  size_t n, cap;
};

/* Randomized sizes (pooled from the matrix) and offsets.  The matrix
   cases are grouped by the function-specific parameters (fill byte,
   expected result); each group produces one result per repetition, and
   every repetition gets a fresh randomized batch.  */
static void
run_mixed_mode (json_ctx_t *ctx, const mb_matrix_t *m, const mb_func_t *f,
		size_t reps)
{
  size_t count = (size_t) mb_opts->batch;
  struct mixed_group *groups = NULL;
  size_t ngroups = 0, cgroups = 0;
  mb_pointers_t probe;

  for (size_t k = 0; k < m->ncases; k++)
    {
      const mb_case_t *c = &m->cases[k];
      long key = 0;
      if (f->sig == MB_SIG_FILL)
	key = (long) c->c;
      else if (f->sig == MB_SIG_CMP)
	key = (long) c->result;

      struct mixed_group *g = NULL;
      for (size_t j = 0; j < ngroups; j++)
	if (groups[j].key == key)
	  {
	    g = &groups[j];
	    break;
	  }
      if (g == NULL)
	{
	  if (ngroups == cgroups)
	    {
	      cgroups = cgroups ? cgroups * 2 : 8;
	      struct mixed_group *ng = realloc (groups,
						cgroups * sizeof *ng);
	      if (ng == NULL)
		{
		  free (groups);
		  return;
		}
	      groups = ng;
	    }
	  g = &groups[ngroups++];
	  memset (g, 0, sizeof *g);
	  g->key = key;
	  g->rep = *c;
	}

      if (f->prepare (c, 0, &probe) <= 0)	/* bounds and --max-len */
	continue;
      int seen = 0;
      for (size_t j = 0; j < g->n; j++)
	if (g->pool[j] == c->len)
	  {
	    seen = 1;
	    break;
	  }
      if (seen)
	continue;
      if (g->n == g->cap)
	{
	  g->cap = g->cap ? g->cap * 2 : 32;
	  size_t *np = realloc (g->pool, g->cap * sizeof *np);
	  if (np == NULL)
	    {
	      free (groups);
	      return;
	    }
	  g->pool = np;
	}
      g->pool[g->n++] = c->len;
    }

  for (size_t j = 0; j < ngroups; j++)
    {
      struct mixed_group *g = &groups[j];
      if (g->n == 0)
	continue;

      size_t minlen = g->pool[0], maxlen = g->pool[0];
      double sum = 0.0;
      for (size_t i = 0; i < g->n; i++)
	{
	  if (g->pool[i] < minlen)
	    minlen = g->pool[i];
	  if (g->pool[i] > maxlen)
	    maxlen = g->pool[i];
	  sum += (double) g->pool[i];
	}
      size_t mean = (size_t) (sum / (double) g->n + 0.5);
      char sizes_str[64];
      snprintf (sizes_str, sizeof sizes_str, "%zu..%zu", minlen, maxlen);
      size_t iters = mb_pick_iters (mb_opts, mean == 0 ? 1 : mean);

      for (size_t r = 0; r < reps; r++)
	{
	  mb_batch_t b;
	  if (batch_build (f, &g->rep, g->pool, g->n, count, &b) != 0)
	    break;
	  for (int i = 0; i < mb_impl_count (); i++)
	    warm_batch (f, mb_impl_get (i), &b, iters);
	  emit_batch (ctx, f, &g->rep, &b, mean, sizes_str,
		      reps > 1 ? (long) r : -1, iters);
	  batch_free (&b);
	}
    }

  for (size_t j = 0; j < ngroups; j++)
    free (groups[j].pool);
  free (groups);
}

static void
run_matrix (json_ctx_t *ctx, const mb_matrix_t *m, const mb_func_t *f)
{
  mb_pointers_t p;
  size_t reps = mb_opts->repeat > 0 ? (size_t) mb_opts->repeat : 1;

  if (mb_opts->measure == MB_MEASURE_OFFSETS)
    {
      run_offsets_mode (ctx, m, f, reps);
      return;
    }
  if (mb_opts->measure == MB_MEASURE_MIXED)
    {
      run_mixed_mode (ctx, m, f, reps);
      return;
    }

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
				     measure_impl (f, mb_impl_get (i), &p,
						   NULL, 0, iters));

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

  rng_state = (uint64_t) o.seed | UINT64_C (1);

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

  /* The batch modes do not fill the source pattern per case (that would
     dominate the run for large sizes); touch the buffers once so that no
     page fault lands inside a measurement.  */
  if (o.measure != MB_MEASURE_HOT)
    {
      mb_fill (mb_buf1.base, mb_buf1.size, o.seed ^ 0x5a5a5a5aUL);
      mb_fill (mb_buf2.base, mb_buf2.size, o.seed ^ 0xa5a5a5a5UL);
    }

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

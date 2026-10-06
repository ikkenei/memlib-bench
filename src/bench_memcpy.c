/* Benchmark memcpy implementations (glibc benchtests methodology).

   Source and destination live in separate buffers; when a matrix case
   asks for both directions (both = 1) the two directions are measured
   separately and told apart by the "dst > src" attribute.

   Everything else is the shared driver core in bench_driver.c.
 */

#include "bench_driver.h"
#include "generic_ref.h"

typedef void *(*proto_t) (void *, const void *, size_t);

static int
prepare (const mb_case_t *c, int dir, mb_pointers_t *p)
{
  if (mb_skip_len (c->len))
    return 0;

  size_t a1 = (size_t) c->a1 & (mb_real_page - 1);
  size_t a2 = (size_t) c->a2 & (mb_real_page - 1);
  if (a1 + c->len >= mb_page_size || a2 + c->len >= mb_page_size)
    return 0;

  char *s1 = (char *) (dir ? mb_buf2.base + a1 : mb_buf1.base + a1);
  char *s2 = (char *) (dir ? mb_buf1.base + a2 : mb_buf2.base + a2);

  /* The source content does not matter for the measurement (the
     buffers are pre-filled once), and filling it per batch element would
     dominate the run for large sizes.  */
  if (mb_opts->measure == MB_MEASURE_HOT)
    for (size_t i = 0, j = 1; i < c->len; i++, j += 23)
      s1[i] = (char) j;

  p->dst = s2;
  p->src = s1;
  p->len = c->len;
  p->a1 = a1;
  p->a2 = a2;
  return c->both ? 2 : 1;
}

static void
attrs (json_ctx_t *ctx, const mb_case_t *c, const mb_pointers_t *p)
{
  json_attr_uint (ctx, "length", c->len);
  json_attr_uint (ctx, "align1", p->a1);
  json_attr_uint (ctx, "align2", p->a2);
  json_attr_uint (ctx, "dst > src",
		  (uintptr_t) p->dst > (uintptr_t) p->src);
}

static const mb_func_t the_function = {
  .name = "memcpy",
  .sig = MB_SIG_COPY,
  .generic_ref = (mb_fn_t) mb_ref_memcpy,
  .w_oracle = mb_oracle_memcpy,
  .prepare = prepare,
  .attrs = attrs,
};

int
main (int argc, char **argv)
{
  return mb_driver_main (&the_function, argc, argv);
}

/* Benchmark memmove implementations (glibc benchtests methodology).

   As in glibc bench-memmove, source and destination live in the SAME
   buffer at controlled offsets, so the measured cases include genuine
   overlapping moves in both directions.

   Everything else is the shared driver core in bench_driver.c.
 */

#include "bench_driver.h"
#include "generic_ref.h"

typedef void *(*proto_t) (void *, const void *, size_t);

static int
prepare (const mb_case_t *c, int dir, mb_pointers_t *p)
{
  (void) dir;				/* always a single direction */

  if (mb_skip_len (c->len))
    return 0;

  size_t a1 = (size_t) c->a1 & (mb_real_page - 1);
  size_t a2 = (size_t) c->a2 & (mb_real_page - 1);
  if (a1 + c->len >= mb_page_size || a2 + c->len >= mb_page_size)
    return 0;

  char *s1 = (char *) (mb_buf2.base + a1);	/* source	*/
  char *s2 = (char *) (mb_buf2.base + a2);	/* destination	*/

  if (mb_opts->measure == MB_MEASURE_HOT)
    for (size_t i = 0, j = 1; i < c->len; i++, j += 23)
      s1[i] = (char) j;

  p->dst = s2;
  p->src = s1;
  p->len = c->len;
  p->a1 = a1;
  p->a2 = a2;
  return 1;
}

static void
attrs (json_ctx_t *ctx, const mb_case_t *c, const mb_pointers_t *p)
{
  json_attr_uint (ctx, "length", c->len);
  json_attr_uint (ctx, "align1", p->a1);
  json_attr_uint (ctx, "align2", p->a2);
}

static const mb_func_t the_function = {
  .name = "memmove",
  .sig = MB_SIG_MOVE,
  .generic_ref = (mb_fn_t) mb_ref_memmove,
  .w_oracle = mb_oracle_memmove,
  .prepare = prepare,
  .attrs = attrs,
};

int
main (int argc, char **argv)
{
  return mb_driver_main (&the_function, argc, argv);
}

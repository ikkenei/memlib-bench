/* Benchmark memset implementations (glibc benchtests methodology).

   The matrix case carries the alignment and the fill byte ("char" in the
   JSON); as in glibc bench-memset the whole destination lives in buf1.

   Everything else is the shared driver core in bench_driver.c.
 */

#include "bench_driver.h"
#include "generic_ref.h"

typedef void *(*proto_t) (void *, int, size_t);

static int
prepare (const mb_case_t *c, int dir, mb_pointers_t *p)
{
  (void) dir;				/* always a single direction */

  if (mb_skip_len (c->len))
    return 0;

  size_t align = (size_t) c->a1 & 4095;
  if (align + c->len > mb_page_size)
    return 0;

  p->dst = mb_buf1.base + align;
  p->src = NULL;
  p->len = c->len;
  p->c = c->c;
  p->a1 = align;
  p->a2 = 0;
  return 1;
}

static void
attrs (json_ctx_t *ctx, const mb_case_t *c, const mb_pointers_t *p)
{
  json_attr_uint (ctx, "length", c->len);
  json_attr_uint (ctx, "alignment", p->a1);
  json_attr_int (ctx, "char", c->c);
}

static const mb_func_t the_function = {
  .name = "memset",
  .sig = MB_SIG_FILL,
  .generic_ref = (mb_fn_t) mb_ref_memset,
  .w_oracle = mb_oracle_memset,
  .prepare = prepare,
  .attrs = attrs,
};

int
main (int argc, char **argv)
{
  return mb_driver_main (&the_function, argc, argv);
}

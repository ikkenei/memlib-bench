/* Benchmark memcmp implementations (glibc benchtests methodology).

   The matrix case carries both offsets and the expected result ("result"
   in the JSON): 0 for equal buffers, +/-1 for buffers differing in the
   last byte.  A sentinel byte is written just past the compared range, as
   in glibc bench-memcmp.

   Everything else is the shared driver core in bench_driver.c.
 */

#include "bench_driver.h"
#include "generic_ref.h"

typedef int (*proto_t) (const void *, const void *, size_t);

static int
prepare (const mb_case_t *c, int dir, mb_pointers_t *p)
{
  (void) dir;				/* always a single direction */

  if (mb_skip_len (c->len))
    return 0;

  size_t a1 = (size_t) c->a1 & 4095;
  size_t a2 = (size_t) c->a2 & 4095;
  if (a1 + c->len + 1 >= mb_page_size || a2 + c->len + 1 >= mb_page_size)
    return 0;

  char *s1 = (char *) (mb_buf1.base + a1);
  char *s2 = (char *) (mb_buf2.base + a2);

  for (size_t i = 0; i < c->len; i++)
    s1[i] = s2[i] = (char) (1 + (23 * i) % 255);

  if (c->len)
    {
      s1[c->len] = (char) a1;
      s2[c->len] = (char) a2;
      s2[c->len - 1] -= (char) c->result;
    }

  p->dst = s1;
  p->src = s2;
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
  json_attr_int (ctx, "result", c->result);
}

static const mb_func_t the_function = {
  .name = "memcmp",
  .sig = MB_SIG_CMP,
  .generic_ref = (mb_fn_t) mb_ref_memcmp,
  .c_oracle = mb_oracle_memcmp,
  .prepare = prepare,
  .attrs = attrs,
};

int
main (int argc, char **argv)
{
  return mb_driver_main (&the_function, argc, argv);
}

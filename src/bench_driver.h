/* Shared driver core for the four function benchmarks.

   Each driver (bench_memcpy.c, bench_memmove.c, bench_memset.c,
   bench_memcmp.c) is a thin description of one function: its call
   signature, the generic C baseline, the correctness oracle, and two
   callbacks that prepare a matrix case (pointers + content) and emit its
   JSON attributes.  Everything else - option parsing, implementation
   registration, matrix loading, buffers, warm-up/measure loops, repeats,
   the JSON document and the check dispatch - lives in bench_driver.c.
 */

#ifndef MB_BENCH_DRIVER_H
#define MB_BENCH_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "bench_drv.h"
#include "check.h"
#include "json-lib.h"

/* Call signature of the measured function.  */
enum
{
  MB_SIG_COPY = 0,		/* void *f (void *, const void *, size_t)  */
  MB_SIG_MOVE,			/* same, but overlapping (memmove)	   */
  MB_SIG_FILL,			/* void *f (void *, int, size_t)	   */
  MB_SIG_CMP			/* int f (const void *, const void *, size_t) */
};

/* Arguments of one measured call, prepared by a driver.  */
typedef struct
{
  void *dst;			/* first argument (destination)		   */
  const void *src;		/* second argument (source, NULL for fill) */
  size_t len;			/* length				   */
  int c;			/* fill byte (MB_SIG_FILL)		   */
  unsigned long a1, a2;		/* offsets as written to the JSON	   */
} mb_pointers_t;

/* One benchmark function.  */
typedef struct
{
  const char *name;		/* "memcpy" (also the matrix section)	   */

  int sig;			/* MB_SIG_*				   */

  mb_fn_t generic_ref;		/* generic C baseline (--generic)	   */

  mb_woracle_t w_oracle;	/* byte oracle for the copy/fill signatures */
  mb_coracle_t c_oracle;	/* byte oracle for MB_SIG_CMP		   */

  /* Prepare direction DIR (0..n-1) of case C.  For DIR == 0 the return
     value is the number of directions (0 = skip the case, 2 = both copy
     directions); for DIR > 0 a non-positive return means "stop".  */
  int (*prepare) (const mb_case_t *c, int dir, mb_pointers_t *p);

  /* Emit the JSON attributes of a case (before its "timings" array).  */
  void (*attrs) (json_ctx_t *ctx, const mb_case_t *c,
		 const mb_pointers_t *p);

  /* Optional: the function-specific parameters of a batch-mode result
     (fill byte, expected result); offsets and sizes are randomized.  */
  void (*attrs_batch) (json_ctx_t *ctx, const mb_case_t *c);
} mb_func_t;

/* Options of the running driver and the OS page size (set by
   mb_driver_main).  */
extern const mb_opts_t *mb_opts;
extern size_t mb_real_page;

/* Run a driver end to end: parse options, register implementations, load
   the matrix, run the benchmark (or the correctness check) and print the
   JSON.  Returns the process exit status.  */
int mb_driver_main (const mb_func_t *func, int argc, char **argv);

/* prepare() helper: true when the case exceeds --max-len.  */
static inline int
mb_skip_len (size_t len)
{
  return mb_opts->max_len != 0 && len > mb_opts->max_len;
}

#endif /* MB_BENCH_DRIVER_H */

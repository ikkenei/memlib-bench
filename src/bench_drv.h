/* Shared driver runtime for the memlib benchmark suite.

   Provides option parsing, implementation selection, the adaptive
   iteration policy and JSON boilerplate helpers used by all four
   benchmark drivers (memcpy / memmove / memset / memcmp).

   Measurement methodology follows glibc benchtests:
   - page-aligned buffers with a PROT_NONE guard page at the end;
   - alignment/length sweeps plus (for copy functions) tests in both
     directions (dst before/after src);
   - a short warm-up burst per test, then N iterations measured with a
     single counter read around the whole loop, reported as average
     nanoseconds per call;
   - glibc-compatible JSON output (benchout_strings schema).

   Unlike glibc (which uses a fixed iteration count per function), the
   number of iterations is adapted per test so that every measurement
   touches roughly the same amount of memory (MB_BUDGET bytes).  This
   keeps total run time bounded while preserving timing resolution.
 */

#ifndef MB_BENCH_DRV_H
#define MB_BENCH_DRV_H

#include <stddef.h>
#include <stdio.h>

#include "bench_common.h"
#include "json-lib.h"
#include "timing.h"

/* Default adaptive policy.  */
#define MB_DEF_BUDGET_MIB 16.0	/* MiB touched per (impl, test)	*/
#define MB_DEF_MIN_ITERS 8	/* lower bound on iterations	*/
#define MB_DEF_MAX_ITERS (1u << 21)	/* upper bound		*/

typedef struct
{
  int check;			/* correctness mode		*/
  int no_libc;			/* drop the libc baseline	*/
  int generic;			/* include the generic C baseline */
  int quick;			/* short run			*/
  long fixed_iters;		/* 0 => adaptive		*/
  double budget_mib;
  size_t min_iters;
  size_t max_iters;
  size_t max_len;		/* 0 => matrix default		*/
  unsigned long seed;		/* pattern seed		*/
  const char *matrix;		/* matrix profile file		*/
  const char *impl_paths[32];	/* custom impl shared objects	*/
  const char *impl_labels[32];
  int impl_count;
} mb_opts_t;

void mb_opts_defaults (mb_opts_t *o);
int mb_opts_parse (mb_opts_t *o, int argc, char **argv,
		   const char *func_symbol, const char *usage_extra);
void mb_opts_usage (const char *argv0, const char *func_symbol,
		    const char *usage_extra, FILE *out);

/* Register "libc" (from dlsym) and "generic" baselines plus all custom
   implementations requested on the command line.  Drivers pass their own
   generic reference function and the symbol they export/import.
   Returns number of registered implementations (>=1) or -1.  */
int mb_register_impls (const mb_opts_t *o, const char *func_symbol,
		       mb_fn_t generic_fn);

/* Iterations for a test touching BYTES bytes per call.  */
size_t mb_pick_iters (const mb_opts_t *o, size_t bytes);

#endif /* MB_BENCH_DRV_H */

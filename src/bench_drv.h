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
#include "matrix.h"
#include "timing.h"

/* Smallest buffer size (bytes) used by the drivers, like glibc's
   MIN_PAGE_SIZE in bench-mem*.c.  */
#define MB_MIN_PAGE_SIZE (131072)

/* Default adaptive policy.  */
#define MB_DEF_BUDGET_MIB 16.0	/* MiB touched per (impl, test)	*/
#define MB_DEF_MIN_ITERS 8	/* lower bound on iterations	*/
#define MB_DEF_MAX_ITERS (1u << 21)	/* upper bound		*/

/* How each matrix case is measured.  */
enum
{
  MB_MEASURE_HOT = 0,		/* repeat the same call in a hot loop	*/
  MB_MEASURE_OFFSETS,		/* fixed sizes, random offsets per call	*/
  MB_MEASURE_MIXED		/* random sizes and offsets per call	*/
};

/* How the number of iterations is chosen.  */
enum
{
  MB_ITERS_BUDGET = 0,		/* fixed byte budget, decided up front	*/
  MB_ITERS_PRECISION		/* grow until the estimate settles	*/
};

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
  long repeat;			/* measurements per test (>=1)	*/
  unsigned long seed;		/* pattern seed		*/
  const char *matrix;		/* matrix profile file		*/
  int measure;			/* MB_MEASURE_*			*/
  long batch;			/* calls per randomized batch		*/
  long mismatch_at;		/* memcmp: mismatch at byte N-1 (0 = off) */
  int iters_mode;		/* MB_ITERS_*			*/
  double epsilon;		/* precision mode target		*/
  double scaling;		/* precision mode growth factor		*/
  long initial_iters;		/* precision mode first sample size	*/
  long min_samples;		/* precision mode limits		*/
  long max_samples;
  double min_duration;		/* seconds				*/
  double max_duration;
  char impl_paths[32][1024];	/* custom impl shared objects	*/
  char impl_labels[32][128];
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

/* Warm-up iterations for a test with ITERS measured iterations. */
size_t mb_warmup_iters (size_t iters);

#endif /* MB_BENCH_DRV_H */

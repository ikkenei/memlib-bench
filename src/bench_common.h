/* Common infrastructure for the memlib benchmark suite.

   Implements the same techniques glibc benchtests use:

   - mmap'ed buffers with a PROT_NONE guard page at the end, so that any
     read/write past the end of a buffer faults immediately;
   - an implementation registry (built-in "libc" and "generic" baselines
     plus user implementations dlopen'ed from shared objects);
   - warm-up code to let the CPU reach its steady-state frequency before
     measurements;
   - a crash reporter that identifies which implementation faulted;
   - deterministic pseudo-random pattern filling for correctness checks.

   Self-contained; no glibc build-system dependencies.
 */

#ifndef MB_BENCH_COMMON_H
#define MB_BENCH_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Implementation registry.                                            */
/* ------------------------------------------------------------------ */

typedef void (*mb_fn_t) (void);

typedef struct
{
  const char *name;	/* label used in JSON "ifuncs" and tables	*/
  mb_fn_t fn;		/* cast to the right prototype when calling	*/
  int kind;		/* 0 = libc baseline, 1 = generic, 2 = custom	*/
} mb_impl_t;

enum
{
  MB_KIND_LIBC = 0,
  MB_KIND_GENERIC = 1,
  MB_KIND_CUSTOM = 2
};

/* Number of registered implementations and accessor.  */
int mb_impl_count (void);
const mb_impl_t *mb_impl_get (int idx);

/* Add an already-resolved function.  Returns 0 or -1 on failure.  */
int mb_impl_add (const char *name, mb_fn_t fn, int kind);

/* Register the system libc implementation of SYMBOL (e.g. "memcpy") as
   the "libc" baseline.  Resolution happens via dlsym(RTLD_DEFAULT).  */
int mb_impl_add_libc (const char *symbol);

/* dlopen PATH and register symbol SYMBOL found inside it under LABEL.
   The library is opened RTLD_LOCAL so it cannot interpose on the
   benchmark driver itself.  Returns 0 or -1 (see mb_err).  */
int mb_impl_add_dlopen (const char *path, const char *symbol,
			const char *label);

/* Last error message (valid after a failing mb_* call).  */
const char *mb_err (void);

/* ------------------------------------------------------------------ */
/* Buffers with guard pages.                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
  unsigned char *base;	/* start of the usable region		*/
  size_t size;		/* usable size in bytes		*/
} mb_buf_t;

/* Allocate buffers big enough for MAX_LEN (plus slack), but never less
   than MIN_PAGE bytes per buffer.  buf1 and buf2 each get a guard page
   at the end.  On success returns the usable size of each buffer
   (>= min (MAX_LEN + slack, ...)); on failure returns 0.  */
size_t mb_buffers_init (size_t min_page, size_t max_len);

void mb_buffers_free (void);

extern mb_buf_t mb_buf1, mb_buf2;
extern size_t mb_page_size;	/* usable size of each buffer	*/

/* ------------------------------------------------------------------ */
/* Misc helpers.                                                       */
/* ------------------------------------------------------------------ */

/* Run the CPU frequency ramp-up loop (glibc's bench_start technique).
   ITERS <= 0 selects the default (100000000).  */
void mb_warmup (long iters);

/* Deterministic pattern fill (xorshift).  */
void mb_fill (unsigned char *p, size_t n, uint64_t seed);

/* Current implementation name, for the crash reporter.  */
extern const char *mb_current_impl;

/* xorshift64 step; *STATE must be non-zero.  Shared by the randomized
   batch parameters and by the size distributions.  */
static inline uint64_t
mb_rng_next (uint64_t *state)
{
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

/* Uniform value in [0, n); n == 0 yields 0.  */
static inline uint64_t
mb_rng_below (uint64_t *state, uint64_t n)
{
  return n > 0 ? mb_rng_next (state) % n : 0;
}

/* Install a SIGSEGV/SIGBUS handler that reports mb_current_impl and the
   faulting address, then exits with a distinct code (4).  Returns the
   previous disposition.  Install once at startup of the driver.  */
void mb_install_crash_reporter (void);

#endif /* MB_BENCH_COMMON_H */

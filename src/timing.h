/* High-precision, low-overhead timing for the memlib benchmark suite.

   The methodology mirrors glibc benchtests (hp-timing):

   - On AArch64 we read the system counter CNTVCT_EL0 (after an ISB) and
     convert counter deltas to nanoseconds using CNTFRQ_EL0.  This is the
     same technique as glibc's sysdeps/aarch64/hp-timing.h and is immune
     to CPU frequency scaling, because the virtual counter ticks at a
     fixed frequency.

   - On any other architecture we fall back to clock_gettime
     (CLOCK_MONOTONIC), which is a vDSO call on Linux and has
     nanosecond granularity.

   All measurements are expressed in nanoseconds per operation.
 */

#ifndef MB_TIMING_H
#define MB_TIMING_H

#include <stdint.h>

#if defined(__aarch64__)

static inline uint64_t
mb_counter_read (void)
{
  uint64_t t;
  __asm__ __volatile__ ("isb; mrs %0, cntvct_el0" : "=r" (t));
  return t;
}

static inline uint64_t
mb_counter_freq (void)
{
  uint64_t f;
  __asm__ __volatile__ ("mrs %0, cntfrq_el0" : "=r" (f));
  return f > 0 ? f : 1;
}

/* Convert a counter delta into nanoseconds.  cntfrq_el0 is read once per
   measurement (per test case), which is cheap enough.  */
static inline uint64_t
mb_counter_diff_ns (uint64_t start, uint64_t end, uint64_t freq)
{
  uint64_t d = end - start;
  return (uint64_t) (((__uint128_t) d * UINT64_C (1000000000)) / freq);
}

#else /* !__aarch64__ */

#include <time.h>

static inline uint64_t
mb_counter_read (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * UINT64_C (1000000000) + ts.tv_nsec;
}

static inline uint64_t
mb_counter_freq (void)
{
  /* Dummy: clock_gettime already returns nanoseconds.  */
  return UINT64_C (1000000000);
}

static inline uint64_t
mb_counter_diff_ns (uint64_t start, uint64_t end, uint64_t freq)
{
  (void) freq;
  return end - start;
}

#endif /* __aarch64__ */

/* Timing type tag, kept identical to glibc for tool compatibility.  glibc
   aarch64 hp-timing also reports nanoseconds.  */
#define MB_TIMING_TYPE "hp_timing"

#endif /* MB_TIMING_H */

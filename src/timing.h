/* High-precision, low-overhead timing for the memlib benchmark suite.

   The methodology mirrors glibc benchtests (hp-timing):

   - On AArch64 we read the system counter CNTVCT_EL0 (after an ISB) and
     convert counter deltas to nanoseconds using CNTFRQ_EL0.  This is the
     same technique as glibc's sysdeps/aarch64/hp-timing.h and is immune
     to CPU frequency scaling, because the virtual counter ticks at a
     fixed frequency.

   - On x86-64 (Linux) we read the invariant TSC (LFENCE;RDTSC), like
     glibc's sysdeps/x86/hp-timing.h.  The TSC frequency is calibrated
     once at startup against clock_gettime(CLOCK_MONOTONIC).  If the CPU
     does not advertise an invariant TSC (CPUID leaf 0x80000007, bit 8)
     or the calibration looks implausible, we transparently fall back to
     clock_gettime.

   - On any other architecture clock_gettime (CLOCK_MONOTONIC) is used;
     on Linux this is a vDSO call with nanosecond granularity.

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

#elif defined(__x86_64__)

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline uint64_t
mb_clock_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * UINT64_C (1000000000) + ts.tv_nsec;
}

/* LFENCE;RDTSC: serialize prior instructions, then read the counter.  */
static inline uint64_t
mb_rdtsc (void)
{
  uint32_t lo, hi;
  __asm__ __volatile__ ("lfence; rdtsc"
			: "=a" (lo), "=d" (hi)
			:
			: "memory");
  return ((uint64_t) hi << 32) | lo;
}

/* Invariant TSC: CPUID leaf 0x80000007, EDX bit 8.  */
static inline int
mb_tsc_invariant (void)
{
  uint32_t eax, ebx, ecx, edx;
  __asm__ __volatile__ ("cpuid"
			: "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
			: "a" (0x80000000U)
			: "memory");
  if (eax < 0x80000007U)
    return 0;
  __asm__ __volatile__ ("cpuid"
			: "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
			: "a" (0x80000007U)
			: "memory");
  return (edx >> 8) & 1;
}

/* Busy-wait calibration of the TSC frequency against CLOCK_MONOTONIC.
   Several short samples are taken and the median used, so a single
   scheduling hiccup does not skew the result.  Returns 0 on failure.  */
static uint64_t
mb_calibrate_tsc (void)
{
  const int n = 5;
  uint64_t samples[5];
  int nsamples = 0;

  for (int i = 0; i < n; i++)
    {
      volatile uint64_t sink = 0;
      uint64_t t0 = mb_clock_ns ();
      uint64_t c0 = mb_rdtsc ();
      uint64_t deadline = t0 + UINT64_C (4000000);	/* 4 ms */
      do
	{
	  sink += sink * 31 + 7;
	}
      while (mb_clock_ns () < deadline);

      uint64_t t1 = mb_clock_ns ();
      uint64_t c1 = mb_rdtsc ();

      if (t1 <= t0)
	continue;
      uint64_t hz = (uint64_t) (((__uint128_t) (c1 - c0)
				 * UINT64_C (1000000000)) / (t1 - t0));
      /* Plausibility window: 100 MHz .. 20 GHz.  */
      if (hz < UINT64_C (100000000) || hz > UINT64_C (20000000000))
	continue;
      samples[nsamples++] = hz;
    }

  if (nsamples == 0)
    return 0;
  /* Median of the valid samples.  */
  for (int i = 0; i < nsamples; i++)
    for (int j = i + 1; j < nsamples; j++)
      if (samples[j] < samples[i])
	{
	  uint64_t t = samples[i];
	  samples[i] = samples[j];
	  samples[j] = t;
	}
  return samples[nsamples / 2];
}

static int mb_tsc_ok = -1;	/* -1 = not yet decided */
static uint64_t mb_tsc_hz;

static inline void
mb_tsc_init (void)
{
  if (mb_tsc_ok >= 0)
    return;
  if (!mb_tsc_invariant ())
    {
      mb_tsc_ok = 0;
      return;
    }
  uint64_t hz = mb_calibrate_tsc ();
  if (hz == 0)
    {
      mb_tsc_ok = 0;
      return;
    }
  mb_tsc_ok = 1;
  mb_tsc_hz = hz;
  if (getenv ("MB_TIMING_DEBUG") != NULL)
    fprintf (stderr, "[memlib] x86-64 TSC timing, %llu Hz\n",
	     (unsigned long long) hz);
}

static inline uint64_t
mb_counter_read (void)
{
  if (mb_tsc_ok < 0)
    mb_tsc_init ();
  return mb_tsc_ok ? mb_rdtsc () : mb_clock_ns ();
}

static inline uint64_t
mb_counter_freq (void)
{
  if (mb_tsc_ok < 0)
    mb_tsc_init ();
  return mb_tsc_ok ? mb_tsc_hz : UINT64_C (1000000000);
}

static inline uint64_t
mb_counter_diff_ns (uint64_t start, uint64_t end, uint64_t freq)
{
  (void) freq;
  if (mb_tsc_ok < 0)
    mb_tsc_init ();
  if (!mb_tsc_ok)
    return end - start;			/* already nanoseconds */
  uint64_t d = end - start;
  return (uint64_t) (((__uint128_t) d * UINT64_C (1000000000)) / mb_tsc_hz);
}

#else /* !__aarch64__ && !__x86_64__ */

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

#endif /* arch selection */

/* Timing type tag, kept identical to glibc for tool compatibility.  glibc
   aarch64 hp-timing also reports nanoseconds.  */
#define MB_TIMING_TYPE "hp_timing"

#endif /* MB_TIMING_H */

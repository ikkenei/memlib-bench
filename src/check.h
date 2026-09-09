/* Correctness-check engines for the memlib benchmark suite.

   The --check mode of each driver verifies that every registered
   implementation (libc, generic and each user-supplied shared object)
   behaves exactly like an obviously-correct byte-wise oracle over an
   adversarial set of cases:

     - lengths covering every interesting boundary (0..N, powers of two,
       power-of-two +/-1, page boundaries, ...);
     - many source/destination alignments (0..63 byte offsets);
     - for memmove: forward and backward overlap at several distances,
       contiguous and disjoint copies;
     - placements near the end of a guard-page-protected mapping, so any
       read or write past the end of the region faults immediately;
     - for memcmp: equal buffers and buffers differing at the first,
       middle or last byte, checking the sign of the result.

   The implementation runs on one arena and the oracle on an identical
   twin arena; the whole arenas are then compared.  That catches wrong
   results as well as writes outside the destination range.

   Every failure is reported with a description of the offending case.
 */

#ifndef MB_CHECK_H
#define MB_CHECK_H

#include "bench_common.h"
#include "bench_drv.h"

/* Impl wrappers hide the real prototypes behind a uniform interface.  */

/* Write-type functions: memcpy / memmove / memset.  SRC is meaningful
   for copy functions, C for memset.  */
typedef void (*mb_wimpl_t) (const mb_impl_t *impl, void *dst,
			    const void *src, int c, size_t len);
typedef void (*mb_woracle_t) (void *dst, const void *src, int c,
			      size_t len);

/* Memcmp.  */
typedef int (*mb_cimpl_t) (const mb_impl_t *impl, const void *a,
			   const void *b, size_t len);
typedef int (*mb_coracle_t) (const void *a, const void *b, size_t len);

/* Run all registered implementations against the case set.  O supplies
   the pattern seed and a maximum length cap.  WHAT is the function name
   used in messages.  Returns the number of failed cases (0 == success).  */
int mb_check_write_run (const char *what, int is_memmove,
			const mb_opts_t *o,
			mb_wimpl_t impl_wrapper, mb_woracle_t oracle);
int mb_check_memcmp_run (const char *what, const mb_opts_t *o,
			 mb_cimpl_t impl_wrapper, mb_coracle_t oracle);

#endif /* MB_CHECK_H */

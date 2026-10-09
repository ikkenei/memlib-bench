/* Size distributions from real workloads (CSV), following the approach of
   llvm-libc's memory benchmarks.

   A distribution file is a plain-text CSV in one of two shapes:

     - one probability (or weight) per line, size = line index:
         0.0059,0.0660,0.0311,...        (one long line, llvm-libc style)
     - one (size, weight) pair per line:
         64,1200
         128,340
         256,17

   `#` starts a comment, blank lines are ignored, and the weights are
   normalized by their sum (so counts and probabilities both work).

   The distributions shipped with the benchmarks live under
   matrices/distributions (one CSV per distribution) and are compiled
   into the drivers (see the Makefile), so they can be selected by name.
   A path can also be given, in which case the file is read at run time.
 */

#ifndef MB_DISTRIBUTIONS_H
#define MB_DISTRIBUTIONS_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
  size_t *sizes;		/* sizes with a non-zero weight		*/
  double *cum;			/* cumulative weight, cum[n-1] == 1.0	*/
  size_t n;
  char name[64];		/* for the JSON output			*/
} mb_dist_t;

void mb_dist_init (mb_dist_t *d);
void mb_dist_free (mb_dist_t *d);

/* Load a distribution by embedded name (e.g. "memcpy_google_a" or the
   llvm-libc spelling "memcpy Google A") or from a file (the argument
   contains a '/' or ends with ".csv").  Returns 0 or -1 (message via
   mb_dist_err).  */
int mb_dist_load (const char *name_or_path, mb_dist_t *d);

/* Largest size of the distribution.  */
size_t mb_dist_max (const mb_dist_t *d);

/* Weighted sample of one size; STATE is the caller's rng state.  */
size_t mb_dist_sample (const mb_dist_t *d, uint64_t *state);

/* Names of the embedded distributions (NULL terminated).  */
const char *const *mb_dist_names (void);

const char *mb_dist_err (void);

#endif /* MB_DISTRIBUTIONS_H */

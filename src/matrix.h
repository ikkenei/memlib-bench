/* Configurable benchmark matrix: sizes, offsets and explicit cases.

   A matrix file is a plain-text profile with one section per function
   (`[memcpy]`, `[memmove]`, `[memset]`, `[memcmp]`).  Two notations can
   be mixed inside a section:

   Flat lists (the original notation, unchanged):

       sizes = 1..16384 24576..98304:8192 131072
       src   = 0 1 2 3
       dst   = 0 1 3
       both  = 1

   Cases (blocks), needed to express the glibc benchtests matrices with
   their correlated offsets (`i`, `i+32`, `P/2+i`, ...):

       case                        # starts a block
       loop i = 0..17:1            # loop variable (A..B powers by
                                   # default, A..B:STEP linear, !pow2
                                   # drops powers of two)
       size = 1 << i               # length expression
       pairs = (0,0) (i,0) (P/2+i,i)
       both = 1

   Value syntax (spaces or commas separate values):
     N          single number (hex allowed)
     A..B       powers of two A, A*2, ... <= B
     A..B:STEP  linear range with inclusive step
     !pow2      (loop lists only) skip values where (v & (v-1)) == 0

   Expressions (block `size`, `pairs`, `align`, `fill`, `diff`) support
   integers, the loop variables, `P` (the OS page size), parentheses and
   the operators + - * / <<.  In blocks, `align`/`fill`/`diff` lists are
   comma-separated so that expressions may contain spaces.

   Keys per function:
     memcpy/memmove : sizes, src, dst, both        / size, pairs, both
     memset         : sizes, align, fill           / size, align, fill
     memcmp         : sizes, src, dst, diff        / size, pairs, diff

   The defaults shipped with the benchmarks live in matrices/glibc_small.txt
   and matrices/glibc_large.txt; they are compiled into the drivers (see
   the Makefile) and used when --matrix is not given.
 */

#ifndef MB_MATRIX_H
#define MB_MATRIX_H

#include <stddef.h>
#include <stdint.h>

#include "distributions.h"

typedef struct
{
  size_t len;			/* length in bytes			*/
  long a1, a2;			/* offsets; for memset a1 = alignment	*/
  int c;			/* memset fill byte			*/
  int result;			/* memcmp expected result		*/
  int pos;			/* memcmp mismatch byte (1-based, 0=last) */
  int both;			/* copy functions: both directions	*/
} mb_case_t;

typedef struct
{
  mb_case_t *cases;
  size_t ncases, cap;
  /* Size distribution, when the profile used 'dist = NAME' (or when the
     driver was given --dist).  The randomized ("mixed") mode samples
     sizes from it instead of using a uniform size pool.  */
  mb_dist_t dist;
} mb_matrix_t;

void mb_matrix_init (mb_matrix_t *m);
void mb_matrix_free (mb_matrix_t *m);

/* Parse a file / a text buffer, keeping only SECTION (the function name).
   Returns 0 on success, -1 on failure (message via mb_matrix_err).  */
int mb_matrix_load (const char *path, const char *section, uint64_t seed,
		    mb_matrix_t *m);
int mb_matrix_parse (const char *text, const char *section, uint64_t seed,
		     mb_matrix_t *m);

/* The matrices compiled into the drivers (matrices/glibc_*.txt).  WITH_LARGE
   also loads the large-size matrix.  */
int mb_matrix_load_default (const char *section, int with_large,
			    uint64_t seed, mb_matrix_t *m);

const char *mb_matrix_err (void);

/* Largest case length (0 when empty).  */
long long mb_matrix_max_len (const mb_matrix_t *m);

#endif /* MB_MATRIX_H */

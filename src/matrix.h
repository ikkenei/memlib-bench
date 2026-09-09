/* Configurable benchmark matrix (sizes and alignment offsets).

   A matrix file is a plain-text, line-oriented profile with one section
   per function:

       [memcpy]
       # comment
       sizes = 1..16384 24576..98304:8192 131072
       src   = 0 1 2 3 7 15 31 63
       dst   = 0 1 3 7 15 31
       both  = 1

   Supported value syntax:
     N              a single (signed) number; sizes/offsets must be >= 0
     A..B           powers of two: A, A*2, ... while <= B
     A..B:STEP      linear range with step STEP
     numbers may be separated by spaces or commas; 0x.. hex accepted.

   Keys by function:
     memcpy/memmove: sizes, src (=align1), dst (=align2), both
     memset:         sizes, align, fill (=c values)
     memcmp:         sizes, src (=align1), dst (=align2), diff (0/1/-1)

   If a file has no section for the requested function the load fails
   with a clear error (so a typo cannot silently change a benchmark).
 */

#ifndef MB_MATRIX_H
#define MB_MATRIX_H

#include <stddef.h>

typedef struct
{
  long long *sizes; size_t nsizes;
  long long *src;   size_t nsrc;
  long long *dst;   size_t ndst;
  long long *align; size_t nalign;
  long long *fill;  size_t nfill;	/* memset fill values (int)	*/
  long long *diff;  size_t ndiff;	/* memcmp expected result	*/
  int both;				/* both copy directions		*/
} mb_matrix_t;

void mb_matrix_init (mb_matrix_t *m);
void mb_matrix_free (mb_matrix_t *m);

/* Parse PATH, keeping only the SECTION block.  Returns 0 on success and
   -1 on failure (message via mb_matrix_err).  On success *M is filled;
   on failure *M is left untouched except init/free by the caller.  */
int mb_matrix_load (const char *path, const char *section, mb_matrix_t *m);

const char *mb_matrix_err (void);

/* Largest requested size (0 if the list is empty).  */
long long mb_matrix_max_size (const mb_matrix_t *m);

#endif

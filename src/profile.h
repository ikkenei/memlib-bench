/* memtrace profiles as benchmark loads.

   A "memtrace profile" (produced by tools/memtrace.py, or a raw trace
   written by it) describes the memory-function calls observed in a real
   program:

       # function,size,count,src_align,dst_align,class,attr
       memcpy,64,1200,0,0,0,0
       memmove,128,44,0,16,2,16
       memset,32,500,0,-,5,0
       memcmp,256,80,0,0,7,0

   `src_align`/`dst_align` are the low bits of the pointers as seen by the
   tracer; the benchmark's buffers are page aligned, so using them as
   offsets reproduces the alignment.  `class`/`attr` describe the call:

     0        plain copy (memcpy, or memmove with src == dst)
     1, 3     memmove, forward / backward, non overlapping
     2, 4     memmove, forward / backward, overlapping (attr = distance)
     5, 6     memset with a zero / non-zero fill byte (attr = the byte)
     7, 8     memcmp with equal / differing buffers
              (attr = 0 -> mismatch at the last byte, N -> byte N-1)

   The loader turns the rows of one function into weighted cases, grouped
   by (class, attr) so that the summary can report each call shape
   separately.  See mb_profile_sample_group().
 */

#ifndef MB_PROFILE_H
#define MB_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "matrix.h"

typedef struct
{
  int cls;
  int attr;
  mb_case_t *cases;		/* one entry per profile row of the group */
  double *cum;			/* cumulative weights			  */
  size_t n;
  size_t min_len, max_len;
  double weight;		/* total count of the group		  */
  size_t mean_len;		/* weighted mean size			  */
} mb_profile_group_t;

typedef struct
{
  mb_profile_group_t *groups;
  size_t ngroups;
  size_t min_len, max_len;
  char name[64];		/* label for the JSON			  */
} mb_profile_t;

void mb_profile_init (mb_profile_t *p);
void mb_profile_free (mb_profile_t *p);

/* Load the rows of FUNC from PATH (0 or -1; message via mb_profile_err).  */
int mb_profile_load (const char *path, const char *func, mb_profile_t *p);

/* Weighted sample of one case from a group.  */
const mb_case_t *mb_profile_sample (const mb_profile_group_t *g,
				    uint64_t *state);

/* Human readable name of a class (for the JSON/summary).  */
const char *mb_profile_class_name (int cls);

const char *mb_profile_err (void);

#endif /* MB_PROFILE_H */

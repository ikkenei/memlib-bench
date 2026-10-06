/* Prototypes for the generic references and byte-wise oracles.  */

#ifndef MB_GENERIC_REF_H
#define MB_GENERIC_REF_H

#include <stddef.h>

void *mb_ref_memcpy (void *dst, const void *src, size_t n);
void *mb_ref_memmove (void *dst, const void *src, size_t n);
void *mb_ref_memset (void *dst, int c, size_t n);
int   mb_ref_memcmp (const void *a, const void *b, size_t n);

/* Oracles use the check engine's signatures (mb_woracle_t/mb_coracle_t).  */
void mb_oracle_memcpy (void *dst, const void *src, int c, size_t n);
void mb_oracle_memmove (void *dst, const void *src, int c, size_t n);
void mb_oracle_memset (void *dst, const void *src, int c, size_t n);
int  mb_oracle_memcmp (const void *a, const void *b, size_t n);

#endif

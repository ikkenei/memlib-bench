/* Reference implementations and byte-wise oracles for the memlib suite.

   Two sets of functions:

   - mb_ref_*   : "generic" timing baselines.  Portable scalar C with a
                  word-at-a-time fast path for well aligned cases.  They
                  are deliberately simple, in the spirit of glibc's
                  generic string implementations used as baselines in
                  the original benchtests.

   - mb_oracle_*: obviously-correct byte loops used as the ground truth
                  by the --check correctness mode.

   Both are pure C, with no calls into libc string functions, so they can
   be used as an independent reference on any platform.
 */

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Byte-wise oracles (ground truth)                                    */
/* ------------------------------------------------------------------ */

void
mb_oracle_memcpy (void *dst, const void *src, int c, size_t n)
{
  (void) c;
  unsigned char *d = dst;
  const unsigned char *s = src;
  for (size_t i = 0; i < n; i++)
    d[i] = s[i];
}

void
mb_oracle_memmove (void *dst, const void *src, int c, size_t n)
{
  (void) c;
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d == s || n == 0)
    return;
  if (d < s)
    {
      for (size_t i = 0; i < n; i++)
	d[i] = s[i];
    }
  else
    {
      size_t i = n;
      while (i > 0)
	{
	  i--;
	  d[i] = s[i];
	}
    }
}

void
mb_oracle_memset (void *dst, const void *src, int c, size_t n)
{
  (void) src;
  unsigned char v = (unsigned char) c;
  unsigned char *d = dst;
  for (size_t i = 0; i < n; i++)
    d[i] = v;
}

int
mb_oracle_memcmp (const void *a, const void *b, size_t n)
{
  const unsigned char *x = a;
  const unsigned char *y = b;
  for (size_t i = 0; i < n; i++)
    {
      if (x[i] != y[i])
	return x[i] < y[i] ? -1 : 1;
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Generic (word-at-a-time) references, used as timing baselines.      */
/* ------------------------------------------------------------------ */

void *
mb_ref_memcpy (void *dst, const void *src, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  /* Head: reach an 8-byte aligned destination.  */
  while (n != 0 && ((uintptr_t) d & 7) != 0)
    {
      *d++ = *s++;
      n--;
    }

  /* Middle: word copies (both pointers aligned).  */
  if (n >= 16 && ((uintptr_t) s & 7) == 0)
    {
      size_t words = n >> 3;
      const uint64_t *ws = (const uint64_t *) (const void *) s;
      uint64_t *wd = (uint64_t *) (void *) d;
      for (size_t i = 0; i < words; i++)
	wd[i] = ws[i];
      d += words * 8;
      s += words * 8;
      n -= words * 8;
    }

  /* Tail.  */
  while (n-- != 0)
    *d++ = *s++;
  return dst;
}

void *
mb_ref_memmove (void *dst, const void *src, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  if (d == s || n == 0)
    return dst;

  if (d < s)
    {
      /* Forward copy (may overlap toward the end).  */
      while (n != 0 && ((uintptr_t) d & 7) != 0)
	{
	  *d++ = *s++;
	  n--;
	}
      if (n >= 16 && ((uintptr_t) s & 7) == 0)
	{
	  size_t words = n >> 3;
	  const uint64_t *ws = (const uint64_t *) (const void *) s;
	  uint64_t *wd = (uint64_t *) (void *) d;
	  for (size_t i = 0; i < words; i++)
	    wd[i] = ws[i];
	  d += words * 8;
	  s += words * 8;
	  n -= words * 8;
	}
      while (n-- != 0)
	*d++ = *s++;
    }
  else
    {
      /* Backward copy from the end (overlap toward the beginning).  */
      unsigned char *de = d + n;
      const unsigned char *se = s + n;
      if ((((uintptr_t) d | (uintptr_t) s) & 7) == 0)
	{
	  while (n >= 8)
	    {
	      de -= 8;
	      se -= 8;
	      *(uint64_t *) (void *) de = *(const uint64_t *) (const void *) se;
	      n -= 8;
	    }
	}
      while (n-- != 0)
	{
	  de--;
	  se--;
	  *de = *se;
	}
    }
  return dst;
}

void *
mb_ref_memset (void *dst, int c, size_t n)
{
  unsigned char v = (unsigned char) c;
  unsigned char *d = dst;

  uint64_t w = 0;
  for (int i = 0; i < 8; i++)
    w = (w << 8) | v;

  while (n != 0 && ((uintptr_t) d & 7) != 0)
    {
      *d++ = v;
      n--;
    }
  while (n >= 8)
    {
      *(uint64_t *) (void *) d = w;
      d += 8;
      n -= 8;
    }
  while (n-- != 0)
    *d++ = v;
  return dst;
}

int
mb_ref_memcmp (const void *a, const void *b, size_t n)
{
  const unsigned char *x = a;
  const unsigned char *y = b;

  /* Byte head until both pointers are aligned.  */
  while (n != 0 && (((uintptr_t) x | (uintptr_t) y) & 7) != 0)
    {
      if (*x != *y)
	return *x < *y ? -1 : 1;
      x++;
      y++;
      n--;
    }

  /* Word middle: compare equality; on a difference fall through to the
     byte scan.  This never reads past a mismatching byte.  */
  while (n >= 8)
    {
      uint64_t wx = *(const uint64_t *) (const void *) x;
      uint64_t wy = *(const uint64_t *) (const void *) y;
      if (wx != wy)
	break;
      x += 8;
      y += 8;
      n -= 8;
    }

  while (n-- != 0)
    {
      if (*x != *y)
	return *x < *y ? -1 : 1;
      x++;
      y++;
    }
  return 0;
}

/* Example user implementation, plain portable C.

   Drop your own implementation(s) into impls/ as .c or .S files that
   export the standard symbols (memcpy / memmove / memset / memcmp).
   `make` compiles each file into build/impls/<name>.so; benchmark it
   with

     ./mb run memcpy example_c          # + correctness:
     ./mb check memcpy example_c

   This one is intentionally simple and correct; it exists to show the
   layout and to sanity-check the harness.  It provides all four
   functions.
 */

#include <stddef.h>
#include <stdint.h>

void *
memcpy (void *dst, const void *src, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  /* Head: get the destination 8-byte aligned.  */
  while (n != 0 && ((uintptr_t) d & 7) != 0)
    {
      *d++ = *s++;
      n--;
    }
  /* Body: 8-byte chunks (needs both pointers aligned).  */
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
  return dst;
}

void *
memmove (void *dst, const void *src, size_t n)
{
  unsigned char *d = dst;
  const unsigned char *s = src;

  if (d == s || n == 0)
    return dst;
  if (d < s)
    {
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
      /* Overlap toward the beginning: copy backwards.  */
      unsigned char *de = d + n;
      const unsigned char *se = s + n;
      while (n-- != 0)
	*--de = *--se;
    }
  return dst;
}

void *
memset (void *dst, int c, size_t n)
{
  unsigned char v = (unsigned char) c;
  unsigned char *d = dst;

  while (n != 0 && ((uintptr_t) d & 7) != 0)
    {
      *d++ = v;
      n--;
    }
  uint64_t w = (uint64_t) v * UINT64_C (0x0101010101010101);
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
memcmp (const void *a, const void *b, size_t n)
{
  const unsigned char *x = a;
  const unsigned char *y = b;

  while (n != 0 && (((uintptr_t) x | (uintptr_t) y) & 7) != 0)
    {
      if (*x != *y)
	return *x < *y ? -1 : 1;
      x++;
      y++;
      n--;
    }
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

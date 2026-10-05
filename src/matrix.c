/* Matrix file parser (implementation).  See matrix.h. */

#include "matrix.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS (1u << 20)
#define LINE_MAX 4096

static char merr[512];

const char *
mb_matrix_err (void)
{
  return merr;
}

void
mb_matrix_init (mb_matrix_t *m)
{
  memset (m, 0, sizeof *m);
}

void
mb_matrix_free (mb_matrix_t *m)
{
  free (m->sizes);
  free (m->src);
  free (m->dst);
  free (m->align);
  free (m->fill);
  free (m->diff);
  memset (m, 0, sizeof *m);
}

long long
mb_matrix_max_size (const mb_matrix_t *m)
{
  long long mx = 0;
  for (size_t i = 0; i < m->nsizes; i++)
    if (m->sizes[i] > mx)
      mx = m->sizes[i];
  return mx;
}

/* ------------------------------------------------------------------ */
/* Growable long long list                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
  long long *v;
  size_t n, cap;
  int overflow;
} list_t;

static int
list_push (list_t *l, long long x)
{
  if (l->overflow)
    return -1;
  if (l->n >= MAX_TOKENS)
    {
      l->overflow = 1;
      return -1;
    }
  if (l->n == l->cap)
    {
      size_t nc = l->cap ? l->cap * 2 : 16;
      long long *nv = realloc (l->v, nc * sizeof *nv);
      if (nv == NULL)
	{
	  l->overflow = 1;
	  return -1;
	}
      l->v = nv;
      l->cap = nc;
    }
  l->v[l->n++] = x;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Token parsing                                                      */
/* ------------------------------------------------------------------ */

/* Which list does a key update?  */
enum key_kind
{
  K_SIZES, K_SRC, K_DST, K_ALIGN, K_FILL, K_DIFF, K_BOTH, K_UNKNOWN
};

static enum key_kind
key_kind_of (const char *key)
{
  if (!strcmp (key, "sizes") || !strcmp (key, "size")
      || !strcmp (key, "lengths") || !strcmp (key, "length"))
    return K_SIZES;
  if (!strcmp (key, "src") || !strcmp (key, "align1")
      || !strcmp (key, "s"))
    return K_SRC;
  if (!strcmp (key, "dst") || !strcmp (key, "align2")
      || !strcmp (key, "d"))
    return K_DST;
  if (!strcmp (key, "align") || !strcmp (key, "alignment")
      || !strcmp (key, "a"))
    return K_ALIGN;
  if (!strcmp (key, "fill") || !strcmp (key, "c")
      || !strcmp (key, "char"))
    return K_FILL;
  if (!strcmp (key, "diff") || !strcmp (key, "result"))
    return K_DIFF;
  if (!strcmp (key, "both"))
    return K_BOTH;
  return K_UNKNOWN;
}

/* Add one number token to the proper list; unsigned keys reject
   negative values.  */
static int
add_token (enum key_kind k, list_t *l, const char *tok, int line_no,
	   long long *both)
{
  char *end = NULL;
  long long x = strtoll (tok, &end, 0);
  if (end == tok || *end != '\0')
    {
      snprintf (merr, sizeof merr, "matrix: line %d: bad number '%s'",
		line_no, tok);
      return -1;
    }
  if (k == K_BOTH)
    {
      if (x != 0 && x != 1)
	{
	  snprintf (merr, sizeof merr,
		    "matrix: line %d: 'both' must be 0 or 1", line_no);
	  return -1;
	}
      *both = (int) x;
      return 0;
    }
  if ((k == K_SIZES || k == K_SRC || k == K_DST || k == K_ALIGN)
      && x < 0)
    {
      snprintf (merr, sizeof merr,
		"matrix: line %d: sizes/offsets must be >= 0 (got %s)",
		line_no, tok);
      return -1;
    }
  if (k == K_DIFF && (x < -1 || x > 1))
    {
      snprintf (merr, sizeof merr,
		"matrix: line %d: 'diff' must be in {-1,0,1} (got %s)",
		line_no, tok);
      return -1;
    }
  if (list_push (l, x) != 0)
    {
      snprintf (merr, sizeof merr,
		"matrix: too many values (limit %u per key)", MAX_TOKENS);
      return -1;
    }
  return 0;
}

/* Expand one token which may be a range "A..B" (powers of two) or
   "A..B:STEP" (linear).  Plain numbers are passed through.  */
static int
add_range_or_value (enum key_kind k, list_t *l, const char *tok,
		    int line_no, long long *both)
{
  const char *dd = strstr (tok, "..");
  if (dd == NULL)
    return add_token (k, l, tok, line_no, both);
  if (k == K_FILL || k == K_DIFF)
    {
      snprintf (merr, sizeof merr,
		"matrix: line %d: ranges are not allowed for this key "
		"('%s')", line_no, tok);
      return -1;
    }

  char *end = NULL;
  long long start = strtoll (tok, &end, 0);
  if (end != dd || start < 0)
    {
      snprintf (merr, sizeof merr, "matrix: line %d: bad range '%s'",
		line_no, tok);
      return -1;
    }

  const char *after = dd + 2;
  long long step = 0;		/* 0 => powers of two		*/
  long long stop;
  const char *colon = strchr (after, ':');
  if (colon != NULL)
    {
      /* A..B:STEP - stop is before the colon, step after it.  */
      char sbuf[32];
      size_t n = (size_t) (colon - after);
      if (n == 0 || n >= sizeof sbuf)
	{
	  snprintf (merr, sizeof merr,
		    "matrix: line %d: bad range '%s'", line_no, tok);
	  return -1;
	}
      memcpy (sbuf, after, n);
      sbuf[n] = '\0';
      char *pe = NULL;
      stop = strtoll (sbuf, &pe, 0);
      if (pe == sbuf || *pe != '\0')
	{
	  snprintf (merr, sizeof merr,
		    "matrix: line %d: bad range end in '%s'", line_no, tok);
	  return -1;
	}
      char *pe2 = NULL;
      long long st = strtoll (colon + 1, &pe2, 0);
      if (pe2 == colon + 1 || *pe2 != '\0' || st <= 0)
	{
	  snprintf (merr, sizeof merr,
		    "matrix: line %d: bad step in range '%s' "
		    "(write A..B:STEP)", line_no, tok);
	  return -1;
	}
      step = st;
    }
  else
    {
      char *pe = NULL;
      stop = strtoll (after, &pe, 0);
      if (pe == after || *pe != '\0')
	{
	  snprintf (merr, sizeof merr,
		    "matrix: line %d: bad range end in '%s'", line_no, tok);
	  return -1;
	}
    }

  if (stop < start)
    {
      snprintf (merr, sizeof merr,
		"matrix: line %d: range end < start in '%s'", line_no, tok);
      return -1;
    }

  if (step != 0)
    {
      for (long long v = start;; v += step)
	{
	  if (list_push (l, v) != 0)
	    {
	      snprintf (merr, sizeof merr,
			"matrix: line %d: too many values in range '%s'",
			line_no, tok);
	      return -1;
	    }
	  if (v > stop - step)
	    break;
	}
    }
  else
    {
      /* Powers of two.  */
      for (long long v = start;;)
	{
	  if (list_push (l, v) != 0)
	    {
	      snprintf (merr, sizeof merr,
			"matrix: line %d: too many values in range '%s'",
			line_no, tok);
	      return -1;
	    }
	  if (v > stop / 2)
	    break;
	  v *= 2;
	}
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* File parsing                                                       */
/* ------------------------------------------------------------------ */

int
mb_matrix_load (const char *path, const char *section, mb_matrix_t *m)
{
  FILE *fp = fopen (path, "r");
  if (fp == NULL)
    {
      snprintf (merr, sizeof merr, "matrix: cannot open '%s'", path);
      return -1;
    }

  list_t lists[6];
  memset (lists, 0, sizeof lists);
  long long both = 1;
  char line[LINE_MAX];
  int active = 0;
  int saw_section = 0;
  int line_no = 0;
  int rc = -1;
  enum key_kind curk = K_UNKNOWN;	/* key of the current value list */
  list_t *curl = NULL;		/* list the current key appends to   */

  while (fgets (line, sizeof line, fp) != NULL)
    {
      line_no++;

      /* Trim trailing newline / whitespace.  */
      size_t ll = strlen (line);
      while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r'
			|| line[ll - 1] == ' ' || line[ll - 1] == '\t'))
	line[--ll] = '\0';

      char *p = line;

      /* Trim leading whitespace.  */
      while (*p == ' ' || *p == '\t')
	p++;

      /* Comments and blank lines.  */
      if (*p == '\0' || *p == '#' || *p == ';')
	continue;

      if (*p == '[')
	{
	  char *end = strchr (p, ']');
	  if (end == NULL)
	    {
	      snprintf (merr, sizeof merr,
			"matrix: line %d: unterminated section header",
			line_no);
	      goto out;
	    }
	  *end = '\0';
	  char *name = p + 1;
	  while (*name == ' ' || *name == '\t')
	    name++;
	  active = strcmp (name, section) == 0;
	  if (active)
	    saw_section = 1;
	  curk = K_UNKNOWN;
	  curl = NULL;
	  continue;
	}

      if (!active)
	continue;

      char *eq = strchr (p, '=');
      enum key_kind k;
      list_t *dst_list = NULL;
      if (eq != NULL)
	{
	  /* key = value...  */
	  *eq = '\0';
	  char *key = p;
	  char *ke = key + strlen (key);
	  while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t'))
	    *--ke = '\0';

	  k = key_kind_of (key);
	  if (k == K_UNKNOWN)
	    {
	      snprintf (merr, sizeof merr,
			"matrix: line %d: unknown key '%s'", line_no, key);
	      goto out;
	    }
	  switch (k)
	    {
	    case K_SIZES: dst_list = &lists[0]; break;
	    case K_SRC:   dst_list = &lists[1]; break;
	    case K_DST:   dst_list = &lists[2]; break;
	    case K_ALIGN: dst_list = &lists[3]; break;
	    case K_FILL:  dst_list = &lists[4]; break;
	    case K_DIFF:  dst_list = &lists[5]; break;
	    default: break;
	    }
	  curk = k;
	  curl = dst_list;
	  p = eq + 1;
	}
      else
	{
	  /* Continuation line: append to the value list of the previous
	     key (allows long lists to be wrapped over several lines).  A
	     line that starts with a known key name usually means the '='
	     was forgotten, which is worth its own message.  */
	  char first[64];
	  size_t fn = 0;
	  while (p[fn] != '\0' && p[fn] != ' ' && p[fn] != '\t'
		 && p[fn] != ',' && fn < sizeof first - 1)
	    {
	      first[fn] = p[fn];
	      fn++;
	    }
	  first[fn] = '\0';
	  if (key_kind_of (first) != K_UNKNOWN)
	    {
	      snprintf (merr, sizeof merr,
			"matrix: line %d: missing '=' after '%s' "
			"(write '%s = ...')", line_no, first, first);
	      goto out;
	    }
	  k = curk;
	  dst_list = curl;
	  if (k == K_UNKNOWN || dst_list == NULL)
	    {
	      snprintf (merr, sizeof merr,
			"matrix: line %d: expected 'key = value'", line_no);
	      goto out;
	    }
	}

      /* Value part: strip comments, then tokenize on spaces/commas.  */
      char *hash = strchr (p, '#');
      if (hash != NULL)
	*hash = '\0';

      char *tok = strtok (p, " \t,");
      while (tok != NULL)
	{
	  if (*tok != '\0')
	    {
	      if (add_range_or_value (k, dst_list, tok, line_no, &both)
		  != 0)
		goto out;
	    }
	  tok = strtok (NULL, " \t,");
	}
    }

  if (!saw_section)
    {
      snprintf (merr, sizeof merr,
		"matrix: '%s' has no [%s] section", path, section);
      goto out;
    }
  if (lists[0].n == 0)
    {
      snprintf (merr, sizeof merr,
		"matrix: [%s] section must define 'sizes'", section);
      goto out;
    }

  m->sizes = lists[0].v; m->nsizes = lists[0].n;
  m->src   = lists[1].v; m->nsrc   = lists[1].n;
  m->dst   = lists[2].v; m->ndst   = lists[2].n;
  m->align = lists[3].v; m->nalign = lists[3].n;
  m->fill  = lists[4].v; m->nfill  = lists[4].n;
  m->diff  = lists[5].v; m->ndiff  = lists[5].n;
  m->both  = (int) both;
  rc = 0;

out:
  fclose (fp);
  if (rc != 0)
    {
      for (int i = 0; i < 6; i++)
	free (lists[i].v);
    }
  return rc;
}

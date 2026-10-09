/* Matrix parsing and expansion (implementation).  See matrix.h. */

#include "matrix.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VALUES (1u << 20)
#define MAX_CASES  (1u << 22)
#define MAX_LOOPS  4
#define LINE_MAX   4096

static char merr[512];

static void
set_err (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (merr, sizeof merr, fmt, ap);
  va_end (ap);
}

const char *
mb_matrix_err (void)
{
  return merr;
}

/* ------------------------------------------------------------------ */
/* Number lists                                                        */
/* ------------------------------------------------------------------ */

typedef struct
{
  long long *v;
  size_t n, cap;
} list_t;

static void
list_free (list_t *l)
{
  free (l->v);
  l->v = NULL;
  l->n = l->cap = 0;
}

static int
list_push (list_t *l, long long x)
{
  if (l->n >= MAX_VALUES)
    return -1;
  if (l->n == l->cap)
    {
      size_t nc = l->cap ? l->cap * 2 : 16;
      long long *nv = realloc (l->v, nc * sizeof *nv);
      if (nv == NULL)
	return -1;
      l->v = nv;
      l->cap = nc;
    }
  l->v[l->n++] = x;
  return 0;
}

static int is_pow2 (long long v)
{
  return v >= 0 && (v & (v - 1)) == 0;
}

/* Append one value token (N, A..B or A..B:STEP) to L.  "*skip_pow2" is set
   when the literal "!pow2" is seen.  */
static int
add_value_token (list_t *l, const char *tok, int line_no, int *skip_pow2)
{
  if (strcmp (tok, "!pow2") == 0)
    {
      if (skip_pow2 == NULL)
	{
	  set_err ("matrix: line %d: !pow2 is only allowed in loop lists",
		   line_no);
	  return -1;
	}
      *skip_pow2 = 1;
      return 0;
    }

  const char *dd = strstr (tok, "..");
  if (dd == NULL)
    {
      char *end = NULL;
      long long x = strtoll (tok, &end, 0);
      if (end == tok || *end != '\0')
	{
	  set_err ("matrix: line %d: bad number '%s'", line_no, tok);
	  return -1;
	}
      if (list_push (l, x) != 0)
	{
	  set_err ("matrix: too many values (limit %u)", MAX_VALUES);
	  return -1;
	}
      return 0;
    }

  char *end = NULL;
  long long start = strtoll (tok, &end, 0);
  if (end != dd)
    {
      set_err ("matrix: line %d: bad range '%s'", line_no, tok);
      return -1;
    }

  const char *after = dd + 2;
  long long stop;
  long long step = 0;			/* 0 => powers of two */
  const char *colon = strchr (after, ':');
  if (colon != NULL)
    {
      char sbuf[32];
      size_t n = (size_t) (colon - after);
      if (n == 0 || n >= sizeof sbuf)
	{
	  set_err ("matrix: line %d: bad range '%s'", line_no, tok);
	  return -1;
	}
      memcpy (sbuf, after, n);
      sbuf[n] = '\0';
      char *pe = NULL;
      stop = strtoll (sbuf, &pe, 0);
      if (pe == sbuf || *pe != '\0')
	{
	  set_err ("matrix: line %d: bad range end in '%s'", line_no, tok);
	  return -1;
	}
      char *pe2 = NULL;
      long long st = strtoll (colon + 1, &pe2, 0);
      if (pe2 == colon + 1 || *pe2 != '\0' || st <= 0)
	{
	  set_err ("matrix: line %d: bad step in '%s' (write A..B:STEP)",
		   line_no, tok);
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
	  set_err ("matrix: line %d: bad range end in '%s'", line_no, tok);
	  return -1;
	}
    }

  if (stop < start)
    {
      set_err ("matrix: line %d: range end < start in '%s'", line_no, tok);
      return -1;
    }

  if (step != 0)
    {
      for (long long v = start;; v += step)
	{
	  if (list_push (l, v) != 0)
	    {
	      set_err ("matrix: too many values in '%s'", tok);
	      return -1;
	    }
	  if (v > stop - step)
	    break;
	}
    }
  else
    {
      if (start == 0)
	{
	  if (list_push (l, 0) != 0)
	    return -1;
	  start = 1;
	}
      for (long long v = start;;)
	{
	  if (list_push (l, v) != 0)
	    {
	      set_err ("matrix: too many values in '%s'", tok);
	      return -1;
	    }
	  if (v > stop / 2)
	    break;
	  v *= 2;
	}
    }
  return 0;
}

/* Filter a loop list in place when !pow2 was requested.  */
static int
filter_pow2 (list_t *l)
{
  size_t w = 0;
  for (size_t i = 0; i < l->n; i++)
    if (!is_pow2 (l->v[i]))
      l->v[w++] = l->v[i];
  l->n = w;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

typedef struct
{
  char name[32];
  list_t vals;
} loopvar_t;

typedef struct
{
  const char *p;			/* cursor			*/
  const char *expr;			/* whole expression (errors)	*/
  const loopvar_t *loops;
  const long long *vals;		/* current loop values		*/
  int nloops;
  long long page;
} eval_t;

static void
eval_ws (eval_t *e)
{
  while (*e->p == ' ' || *e->p == '\t')
    e->p++;
}

static int eval_add (eval_t *e, long long *out);

static int
eval_primary (eval_t *e, long long *out)
{
  eval_ws (e);
  if (*e->p == '(')
    {
      e->p++;
      if (eval_add (e, out) != 0)
	return -1;
      eval_ws (e);
      if (*e->p != ')')
	{
	  set_err ("matrix: unbalanced parentheses in '%s'", e->expr);
	  return -1;
	}
      e->p++;
      return 0;
    }
  if (*e->p == '-')
    {
      e->p++;
      if (eval_primary (e, out) != 0)
	return -1;
      *out = -*out;
      return 0;
    }
  if (isdigit ((unsigned char) *e->p))
    {
      char *end = NULL;
      long long v = strtoll (e->p, &end, 0);
      if (end == e->p)
	{
	  set_err ("matrix: bad number in '%s'", e->expr);
	  return -1;
	}
      e->p = end;
      *out = v;
      return 0;
    }
  if (isalpha ((unsigned char) *e->p) || *e->p == '_')
    {
      char name[32];
      size_t n = 0;
      while ((isalnum ((unsigned char) *e->p) || *e->p == '_')
	     && n < sizeof name - 1)
	name[n++] = *e->p++;
      name[n] = '\0';
      if (strcmp (name, "P") == 0 || strcmp (name, "page") == 0)
	{
	  *out = e->page;
	  return 0;
	}
      for (int i = 0; i < e->nloops; i++)
	if (strcmp (e->loops[i].name, name) == 0)
	  {
	    *out = e->vals[i];
	    return 0;
	  }
      set_err ("matrix: unknown name '%s' in '%s'", name, e->expr);
      return -1;
    }
  set_err ("matrix: unexpected character in '%s'", e->expr);
  return -1;
}

static int
eval_mul (eval_t *e, long long *out)
{
  if (eval_primary (e, out) != 0)
    return -1;
  for (;;)
    {
      eval_ws (e);
      char op = *e->p;
      if (op == '*' || op == '/' || op == '<')
	{
	  if (op == '<')
	    {
	      if (e->p[1] != '<')
		break;
	      e->p++;
	    }
	  e->p++;
	  long long rhs;
	  if (eval_primary (e, &rhs) != 0)
	    return -1;
	  if (op == '*')
	    *out *= rhs;
	  else if (op == '/')
	    {
	      if (rhs == 0)
		{
		  set_err ("matrix: division by zero in '%s'", e->expr);
		  return -1;
		}
	      *out /= rhs;
	    }
	  else
	    *out = (rhs >= 0 && rhs < 63) ? (*out << rhs) : 0;
	}
      else
	break;
    }
  return 0;
}

static int
eval_add (eval_t *e, long long *out)
{
  if (eval_mul (e, out) != 0)
    return -1;
  for (;;)
    {
      eval_ws (e);
      char op = *e->p;
      if (op != '+' && op != '-')
	break;
      e->p++;
      long long rhs;
      if (eval_mul (e, &rhs) != 0)
	return -1;
      *out = (op == '+') ? (*out + rhs) : (*out - rhs);
    }
  return 0;
}

static int
eval_expr (const char *expr, const loopvar_t *loops, const long long *vals,
	   int nloops, long long page, long long *out)
{
  eval_t e;
  e.p = expr;
  e.expr = expr;
  e.loops = loops;
  e.vals = vals;
  e.nloops = nloops;
  e.page = page;
  if (eval_add (&e, out) != 0)
    return -1;
  eval_ws (&e);
  if (*e.p != '\0')
    {
      set_err ("matrix: trailing characters in expression '%s'", expr);
      return -1;
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Groups                                                             */
/* ------------------------------------------------------------------ */

enum
{ K_NONE = 0, K_SIZES, K_SRC, K_DST, K_ALIGN, K_FILL, K_DIFF, K_BOTH,
  K_LOOP, K_CASE, K_SIZE, K_PAIRS, K_DIST, K_DIST_SAMPLES };

enum
{ G_NONE = 0, G_FLAT, G_BLOCK };

typedef struct
{
  char *e1, *e2;
} pair_t;

typedef struct
{
  int mode;				/* G_*				*/
  int line_no;

  /* flat lists */
  list_t sizes, src, dst, align, fill, diff;
  int both;
  char *dist_name;		/* 'dist = NAME'			*/
  long dist_samples;		/* sizes to sample for the cases	*/

  /* block */
  loopvar_t loops[MAX_LOOPS];
  int nloops;
  int last_loop;			/* index used by continuation lines	*/
  char *size_expr;
  pair_t *pairs;
  size_t npairs, cpairs;
  char **aligns;
  size_t naligns, caligns;
  char **fills;
  size_t nfills, cfills;
  char **diffs;
  size_t ndiffs, cdiffs;
  int block_both;
} group_t;

static void
group_reset (group_t *g)
{
  list_free (&g->sizes);
  list_free (&g->src);
  list_free (&g->dst);
  list_free (&g->align);
  list_free (&g->fill);
  list_free (&g->diff);
  for (int i = 0; i < g->nloops; i++)
    list_free (&g->loops[i].vals);
  for (size_t i = 0; i < g->npairs; i++)
    {
      free (g->pairs[i].e1);
      free (g->pairs[i].e2);
    }
  for (size_t i = 0; i < g->naligns; i++)
    free (g->aligns[i]);
  for (size_t i = 0; i < g->nfills; i++)
    free (g->fills[i]);
  for (size_t i = 0; i < g->ndiffs; i++)
    free (g->diffs[i]);
  free (g->dist_name);
  memset (g, 0, sizeof *g);
}

static int
case_add (mb_matrix_t *m, size_t len, long a1, long a2, int c, int result,
	  int both)
{
  if (m->ncases >= MAX_CASES)
    {
      set_err ("matrix: too many cases (limit %u)", MAX_CASES);
      return -1;
    }
  if (m->ncases == m->cap)
    {
      size_t nc = m->cap ? m->cap * 2 : 256;
      mb_case_t *nv = realloc (m->cases, nc * sizeof *nv);
      if (nv == NULL)
	{
	  set_err ("matrix: out of memory");
	  return -1;
	}
      m->cases = nv;
      m->cap = nc;
    }
  mb_case_t *k = &m->cases[m->ncases++];
  k->len = len;
  k->a1 = a1;
  k->a2 = a2;
  k->c = c;
  k->result = result;
  k->pos = 0;
  k->both = both;
  return 0;
}

static int
str_list_add (char ***arr, size_t *n, size_t *cap, const char *s)
{
  if (*n == *cap)
    {
      size_t nc = *cap ? *cap * 2 : 8;
      char **nv = realloc (*arr, nc * sizeof *nv);
      if (nv == NULL)
	return -1;
      *arr = nv;
      *cap = nc;
    }
  char *copy = strdup (s);
  if (copy == NULL)
    return -1;
  (*arr)[(*n)++] = copy;
  return 0;
}

static int
pair_add (group_t *g, const char *e1, const char *e2)
{
  if (g->npairs == g->cpairs)
    {
      size_t nc = g->cpairs ? g->cpairs * 2 : 8;
      pair_t *nv = realloc (g->pairs, nc * sizeof *nv);
      if (nv == NULL)
	return -1;
      g->pairs = nv;
      g->cpairs = nc;
    }
  g->pairs[g->npairs].e1 = strdup (e1);
  g->pairs[g->npairs].e2 = strdup (e2);
  if (g->pairs[g->npairs].e1 == NULL || g->pairs[g->npairs].e2 == NULL)
    return -1;
  g->npairs++;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Expansion                                                          */
/* ------------------------------------------------------------------ */

static long long
page_size (void)
{
#ifdef _SC_PAGESIZE
  long p = sysconf (_SC_PAGESIZE);
  if (p > 0)
    return (long long) p;
#endif
  return 4096;
}

static int
expand_flat (group_t *g, const char *section, mb_matrix_t *m, uint64_t seed)
{
  int line = g->line_no;

  if (g->dist_name != NULL)
    {
      /* The sizes come from a distribution: sample a bounded set of
	 distinct sizes for the individual cases, and remember the whole
	 distribution for the randomized ("mixed") measurement mode.  */
      if (g->sizes.n != 0)
	{
	  set_err ("matrix: line %d: use either 'sizes' or 'dist', not both",
		   line);
	  return -1;
	}
      mb_dist_t d;
      mb_dist_init (&d);
      if (mb_dist_load (g->dist_name, &d) != 0)
	{
	  set_err ("matrix: line %d: %s", line, mb_dist_err ());
	  return -1;
	}
      size_t want = g->dist_samples > 0 ? (size_t) g->dist_samples : 64;
      uint64_t st = seed | UINT64_C (1);
      size_t attempts = 0, max_attempts = want * 64 + 1024;
      while (g->sizes.n < want && g->sizes.n < d.n && attempts < max_attempts)
	{
	  attempts++;
	  size_t v = mb_dist_sample (&d, &st);
	  int seen = 0;
	  for (size_t i = 0; i < g->sizes.n; i++)
	    if (g->sizes.v[i] == (long long) v)
	      {
		seen = 1;
		break;
	      }
	  if (!seen && list_push (&g->sizes, (long long) v) != 0)
	    {
	      mb_dist_free (&d);
	      set_err ("matrix: too many values");
	      return -1;
	    }
	}
      /* Cases in ascending size order.  */
      for (size_t i = 0; i + 1 < g->sizes.n; i++)
	for (size_t j = i + 1; j < g->sizes.n; j++)
	  if (g->sizes.v[j] < g->sizes.v[i])
	    {
	      long long t = g->sizes.v[i];
	      g->sizes.v[i] = g->sizes.v[j];
	      g->sizes.v[j] = t;
	    }
      mb_dist_free (&m->dist);
      m->dist = d;
    }

  if (g->sizes.n == 0)
    {
      set_err ("matrix: line %d: the [%s] group must define 'sizes' or "
	       "'dist'", line, section);
      return -1;
    }

  if (strcmp (section, "memcpy") == 0)
    {
      if (g->src.n == 0 || g->dst.n == 0)
	{
	  set_err ("matrix: line %d: [memcpy] needs 'src' and 'dst'",
		   line);
	  return -1;
	}
      for (size_t a = 0; a < g->sizes.n; a++)
	for (size_t i = 0; i < g->src.n; i++)
	  for (size_t j = 0; j < g->dst.n; j++)
	    if (case_add (m, (size_t) g->sizes.v[a], g->src.v[i],
			  g->dst.v[j], 0, 0, g->both) != 0)
	      return -1;
      return 0;
    }

  if (strcmp (section, "memmove") == 0)
    {
      if (g->src.n == 0 && g->dst.n == 0)
	{
	  set_err ("matrix: line %d: [memmove] needs a 'src' and/or 'dst' "
		   "offset list", line);
	  return -1;
	}
      if (g->src.n == g->dst.n)
	{
	  int same = 1;
	  for (size_t i = 0; i < g->src.n; i++)
	    if (g->src.v[i] != g->dst.v[i])
	      same = 0;
	  if (same)
	    {
	      /* Same offset list: every unordered pair, both directions.  */
	      for (size_t a = 0; a < g->sizes.n; a++)
		for (size_t i = 0; i < g->src.n; i++)
		  for (size_t j = i + 1; j < g->src.n; j++)
		    {
		      if (g->src.v[i] == g->src.v[j])
			continue;
		      if (case_add (m, (size_t) g->sizes.v[a], g->src.v[i],
				    g->src.v[j], 0, 0, 0) != 0
			  || case_add (m, (size_t) g->sizes.v[a],
				       g->src.v[j], g->src.v[i], 0, 0,
				       0) != 0)
			return -1;
		    }
	      return 0;
	    }
	}
      for (size_t a = 0; a < g->sizes.n; a++)
	for (size_t i = 0; i < g->src.n; i++)
	  for (size_t j = 0; j < g->dst.n; j++)
	    {
	      if (g->src.v[i] == g->dst.v[j])
		continue;
	      if (case_add (m, (size_t) g->sizes.v[a], g->src.v[i],
			    g->dst.v[j], 0, 0, 0) != 0)
		return -1;
	      if (g->both
		  && case_add (m, (size_t) g->sizes.v[a], g->dst.v[j],
			       g->src.v[i], 0, 0, 0) != 0)
		return -1;
	    }
      return 0;
    }

  if (strcmp (section, "memset") == 0)
    {
      if (g->align.n == 0 || g->fill.n == 0)
	{
	  set_err ("matrix: line %d: [memset] needs 'align' and 'fill'",
		   line);
	  return -1;
	}
      for (size_t a = 0; a < g->sizes.n; a++)
	for (size_t i = 0; i < g->align.n; i++)
	  for (size_t j = 0; j < g->fill.n; j++)
	    if (case_add (m, (size_t) g->sizes.v[a], g->align.v[i], 0,
			  (int) g->fill.v[j], 0, 0) != 0)
	      return -1;
      return 0;
    }

  if (strcmp (section, "memcmp") == 0)
    {
      if (g->src.n == 0 || g->dst.n == 0 || g->diff.n == 0)
	{
	  set_err ("matrix: line %d: [memcmp] needs 'src', 'dst' and "
		   "'diff'", line);
	  return -1;
	}
      for (size_t a = 0; a < g->sizes.n; a++)
	for (size_t i = 0; i < g->src.n; i++)
	  for (size_t j = 0; j < g->dst.n; j++)
	    for (size_t r = 0; r < g->diff.n; r++)
	      if (case_add (m, (size_t) g->sizes.v[a], g->src.v[i],
			    g->dst.v[j], 0, (int) g->diff.v[r], 0) != 0)
		return -1;
      return 0;
    }

  set_err ("matrix: unknown function section '[%s]'", section);
  return -1;
}

static int
expand_block (group_t *g, const char *section, mb_matrix_t *m, uint64_t seed)
{
  (void) seed;
  long long page = page_size ();
  int line = g->line_no;
  long long vals[MAX_LOOPS] = { 0, 0, 0, 0 };

  if (g->size_expr == NULL)
    {
      set_err ("matrix: line %d: the [%s] case block has no 'size'",
	       line, section);
      return -1;
    }
  int is_memset = strcmp (section, "memset") == 0;
  int is_memcmp = strcmp (section, "memcmp") == 0;
  int is_memcpy = strcmp (section, "memcpy") == 0;
  int is_memmove = strcmp (section, "memmove") == 0;
  if (!is_memset && !is_memcmp && !is_memcpy && !is_memmove)
    {
      set_err ("matrix: unknown function section '[%s]'", section);
      return -1;
    }
  if (!is_memset && g->npairs == 0)
    {
      set_err ("matrix: line %d: the [%s] case block has no 'pairs'",
	       line, section);
      return -1;
    }
  if (is_memset && (g->naligns == 0 || g->nfills == 0))
    {
      set_err ("matrix: line %d: the [memset] case block needs 'align' "
	       "and 'fill'", line);
      return -1;
    }
  if (is_memcmp && g->ndiffs == 0)
    {
      set_err ("matrix: line %d: the [memcmp] case block needs 'diff'",
	       line);
      return -1;
    }

  /* Odometer over the loop variables (no loops => one iteration).  */
  size_t idx[MAX_LOOPS] = { 0, 0, 0, 0 };
  for (int i = 0; i < g->nloops; i++)
    vals[i] = g->loops[i].vals.v[0];
  for (;;)
    {
      long long len;
      if (eval_expr (g->size_expr, g->loops, vals, g->nloops, page,
		     &len) != 0)
	return -1;

      if (is_memset)
	{
	  for (size_t a = 0; a < g->naligns; a++)
	    {
	      long long al;
	      if (eval_expr (g->aligns[a], g->loops, vals, g->nloops, page,
			     &al) != 0)
		return -1;
	      for (size_t f = 0; f < g->nfills; f++)
		{
		  long long c;
		  if (eval_expr (g->fills[f], g->loops, vals, g->nloops,
				 page, &c) != 0)
		    return -1;
		  if (case_add (m, (size_t) len, al, 0, (int) c, 0, 0) != 0)
		    return -1;
		}
	    }
	}
      else
	{
	  for (size_t p = 0; p < g->npairs; p++)
	    {
	      long long a1, a2;
	      if (eval_expr (g->pairs[p].e1, g->loops, vals, g->nloops,
			     page, &a1) != 0
		  || eval_expr (g->pairs[p].e2, g->loops, vals, g->nloops,
				page, &a2) != 0)
		return -1;
	      if (is_memcmp)
		{
		  for (size_t r = 0; r < g->ndiffs; r++)
		    {
		      long long res;
		      if (eval_expr (g->diffs[r], g->loops, vals, g->nloops,
				     page, &res) != 0)
			return -1;
		      if (case_add (m, (size_t) len, a1, a2, 0, (int) res,
				    0) != 0)
			return -1;
		    }
		}
	      else if (case_add (m, (size_t) len, a1, a2, 0, 0,
				 is_memcpy ? g->block_both : 0) != 0)
		return -1;
	    }
	}

      /* Next loop combination (odometer).  */
      int i = g->nloops - 1;
      while (i >= 0 && ++idx[i] >= g->loops[i].vals.n)
	{
	  idx[i] = 0;
	  vals[i] = g->loops[i].vals.v[0];
	  i--;
	}
      if (i < 0)
	break;
      vals[i] = g->loops[i].vals.v[idx[i]];
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Line-level parsing                                                 */
/* ------------------------------------------------------------------ */

static int
key_of (const char *key)
{
  if (!strcmp (key, "sizes") || !strcmp (key, "size")
      || !strcmp (key, "lengths") || !strcmp (key, "length"))
    return K_SIZES;
  if (!strcmp (key, "src") || !strcmp (key, "align1") || !strcmp (key, "s"))
    return K_SRC;
  if (!strcmp (key, "dst") || !strcmp (key, "align2") || !strcmp (key, "d"))
    return K_DST;
  if (!strcmp (key, "align") || !strcmp (key, "alignment")
      || !strcmp (key, "a"))
    return K_ALIGN;
  if (!strcmp (key, "fill") || !strcmp (key, "c") || !strcmp (key, "char"))
    return K_FILL;
  if (!strcmp (key, "diff") || !strcmp (key, "result"))
    return K_DIFF;
  if (!strcmp (key, "both"))
    return K_BOTH;
  if (!strcmp (key, "loop"))
    return K_LOOP;
  if (!strcmp (key, "case") || !strcmp (key, "block"))
    return K_CASE;
  if (!strcmp (key, "pairs"))
    return K_PAIRS;
  if (!strcmp (key, "dist") || !strcmp (key, "distribution"))
    return K_DIST;
  if (!strcmp (key, "dist-samples") || !strcmp (key, "dist_samples"))
    return K_DIST_SAMPLES;
  return K_NONE;
}

static void
rtrim (char *s)
{
  size_t n = strlen (s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'
		   || s[n - 1] == '\n' || s[n - 1] == '\r'))
    s[--n] = '\0';
}

static const char *
skip_ws (const char *p)
{
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

/* Parse "(e1,e2) (e3,e4) ..." and append to G.  */
static int
parse_pairs (group_t *g, const char *p, int line_no)
{
  for (;;)
    {
      p = skip_ws (p);
      while (*p == ',')
	p = skip_ws (p + 1);
      if (*p == '\0')
	return 0;
      if (*p != '(')
	{
	  set_err ("matrix: line %d: expected '(' in pairs list", line_no);
	  return -1;
	}
      p++;
      char e1[256], e2[256];
      size_t n1 = 0, n2 = 0;
      int second = 0;
      while (*p != '\0' && *p != ')')
	{
	  if (*p == ',' && !second)
	    {
	      second = 1;
	      p++;
	      continue;
	    }
	  char *dst = second ? e2 : e1;
	  size_t *n = second ? &n2 : &n1;
	  if (*n < sizeof e1 - 1)
	    dst[(*n)++] = *p;
	  p++;
	}
      if (*p != ')')
	{
	  set_err ("matrix: line %d: unterminated pair in pairs list",
		   line_no);
	  return -1;
	}
      p++;
      e1[n1] = '\0';
      e2[n2] = '\0';
      rtrim (e1);
      rtrim (e2);
      if (n1 == 0 || !second)
	{
	  set_err ("matrix: line %d: bad pair '(%.*s)'", line_no,
		   (int) (p - g->size_expr), e1);
	  return -1;
	}
      if (pair_add (g, e1, e2) != 0)
	{
	  set_err ("matrix: out of memory");
	  return -1;
	}
    }
}

/* Append the value text of key K (in group G) to the group.  */
static int
append_key_values (group_t *g, int k, const char *val, int line_no)
{
  if (k == K_PAIRS)
    return parse_pairs (g, val, line_no);

  if (k == K_BOTH)
    {
      char *end = NULL;
      long long x = strtoll (skip_ws (val), &end, 0);
      if (end == val || (x != 0 && x != 1))
	{
	  set_err ("matrix: line %d: 'both' must be 0 or 1", line_no);
	  return -1;
	}
      if (g->mode == G_BLOCK)
	g->block_both = (int) x;
      else
	g->both = (int) x;
      return 0;
    }

  if (k == K_SIZE)
    {
      free (g->size_expr);
      g->size_expr = strdup (skip_ws (val));
      if (g->size_expr == NULL)
	return -1;
      return 0;
    }

  if (k == K_DIST)
    {
      if (g->mode != G_FLAT)
	{
	  set_err ("matrix: line %d: 'dist' is only valid with the flat "
		   "notation (sizes/src/dst or align/fill)", line_no);
	  return -1;
	}
      free (g->dist_name);
      g->dist_name = strdup (skip_ws (val));
      if (g->dist_name == NULL)
	return -1;
      /* Drop trailing spaces left by the raw value.  */
      char *e = g->dist_name + strlen (g->dist_name);
      while (e > g->dist_name && (e[-1] == ' ' || e[-1] == '\t'))
	*--e = '\0';
      return 0;
    }

  if (k == K_DIST_SAMPLES)
    {
      char *end = NULL;
      long v = strtol (skip_ws (val), &end, 0);
      if (end == val || v <= 0)
	{
	  set_err ("matrix: line %d: 'dist-samples' must be positive",
		   line_no);
	  return -1;
	}
      g->dist_samples = v;
      return 0;
    }

  /* Expression lists in blocks are comma separated (so that expressions
     may contain spaces); flat lists accept spaces and commas.  */
  if (g->mode == G_BLOCK
      && (k == K_ALIGN || k == K_FILL || k == K_DIFF))
    {
      char *copy = strdup (val);
      if (copy == NULL)
	return -1;
      char *tok = strtok (copy, ",");
      while (tok != NULL)
	{
	  while (*tok == ' ' || *tok == '\t')
	    tok++;
	  rtrim (tok);
	  if (*tok != '\0')
	    {
	      char ***arr;
	      size_t *n, *cap;
	      if (k == K_ALIGN)
		{ arr = &g->aligns; n = &g->naligns; cap = &g->caligns; }
	      else if (k == K_FILL)
		{ arr = &g->fills; n = &g->nfills; cap = &g->cfills; }
	      else
		{ arr = &g->diffs; n = &g->ndiffs; cap = &g->cdiffs; }
	      if (str_list_add (arr, n, cap, tok) != 0)
		{
		  free (copy);
		  set_err ("matrix: out of memory");
		  return -1;
		}
	    }
	  tok = strtok (NULL, ",");
	}
      free (copy);
      return 0;
    }

  list_t *l = NULL;
  switch (k)
    {
    case K_SIZES: l = &g->sizes; break;
    case K_SRC:   l = &g->src; break;
    case K_DST:   l = &g->dst; break;
    case K_ALIGN: l = &g->align; break;
    case K_FILL:  l = &g->fill; break;
    case K_DIFF:  l = &g->diff; break;
    default:
      set_err ("matrix: line %d: unexpected key", line_no);
      return -1;
    }

  char *copy = strdup (val);
  if (copy == NULL)
    return -1;
  char *tok = strtok (copy, " \t,");
  while (tok != NULL)
    {
      if (add_value_token (l, tok, line_no, NULL) != 0)
	{
	  free (copy);
	  return -1;
	}
      tok = strtok (NULL, " \t,");
    }
  free (copy);
  return 0;
}

/* "loop <var> = <values>" */
static int
parse_loop (group_t *g, const char *val, int line_no)
{
  if (g->mode != G_BLOCK)
    {
      set_err ("matrix: line %d: 'loop' outside a case block", line_no);
      return -1;
    }
  if (g->nloops >= MAX_LOOPS)
    {
      set_err ("matrix: line %d: at most %d loop variables per block",
	       line_no, MAX_LOOPS);
      return -1;
    }
  const char *p = skip_ws (val);
  char name[32];
  size_t n = 0;
  while (*p != '\0' && *p != '=' && !isspace ((unsigned char) *p)
	 && n < sizeof name - 1)
    name[n++] = *p++;
  name[n] = '\0';
  p = skip_ws (p);
  if (*p != '=')
    {
      set_err ("matrix: line %d: expected 'loop <var> = <values>'",
	       line_no);
      return -1;
    }
  p++;
  if (n == 0)
    {
      set_err ("matrix: line %d: missing loop variable name", line_no);
      return -1;
    }

  loopvar_t *lv = &g->loops[g->nloops];
  memset (lv, 0, sizeof *lv);
  snprintf (lv->name, sizeof lv->name, "%s", name);

  int skip_pow2 = 0;
  char *copy = strdup (p);
  if (copy == NULL)
    return -1;
  char *tok = strtok (copy, " \t,");
  while (tok != NULL)
    {
      if (add_value_token (&lv->vals, tok, line_no, &skip_pow2) != 0)
	{
	  free (copy);
	  return -1;
	}
      tok = strtok (NULL, " \t,");
    }
  free (copy);
  if (skip_pow2)
    filter_pow2 (&lv->vals);
  if (lv->vals.n == 0)
    {
      set_err ("matrix: line %d: empty loop list", line_no);
      return -1;
    }
  g->last_loop = g->nloops;
  g->nloops++;
  return 0;
}

/* Append more values to the most recent loop variable (continuation).  */
static int
loop_append (group_t *g, const char *val, int line_no)
{
  if (g->nloops == 0 || g->last_loop >= g->nloops)
    {
      set_err ("matrix: line %d: continuation of an unknown loop",
	       line_no);
      return -1;
    }
  loopvar_t *lv = &g->loops[g->last_loop];
  int skip_pow2 = 0;
  char *copy = strdup (val);
  if (copy == NULL)
    return -1;
  char *tok = strtok (copy, " \t,");
  while (tok != NULL)
    {
      if (add_value_token (&lv->vals, tok, line_no, &skip_pow2) != 0)
	{
	  free (copy);
	  return -1;
	}
      tok = strtok (NULL, " \t,");
    }
  free (copy);
  if (skip_pow2)
    filter_pow2 (&lv->vals);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Top level                                                          */
/* ------------------------------------------------------------------ */

int
mb_matrix_parse (const char *text, const char *section, uint64_t seed,
		 mb_matrix_t *m)
{
  group_t g;
  memset (&g, 0, sizeof g);

  int active = 0, saw_section = 0, line_no = 0;
  int last_key = K_NONE;
  int rc = -1;

  const char *p = text;
  char line[LINE_MAX];

  while (*p != '\0')
    {
      /* Copy one line.  */
      size_t n = 0;
      while (*p != '\0' && *p != '\n' && n < sizeof line - 1)
	line[n++] = *p++;
      if (*p == '\n')
	p++;
      line[n] = '\0';
      line_no++;
      rtrim (line);

      char *s = line;
      s = (char *) skip_ws (s);
      if (*s == '\0' || *s == '#' || *s == ';')
	continue;

      if (*s == '[')
	{
	  char *end = strchr (s, ']');
	  if (end == NULL)
	    {
	      set_err ("matrix: line %d: unterminated section header",
		       line_no);
	      goto out;
	    }
	  *end = '\0';
	  char *name = (char *) skip_ws (s + 1);
	  rtrim (name);
	  if (active && g.mode != G_NONE)
	    {
	      int bad = (g.mode == G_FLAT)
		? expand_flat (&g, section, m, seed)
		: expand_block (&g, section, m, seed);
	      group_reset (&g);
	      if (bad != 0)
		goto out;
	    }
	  active = strcmp (name, section) == 0;
	  if (active)
	    saw_section = 1;
	  last_key = K_NONE;
	  continue;
	}

      if (!active)
	continue;

      /* `loop <var> = <values>`: the '=' belongs to the loop, not to a
         key, so it is handled before the generic key/value split.  */
      if (strncmp (s, "loop", 4) == 0 && (s[4] == ' ' || s[4] == '\t'))
	{
	  if (g.mode != G_BLOCK)
	    {
	      if (g.mode == G_FLAT)
		{
		  int bad = expand_flat (&g, section, m, seed);
		  group_reset (&g);
		  if (bad != 0)
		    goto out;
		}
	      g.mode = G_BLOCK;
	      g.line_no = line_no;
	      g.block_both = 1;
	    }
	  if (parse_loop (&g, s + 4, line_no) != 0)
	    goto out;
	  last_key = K_LOOP;
	  continue;
	}

      char *eq = strchr (s, '=');
      int k;
      char *val = NULL;

      if (eq != NULL)
	{
	  *eq = '\0';
	  char *key = s;
	  rtrim (key);
	  val = eq + 1;

	  k = key_of (key);
	  if (k == K_NONE)
	    {
	      set_err ("matrix: line %d: unknown key '%s'", line_no, key);
	      goto out;
	    }
	  /* In a case block "size" is the length expression, not the flat
	     "sizes" list.  */
	  if (k == K_SIZES && strcmp (key, "size") == 0
	      && g.mode != G_FLAT)
	    k = K_SIZE;

	  /* Start a new group when needed.  */
	  int start_block = (k == K_LOOP || k == K_CASE);
	  int start_flat = ((k == K_SIZES || k == K_DIST)
			    && g.mode != G_FLAT);
	  if (k == K_CASE)
	    {
	      if (g.mode != G_NONE)
		{
		  int bad = (g.mode == G_FLAT)
		    ? expand_flat (&g, section, m, seed)
		    : expand_block (&g, section, m, seed);
		  group_reset (&g);
		  if (bad != 0)
		    goto out;
		}
	      g.mode = G_BLOCK;
	      g.line_no = line_no;
	      g.block_both = 1;
	      last_key = K_NONE;
	      continue;
	    }
	  if (start_block || start_flat || g.mode == G_NONE)
	    {
	      if (g.mode != G_NONE)
		{
		  int bad = (g.mode == G_FLAT)
		    ? expand_flat (&g, section, m, seed)
		    : expand_block (&g, section, m, seed);
		  group_reset (&g);
		  if (bad != 0)
		    goto out;
		}
	      g.mode = start_block ? G_BLOCK : G_FLAT;
	      g.line_no = line_no;
	      if (g.mode == G_FLAT)
		{
		  g.both = 1;
		  g.dist_samples = 64;
		}
	      else
		g.block_both = 1;
	    }
	}
      else if (strcmp (s, "case") == 0 || strcmp (s, "block") == 0)
	{
	  /* A bare `case` starts a new case block.  */
	  if (g.mode != G_NONE)
	    {
	      int bad = (g.mode == G_FLAT)
		? expand_flat (&g, section, m, seed)
		: expand_block (&g, section, m, seed);
	      group_reset (&g);
	      if (bad != 0)
		goto out;
	    }
	  g.mode = G_BLOCK;
	  g.line_no = line_no;
	  g.block_both = 1;
	  last_key = K_NONE;
	  continue;
	}
      else
	{
	  /* Continuation line: append to the previous key.  A line that
	     starts with a known key name usually means the '=' was
	     forgotten, which deserves its own message.  */
	  char first[64];
	  size_t fn = 0;
	  const char *q = s;
	  while (*q != '\0' && *q != ' ' && *q != '\t' && *q != ','
		 && fn < sizeof first - 1)
	    first[fn++] = *q++;
	  first[fn] = '\0';
	  if (key_of (first) != K_NONE)
	    {
	      set_err ("matrix: line %d: missing '=' after '%s' "
		       "(write '%s = ...')", line_no, first, first);
	      goto out;
	    }
	  if (last_key == K_NONE || last_key == K_CASE || last_key == K_SIZE)
	    {
	      set_err ("matrix: line %d: expected 'key = value'", line_no);
	      goto out;
	    }
	  k = last_key;
	  val = s;
	}

      /* Trailing comments are allowed after a value ("... # note").  */
      if (val != NULL)
	{
	  char *hash = strchr (val, '#');
	  if (hash != NULL)
	    *hash = '\0';
	}

      if (k == K_LOOP)
	{
	  if (loop_append (&g, val, line_no) != 0)
	    goto out;
	}
      else if (k == K_CASE)
	{
	  /* handled above */
	}
      else if (append_key_values (&g, k, val, line_no) != 0)
	goto out;

      last_key = k;
    }

  if (!saw_section)
    {
      set_err ("matrix: no [%s] section", section);
      goto out;
    }
  if (g.mode != G_NONE)
    {
      int bad = (g.mode == G_FLAT) ? expand_flat (&g, section, m, seed)
				   : expand_block (&g, section, m, seed);
      if (bad != 0)
	goto out;
    }
  if (m->ncases == 0)
    {
      set_err ("matrix: [%s] produced no cases", section);
      goto out;
    }
  rc = 0;

out:
  group_reset (&g);
  return rc;
}

int
mb_matrix_load (const char *path, const char *section, uint64_t seed,
		mb_matrix_t *m)
{
  FILE *fp = fopen (path, "r");
  if (fp == NULL)
    {
      set_err ("matrix: cannot open '%s'", path);
      return -1;
    }
  size_t cap = 8192, n = 0;
  char *buf = malloc (cap);
  if (buf == NULL)
    {
      fclose (fp);
      set_err ("matrix: out of memory");
      return -1;
    }
  for (;;)
    {
      if (n == cap)
	{
	  cap *= 2;
	  char *nb = realloc (buf, cap);
	  if (nb == NULL)
	    {
	      free (buf);
	      fclose (fp);
	      set_err ("matrix: out of memory");
	      return -1;
	    }
	  buf = nb;
	}
      size_t got = fread (buf + n, 1, cap - n, fp);
      n += got;
      if (got == 0)
	break;
    }
  fclose (fp);
  if (n == cap)
    {
      char *nb = realloc (buf, cap + 1);
      if (nb == NULL)
	{
	  free (buf);
	  set_err ("matrix: out of memory");
	  return -1;
	}
      buf = nb;
    }
  buf[n] = '\0';
  int rc = mb_matrix_parse (buf, section, seed, m);
  free (buf);
  return rc;
}

void
mb_matrix_init (mb_matrix_t *m)
{
  memset (m, 0, sizeof *m);
  mb_dist_init (&m->dist);
}

void
mb_matrix_free (mb_matrix_t *m)
{
  free (m->cases);
  mb_dist_free (&m->dist);
  memset (m, 0, sizeof *m);
}

long long
mb_matrix_max_len (const mb_matrix_t *m)
{
  long long mx = 0;
  for (size_t i = 0; i < m->ncases; i++)
    if ((long long) m->cases[i].len > mx)
      mx = (long long) m->cases[i].len;
  return mx;
}

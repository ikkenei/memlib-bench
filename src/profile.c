/* memtrace profile loading (implementation).  See profile.h.  */

#include "profile.h"

#include "bench_common.h"	/* mb_rng_next */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 1024
#define MAX_ROWS ((size_t) 1 << 22)

static char perr[512];

/* Strict integer parser: the whole (trimmed) field must be a number.  */
static int
parse_long (const char *field, long *val)
{
  char *end = NULL;
  long v = strtol (field, &end, 0);
  if (end == field || *end != '\0')
    return -1;
  *val = v;
  return 0;
}

static void
set_err (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (perr, sizeof perr, fmt, ap);
  va_end (ap);
}

const char *
mb_profile_err (void)
{
  return perr;
}

void
mb_profile_init (mb_profile_t *p)
{
  memset (p, 0, sizeof *p);
}

void
mb_profile_free (mb_profile_t *p)
{
  for (size_t i = 0; i < p->ngroups; i++)
    {
      free (p->groups[i].cases);
      free (p->groups[i].cum);
    }
  free (p->groups);
  memset (p, 0, sizeof *p);
}

const char *
mb_profile_class_name (int cls)
{
  switch (cls)
    {
    case 0: return "plain";
    case 1: return "fwd-disjoint";
    case 2: return "fwd-overlap";
    case 3: return "bwd-disjoint";
    case 4: return "bwd-overlap";
    case 5: return "zero";
    case 6: return "nonzero";
    case 7: return "equal";
    case 8: return "differs";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */

static int
add_group (mb_profile_t *p, int cls, int attr, mb_profile_group_t **out)
{
  for (size_t i = 0; i < p->ngroups; i++)
    if (p->groups[i].cls == cls && p->groups[i].attr == attr)
      {
	*out = &p->groups[i];
	return 0;
      }
  mb_profile_group_t *ng = realloc (p->groups,
				    (p->ngroups + 1) * sizeof *ng);
  if (ng == NULL)
    {
      set_err ("out of memory");
      return -1;
    }
  p->groups = ng;
  mb_profile_group_t *g = &p->groups[p->ngroups++];
  memset (g, 0, sizeof *g);
  g->cls = cls;
  g->attr = attr;
  *out = g;
  return 0;
}

/* Turn one profile row into a case for FUNC.  */
static void
make_case (const char *func, size_t size, long sa, long da, int cls,
	   int attr, mb_case_t *c)
{
  memset (c, 0, sizeof *c);
  c->len = size;
  c->pos = 0;

  if (strcmp (func, "memcpy") == 0)
    {
      c->a1 = sa < 0 ? 0 : sa;
      c->a2 = da < 0 ? 0 : da;
    }
  else if (strcmp (func, "memmove") == 0)
    {
      long src = sa < 0 ? 0 : sa;
      c->a1 = src;
      if (cls == 0)
	c->a2 = src;				/* src == dst		*/
      else if (cls == 1)			/* forward, disjoint	*/
	c->a2 = src + (long) size + 64;
      else if (cls == 2)			/* forward, overlapping	*/
	c->a2 = src + (attr > 0 ? attr : 1);
      else if (cls == 3)			/* backward, disjoint	*/
	c->a2 = src - (long) size - 64 < 0 ? 0 : src - (long) size - 64;
      else					/* backward, overlapping */
	c->a2 = src - (attr > 0 ? attr : 1) < 0 ? src + 1
						: src - (attr > 0 ? attr : 1);
    }
  else if (strcmp (func, "memset") == 0)
    {
      c->a1 = sa < 0 ? 0 : sa;
      c->c = cls == 5 ? 0 : attr;
    }
  else if (strcmp (func, "memcmp") == 0)
    {
      c->a1 = sa < 0 ? 0 : sa;
      c->a2 = da < 0 ? 0 : da;
      c->result = cls == 7 ? 0 : 1;
      c->pos = attr;				/* 0 = last byte	*/
    }
}

int
mb_profile_load (const char *path, const char *func, mb_profile_t *p)
{
  FILE *fp = fopen (path, "r");
  if (fp == NULL)
    {
      set_err ("cannot open '%s': %s", path, strerror (errno));
      return -1;
    }

  size_t total_rows = 0, used_rows = 0;
  char line[LINE_MAX];
  int lineno = 0;
  int rc = -1;

  while (fgets (line, sizeof line, fp) != NULL)
    {
      lineno++;
      char *hash = strchr (line, '#');
      if (hash != NULL)
	*hash = '\0';

      /* Trim.  */
      char *s = line;
      while (*s == ' ' || *s == '\t')
	s++;
      char *e = s + strlen (s);
      while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '
		       || e[-1] == '\t'))
	*--e = '\0';
      if (*s == '\0')
	continue;

      char *fields[7];
      int nf = 0;
      char *tok = strtok (s, ",");
      while (tok != NULL && nf < 7)
	{
	  while (*tok == ' ' || *tok == '\t')
	    tok++;
	  char *te = tok + strlen (tok);
	  while (te > tok && (te[-1] == ' ' || te[-1] == '\t'))
	    *--te = '\0';
	  fields[nf++] = tok;
	  tok = strtok (NULL, ",");
	}
      if (nf != 7)
	{
	  set_err ("%s:%d: expected 7 comma separated fields", path, lineno);
	  goto out;
	}
      if (strcmp (fields[0], func) != 0)
	continue;
      if (total_rows >= MAX_ROWS)
	{
	  set_err ("%s: too many rows (limit %zu)", path, MAX_ROWS);
	  goto out;
	}
      total_rows++;

      long size, count, sa = -1, da = -1, cls, attr;
      if (parse_long (fields[1], &size) != 0 || size < 0)
	{
	  set_err ("%s:%d: bad size '%s'", path, lineno, fields[1]);
	  goto out;
	}
      if (parse_long (fields[2], &count) != 0 || count <= 0)
	{
	  set_err ("%s:%d: bad count '%s'", path, lineno, fields[2]);
	  goto out;
	}
      if (fields[3][0] != '-' && (parse_long (fields[3], &sa) != 0
				  || sa < 0))
	{
	  set_err ("%s:%d: bad source alignment '%s'", path, lineno,
		   fields[3]);
	  goto out;
	}
      if (fields[4][0] != '-' && (parse_long (fields[4], &da) != 0
				  || da < 0))
	{
	  set_err ("%s:%d: bad destination alignment '%s'", path, lineno,
		   fields[4]);
	  goto out;
	}
      if (parse_long (fields[5], &cls) != 0 || cls < 0 || cls > 8)
	{
	  set_err ("%s:%d: bad class '%s'", path, lineno, fields[5]);
	  goto out;
	}
      if (parse_long (fields[6], &attr) != 0 || attr < 0)
	{
	  set_err ("%s:%d: bad attr '%s'", path, lineno, fields[6]);
	  goto out;
	}

      mb_profile_group_t *g = NULL;
      if (add_group (p, (int) cls, (int) attr, &g) != 0)
	goto out;

      mb_case_t c;
      make_case (func, (size_t) size, sa, da, (int) cls, (int) attr, &c);

      mb_case_t *nc = realloc (g->cases, (g->n + 1) * sizeof *nc);
      if (nc == NULL)
	{
	  set_err ("out of memory");
	  goto out;
	}
      g->cases = nc;
      g->cases[g->n] = c;
      double *nw = realloc (g->cum, (g->n + 1) * sizeof *nw);
      if (nw == NULL)
	{
	  set_err ("out of memory");
	  goto out;
	}
      g->cum = nw;
      g->cum[g->n] = (double) count;
      g->n++;
      g->weight += (double) count;
      if (g->n == 1 || c.len < g->min_len)
	g->min_len = c.len;
      if (g->n == 1 || c.len > g->max_len)
	g->max_len = c.len;
      used_rows++;
    }

  if (used_rows == 0)
    {
      set_err ("%s: no '%s' rows found", path, func);
      goto out;
    }

  /* Cumulative weights per group and the global size range.  */
  for (size_t i = 0; i < p->ngroups; i++)
    {
      mb_profile_group_t *g = &p->groups[i];
      double acc = 0.0, sum = 0.0;
      for (size_t j = 0; j < g->n; j++)
	{
	  acc += g->cum[j];
	  sum += g->cum[j] * (double) g->cases[j].len;
	  g->cum[j] = acc;
	}
      for (size_t j = 0; j < g->n; j++)
	g->cum[j] /= acc;
      g->cum[g->n - 1] = 1.0;
      g->mean_len = (size_t) (sum / acc + 0.5);
      if (i == 0 || g->min_len < p->min_len)
	p->min_len = g->min_len;
      if (i == 0 || g->max_len > p->max_len)
	p->max_len = g->max_len;
    }

  const char *base = strrchr (path, '/');
  snprintf (p->name, sizeof p->name, "%s", base != NULL ? base + 1 : path);
  rc = 0;

out:
  fclose (fp);
  if (rc != 0)
    mb_profile_free (p);
  return rc;
}

const mb_case_t *
mb_profile_sample (const mb_profile_group_t *g, uint64_t *state)
{
  if (g->n == 0)
    return NULL;
  if (g->n == 1)
    return &g->cases[0];

  double u = (double) (mb_rng_next (state) >> 11)
	     * (1.0 / 9007199254740992.0);
  size_t lo = 0, hi = g->n - 1;
  while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      if (g->cum[mid] <= u)
	lo = mid + 1;
      else
	hi = mid;
    }
  return &g->cases[lo];
}

/* Size distributions from real workloads (implementation).  See
   distributions.h.  */

#include "distributions.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"	/* mb_rng_next */
#include "distributions_gen.h"	/* generated table of the vendored csv */

#define MB_DIST_MAX_ENTRIES ((size_t) 1 << 20)

static char derr[512];

static void
set_err (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vsnprintf (derr, sizeof derr, fmt, ap);
  va_end (ap);
}

const char *
mb_dist_err (void)
{
  return derr;
}

void
mb_dist_init (mb_dist_t *d)
{
  memset (d, 0, sizeof *d);
}

void
mb_dist_free (mb_dist_t *d)
{
  free (d->sizes);
  free (d->cum);
  memset (d, 0, sizeof *d);
}

size_t
mb_dist_max (const mb_dist_t *d)
{
  return d->n ? d->sizes[d->n - 1] : 0;
}

/* Keep lowercase alphanumerics only, so "memcpy Google A",
   "MemcpyGoogleA" and "memcpy_google_a" all match.  */
static void
normalize (const char *in, char *out, size_t outsize)
{
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *) in;
       *p != '\0' && o + 1 < outsize; p++)
    if (isalnum (*p))
      out[o++] = (char) tolower (*p);
  out[o] = '\0';
}

static const char *
find_embedded (const char *name)
{
  char want[64];
  normalize (name, want, sizeof want);
  for (size_t i = 0; i < MB_DIST_TABLE_SIZE; i++)
    {
      char have[64];
      normalize (mb_dist_table[i].name, have, sizeof have);
      if (strcmp (have, want) == 0)
	return mb_dist_table[i].csv;
    }
  return NULL;
}

const char *const *
mb_dist_names (void)
{
  static const char *names[MB_DIST_TABLE_SIZE + 1];
  static int ready = 0;
  if (!ready)
    {
      for (size_t i = 0; i < MB_DIST_TABLE_SIZE; i++)
	names[i] = mb_dist_table[i].name;
      names[MB_DIST_TABLE_SIZE] = NULL;
      ready = 1;
    }
  return names;
}

/* ------------------------------------------------------------------ */
/* CSV parsing                                                        */
/* ------------------------------------------------------------------ */

/* Split the string at *P at the next comma; returns the field (trimmed)
   and advances *P past the separator (or to the end).  */
static char *
next_field (char **p)
{
  char *start = *p;
  char *comma = strchr (start, ',');
  if (comma != NULL)
    {
      *comma = '\0';
      *p = comma + 1;
    }
  else
    *p = start + strlen (start);

  char *end = start + strlen (start);
  while (end > start && (end[-1] == ' ' || end[-1] == '\t'
			 || end[-1] == '\r'))
    *--end = '\0';
  while (*start == ' ' || *start == '\t')
    start++;
  return start;
}

static int
is_number (const char *s)
{
  if (s == NULL || *s == '\0')
    return 0;
  char *end = NULL;
  strtod (s, &end);
  return end != s && *end == '\0';
}

/* First field of LINE is a number? (non-destructive version of
   next_field(), used for the shape detection pass).  */
static int
first_field_is_number (const char *line)
{
  char buf[64];
  size_t n = 0;
  while (line[n] != '\0' && line[n] != ',' && n < sizeof buf - 1)
    {
      buf[n] = line[n];
      n++;
    }
  buf[n] = '\0';
  char *end = buf + strlen (buf);
  while (end > buf && (end[-1] == ' ' || end[-1] == '\t'))
    *--end = '\0';
  char *start = buf;
  while (*start == ' ' || *start == '\t')
    start++;
  return is_number (start);
}

static int
count_fields (const char *line)
{
  int n = 1;
  for (const char *p = line; *p != '\0'; p++)
    if (*p == ',')
      n++;
  return n;
}

/* Grow the (sizes, weights) arrays when they are full.  */
static int
grow_arrays (size_t **sizes, double **weights, size_t *cap)
{
  size_t nc = *cap ? *cap * 2 : 1024;
  if (nc > MB_DIST_MAX_ENTRIES)
    {
      set_err ("too many entries (limit %zu)", MB_DIST_MAX_ENTRIES);
      return -1;
    }
  size_t *ns = realloc (*sizes, nc * sizeof *ns);
  if (ns == NULL)
    {
      set_err ("out of memory");
      return -1;
    }
  *sizes = ns;
  double *nw = realloc (*weights, nc * sizeof *nw);
  if (nw == NULL)
    {
      set_err ("out of memory");
      return -1;
    }
  *weights = nw;
  *cap = nc;
  return 0;
}

static int
parse_csv (const char *text, const char *name, mb_dist_t *d)
{
  char *copy = strdup (text);
  if (copy == NULL)
    {
      set_err ("out of memory");
      return -1;
    }

  size_t cap = 0, n = 0;
  size_t *sizes = NULL;
  double *weights = NULL;

  /* Decide the shape from the first data line: two numeric columns mean
     "size,weight" pairs, anything else means "one weight per field"
     with size = running index.  A leading non-numeric header line (e.g.
     "size,count") is skipped.  This pass works on a scratch copy because
     it terminates lines while scanning.  */
  char *scratch = strdup (copy);
  if (scratch == NULL)
    {
      free (copy);
      set_err ("out of memory");
      return -1;
    }
  int pair_form = -1;
  for (char *line = scratch; *line != '\0';)
    {
      char *nl = strchr (line, '\n');
      if (nl != NULL)
	*nl++ = '\0';
      char *hash = strchr (line, '#');
      if (hash != NULL)
	*hash = '\0';

      char *p = line;
      while (*p == ' ' || *p == '\t')
	p++;
      if (*p != '\0')
	{
	  if (count_fields (p) == 2 && first_field_is_number (p))
	    {
	      pair_form = 1;
	      break;
	    }
	  if (count_fields (p) == 2 && pair_form == -1)
	    {
	      /* Assume a header such as "size,count" and look at the next
		 line; a line that is not a header ends the detection.  */
	      line = nl != NULL ? nl : line + strlen (line);
	      if (nl == NULL)
		break;
	      continue;
	    }
	  pair_form = 0;
	  break;
	}
      line = nl != NULL ? nl : line + strlen (line);
      if (nl == NULL)
	break;
    }

  free (scratch);

  if (pair_form == -1)
    {
      free (copy);
      set_err ("distribution '%s' is empty", name);
      return -1;
    }

  /* Collect the entries.  */
  size_t index = 0;
  for (char *line = copy; *line != '\0';)
    {
      char *nl = strchr (line, '\n');
      if (nl != NULL)
	*nl++ = '\0';
      char *hash = strchr (line, '#');
      if (hash != NULL)
	*hash = '\0';

      char *p = line;
      while (*p == ' ' || *p == '\t')
	p++;
      if (*p == '\0')
	{
	  line = nl != NULL ? nl : line + strlen (line);
	  if (nl == NULL)
	    break;
	  continue;
	}

      if (pair_form)
	{
	  if (count_fields (p) != 2)
	    {
	      free (copy);
	      free (sizes);
	      free (weights);
	      set_err ("distribution '%s': expected 'size,weight' per line",
		       name);
	      return -1;
	    }
	  char *f = p;
	  char *first = next_field (&f);
	  char *second = next_field (&f);
	  if (!is_number (first))
	    {
	      /* Header line inside the file: ignore it.  */
	      line = nl != NULL ? nl : line + strlen (line);
	      if (nl == NULL)
		break;
	      continue;
	    }
	  if (!is_number (second))
	    {
	      free (copy);
	      free (sizes);
	      free (weights);
	      set_err ("distribution '%s': bad weight '%s'", name, second);
	      return -1;
	    }
	  if (n == cap && grow_arrays (&sizes, &weights, &cap) != 0)
	    {
	      free (copy);
	      free (sizes);
	      free (weights);
	      return -1;
	    }
	  sizes[n] = (size_t) strtoull (first, NULL, 10);
	  weights[n] = strtod (second, NULL);
	  n++;
	}
      else
	{
	  char *f = p;
	  while (*f != '\0')
	    {
	      char *field = next_field (&f);
	      double weight = 0.0;
	      if (*field != '\0')
		{
		  if (!is_number (field))
		    {
		      free (copy);
		      free (sizes);
		      free (weights);
		      set_err ("distribution '%s': bad weight '%s'",
			       name, field);
		      return -1;
		    }
		  weight = strtod (field, NULL);
		}
	      if (weight > 0.0)
		{
		  if (n == cap && grow_arrays (&sizes, &weights, &cap) != 0)
		    {
		      free (copy);
		      free (sizes);
		      free (weights);
		      return -1;
		    }
		  sizes[n] = index;
		  weights[n] = weight;
		  n++;
		}
	      index++;
	    }
	}

      line = nl != NULL ? nl : line + strlen (line);
      if (nl == NULL)
	break;
    }
  free (copy);

  if (getenv ("MB_DEBUG_DIST") != NULL)
    {
      fprintf (stderr, "[dbg] parsed %zu entries (pair_form=%d):", n,
	       pair_form);
      for (size_t i = 0; i < n && i < 5; i++)
	fprintf (stderr, " %zu=%.4g", sizes[i], weights[i]);
      fputc ('\n', stderr);
    }

  if (n == 0)
    {
      free (sizes);
      free (weights);
      set_err ("distribution '%s' has no non-zero weights", name);
      return -1;
    }

  /* Normalize and accumulate.  */
  double total = 0.0;
  for (size_t i = 0; i < n; i++)
    total += weights[i];
  if (total <= 0.0)
    {
      free (sizes);
      free (weights);
      set_err ("distribution '%s': weights sum to zero", name);
      return -1;
    }

  double *cum = malloc (n * sizeof *cum);
  if (cum == NULL)
    {
      free (sizes);
      free (weights);
      set_err ("out of memory");
      return -1;
    }
  double acc = 0.0;
  for (size_t i = 0; i < n; i++)
    {
      acc += weights[i] / total;
      cum[i] = acc;
    }
  cum[n - 1] = 1.0;
  free (weights);

  d->sizes = sizes;
  d->cum = cum;
  d->n = n;
  snprintf (d->name, sizeof d->name, "%s", name);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Loading                                                            */
/* ------------------------------------------------------------------ */

static char *
read_file (const char *path)
{
  FILE *fp = fopen (path, "r");
  if (fp == NULL)
    return NULL;

  size_t cap = 8192, n = 0;
  char *buf = malloc (cap);
  if (buf == NULL)
    {
      fclose (fp);
      return NULL;
    }
  for (;;)
    {
      if (n + 1 == cap)
	{
	  cap *= 2;
	  char *nb = realloc (buf, cap);
	  if (nb == NULL)
	    {
	      free (buf);
	      fclose (fp);
	      return NULL;
	    }
	  buf = nb;
	}
      size_t got = fread (buf + n, 1, cap - n - 1, fp);
      n += got;
      if (got == 0)
	break;
    }
  fclose (fp);
  buf[n] = '\0';
  return buf;
}

int
mb_dist_load (const char *name_or_path, mb_dist_t *d)
{
  size_t len = strlen (name_or_path);
  int is_path = strchr (name_or_path, '/') != NULL
		|| (len > 4 && strcmp (name_or_path + len - 4, ".csv") == 0);

  char *text = NULL;
  const char *csv = NULL;
  char label[64];

  if (is_path)
    {
      text = read_file (name_or_path);
      if (text == NULL)
	{
	  set_err ("cannot read the distribution file '%s'", name_or_path);
	  return -1;
	}
      csv = text;
      const char *base = strrchr (name_or_path, '/');
      snprintf (label, sizeof label, "%s",
		base != NULL ? base + 1 : name_or_path);
      char *dot = strrchr (label, '.');
      if (dot != NULL)
	*dot = '\0';
    }
  else
    {
      csv = find_embedded (name_or_path);
      if (csv == NULL)
	{
	  set_err ("unknown distribution '%s' (see --list-dists)",
		   name_or_path);
	  return -1;
	}
      snprintf (label, sizeof label, "%s", name_or_path);
    }

  int rc = parse_csv (csv, label, d);
  free (text);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Sampling                                                           */
/* ------------------------------------------------------------------ */

size_t
mb_dist_sample (const mb_dist_t *d, uint64_t *state)
{
  if (d->n == 0)
    return 0;
  if (d->n == 1)
    return d->sizes[0];

  double u = (double) (mb_rng_next (state) >> 11)
	     * (1.0 / 9007199254740992.0);	/* [0, 1) */
  size_t lo = 0, hi = d->n - 1;
  while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      if (d->cum[mid] <= u)
	lo = mid + 1;
      else
	hi = mid;
    }
  return d->sizes[lo];
}

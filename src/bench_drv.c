/* Shared driver runtime for the memlib benchmark suite (implementation).
   See bench_drv.h. */

#include "bench_drv.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

void
mb_opts_defaults (mb_opts_t *o)
{
  memset (o, 0, sizeof *o);
  o->budget_mib = MB_DEF_BUDGET_MIB;
  o->min_iters = MB_DEF_MIN_ITERS;
  o->max_iters = MB_DEF_MAX_ITERS;
  o->seed = 0x1234abcd;
}

static const char *
next_arg (int argc, char **argv, int *i, const char *opt, const char *inlinev)
{
  if (inlinev != NULL)
    return inlinev;
  if (*i + 1 < argc)
    return argv[++*i];
  fprintf (stderr, "option %s requires an argument\n", opt);
  exit (2);
}

static long
parse_long (const char *opt, const char *v)
{
  char *end = NULL;
  long r = strtol (v, &end, 0);
  if (end == v || *end != '\0')
    {
      fprintf (stderr, "invalid numeric value for %s: %s\n", opt, v);
      exit (2);
    }
  return r;
}

static double
parse_double (const char *opt, const char *v)
{
  char *end = NULL;
  double r = strtod (v, &end);
  if (end == v || *end != '\0')
    {
      fprintf (stderr, "invalid numeric value for %s: %s\n", opt, v);
      exit (2);
    }
  return r;
}

/* Split "PATH[=LABEL]" into caller-provided path/label buffers.  Each
   --impl keeps its own storage, so repeating the option works.  */
static void
parse_impl_spec (const char *spec, char *pbuf, size_t psize,
		 char *lbuf, size_t lsize)
{
  const char *eq = strrchr (spec, '=');
  size_t plen = eq != NULL ? (size_t) (eq - spec) : strlen (spec);

  if (plen == 0 || plen >= psize)
    {
      fprintf (stderr, "invalid --impl specification: %s\n", spec);
      exit (2);
    }
  memcpy (pbuf, spec, plen);
  pbuf[plen] = '\0';

  if (eq != NULL && eq[1] != '\0')
    {
      snprintf (lbuf, lsize, "%s", eq + 1);
      return;
    }

  /* Default label: basename without extension.  */
  const char *b = strrchr (pbuf, '/');
  b = (b == NULL) ? pbuf : b + 1;
  size_t blen = strlen (b);
  if (blen >= lsize)
    blen = lsize - 1;
  memcpy (lbuf, b, blen);
  lbuf[blen] = '\0';
  char *dot = strrchr (lbuf, '.');
  if (dot != NULL)
    *dot = '\0';
}

int
mb_opts_parse (mb_opts_t *o, int argc, char **argv, const char *func_symbol,
	       const char *usage_extra)
{
  const char *av0 = argv[0];

  for (int i = 1; i < argc; i++)
    {
      const char *a = argv[i];
      const char *v = NULL;	/* inline value after '=' if any	*/

      if (strcmp (a, "-h") == 0 || strcmp (a, "--help") == 0)
	{
	  mb_opts_usage (av0, func_symbol, usage_extra, stdout);
	  exit (0);
	}
      if (strncmp (a, "--", 2) == 0)
	{
	  const char *eq = strchr (a + 2, '=');
	  size_t klen = eq != NULL ? (size_t) (eq - (a + 2))
				   : strlen (a + 2);
	  char key[64];
	  if (klen >= sizeof key)
	    klen = sizeof key - 1;
	  memcpy (key, a + 2, klen);
	  key[klen] = '\0';
	  if (eq != NULL)
	    v = eq + 1;

	  if (strcmp (key, "check") == 0)
	    o->check = 1;
	  else if (strcmp (key, "no-libc") == 0)
	    o->no_libc = 1;
	  else if (strcmp (key, "generic") == 0)
	    o->generic = 1;
	  else if (strcmp (key, "no-generic") == 0)
	    ; /* accepted for compatibility; generic is off by default */
	  else if (strcmp (key, "quick") == 0)
	    o->quick = 1;
	  else if (strcmp (key, "iters") == 0)
	    o->fixed_iters = parse_long ("--iters", next_arg (argc, argv, &i,
							      "--iters", v));
	  else if (strcmp (key, "budget") == 0)
	    o->budget_mib = parse_double ("--budget",
					  next_arg (argc, argv, &i, "--budget",
						    v));
	  else if (strcmp (key, "min-iters") == 0)
	    o->min_iters = (size_t) parse_long ("--min-iters",
						next_arg (argc, argv, &i,
							  "--min-iters", v));
	  else if (strcmp (key, "max-len") == 0)
	    o->max_len = (size_t) parse_long ("--max-len",
					      next_arg (argc, argv, &i,
							"--max-len", v));
	  else if (strcmp (key, "seed") == 0)
	    o->seed = (unsigned long) parse_long ("--seed",
						  next_arg (argc, argv, &i,
							    "--seed", v));
	  else if (strcmp (key, "matrix") == 0)
	    o->matrix = next_arg (argc, argv, &i, "--matrix", v);
	  else if (strcmp (key, "impl") == 0)
	    {
	      const char *spec = next_arg (argc, argv, &i, "--impl", v);
	      if (o->impl_count >= 32)
		{
		  fprintf (stderr, "too many --impl options (max 32)\n");
		  exit (2);
		}
	      parse_impl_spec (spec, o->impl_paths[o->impl_count],
			       sizeof o->impl_paths[0],
			       o->impl_labels[o->impl_count],
			       sizeof o->impl_labels[0]);
	      o->impl_count++;
	    }
	  else
	    {
	      fprintf (stderr, "unknown option: %s\n", a);
	      mb_opts_usage (av0, func_symbol, usage_extra, stderr);
	      exit (2);
	    }
	}
      else
	{
	  fprintf (stderr, "unexpected argument: %s\n", a);
	  mb_opts_usage (av0, func_symbol, usage_extra, stderr);
	  exit (2);
	}
    }

  if (o->quick)
    {
      if (o->fixed_iters == 0)
	{
	  o->budget_mib = 1.0;
	  o->min_iters = 4;
	  o->max_iters = 1u << 17;
	}
      if (o->max_len == 0 || o->max_len > (1u << 16))
	o->max_len = o->max_len == 0 ? (1u << 16) : o->max_len;
    }

  if (o->fixed_iters > 0)
    {
      /* Fixed iteration count mode: ignore the adaptive budget.  */
      o->budget_mib = 0.0;
    }
  return 0;
}

void
mb_opts_usage (const char *argv0, const char *func_symbol,
	       const char *usage_extra, FILE *out)
{
  fprintf (out,
    "Usage: %s [options]\n"
    "\n"
    "Benchmark implementations of %s.  Measures average nanoseconds per\n"
    "call over the glibc benchtests alignment/length matrices and prints\n"
    "the result as a glibc-compatible JSON document on stdout (unless\n"
    "--check is given, in which case correctness is verified instead).\n"
    "\n"
    "Options:\n"
    "  -h, --help                 show this help\n"
    "      --impl PATH[=LABEL]    dlopen a shared object exporting the\n"
    "                             symbol \"%s\" and benchmark it under\n"
    "                             LABEL (default: file name).  Repeatable.\n"
    "      --no-libc              do not include the libc baseline\n"
    "      --generic             also include the generic C baseline\n"
    "      --check                correctness mode (no JSON output)\n"
    "      --iters N              fixed iterations per test\n"
    "      --budget MB            adaptive: MiB touched per (impl, test)\n"
    "                             (default: %.0f)\n"
    "      --quick                short run (reduced budget + max length)\n"
    "      --max-len N            cap tested lengths at N bytes\n"
    "      --matrix FILE          use a matrix profile file (sizes and\n"
    "                             offsets) instead of the built-in matrix\n"
    "      --seed N               pattern seed for --check\n"
    "%s",
    argv0, func_symbol, func_symbol, MB_DEF_BUDGET_MIB,
    usage_extra != NULL ? usage_extra : "");
}

/* ------------------------------------------------------------------ */
/* Implementation registration                                         */
/* ------------------------------------------------------------------ */

int
mb_register_impls (const mb_opts_t *o, const char *func_symbol,
		   mb_fn_t generic_fn)
{
  if (!o->no_libc)
    {
      if (mb_impl_add_libc (func_symbol) != 0)
	{
	  fprintf (stderr, "error: cannot add libc baseline for %s: %s\n",
		   func_symbol, mb_err ());
	  return -1;
	}
    }
  if (o->generic)
    {
      if (mb_impl_add ("generic", generic_fn, MB_KIND_GENERIC) != 0)
	{
	  fprintf (stderr, "error: cannot add generic baseline: %s\n",
		   mb_err ());
	  return -1;
	}
    }
  for (int i = 0; i < o->impl_count; i++)
    {
      int rc = mb_impl_add_dlopen (o->impl_paths[i], func_symbol,
				   o->impl_labels[i]);
      if (rc == 0)
	continue;
      /* Missing symbol: not an error if we already have baselines.  */
      fprintf (stderr,
	       "warning: %s does not provide \"%s\"; skipping it "
	       "for this function (%s)\n",
	       o->impl_paths[i], func_symbol, mb_err ());
    }
  if (mb_impl_count () == 0)
    {
      fprintf (stderr,
	       "error: no implementations to test (use --no-libc only "
	       "together with --impl)\n");
      return -1;
    }
  return mb_impl_count ();
}

/* ------------------------------------------------------------------ */
/* Iterations / measurement helpers                                    */
/* ------------------------------------------------------------------ */

size_t
mb_pick_iters (const mb_opts_t *o, size_t bytes)
{
  if (o->fixed_iters > 0)
    return (size_t) o->fixed_iters;

  double budget = o->budget_mib * 1048576.0;
  double want = budget / (double) (bytes == 0 ? 1 : bytes);
  if (want < (double) o->min_iters)
    return o->min_iters;
  if (want > (double) o->max_iters)
    return o->max_iters;
  return (size_t) want;
}

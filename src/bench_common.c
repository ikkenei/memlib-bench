/* Common infrastructure for the memlib benchmark suite (implementation).

   See bench_common.h for a description.  This file is self-contained and
   only depends on POSIX (mmap, dlfcn) and libc.
 */

#define _GNU_SOURCE 1		/* dladdr, Dl_info */

#include "bench_common.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Implementation registry                                             */
/* ------------------------------------------------------------------ */

#define MB_MAX_IMPLS 64

static mb_impl_t impls[MB_MAX_IMPLS];
static int impl_count;
static char errbuf[512];

int
mb_impl_count (void)
{
  return impl_count;
}

const mb_impl_t *
mb_impl_get (int idx)
{
  return (idx >= 0 && idx < impl_count) ? &impls[idx] : NULL;
}

int
mb_impl_add (const char *name, mb_fn_t fn, int kind)
{
  if (impl_count >= MB_MAX_IMPLS)
    {
      snprintf (errbuf, sizeof errbuf, "too many implementations "
		"(limit %d)", MB_MAX_IMPLS);
      return -1;
    }
  if (name == NULL || fn == NULL)
    {
      snprintf (errbuf, sizeof errbuf, "null implementation name/fn");
      return -1;
    }
  impls[impl_count].name = name;
  impls[impl_count].fn = fn;
  impls[impl_count].kind = kind;
  impl_count++;
  return 0;
}

int
mb_impl_add_libc (const char *symbol)
{
  /* Resolve through the global scope: this returns libc's own
     implementation (through the IFUNC resolver on glibc).  We never call
     the measured functions by name in the driver, only through these
     pointers, so the compiler cannot rewrite the calls.  */
  void *fn = dlsym (RTLD_DEFAULT, symbol);
  if (fn == NULL)
    {
      snprintf (errbuf, sizeof errbuf, "dlsym(RTLD_DEFAULT, \"%s\"): %s",
		symbol, dlerror ());
      return -1;
    }
  return mb_impl_add ("libc", (mb_fn_t) fn, MB_KIND_LIBC);
}

int
mb_impl_add_dlopen (const char *path, const char *symbol,
		    const char *label)
{
  void *h = dlopen (path, RTLD_NOW | RTLD_LOCAL);
  if (h == NULL)
    {
      snprintf (errbuf, sizeof errbuf, "dlopen(\"%s\"): %s", path,
		dlerror ());
      return -1;
    }
  dlerror ();	/* clear */
  void *fn = dlsym (h, symbol);
  const char *e = dlerror ();
  if (e != NULL)
    {
      snprintf (errbuf, sizeof errbuf,
		"dlsym(%s, \"%s\"): %s", path, symbol, e);
      return -1;
    }
  /* dlsym() on a library handle also searches the dependencies of that
     library, so a shared object that does not implement the symbol at all
     would silently report libc's implementation under its own label.
     Require the symbol to be defined by the object we opened.  */
  Dl_info info;
  if (dladdr (fn, &info) != 0 && info.dli_fname != NULL)
    {
      char want[PATH_MAX], got[PATH_MAX];
      const char *w = realpath (path, want);
      const char *g = realpath (info.dli_fname, got);
      if (w != NULL && g != NULL && strcmp (w, g) != 0)
	{
	  snprintf (errbuf, sizeof errbuf,
		    "%s does not define \"%s\" (it resolves to %s)",
		    path, symbol, info.dli_fname);
	  dlclose (h);
	  return -1;
	}
    }
  return mb_impl_add (label, (mb_fn_t) fn, MB_KIND_CUSTOM);
}

const char *
mb_err (void)
{
  return errbuf;
}

/* ------------------------------------------------------------------ */
/* Buffers with guard pages                                            */
/* ------------------------------------------------------------------ */

mb_buf_t mb_buf1, mb_buf2;
size_t mb_page_size;

static void
buf_free (mb_buf_t *b)
{
  if (b->base != NULL)
    munmap (b->base, b->size + mb_page_size);
  b->base = NULL;
  b->size = 0;
}

static int
buf_alloc (mb_buf_t *b, size_t size)
{
  size_t alloc_size = size + mb_page_size;	/* guard page	*/
  unsigned char *p = mmap (NULL, alloc_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return -1;
  if (mprotect (p + size, mb_page_size, PROT_NONE) != 0)
    {
      munmap (p, alloc_size);
      return -1;
    }
  b->base = p;
  b->size = size;
  return 0;
}

size_t
mb_buffers_init (size_t min_page, size_t max_len)
{
  size_t os_page = (size_t) sysconf (_SC_PAGESIZE);
  if (os_page == (size_t) -1)
    os_page = 4096;

  /* Need room for MAX_LEN plus slack for alignment tricks (up to a page
     of headroom on each side when the tested region sits near the end). */
  size_t need = max_len + os_page * 2;
  mb_page_size = min_page;
  if (mb_page_size < need)
    mb_page_size = need;
  mb_page_size = (mb_page_size + os_page - 1) & ~(os_page - 1);

  mb_buffers_free ();
  if (buf_alloc (&mb_buf1, mb_page_size) != 0)
    return 0;
  if (buf_alloc (&mb_buf2, mb_page_size) != 0)
    {
      buf_free (&mb_buf1);
      return 0;
    }
  return mb_page_size;
}

void
mb_buffers_free (void)
{
  buf_free (&mb_buf1);
  buf_free (&mb_buf2);
}

/* ------------------------------------------------------------------ */
/* Misc helpers                                                        */
/* ------------------------------------------------------------------ */

void
mb_warmup (long iters)
{
  static volatile unsigned int acc = 0;
  if (iters <= 0)
    iters = 100000000L;
  for (long k = 0; k < iters; k++)
    acc += 23u * acc + 2u;
}

void
mb_fill (unsigned char *p, size_t n, uint64_t seed)
{
  uint64_t x = seed ? seed : UINT64_C (0x9e3779b97f4a7c15);
  for (size_t i = 0; i < n; i++)
    {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      p[i] = (unsigned char) x;
    }
}

const char *mb_current_impl = NULL;

static void
crash_handler (int sig, siginfo_t *si, void *uc)
{
  (void) uc;
  char msg[256];
  int n = 0;
  const char *impl = mb_current_impl ? mb_current_impl : "?";
  n += snprintf (msg + n, sizeof msg - (size_t) n,
		 "\n[memlib] %s faulted (signal %d) at address %p "
		 "during %s\n",
		 sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", sig,
		 si->si_addr, impl);
  /* Async-signal-safe fallback if snprintf misbehaved.  */
  write (2, msg, (size_t) (n > 0 ? n : 0));
  _exit (4);
}

void
mb_install_crash_reporter (void)
{
  struct sigaction sa;
  memset (&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction (SIGSEGV, &sa, NULL);
  sigaction (SIGBUS, &sa, NULL);
}

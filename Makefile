# memlib_bench — standalone microbenchmarks for memcpy / memmove /
# memset / memcmp, following glibc benchtests methodology.
#
# Usage:
#   make                 build benchmark drivers + implementation .so files
#   make drivers         build only the four benchmark drivers
#   make impls           build user implementations in impls/ into .so files
#   make clean
#
# Supported targets: x86-64 Linux and aarch64 Linux.  A plain `make` builds
# natively on both; CROSS is only needed to build for a different machine:
#   make CROSS=aarch64-linux-gnu-            # aarch64 from x86 host
#   make CROSS=aarch64-linux-gnu- MB_ARCH=-march=armv8-a
#   make MB_ARCH=-march=native               # native build, tune for this CPU
#
# Per-implementation compile flags (e.g. for SVE):
#   make impls CFLAGS_myimpl=-O3 -march=armv8.2-a+sve    (see below)
#
# The CLI wrapper is `./mb` (build/list/run/check/plot) and drives these
# binaries.

CROSS       ?=
CC          := $(CROSS)gcc
CFLAGS      ?= -O2 -g
MB_ARCH     ?=
WARN         = -Wall -Wextra
STD          = -std=gnu11
INCLUDES     = -Isrc -I$(BUILD)/gen

# glibc default matrices are compiled into the drivers from the .txt files.
MATRIX_SMALL := matrices/glibc_small.txt
MATRIX_LARGE := matrices/glibc_large.txt
MATRIX_HDR   = $(BUILD)/gen/matrix_glibc.h

# ------------------------------------------------------------------
# Layout
# ------------------------------------------------------------------
BUILD       := build
OBJDIR      := $(BUILD)/obj
IMPLDIR     := $(BUILD)/impls
RESULTS     := results

BENCH_BINS  := $(BUILD)/bench_memcpy $(BUILD)/bench_memmove \
               $(BUILD)/bench_memset $(BUILD)/bench_memcmp

COMMON_SRCS := src/json-lib.c src/bench_common.c src/bench_drv.c \
               src/generic_ref.c src/check.c src/matrix.c
COMMON_OBJS := $(COMMON_SRCS:src/%.c=$(OBJDIR)/%.o)

# User implementations: every .c in impls/ is built into its own .so
# exporting the standard symbols memcpy/memmove/memset/memcmp (subset ok).
IMPL_CS    := $(wildcard impls/*.c)

# Assembly implementations: the bundled examples are arch-specific and
# build only when the toolchain targets that architecture.  Any other .S
# file in impls/ is always built (it is the user's responsibility to match
# the target).
TARGET_MACH := $(shell $(CC) -dumpmachine 2>/dev/null)
IMPL_ASMS   := $(wildcard impls/*.S)
ifneq ($(findstring aarch64,$(TARGET_MACH)),aarch64)
IMPL_ASMS := $(filter-out impls/example_neon.S,$(IMPL_ASMS))
endif
ifneq ($(findstring x86_64,$(TARGET_MACH)),x86_64)
IMPL_ASMS := $(filter-out impls/example_sse2.S,$(IMPL_ASMS))
endif

IMPL_SOS   := $(patsubst impls/%.c,$(IMPLDIR)/%.so,$(IMPL_CS)) \
              $(patsubst impls/%.S,$(IMPLDIR)/%.so,$(IMPL_ASMS))

# ------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------
.PHONY: all drivers impls clean

all: drivers impls

drivers: $(BENCH_BINS)

impls: $(IMPL_SOS)

clean:
	rm -rf $(BUILD)

$(OBJDIR) $(IMPLDIR) $(RESULTS) $(BUILD)/gen:
	mkdir -p $@

# One C string per line; the drivers parse them like a --matrix file.
$(MATRIX_HDR): $(MATRIX_SMALL) $(MATRIX_LARGE) | $(BUILD)/gen
	{ echo 'static const char mb_glibc_small_text[] ='; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $(MATRIX_SMALL); \
	  echo ';'; \
	  echo 'static const char mb_glibc_large_text[] ='; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $(MATRIX_LARGE); \
	  echo ';'; \
	} > $@

# ------------------------------------------------------------------
# Benchmark drivers
# ------------------------------------------------------------------
$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(WARN) $(STD) $(MB_ARCH) $(INCLUDES) -c -o $@ $<

$(OBJDIR)/bench_drv.o: $(MATRIX_HDR)

$(BUILD)/bench_memcpy: src/bench_memcpy.c $(COMMON_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(STD) $(MB_ARCH) $(INCLUDES) -fno-builtin \
	    -o $@ $< $(COMMON_OBJS) -ldl

$(BUILD)/bench_memmove: src/bench_memmove.c $(COMMON_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(STD) $(MB_ARCH) $(INCLUDES) -fno-builtin \
	    -o $@ $< $(COMMON_OBJS) -ldl

$(BUILD)/bench_memset: src/bench_memset.c $(COMMON_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(STD) $(MB_ARCH) $(INCLUDES) -fno-builtin \
	    -o $@ $< $(COMMON_OBJS) -ldl

$(BUILD)/bench_memcmp: src/bench_memcmp.c $(COMMON_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(STD) $(MB_ARCH) $(INCLUDES) -fno-builtin \
	    -o $@ $< $(COMMON_OBJS) -ldl

# ------------------------------------------------------------------
# User implementations -> shared objects
#
# Per-file flags:  make impls "CFLAGS_myimpl=-march=armv8.2-a+sve"
# names the flag variable after the base name of impls/myimpl.c.
# ------------------------------------------------------------------
$(IMPLDIR)/%.so: impls/%.c | $(IMPLDIR)
	$(CC) $(CFLAGS) $(WARN) $(STD) $(MB_ARCH) \
	    $(CFLAGS_$(basename $(notdir $@))) \
	    -fno-builtin -O3 -fPIC -shared -o $@ $<

$(IMPLDIR)/%.so: impls/%.S | $(IMPLDIR)
	$(CC) $(CFLAGS) $(MB_ARCH) $(CFLAGS_$(basename $(notdir $@))) \
	    -shared -fPIC -o $@ $<

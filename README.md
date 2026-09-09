# memlib_bench

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL%20v2.1-blue.svg)](https://www.gnu.org/licenses/lgpl-2.1)
[![Platform](https://img.shields.io/badge/platform-aarch64--linux-lightgrey)](https://github.com/ikkenei/memlib-bench)

**Standalone microbenchmarks for `memcpy`, `memmove`, `memset` and `memcmp`** — built
on the glibc benchtests methodology (`benchtests/bench-mem*.c`) — with a small CLI to
benchmark **your own** implementations of these functions on real hardware.

- Measure average **nanoseconds per call** over glibc-style size/alignment matrices
  (including memmove overlap and both copy directions), plus an optional "large" mode.
- Compare in a single run: the system **libc** implementation (via `dlsym`; on aarch64
  this is the regular IFUNC variant), an optional portable **generic C** reference
  (`--generic`), and any number of **your** implementations — no rebuilds needed.
- **Verify correctness** (`mb check`): boundary lengths, alignments, memmove overlap in
  both directions; reads/writes past the end fault into a guard page and the offending
  implementation is reported by name.
- Emit **glibc-compatible JSON** (benchout schema) and render comparison tables
  (`mb run`, `--table full`) or **GB/s throughput graphs** (`tools/plot_mem.py`).

The project is self-contained: it needs no glibc build system, and the pieces taken
from glibc (see [License & provenance](#license--provenance)) were vendored because
they are standalone.

> This project was vibe-coded with the DeepSeek-V4-Flash model running in the
> **pi** coding agent harness. The code has been reviewed and tested; the note
> above is for full transparency.

---

## Features

- ns-per-call measurements over the glibc test matrices: powers of two, boundary and
  misaligned sizes, page-edge alignments; for `memcpy` both directions (`dst > src`);
  for `memmove` genuine overlap in one buffer; for `memset` several fill values; for
  `memcmp` equal and differing buffers.
- Multiple implementations in one run: system libc, `--generic` C reference and any
  number of `impls/` shared objects.
- `--check` correctness mode against an obviously-correct byte-wise oracle.
- JSON output compatible with `benchout_strings.schema.json`, so results can also be
  analyzed with the original glibc tooling (`tools/compare_strings.py`).
- Configurable test matrix: sizes and offsets can live in a plain-text **profile
  file** (`--matrix`), no recompilation needed.
- `tools/plot_mem.py`: GB/s vs size plots, one figure per alignment geometry, all
  implementations overlaid, L1-cache marker line.

## Platform

Primary target: **aarch64**. Timing uses the system counter `CNTVCT_EL0`/`CNTFRQ_EL0`
(as glibc's `sysdeps/aarch64/hp-timing.h`), so measurements are insensitive to CPU
frequency scaling. On other architectures a `clock_gettime(CLOCK_MONOTONIC)` fallback
(vDSO) is used. All values are reported in nanoseconds per operation.

## Layout

```
Makefile             build drivers and implementations
mb                   CLI (Python 3.6+, stdlib only; optional jsonschema/matplotlib)
LICENSE              MIT license for the original code
src/                 framework and drivers
  bench_common.[ch]    guard-page buffers, implementation registry, crash reporter
  bench_drv.[ch]       option parsing, adaptive iteration policy
  timing.h             timer (cntvct_el0 on aarch64, clock_gettime elsewhere)
  check.[ch]           correctness engine (byte-wise oracle)
  generic_ref.[ch]     generic C reference + oracle functions
  matrix.[ch]          matrix-profile parser
  json-lib.[ch]        (vendored from glibc) JSON writer, glibc format
  bench_memcpy.c / bench_memmove.c / bench_memset.c / bench_memcmp.c
impls/               put your implementations here (see below)
tools/               (vendored from glibc benchtests/scripts)
  compare_strings.py, plot_strings.py, benchout_strings.schema.json, ...
  plot_mem.py        GB/s throughput graphs for this project
matrices/            example matrix profiles
build/               build output (created by make)
results/             run JSON files (created by mb)
plots/               graphs (created by plot_mem.py)
```

## Quick start

```sh
make                 # native build
# cross build for aarch64:
#   make CROSS=aarch64-linux-gnu-
#   make CROSS=aarch64-linux-gnu- MB_ARCH="-march=armv8.2-a+sve"

./mb list

./mb check memcpy                  # correctness of libc (default baseline)
./mb run  memcpy --quick           # short benchmark vs libc

./mb run memcpy --generic          # also add the portable generic C reference
./mb run memcpy memmove --impl example_c
./mb check all --impl example_c    # all four functions, all implementations
```

A full single-function run (without `--quick`) takes about a minute and prints a
median-per-length table plus a geo-mean row. Raw JSON is also saved:

```sh
./mb run memcmp --impl example_c -o res.json
./mb run --table full --gmean -b libc memcpy --impl example_c   # detailed glibc-style table

# throughput graphs (GB/s), own script tools/plot_mem.py
python3 tools/plot_mem.py results/latest.json -o plots
```

`mb run` without function names runs all four functions. `mb memcpy --impl x` is
shorthand for `mb run memcpy --impl x`.

## Your implementations (`impls/`)

Drop a `.c` file (or `.S` for aarch64) into `impls/` that exports the standard
symbols. `make` builds each file into its own `build/impls/<name>.so`; the CLI
picks it up by name:

```sh
# impls/my_neon.c / my_wide.S  ->  build/impls/my_neon.so ...
./mb run memcpy my_neon my_wide
./mb run all --impl build/impls/my_neon.so /abs/path/impl.so
```

Rules:

- An implementation does not have to provide all four functions — missing symbols are
  skipped with a warning for the affected function.
- Symbols must have the standard signatures and default visibility
  (`memcpy`, `memmove`, `memset`, `memcmp`).
- Libraries are loaded with `dlopen(RTLD_LOCAL)`, so they cannot interpose on the
  driver itself.
- Assembly `.S` files are built only when the toolchain targets aarch64.
- Per-file compile flags: `make CFLAGS_myimpl="-march=armv8.2-a+sve"` (variable named
  after the file base name). Ready-made examples: `example_c.c` (portable C) and
  `example_neon.S` (aarch64 NEON) in `impls/`.

## Baselines

By default the only baseline is the system **libc**. The portable generic C reference
(a deterministic "low bar" that does not depend on the libc version) is opt-in:

| Flag | Meaning |
|---|---|
| `--no-libc` | drop the libc baseline (on by default) |
| `--generic` | include the generic C reference baseline (off by default) |

`--no-generic` is still accepted as a no-op for compatibility. The percentage base in
tables defaults to `libc` (then the first implementation); change it with `-b/--base`.

## Driver options (`mb run|check ...`)

| Option | Meaning |
|---|---|
| `--impl NAME\|PATH` | implementation to test (repeatable; `PATH=LABEL` for a custom name) |
| `--no-libc` / `--generic` | baseline selection (see above) |
| `--check` | correctness mode (`mb check`) |
| `--quick` | short run: ~1 MiB budget/test, max length 64 KiB |
| `--iters N` | fixed iterations per test (instead of adaptive) |
| `--budget MB` | adaptive budget, MiB per (impl, test); default 16 |
| `--max-len N` | cap tested lengths; above 64 KiB the "large" cases are added |
| `--matrix FILE` | run the sizes/offsets from a matrix profile (below) |
| `--seed N` | pattern seed for `--check` |
| `--no-warmup` | skip the CPU frequency ramp-up loop |

## Matrix profiles (`--matrix FILE`)

Instead of the built-in matrix, sizes and offsets can be configured with a plain-text
file containing one section per function:

```sh
./mb run memcpy memmove --matrix matrices/example.txt
```

Format (full example: `matrices/example.txt`):

```
[memcpy]          # section named after the function
sizes = 1..16384 24576..98304:8192 131072   # lengths in bytes
src   = 0 1 2 3 7 15 31 63                  # source offsets
 dst  = 0 1 3 7 15 31
both  = 1
```

Syntax and semantics:

- Values are separated by spaces or commas; lists can continue on the next lines;
  comments start with `#` or `;`.
- List elements: plain numbers (hex allowed, `0x..`), powers of two `A..B`, or a
  linear range `A..B:STEP`. Negative numbers are allowed only for `fill`/`diff`.
- Keys per function:
  - `memcpy`, `memcmp`: `sizes`, `src`, `dst`, `both`;
  - `memmove`: same keys — all offsets live in one buffer, so pairs overlap; when the
    `src` and `dst` lists are identical every unordered pair is run in both directions,
    otherwise directed `src→dst` pairs are run and `both = 1` adds the reverse;
  - `memset`: `sizes`, `align`, `fill` (byte values, e.g. `0 0xff -1`);
  - `memcmp`: `sizes`, `src`, `dst`, `diff` (`0` = equal buffers, `1`/`-1` = differ at
    the last byte; attribute `result`).
- `sizes` is required; a missing section, an unknown key or a bad value is an error
  with a line number (no silent fallbacks).
- Buffers grow automatically to the largest size in the file; `--max-len`/`--quick`
  still act as an upper bound on lengths.

## Editing the built-in matrix

Without a profile file the built-in glibc matrices are used; they live in the driver
sources, just like the original glibc benchtests:

| File | Matrix (in `main`) | Alignment mask |
|---|---|---|
| `src/bench_memcpy.c` | powers of two, small sizes, `16*i`, `32*i`, `2048+64*i` | `& (real_page-1)` |
| `src/bench_memmove.c` | same + fixed `(0,32)` pairs | `& (real_page-1)` |
| `src/bench_memset.c` | powers of two, `i`, `4096-i`, `32*i`, + a loop over `c` | `& 4095` |
| `src/bench_memcmp.c` | `i`, `2<<i`, `16<<i`, `8<<i` | `& 4095` |

Each case is a `do_test(&json_ctx, align1, align2, len, both)` call: sizes are the
`len` argument, offsets the first arguments, `both` enables both copy directions.
Buffers (`MIN_PAGE_SIZE` = 131072 at the top of each file) grow automatically with
`--max-len`, so for large lengths prefer `--max-len` or a matrix profile instead of
code edits.

## Correctness check (`mb check`)

Each registered implementation is compared with a byte-wise oracle on adversarial
cases. The implementation runs in one arena, the oracle in an identical twin, and the
whole arenas are compared afterwards — catching wrong results as well as writes
outside `[dst, dst+len)`. Cases are also placed at the end of the mapping so that
reads/writes past `len` hit the guard page; the crash reporter prints the
implementation name and address. `memcmp` is checked by result sign on equal and
differing buffers (including a difference in the last byte). All registered
implementations are checked (by default libc plus whatever `--impl` you pass), which
additionally validates the oracle itself.

## Throughput graphs (`tools/plot_mem.py`)

Reads a benchout JSON file and plots **GB/s = bytes/ns** vs size:

```sh
python3 tools/plot_mem.py results/latest.json -o plots            # all functions
python3 tools/plot_mem.py res.json --func memcpy --func memset
python3 tools/plot_mem.py res.json --func memcpy --match 'align1=0' \
        --cache-size 65536 -o plots/memcpy-a0
```

- Multiple implementations on one figure, one colored curve each, shared legend.
- One figure per geometry: `(align1, align2[, direction])` for the copy functions and
  `memcmp`; `(alignment, fill byte)` for `memset`.
- A vertical dashed line marks the L1 cache size (default 64 KiB, `--cache-size N`,
  `0` disables) when the size range covers it.
- The x axis becomes logarithmic automatically when the range spans more than a decade
  (`--xlog`/`--xlinear` force it).
- Options: `--func`, `--match`, `--cache-size`, `--fmt png|pdf|svg`, `--dpi`,
  `--xlog`/`--xlinear`, `--max-figs N`. Only requires `matplotlib`; Python >= 3.6.

## Methodology (glibc benchtests)

- Buffers are `mmap`ed with a `PROT_NONE` **guard page** at the end — going out of
  bounds during a measurement or a check faults immediately.
- Test matrices follow `bench-memcpy/memmove/memset/memcmp.c`: powers of two and
  `2^k±…`, small sizes across all alignments (`align1`/`align2` 0..page), page-edge
  tests; both directions for copies; genuine overlap for `memmove`; several fill
  values for `memset`; equal/differing buffers for `memcmp`.
- Iterations are adaptive: every measurement touches roughly `--budget` bytes, giving
  stable numbers both for tiny sizes (many iterations) and large ones (bounded run
  time) — unlike glibc's fixed `INNER_LOOP_ITERS*`. `--iters` restores fixed counts.
- Each call's time = (counter delta around the whole loop)/iterations; before measuring
  a warm-up loop lets the CPU reach its steady-state frequency (like glibc's
  `bench_start`).
- JSON output matches `benchout_strings.schema.json`, so files can be analyzed with the
  glibc tools directly:
  ```sh
  python3 tools/compare_strings.py -a length,align1,align2 \
      -i results/latest.json -s tools/benchout_strings.schema.json --gmean
  ```

## Cross-compilation for aarch64

A cross toolchain with a sysroot (headers and libc for aarch64) is required, e.g. on
Fedora:

```sh
sudo dnf install gcc-aarch64-linux-gnu glibc-devel-aarch64
make CROSS=aarch64-linux-gnu-
```

`./mb build --cross` does the same. Run the benchmarks on the aarch64 machine itself
(otherwise measurements are meaningless). `qemu-aarch64` can be used for correctness
checks on an x86 host.

## License & provenance

The original code of this project (everything under `src/`, `cli/`, `tools/plot_mem.py`,
the `impls/` examples, `Makefile`, `matrices/`, `mb`) is released under the
**MIT License** — see [LICENSE](LICENSE).

Several files were **vendored from the GNU C Library benchtests** and remain under the
GNU Lesser General Public License, version 2.1 or later (as stated in their headers):

- `src/json-lib.c`, `src/json-lib.h` — JSON writer;
- `tools/compare_strings.py`, `tools/plot_strings.py`, `tools/benchout.schema.json`,
  `tools/benchout_strings.schema.json` — analysis scripts and schemas.

Those files carry their original copyright headers
(`Copyright (C) <year>-2026 Free Software Foundation, Inc.`). The benchmark
methodology (guard pages, alignment/size matrices, timing technique) is adapted from
the glibc benchtests; the implementations of that methodology in this repository are
original.

## Dependencies

- Nothing beyond a C compiler and Python 3.6+ is needed for `make`, `mb run`
  (summary tables), `mb check`, `mb list`.
- Detailed glibc tables (`--table full`): the Python module `jsonschema`.
- Graphs (`tools/plot_mem.py`, or `mb plot` / glibc `plot_strings.py`): `matplotlib`
  (+`jsonschema`, `numpy` for the glibc script).

## Notes

- Numbers in the JSON are nanoseconds per call; when comparing runs done on different
  CPUs or frequencies remember they are machine-specific.
- `make clean` removes only `build/`; run results stay in `results/` (delete them
  yourself), graphs in `plots/`.

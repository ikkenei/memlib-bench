# memlib_bench

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL%20v2.1-blue.svg)](https://www.gnu.org/licenses/lgpl-2.1)
[![Arch](https://img.shields.io/badge/arch-aarch64%20%7C%20x86--64-lightgrey)](https://github.com/ikkenei/memlib-bench)

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
- Repeat every measurement (`--repeat N`) and get statistics (median/min/max/mean,
  coefficient of variation, 95% CI) in the summary; each run is kept in the JSON.
- Choose the measurement methodology (`--measure`): the glibc-style hot loop
  (default), randomized offsets, or a mixed stream of random sizes and offsets
  (as in llvm-libc's benchmarks); optional precision-driven iteration control
  (`--iters-mode precision`).

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
- Configurable test matrix: sizes and offsets live in plain-text **profile files**
  (`--matrix`), no recompilation needed - the built-in glibc matrices are such files
  too (`matrices/glibc_small.txt`, `matrices/glibc_large.txt`).
- `tools/plot_mem.py` (`mb plot`): GB/s vs size plots, one figure per full matrix
  parameter combination (`src`×`dst`×`dir`, `align`×`fill`, ...), all implementations
  overlaid as curves, adaptive B/KB/MB size labels, L1-cache marker line.

## Platform

Supported natively (Linux + glibc):

- **aarch64** — the primary target. Timing uses the system counter
  `CNTVCT_EL0`/`CNTFRQ_EL0` (as glibc's `sysdeps/aarch64/hp-timing.h`), which is immune
  to CPU frequency scaling. ASM example: `example_neon.S`.
- **x86-64** — fully supported. Timing uses the invariant TSC (`LFENCE;RDTSC`), like
  glibc's `sysdeps/x86/hp-timing.h`: the TSC frequency is calibrated once at startup
  against `clock_gettime(CLOCK_MONOTONIC)`, and the harness transparently falls back to
  `clock_gettime` when the CPU has no invariant TSC. ASM example: `example_sse2.S`.

On any other architecture a `clock_gettime(CLOCK_MONOTONIC)` fallback (vDSO) is used.
All values are reported in nanoseconds per operation. The active timing backend can be
inspected with `MB_TIMING_DEBUG=1`.

## Layout

```
Makefile             build drivers and implementations
mb                   CLI (Python 3.6+, stdlib only; optional jsonschema/matplotlib)
LICENSE              MIT license for the original code
src/                 framework and drivers
  bench_common.[ch]    guard-page buffers, implementation registry, crash reporter
  bench_drv.[ch]       option parsing, adaptive iteration policy, embedded matrices
  bench_driver.[ch]    shared driver core: main flow, matrix runner, warm-up/
                       measurement loops, repeats, JSON, check dispatch
  timing.h             timer (cntvct_el0 on aarch64, clock_gettime elsewhere)
  check.[ch]           correctness engine (byte-wise oracle)
  generic_ref.[ch]     generic C reference + oracle functions
  matrix.[ch]          matrix-profile parser
  json-lib.[ch]        (vendored from glibc) JSON writer, glibc format
  bench_memcpy.c / bench_memmove.c / bench_memset.c / bench_memcmp.c
                       (thin per-function descriptions: signature, generic
                       reference, oracle, case geometry, JSON attributes)
impls/               put your implementations here (see below)
tools/               (vendored from glibc benchtests/scripts)
  compare_strings.py, plot_strings.py, benchout_strings.schema.json, ...
  plot_mem.py        GB/s throughput graphs for this project
matrices/            matrix profiles: glibc_small.txt / glibc_large.txt (the
                     built-in defaults, compiled into the drivers) and
                     example.txt (all notation variants, flat + blocks)
  distributions/     size distributions from real workloads (llvm-libc CSV),
                     compiled into the drivers and selectable with --dist
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

# throughput graphs (GB/s): one figure per matrix parameter value
./mb plot results/latest.json -o plots
# (the script can also be called directly: python3 tools/plot_mem.py ...)
```

`mb run` without function names runs all four functions. `mb memcpy --impl x` is
shorthand for `mb run memcpy --impl x`.

`mb run`/`mb check` rebuild the benchmark drivers (and any `impls/` shared object
whose source is newer) automatically when they are missing or out of date — so
after a `git pull` you can run `./mb run ...` directly. Use `--no-build` to disable
this and fail with an error instead.

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

- An implementation does not have to provide all four functions — a symbol the
  library does not define itself is skipped with a warning for the affected
  function.  (The check matters because `dlsym()` on a library handle would
  otherwise find libc's implementation through the library's dependencies and
  report it under the label of the library.)
- Symbols must have the standard signatures and default visibility
  (`memcpy`, `memmove`, `memset`, `memcmp`).
- Libraries are loaded with `dlopen(RTLD_LOCAL)`, so they cannot interpose on the
  driver itself.
- Assembly `.S` files are built only when the toolchain targets aarch64.
- Per-file compile flags: `make CFLAGS_myimpl="-march=armv8.2-a+sve"` (variable named
  after the file base name). Ready-made examples: `example_c.c` (portable C), `example_neon.S` (aarch64
  NEON) and `example_sse2.S` (x86-64 SSE2) in `impls/`. The `.S` examples build only
  when the toolchain targets their architecture.

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
| `--max-len N` | cap tested lengths; above 128 KiB the large matrix is added |
| `--matrix FILE` | run the sizes/offsets from a matrix profile (below) |
| `--seed N` | pattern seed for `--check` |
| `--repeat N` | measure every test N times (each run is stored in the JSON) |
| `--measure hot\|offsets\|mixed` | measurement mode (default `hot`, see below) |
| `--batch N` | calls per randomized batch in `offsets`/`mixed` (default 1024) |
| `--dist NAME\|FILE` | size distribution for `--measure mixed` (see `mb list`) |
| `--list-dists` | print the embedded distribution names and exit |
| `--iters-mode budget\|precision` | iteration policy (default `budget`) |
| `--epsilon X` | precision target for `--iters-mode precision` (default 0.01) |
| `--scaling X` | iteration growth factor in precision mode (default 1.4) |
| `--initial-iters` / `--min-samples` / `--max-samples` | precision mode limits |
| `--min-duration` / `--max-duration` | precision mode time limits (s) |
| `--mismatch-at N` | `memcmp`: place the mismatch at byte N-1 |
| `--summary combo\|size` | summary layout (default `combo`: combinations × size regions) |
| `--regions LIST` | size-region boundaries for `--summary combo` (default `16,64,512,4096,65536`) |
| `--stats median\|min\|max\|mean` | statistic over repeated runs (default `median`) |
| `--match SUBSTR` | only summary rows whose combination label contains SUBSTR |
| `--gmean` | add a geo-mean block/row over the whole summary |
| `--no-warmup` | skip the CPU frequency ramp-up loop |
| `--no-build` | do not rebuild drivers/implementations automatically |

## Summary table (`mb run`)

By default the summary is broken down **by parameters**, with sizes aggregated into
**regions**; the cells are throughput:

```
Function: memcpy
cells: geo-mean GB/s over the sizes in the region (statistic: median of 3 runs; % vs libc)
  combinations / GB/s      ≤16    17-64   65-512   513-4K 4097-64K
  src=0 dst=0 dir=0
    libc                1.61    27.44    83.86   178.73   105.65
    example_c         -36.1%   -62.1%   -66.8%   -71.3%   -37.4%
  src=0 dst=3 dir=0
    libc                1.74    28.20    71.11    75.20    52.23
    example_c         -61.2%   -83.3%   -75.5%   -41.8%   -16.9%
  ...
  repeat noise (CoV over 3 runs): libc: median 0.7%, max 10.3%; example_c: median 1.4%, max 25.6%
```

- Rows are the full parameter combinations (`src`/`dst`/`dir` for `memcpy`,
  `align`/`fill` for `memset`, ...), so different code paths are not averaged
  together.  `--match dir=1` keeps only the matching rows.
- Columns are size regions (upper bounds from `--regions`, default 16 B, 64 B, 512 B,
  4 KiB, 64 KiB); each cell is the geometric mean of `length / time` over the sizes
  of that region.
- The base implementation (default `libc`) shows absolute GB/s, the others show the
  difference in percent.
- `--summary size` switches to the flat layout: one row per tested size, cells in
  nanoseconds per call, aggregated over all combinations.
- `--stats min|max|mean` selects the statistic used for repeated runs (default
  `median`); `--gmean` adds a geo-mean block over all cells.

### Repeats (`--repeat N`)

`--repeat N` measures every test N times (after the warm-up and with the same
iteration budget).  Every run is a separate object in the JSON, tagged with
`"run": k`, so the raw data can be analysed per run; the summary reports the chosen
statistic plus a statistics line - median/max coefficient of variation and the
median 95% confidence interval (t-distribution) over the cells - and
`tools/plot_mem.py` aggregates the runs with a median.

## Measurement modes (`--measure`)

The default mode repeats *one* call in a hot loop, exactly like glibc's benchtests.
That is fast and reproducible, but the branch predictor learns the call - both the
size-dependent branches and the alignment - so small sizes are measured under
idealized conditions.  Two additional modes (modelled on llvm-libc's benchmarks)
measure the same functions with randomized parameters:

| Mode | Parameters | Reported as |
|---|---|---|
| `hot` (default) | the same size and offsets every call | one object per matrix case |
| `offsets` | the matrix sizes; **random offsets** on every call | one object per matrix case (`offsets="random"`) |
| `mixed` | **random sizes** (drawn from the matrix's size pool) and **random offsets** | one object per (fill byte / expected result) group and repetition |

```sh
./mb run memcpy --measure offsets --batch 1024 --repeat 5   # average over misalignment
./mb run memcpy --measure mixed   --batch 1024 --repeat 5   # random size stream
./mb run memcpy --measure offsets --seed 12345              # reproducible batches
```

- In `offsets`/`mixed` a *batch* of `--batch` calls is prepared (randomized with
  `--seed`), warmed up and then cycled inside the timed loop, so the measured call
  sees varying parameters and the predictor cannot specialize.  `memmove` keeps the
  overlap distance of the matrix case while the offset moves; the other functions
  randomize the offsets independently within a page.
- `offsets` keeps one result per size, so the plots and the summary stay comparable
  with `hot`; `mixed` reports a single average per batch (`length` is the mean size,
  `sizes="min..max"` is the pool) - it is meant for the summary table, not for
  per-size graphs.
- Neither mode is directly comparable with `hot`/glibc numbers: they include the
  cost of unpredictable branches and misalignment on purpose.
### Size distributions (`--dist`, llvm-libc's distribution mode)

`--measure mixed` draws its sizes from the matrix's size pool by default; with
`--dist` they come from an empirical distribution instead, which is how
llvm-libc measures the average cost on a real workload:

```sh
./mb list                                                   # the names
./mb run memcpy --measure mixed --dist memcpy_google_a --repeat 5
./mb run memcpy --measure mixed --dist uniform_384_to_4096
./mb run memcmp --measure mixed --dist ./my-sizes.csv       # your own CSV
```

The 37 distributions under `matrices/distributions/` come from llvm-libc (see
their README for the license and provenance); every size is sampled per call
with its empirical weight, so the reported GB/s is the average over the
workload mix rather than over a uniform size range.  The name is matched
ignoring case, spaces and underscores, so `memcpy_google_a`, `memcpy Google A`
and `MemcpyGoogleA` are equivalent.

Two CSV shapes are accepted, and weights are normalized (so counts work too):

```
0.0059,0.0661,0.0312,...        # one weight per column, size = index
64,1200                         # size,weight per line (a header is allowed)
128,340
```

In a matrix profile the distribution can also provide the sizes of the cases;
`dist-samples` bounds how many distinct sizes are expanded:

```
[memcpy]
dist = memcpy_google_a
dist-samples = 32
src = 0 3
dst = 0
```

- `--iters-mode precision` (with `--epsilon`, `--scaling`, `--min-samples`,
  `--max-samples`, `--min-duration`, `--max-duration`) replaces the byte budget with
  llvm-libc's stopping rule: iterations grow geometrically until the running mean
  settles within `--epsilon`; the value is the cumulative mean.
- `--mismatch-at N` (memcmp) moves the difference from the last byte of the range to
  byte `N-1`, which exercises early exit.
- Diagnostics: `MB_DEBUG_BATCH=1` prints the first batch parameters and the
  sampled mean size, `MB_DEBUG_PRECISION=1` prints the samples/calls used per
  precision measurement, `MB_DEBUG_DIST=1` prints the parsed distribution size.

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

- `key = value` is required (the `=` sign is not optional); sections are `[name]`,
  values are separated by spaces or commas and lists can continue on the next lines;
  comments start with `#` or `;`.
- List elements: plain numbers (hex allowed, `0x..`), powers of two `A..B`, or a
  linear range `A..B:STEP`. Negative numbers are allowed only for `fill`/`diff`.
- A key may be repeated, and every occurrence appends to the same list, so values
  and ranges of any kind can be freely combined:
  ```
  sizes = 1..16384           # powers of two: 1, 2, 4, ... 16384
  sizes = 24576..98304:8192  # linear: 24576, 32768, ... 98304
  sizes = 131072             # single value
  ```
- `dist = NAME` (or a CSV path) replaces `sizes`: the sizes come from an
  empirical distribution, `dist-samples = N` bounds how many distinct sizes
  are expanded into cases (default 64).  See the distributions section above.
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

### Case blocks

The glibc matrices have offsets *correlated* with the loop index (`i`, `i+32`,
`P/2+i`, ...), which a cross product of two lists cannot express.  Such matrices are
written as **case blocks** instead:

```
[memcpy]
case                      # start a block (a bare `loop` starts one too)
loop i = 0..17:1          # loop variable
size = 1 << i             # length expression
pairs = (0,0) (i,0) (i+32,0) (0,i) (i,i) (P/2,0) (P/2+i,i)
both = 1
```

- `case` / `loop <var> = <values>` start a block; a block may have up to four loop
  variables (their cartesian product is used) and the keys `size`, `pairs`, `align`,
  `fill`, `diff`, `both`.
- `size` is an expression; `pairs` is a list of `(a1,a2)` expressions for
  `memcpy`/`memmove`/`memcmp`; `align`/`fill` (memset) and `diff` (memcmp) are
  comma-separated expression lists, so expressions may contain spaces.
- Expressions support integers (hex allowed), the loop variables, `P` (the OS page
  size), parentheses and `+ - * / <<`.
- In a loop list `A..B` still means powers of two, `A..B:STEP` is linear and
  `!pow2` drops values where `(v & (v-1)) == 0` (as glibc's `if (i & (i-1))` does).
- For `memmove` the pairs are directed (`a1` = source, `a2` = destination), so both
  overlap directions are written explicitly, as in `matrices/glibc_small.txt`.

The flat and the block notation can be mixed in one section.

## Built-in (default) matrices

Without `--matrix` the drivers run the matrices from

| File | Content |
|---|---|
| `matrices/glibc_small.txt` | the glibc benchtests default matrices |
| `matrices/glibc_large.txt` | large-size cases, loaded when `--max-len` > 128 KiB |

They are the files listed above — the benchmark code contains no matrix of its own.
The Makefile compiles them into the drivers (`build/gen/matrix_glibc.h`), so editing
a file and running `make` (or just `./mb run ...`, which rebuilds automatically)
applies the change; `./mb run --matrix matrices/glibc_small.txt ...` runs the same
matrix from the file without rebuilding.  The alignment mask applied by `do_test`
(`& (page-1)` for the copy functions, `& 4095` for `memset`/`memcmp`) is unchanged.

## Adding a benchmarked function

All the shared machinery lives in `src/bench_driver.[ch]`; each driver is a thin
description of one function (about 60 lines).  Adding another function (say
`bzero`, which has the `memset` signature) means:

1. add its section to the matrix profiles (`matrices/glibc_small.txt`, ...),
2. write `src/bench_bzero.c` with two callbacks - `prepare()` turns a matrix case
   into pointers plus content, `attrs()` writes its JSON attributes - and a
   descriptor:

   ```c
   static int prepare (const mb_case_t *c, int dir, mb_pointers_t *p) { ... }
   static void attrs (json_ctx_t *ctx, const mb_case_t *c,
                      const mb_pointers_t *p) { ... }

   static const mb_func_t the_function = {
     .name = "bzero", .sig = MB_SIG_FILL,
     .generic_ref = (mb_fn_t) mb_ref_memset,
     .w_oracle = mb_oracle_memset,
     .prepare = prepare, .attrs = attrs,
   };

   int main (int argc, char **argv)
   {
     return mb_driver_main (&the_function, argc, argv);
   }
   ```

3. add a build rule (copy one of the `build/bench_*` rules in the Makefile) and
   the name to `FUNCS` in `cli/mb.py`.

`MB_SIG_COPY` / `MB_SIG_MOVE` / `MB_SIG_FILL` / `MB_SIG_CMP` select the measured
call and the warm-up/measurement loop, so no timing code is duplicated.

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

## Throughput graphs (`mb plot`, `tools/plot_mem.py`)

Reads a benchout JSON file and plots **GB/s = bytes/ns** vs size, with every
implementation as a colored curve:

```sh
./mb plot results/latest.json -o plots                    # all functions, all combos
./mb plot res.json --func memcpy --func memset             # subset
./mb plot res.json --match 'dst=0'                         # combos containing dst=0
./mb plot res.json --mode param --param src --param fill   # per-parameter view
./mb plot res.json --stats mean --band p5-p95              # mean curve + spread
./mb plot res.json --xlog --cache-size auto                # log axis + L1 marker
./mb plot --help                                           # all options
```

`mb plot` forwards its arguments to `tools/plot_mem.py`, so the script can also be
invoked directly (`python3 tools/plot_mem.py ...`). The glibc-style timing plots
(absolute timings, relative/max/throughput variants, graphs by variant) remain
available as `mb plot-glibc` (`tools/plot_strings.py`).

**Layout (`--mode`):**

| Mode | Figures |
|---|---|
| `combo` (default) | one figure per full parameter combination - every combination of the non-length attributes: `memcpy_src_0_dst_3_dir_1.png`, `memset_align_1_fill_255.png`, `memcmp_src_0_dst_3_result_-1.png` |
| `geometry` | alias of `combo` (the combination is the case "geometry"); kept for older commands |
| `param` | one figure per parameter value - `memcpy_src_0.png`, `memset_fill_255.png`, `memcmp_result_-1.png`; the remaining parameters are aggregated |
| `params` | alias of `param` (plural spelling) |

**Curves and statistics:**

- every implementation is one curve; in `param` mode the samples of a point are the
  repetitions plus the aggregated parameter combinations.
- `--stats median|mean|min|max` picks the plotted value (default `median`);
  `--band none|min-max|p5-p95|ci95` adds a semi-transparent band computed from the
  same samples (percentile range, full range, or the 95% t confidence interval).
  The title records the composition, e.g. `GB/s, mean of 5 runs, p5-p95 band`.
- `--repeat N` at benchmark time is what makes the bands meaningful; without
  repetitions a band is only drawn where several samples happen to share a point.

**Axes:** the x axis starts at zero and uses round tick values (powers of two of the
display unit: `0, 8, 16 ... 64 B` or `0, 16 KB ... 128 KB`) - `--xlog` switches to a
base-2 log axis with power-of-two ticks.  The y axis starts at zero as well;
`--xlim LO[,HI]` and `--ylim LO[,HI]` set explicit ranges (bytes / GB/s).

**Cache marker:** nothing is drawn by default, because the machine that produced the
JSON is often not the one analysing it.  Use `--cache-size N` for an explicit byte
count (`0x10000` works) or `--cache-size auto` to read the L1 data-cache size from
sysfs of the *current* machine.

Other options: `--func`, `--param`, `--match`, `--fmt png|pdf|svg`, `--dpi`,
`--max-figs N`.  Only requires `matplotlib`; Python >= 3.6.

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
  `bench_start`).  The per-test warm-up (present for all four functions; glibc warms
  only the copy functions) is run once, before the `--repeat N` measurements.
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

The original code of this project (everything under `src/`, `cli/`,
`tools/plot_mem.py`, the `impls/` examples, `Makefile`, `mb`, and `matrices/`
except the distributions directory) is released under the **MIT License** - see
[LICENSE](LICENSE).

**Vendored from the GNU C Library** (benchtests), under the GNU Lesser General
Public License, version 2.1 or later (as stated in their headers):

- `src/json-lib.c`, `src/json-lib.h` - JSON writer;
- `tools/compare_strings.py`, `tools/plot_strings.py`, `tools/benchout.schema.json`,
  `tools/benchout_strings.schema.json` - analysis scripts and schemas.

Those files carry their original copyright headers
(`Copyright (C) <year>-2026 Free Software Foundation, Inc.`).  The benchmark
methodology (guard pages, alignment/size matrices, timing technique) is adapted
from the glibc benchtests; the implementations of that methodology in this
repository are original.

**Vendored from the LLVM project** (libc benchmarks), under the Apache License
v2.0 with LLVM Exceptions:

- `matrices/distributions/*.csv` - the memory function size distributions
  observed in production (see `matrices/distributions/README.md`).

## Dependencies

- Nothing beyond a C compiler and Python 3.6+ is needed for `make`, `mb run`
  (summary tables), `mb check`, `mb list`.
- Detailed glibc tables (`--table full`): the Python module `jsonschema`.
- Graphs (`mb plot` → `tools/plot_mem.py`, `mb plot-glibc` → `tools/plot_strings.py`):
  `matplotlib` (+`jsonschema`, `numpy` for the glibc script).

## Notes

- Numbers in the JSON are nanoseconds per call; when comparing runs done on different
  CPUs or frequencies remember they are machine-specific.
- `make clean` removes only `build/`; run results stay in `results/` (delete them
  yourself), graphs in `plots/`.

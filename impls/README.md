# Your implementations

Put your own `memcpy` / `memmove` / `memset` / `memcmp` implementations here as
`.c` or `.S` (aarch64) files exporting the standard symbols. `make` (or `mb run` /
`mb check`) builds each file into `build/impls/<name>.so`.

Examples in this directory: `example_c.c` (portable C), `example_neon.S`
(aarch64 NEON), `example_sse2.S` (x86-64 SSE2). The assembly examples are built
only when the toolchain targets their architecture. See the main README
("Your implementations") for details.

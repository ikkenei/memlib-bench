# Your implementations

Put your own `memcpy` / `memmove` / `memset` / `memcmp` implementations here as
`.c` or `.S` (aarch64) files exporting the standard symbols. `make` (or `mb run` /
`mb check`) builds each file into `build/impls/<name>.so`.

Examples in this directory: `example_c.c` (portable C), `example_neon.S`
(aarch64 NEON). See the main README ("Your implementations") for details.

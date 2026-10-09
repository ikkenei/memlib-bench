# Size distributions

These 37 CSV files are the memory-function size distributions from the
[LLVM libc benchmarks](https://github.com/llvm/llvm-project/tree/main/libc/benchmarks/distributions)
(`libc/benchmarks/distributions/*.csv`), redistributed here unchanged.

They were collected from production workloads at Google (servers databases,
realtime and batch jobs) and reflect how small the typical `mem*` call is: for
memcpy, 96% of the calls operate on 128 bytes or fewer.  `uniform 384 to 4096`
is the uniform baseline used by LLVM for comparison.

Each file holds one probability (or weight) per size, size = column index:

```
0.00594645,0.0660518,0.0311743,...
```

The LLVM project is licensed under the Apache License v2.0 with LLVM Exceptions;
these data files are redistributed under the same terms.  See the LLVM
`LICENSE.TXT` (Apache-2.0 WITH LLVM-exception) for the full text.

## Using them

The drivers compile these files in, so they can be selected by name; the name
is compared ignoring case, spaces and underscores, therefore
`MemcpyGoogleA`, `memcpy Google A` and `memcpy_google_a` are the same
distribution:

```sh
./mb list                                          # all names
./mb run memcpy --measure mixed --dist memcpy_google_a
./mb run memcpy --measure mixed --dist uniform_384_to_4096 --repeat 5
```

A file path can be used as well, which is the way to bring your own profile
(`size,weight` pairs, or one weight per line, `#` starts a comment):

```sh
./mb run memcmp --measure mixed --dist ./my-sizes.csv
```

In a matrix profile the distribution can also be attached to the sizes of the
cases (`dist-samples` bounds how many distinct sizes are expanded into cases):

```
[memcpy]
dist = memcpy_google_a
dist-samples = 32
src = 0 3
dst = 0
```

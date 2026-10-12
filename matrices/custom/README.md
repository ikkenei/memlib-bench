# Your own loads

Put your own benchmark inputs here.  Everything in this directory except this
file is ignored by git (`matrices/custom/*`), so the files you add never show
up in `git status` and `git pull` never touches or conflicts with them.

```sh
# start from the annotated reference (uses every notation variant)
cp matrices/example.txt matrices/custom/my_load.txt
./mb run memcpy --matrix matrices/custom/my_load.txt
./mb run memmove memset --matrix matrices/custom/my_load.txt --measure mixed
```

The syntax is documented in the main README, section *Matrix profiles*
(`--matrix FILE`); `matrices/example.txt` is a matrix using every notation
variant and `matrices/glibc_small.txt` / `matrices/glibc_large.txt` are the
real glibc matrices.

Other user data of this project belongs here too, since it is likewise never
committed:

* `*.txt` - matrix profiles for `--matrix`, and captures from
  `tools/memtrace.py` for `--profile` (also plain text);
* `*.csv` - size distributions for `--dist`: `size,weight` pairs or one
  weight per line (size = column index), see `matrices/distributions/`.

Keep the files out of the parent directories: `matrices/*.txt` is tracked
(the built-in matrices), which is exactly what this directory avoids.

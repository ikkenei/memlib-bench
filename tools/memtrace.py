#!/usr/bin/env python3
"""memtrace.py — capture the memory-access patterns of a running program.

Attaches bpftrace uprobes to the `mem*` implementations used by a process
(glibc builds them as IFUNCs, so the probes go to the concrete
implementations such as `__memcpy_avx_unaligned` / `__memcpy_neon` rather
than to the public `memcpy` symbol, which only points at the resolver),
and records, per call:

    function, size, src alignment, dst alignment, class, attr

  class 0   plain copy (memcpy, or memmove with src == dst)
  1..4      memmove: 1 forward disjoint, 2 forward overlap,
            3 backward disjoint, 4 backward overlap
            (attr = overlap distance bucket in bytes)
  5, 6      memset with a zero / non-zero fill byte (attr = the byte)
  7, 8      memcmp with equal / differing buffers
            (attr = 0; with --memcmp-window it is the 1-based position of
             the first difference inside the window, 0 = beyond it)

`src alignment`/`dst alignment` are the low bits of the pointers
(`& --align-mask`, 63 by default).  That is exactly what the benchmark
needs to reproduce the alignment: its buffers are page aligned, so an
offset with the same low bits behaves the same way.

Two capture sizes:

  attach-stats  aggregate in the kernel (a bpftrace map) and write a
                compact profile; cheap per call, use it for long runs and
                high call rates;
  attach-trace  write one line per call (the same file format with
                count = 1); exact call sequence, but the per-call printf
                dominates the cost.

Both write the "memtrace profile" text format, so a trace can be
aggregated later (`convert`) and both can be fed to the benchmark:

    ./mb run memcpy --measure mixed --profile trace.profile
    ./mb run memcpy --measure mixed --dist trace-sizes.csv

Cost: an uprobe fires on every call, which for a typical workload (96% of
memcpy calls are <= 128 bytes) is far more expensive than the call
itself.  This tool is for collecting patterns, not for timing - measure
the resulting profile without the tracer attached.

Requires: bpftrace and root (or CAP_BPF/CAP_PERFMON), Linux with uprobes.
`list`, `script`, `convert` and `show` work without privileges.
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

FUNCS = ("memcpy", "memmove", "memset", "memcmp")
FUNC_ID = {f: i for i, f in enumerate(FUNCS)}

CLASS_NAMES = {
    0: "plain", 1: "fwd-disjoint", 2: "fwd-overlap", 3: "bwd-disjoint",
    4: "bwd-overlap", 5: "zero", 6: "nonzero", 7: "equal", 8: "differs",
}

PROFILE_HEADER = """# memtrace profile v1
# function,size,count,src_align,dst_align,class,attr
#   src_align/dst_align : pointer low bits (0..mask): alignment, "-" = unused
#   class : 0 plain | 1 fwd-disjoint | 2 fwd-overlap | 3 bwd-disjoint |
#           4 bwd-overlap | 5 memset-zero | 6 memset-nonzero |
#           7 memcmp-equal | 8 memcmp-differs
#   attr  : overlap distance bucket (class 1-4), fill byte (class 5/6),
#           mismatch position (class 8), 0 otherwise
"""


def err(msg):
    sys.stderr.write("memtrace: %s\n" % msg)


def die(msg, code=2):
    err(msg)
    sys.exit(code)


# ----------------------------------------------------------------------
# ELF symbols
# ----------------------------------------------------------------------

SYM_RE = re.compile(r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+(.+?)\s*$")


def read_symbols(path):
    """Return [{value, size, type, bind, name}] from `readelf -sW`."""
    try:
        out = subprocess.run(["readelf", "-sW", path], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        die("cannot read symbols from %s: %s" % (path, e))
    syms = []
    for line in out.stdout.decode("utf-8", "replace").splitlines():
        m = SYM_RE.match(line)
        if not m:
            continue
        syms.append({"value": int(m.group(1), 16), "size": int(m.group(2)),
                     "type": m.group(3), "bind": m.group(4),
                     "name": m.group(5).split("@")[0]})
    return syms


def find_libc(pid=None):
    """Path of the libc mapped into PID (or into this process)."""
    maps = "/proc/%s/maps" % (pid if pid else "self")
    try:
        with open(maps) as f:
            for line in f:
                if "libc.so" in line or "libc-2." in line:
                    return line.split()[-1]
    except OSError as e:
        die("cannot read %s: %s" % (maps, e))
    die("no libc found in %s; pass --libc explicitly" % maps)


def select_symbols(syms, funcs):
    """Pick the probe addresses for FUNCS.

    The concrete implementations (`__memcpy_*`, excluding the `*_chk`
    variants, which fall through into them) are preferred; the public
    symbol is used when it is a real function (musl, static libc, ...).
    Addresses shared by several functions - glibc aliases memcpy and
    memmove to the same code - are probed once and summarised in the
    notes.  Returns (probes, notes) with probes = [(name, addr, func)].
    """
    probes = {}
    notes = []
    shared = []
    for f in funcs:
        candidates = [s for s in syms
                      if s["type"] == "FUNC" and "_chk" not in s["name"]
                      and s["name"].startswith("__" + f + "_")]
        if not candidates:
            candidates = [s for s in syms
                          if s["name"] == f and s["type"] == "FUNC"]
        if not candidates:
            candidates = [s for s in syms if s["name"] == f]
            if candidates:
                notes.append("%s is %s here (not FUNC): the probe may hit "
                             "the IFUNC resolver instead of an implementation"
                             % (f, candidates[0]["type"]))
        if not candidates:
            notes.append("no symbols found for %s" % f)
        for s in candidates:
            if s["value"] in probes:
                if probes[s["value"]][1] != f:
                    shared.append((probes[s["value"]][1], f,
                                   probes[s["value"]][0]))
                continue
            probes[s["value"]] = (s["name"], f)
    if shared:
        pairs = sorted(set((a, b) for a, b, _ in shared))
        notes.append("%d address(es) are shared between %s (e.g. %s); each is "
                     "probed once, as %s"
                     % (len(shared),
                        ", ".join("%s=%s" % p for p in pairs),
                        shared[0][2], shared[0][0]))
    return (sorted((n, a, f) for a, (n, f) in probes.items()), notes)


# ----------------------------------------------------------------------
# bpftrace program
# ----------------------------------------------------------------------

def _align(expr, mask):
    return "(%s) & %d" % (expr, mask)


def _memmove_class():
    """Class/attr of a memmove from arg0 (dst), arg1 (src), arg2."""
    return [
        "$d = 0;",
        "if (arg0 == arg1) { $c = 0; }",
        "else if (arg0 < arg1) {",
        "  $d = arg1 - arg0;",
        "  if ($d >= arg2) { $c = 1; } else { $c = 2; }",
        "} else {",
        "  $d = arg0 - arg1;",
        "  if ($d >= arg2) { $c = 3; } else { $c = 4; }",
        "}",
        "$b = 0;",
        "if ($c >= 2) {",
        "  if ($d >= 64) { $b = 64; }",
        "  else if ($d >= 32) { $b = 32; }",
        "  else if ($d >= 16) { $b = 16; }",
        "  else if ($d >= 8) { $b = 8; }",
        "  else if ($d >= 4) { $b = 4; }",
        "  else if ($d >= 2) { $b = 2; }",
        "  else { $b = 1; }",
        "}",
    ]


def _memcmp_window(window):
    """Compare the first WINDOW bytes from the entry probe (no uretprobe)."""
    return [
        "$p = buf(arg0, %d);" % window,
        "$q = buf(arg1, %d);" % window,
        "$n = arg2 < %d ? arg2 : %d;" % (window, window),
        "$m = 0;",
        "unroll(%d) {" % window,
        "  if ($m == 0) { if (i < $n) { if ($p[i] != $q[i]) { $m = i + 1; } } }",
        "}",
        "$c = $m == 0 ? 7 : 8;",
    ]


def probe_body(func, mode, mask, window):
    """Statements of one probe; $c/$a hold class/attr at the end."""
    lines = []
    attr = "0"
    if func == "memcpy":
        lines.append("$c = 0;")
    elif func == "memmove":
        lines += _memmove_class()
        attr = "$b"
    elif func == "memset":
        lines += ["$v = arg1 & 255;", "$c = $v == 0 ? 5 : 6;"]
        attr = "$v"
    elif func == "memcmp" and window:
        lines += _memcmp_window(window)
        attr = "$m"
    else:
        die("probe_body: unexpected %s" % func)

    if mode == "trace":
        lines.append('printf("%s,%%llu,1,%%llu,%%llu,%%d,%%d\\n", arg2, %s,'
                     " %s, $c, %s);" % (func, _align("arg0", mask),
                                        _align("arg1", mask), attr))
    else:
        lines.append("@st[%d, arg2, %s, %s, $c, %s] = count();"
                     % (FUNC_ID[func], _align("arg0", mask),
                        _align("arg1", mask), attr))
    return lines


def gen_script(libc, probes, funcs, mode, mask, window, duration):
    """Generate the bpftrace program text."""
    out = ["/* generated by tools/memtrace.py - do not edit */"]
    if mode == "trace":
        out.append('printf("# trace: function,size,count,src_align,'
                   'dst_align,class,attr\\n");')

    memcmp_needs_result = "memcmp" in funcs and not window
    for name, addr, func in probes:
        out.append("")
        out.append("/* %s @ 0x%x (%s) */" % (name, addr, func))

        if func == "memcmp" and memcmp_needs_result:
            # The comparison result is only known at return; carry the entry
            # arguments through a per-thread map (two map operations, but
            # independent of the bpftrace version).
            out.append("uprobe:%s:%s" % (libc, name))
            out.append("{")
            out.append("  @e[tid] = ((arg2 & 0xffffffff) << 32) | ((%s) "
                       "<< 16) | (%s);"
                       % (_align("arg0", mask), _align("arg1", mask)))
            out.append("}")
            out.append("uretprobe:%s:%s" % (libc, name))
            out.append("{")
            out.append("  $v = @e[tid];")
            out.append("  delete(@e[tid]);")
            out.append("  $c = retval == 0 ? 7 : 8;")
            out.append("  $size = $v >> 32;")
            out.append("  $sa = ($v >> 16) & 0xffff;")
            out.append("  $da = $v & 0xffff;")
            if mode == "trace":
                out.append('  printf("memcmp,%llu,1,%llu,%llu,%d,%d\\n", '
                           "$size, $sa, $da, $c, 0);")
            else:
                out.append("  @st[%d, $size, $sa, $da, $c, 0] = count();"
                           % FUNC_ID[func])
            out.append("}")
            continue

        out.append("uprobe:%s:%s" % (libc, name))
        out.append("{")
        for line in probe_body(func, mode, mask, window):
            out.append("  " + line)
        out.append("}")

    if mode == "stats":
        out.append("")
        out.append("END { print(@st); }")
    if duration:
        out.append("")
        out.append("interval:s:%d { exit(); }" % duration)
    return "\n".join(out) + "\n"


# ----------------------------------------------------------------------
# Profile / trace files
# ----------------------------------------------------------------------

STATS_LINE_RE = re.compile(r"^\s*\[([^\]]*)\]\s*:\s*(\d+)\s*$")
TRACE_RE = re.compile(r"^([a-z]+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)$")


def parse_stats(text):
    """Parse `print(@st)` output: [f, size, sa, da, class, attr]: count."""
    rows = []
    bad = 0
    for line in text.splitlines():
        m = STATS_LINE_RE.match(line)
        if not m:
            if line.strip():
                bad += 1
            continue
        parts = [p.strip() for p in m.group(1).split(",")]
        try:
            fid, size, sa, da, cls, attr = (int(p) for p in parts)
        except (ValueError, TypeError):
            bad += 1
            continue
        if len(parts) != 6 or not 0 <= fid < len(FUNCS):
            bad += 1
            continue
        rows.append((FUNCS[fid], size, sa, da, cls, attr, int(m.group(2))))
    if bad:
        err("%d unrecognised line(s) in the bpftrace output (ignored)" % bad)
    return rows


def parse_trace(text):
    rows = []
    bad = 0
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("function,") or not s:
            continue
        m = TRACE_RE.match(s)
        if not m or m.group(1) not in FUNC_ID:
            bad += 1
            continue
        rows.append((m.group(1), int(m.group(2)), int(m.group(3)),
                     int(m.group(4)), int(m.group(5)), int(m.group(6)),
                     int(m.group(7))))
    if bad:
        err("%d unrecognised line(s) in the trace (ignored)" % bad)
    return rows


def aggregate(rows):
    """Sum the counts of identical (function, size, align, class, attr)."""
    out = collections.OrderedDict()
    for func, size, sa, da, cls, attr, count in rows:
        key = (func, size, sa, da, cls, attr)
        out[key] = out.get(key, 0) + count
    return [(k[0], k[1], k[2], k[3], k[4], k[5], v) for k, v in out.items()]


def write_profile(path, rows):
    rows = aggregate(rows)
    rows.sort(key=lambda r: (FUNC_ID.get(r[0], 99), r[1], r[2], r[3], r[4], r[5]))
    with open(path, "w") as f:
        f.write(PROFILE_HEADER)
        for func, size, sa, da, cls, attr, count in rows:
            f.write("%s,%d,%d,%s,%s,%d,%d\n"
                    % (func, size, count, sa if sa >= 0 else "-",
                       da if da >= 0 else "-", cls, attr))
    return len(rows), sum(r[6] for r in rows)


def read_profile(path, func=None):
    """Read a profile/trace file; returns the same shape as aggregate()."""
    rows = []
    skipped = 0
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.split("#")[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) != 7:
                die("%s:%d: expected 7 comma separated fields" % (path, lineno))
            fname = parts[0]
            if fname not in FUNC_ID:
                skipped += 1
                continue
            if func is not None and fname != func:
                continue
            try:
                size = int(parts[1], 0)
                count = int(parts[2], 0)
                sa = -1 if parts[3] in ("-", "") else int(parts[3], 0)
                da = -1 if parts[4] in ("-", "") else int(parts[4], 0)
                cls = int(parts[5], 0)
                attr = int(parts[6], 0)
            except ValueError as e:
                die("%s:%d: %s" % (path, lineno, e))
            if count <= 0:
                continue
            rows.append((fname, size, sa, da, cls, attr, count))
    if skipped:
        err("%s: skipped %d non-profile line(s)" % (path, skipped))
    return rows


def write_sizes_csv(path, rows, func=None):
    """A plain `size,weight` CSV for the benchmark's --dist."""
    per_size = collections.Counter()
    for fname, size, sa, da, cls, attr, count in rows:
        if func is not None and fname != func:
            continue
        per_size[size] += count
    with open(path, "w") as f:
        if not per_size:
            return 0
        f.write("# memtrace size distribution: %d calls, sizes %d..%d\n"
                % (sum(per_size.values()), min(per_size), max(per_size)))
        for size in sorted(per_size):
            f.write("%d,%d\n" % (size, per_size[size]))
    return len(per_size)


# ----------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------

def resolve_libc(args):
    if args.libc and args.libc != "auto":
        return args.libc
    return find_libc(getattr(args, "pid", None))


def build_probes(args, quiet=False):
    libc = resolve_libc(args)
    probes, notes = select_symbols(read_symbols(libc), args.funcs)
    if not quiet:
        for n in notes:
            err("note: %s" % n)
    if not probes:
        die("no probe symbols found in %s" % libc)
    return libc, probes


def cmd_list(args):
    libc, probes = build_probes(args)
    print("libc: %s" % libc)
    print("probes (%d):" % len(probes))
    for name, addr, func in probes:
        print("  %-10s %-42s 0x%x" % (func, name, addr))
    return 0


def cmd_script(args):
    libc, probes = build_probes(args)
    script = gen_script(libc, probes, args.funcs, args.mode, args.align_mask,
                        args.memcmp_window, args.duration)
    if args.output:
        with open(args.output, "w") as f:
            f.write(script)
        err("bpftrace program written to %s" % args.output)
    else:
        sys.stdout.write(script)
    return 0


def run_bpftrace(script, args, stream):
    bpftrace = args.bpftrace or shutil.which("bpftrace")
    if not bpftrace:
        die("bpftrace not found; install it (dnf/apt install bpftrace) or "
            "pass --bpftrace PATH")
    path = args.script_file or "/tmp/memtrace.bt"
    with open(path, "w") as f:
        f.write(script)
    cmd = [bpftrace]
    if args.pid:
        cmd += ["-p", str(args.pid)]
    if args.command:
        cmd += ["-c", " ".join(args.command)]
    cmd.append(path)
    err("running: %s" % " ".join(cmd))
    if stream:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
        return proc.stdout
    try:
        out = subprocess.run(cmd, stdout=subprocess.PIPE)
    except KeyboardInterrupt:
        die("interrupted", 130)
    return out.stdout


def cmd_attach(args):
    if not args.pid and not args.command:
        die("attach needs --pid PID or -c COMMAND")
    libc, probes = build_probes(args)
    script = gen_script(libc, probes, args.funcs, args.mode, args.align_mask,
                        args.memcmp_window, args.duration)
    if args.print_script:
        sys.stderr.write("--- bpftrace program ---\n%s---\n" % script)
    if not args.output:
        if args.print_script:
            return 0
        die("attach needs -o FILE (or use --print-script)")

    if args.mode == "stats":
        stdout = run_bpftrace(script, args, stream=False)
        rows = parse_stats(stdout.decode("utf-8", "replace"))
        if not rows:
            die("no samples collected (did the program call these functions, "
                "and did bpftrace attach?)")
        nrows, ncalls = write_profile(args.output, rows)
        err("%d geometries, %d calls -> %s" % (nrows, ncalls, args.output))
        if args.sizes_csv:
            n = write_sizes_csv(args.sizes_csv, rows)
            err("size distribution (%d sizes) -> %s" % (n, args.sizes_csv))
    else:
        stream = run_bpftrace(script, args, stream=True)
        with open(args.output, "w") as f:
            f.write(PROFILE_HEADER)
            for raw in stream:
                line = raw.decode("utf-8", "replace")
                sys.stdout.write(line)
                f.write(line)
        err("trace -> %s" % args.output)
        if args.sizes_csv:
            n = write_sizes_csv(args.sizes_csv, read_profile(args.output))
            err("size distribution (%d sizes) -> %s" % (n, args.sizes_csv))
    return 0


def cmd_convert(args):
    rows = read_profile(args.input, args.func)
    if args.sample > 1:
        rows = [(f, s, sa, da, c, a, max(1, cnt // args.sample))
                for f, s, sa, da, c, a, cnt in rows]
        err("counts scaled down by %d" % args.sample)
    if not rows:
        die("no rows to convert")
    if args.profile:
        nrows, ncalls = write_profile(args.profile, rows)
        err("%d geometries, %d calls -> %s" % (nrows, ncalls, args.profile))
    if args.sizes_csv:
        n = write_sizes_csv(args.sizes_csv, rows, args.func)
        err("size distribution (%d sizes) -> %s" % (n, args.sizes_csv))
    return 0


def cmd_show(args):
    rows = read_profile(args.input, args.func)
    if not rows:
        die("no rows for this function in %s" % args.input)
    calls = sum(r[6] for r in rows)
    print("%s: %d rows, %d calls" % (args.input, len(rows), calls))
    per_func = collections.Counter()
    for r in rows:
        per_func[r[0]] += r[6]
    for fname in sorted(per_func):
        count = per_func[fname]
        if count <= 0:
            continue
        frows = [r for r in rows if r[0] == fname]
        by_size = collections.Counter()
        by_cls = collections.Counter()
        by_align = collections.Counter()
        for _, size, sa, da, cls, attr, c in frows:
            by_size[size] += c
            by_cls[cls] += c
            by_align[(sa, da)] += c
        print("  %-8s %10d calls (%.1f%%)" % (fname, count,
                                              100.0 * count / calls))
        print("    sizes:  %s" % ", ".join(
            "%d:%.0f%%" % (s, 100.0 * c / count)
            for s, c in by_size.most_common(6)))
        print("    class:  %s" % ", ".join(
            "%s:%.0f%%" % (CLASS_NAMES.get(c, str(c)), 100.0 * v / count)
            for c, v in by_cls.most_common(6)))
        print("    align:  %s" % ", ".join(
            "(%s,%s):%.0f%%" % ("-" if s < 0 else s, "-" if d < 0 else d,
                                100.0 * c / count)
            for (s, d), c in by_align.most_common(6)))
    return 0


# ----------------------------------------------------------------------
# main
# ----------------------------------------------------------------------

def main(argv=None):
    EPILOG = """\
examples:
  memtrace.py list --pid 1234
  memtrace.py script --mode stats -o /tmp/mem.bt          # inspect first
  sudo memtrace.py attach-stats --pid 1234 -o trace.profile
  sudo memtrace.py attach-stats --pid 1234 -o t.profile --sizes-csv t.csv
  sudo memtrace.py attach-trace --pid 1234 -o trace.csv --duration 10
  sudo memtrace.py attach-stats -c ./myapp --stats -o t.profile
  memtrace.py convert --input trace.csv --profile trace.profile
  memtrace.py show --input trace.profile
  ./mb run memcpy --measure mixed --profile trace.profile

notes:
  * bpftrace needs root (or CAP_BPF/CAP_PERFMON); list/script/convert/show
    and --print-script work without privileges
  * the probe overhead dominates small calls: collect patterns with the
    tracer, then measure the profile without it
  * alignments are the pointer low bits (& --align-mask), which the
    benchmark reproduces with page-aligned buffers
"""
    # Shared options are accepted both before and after the subcommand;
    # SUPPRESS keeps the top-level value when the subcommand does not set it.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--libc", default=argparse.SUPPRESS,
                        help="libc to probe (default: the one mapped in "
                             "--pid, or this process's libc)")
    common.add_argument("--funcs", default=argparse.SUPPRESS,
                        help="comma separated functions (default: all four)")
    common.add_argument("--align-mask", type=int, default=argparse.SUPPRESS,
                        help="alignment mask applied to the pointers "
                             "(default 63)")
    ap = argparse.ArgumentParser(
        prog="memtrace.py", parents=[common],
        description="Capture the mem* access patterns of a running program "
                    "(bpftrace uprobes) and write a memtrace profile for "
                    "`mb run --measure mixed --profile`.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=EPILOG)
    sub = ap.add_subparsers(dest="cmd")

    def add_attach(name, mode):
        sp = sub.add_parser(name, parents=[common], help="attach bpftrace to a running "
                                       "process (mode: %s)" % mode)
        sp.add_argument("--pid", type=int, help="attach to this PID")
        sp.add_argument("-c", "--command", nargs=argparse.REMAINDER,
                        help="launch this command under the tracer")
        sp.add_argument("-o", "--output", help="output profile/trace file")
        sp.add_argument("--sizes-csv", help="also write a size,weight CSV "
                                            "for --dist")
        sp.add_argument("--duration", type=int, default=0,
                        help="stop after N seconds (0 = until interrupted)")
        sp.add_argument("--memcmp-window", type=int, default=0,
                        help="compare the first N bytes of the memcmp buffers "
                             "in the entry probe (0/8/16/32/64) to classify "
                             "the mismatch position; one extra read per call")
        sp.add_argument("--bpftrace", help="bpftrace binary to use")
        sp.add_argument("--print-script", action="store_true",
                        help="print the generated bpftrace program")
        sp.add_argument("--script-file", help="where to write the generated "
                                              "program (default /tmp/...)")
        sp.set_defaults(handler=cmd_attach, mode=mode)

    add_attach("attach-stats", "stats")
    add_attach("attach-trace", "trace")

    lp = sub.add_parser("list", parents=[common], help="list the probe symbols of a libc")
    lp.add_argument("--pid", type=int, help="process whose libc to inspect")
    lp.set_defaults(handler=cmd_list)

    sp = sub.add_parser("script", parents=[common], help="only generate the bpftrace program")
    sp.add_argument("--pid", type=int)
    sp.add_argument("--mode", default="stats", choices=["stats", "trace"])
    sp.add_argument("--duration", type=int, default=0)
    sp.add_argument("--memcmp-window", type=int, default=0)
    sp.add_argument("-o", "--output", help="write to this file instead of "
                                           "stdout")
    sp.set_defaults(handler=cmd_script)

    cv = sub.add_parser("convert", parents=[common], help="aggregate a trace into a profile "
                                        "and/or a size CSV")
    cv.add_argument("--input", required=True)
    cv.add_argument("--profile")
    cv.add_argument("--sizes-csv")
    cv.add_argument("--func", choices=FUNCS)
    cv.add_argument("--sample", type=int, default=1,
                    help="scale the call counts down by N (keeps the ratios)")
    cv.set_defaults(handler=cmd_convert)

    sh = sub.add_parser("show", parents=[common], help="summarise a profile")
    sh.add_argument("--input", required=True)
    sh.add_argument("--func", choices=FUNCS)
    sh.set_defaults(handler=cmd_show)

    args = ap.parse_args(argv)
    if not getattr(args, "handler", None):
        ap.print_help()
        return 2
    if not hasattr(args, "libc"):
        args.libc = None
    if not hasattr(args, "funcs"):
        args.funcs = ",".join(FUNCS)
    if not hasattr(args, "align_mask"):
        args.align_mask = 63
    args.funcs = tuple(f.strip() for f in args.funcs.split(",") if f.strip())
    for f in args.funcs:
        if f not in FUNC_ID:
            die("unknown function '%s' (choose from %s)" % (f, ", ".join(FUNCS)))
    if args.align_mask <= 0 or (args.align_mask + 1) & args.align_mask:
        die("--align-mask must be one less than a power of two (e.g. 63)")
    if args.align_mask > 0xffff:
        die("--align-mask must be <= 65535")
    if getattr(args, "memcmp_window", 0) not in (0, 8, 16, 32, 64):
        die("--memcmp-window must be 0, 8, 16, 32 or 64")
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""plot_mem.py — throughput graphs (GB/s vs size) from memlib JSON output.

Reads a glibc-benchout JSON file produced by `mb run` (or by a single
benchmark driver) and renders one PNG/SVG/PDF figure per "geometry":

  - memcpy / memmove / memcmp : one figure per (align1, align2[, dir])
    pair; every implementation is a separate colored curve;
  - memset                 : one figure per (alignment, fill-byte) pair;

Each figure plots the implementation throughput

        GB/s = length_bytes / time_ns

against the tested size.  If the tested sizes cover the L1 cache size on
the target a vertical dashed line is drawn at that spot (default: the L1
data-cache size auto-detected from sysfs; when it cannot be determined the
line is omitted unless --cache-size is given).

Requires: matplotlib.  Python >= 3.6.

Usage:
    python3 tools/plot_mem.py results/latest.json -o plots
    python3 tools/plot_mem.py results/latest.json --func memcpy \
            --match 'align1=0' --cache-size 65536
"""

import argparse
import hashlib
import json
import os
import re
import sys

try:
    import matplotlib
    matplotlib.use("Agg")           # headless: write files, no GUI
    import matplotlib.pyplot as plt
except ImportError:
    sys.stderr.write("error: matplotlib is required (pip install matplotlib)\n")
    sys.exit(2)

# Default comparison-impl color cycle, shared across figures so a given
# implementation keeps the same color everywhere.
PALETTE = [
    "#1f77b4", "#d62728", "#2ca02c", "#ff7f0e", "#9467bd", "#8c564b",
    "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
    "#393b79", "#843c39", "#637939", "#7b4173", "#5254a3",
]

# Attributes that define a "geometry": every key except the x axis
# (length) and the measurement array.
XKEY = "length"
AXIS_LABEL = "size, bytes"
YLABEL = "GB/s"


def human_size(n):
    n = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024.0:
            return "%.0f %s" % (n, unit)
        n /= 1024.0
    return "%.0f TiB" % n


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", s)
    s = s.strip("_")
    if len(s) > 80:
        h = hashlib.sha1(s.encode("utf-8")).hexdigest()[:8]
        s = s[:68] + "_" + h
    return s or "figure"


def detect_l1d():
    """Read the L1 data-cache size from sysfs (Linux), in bytes.

    Returns None when it cannot be determined.
    """
    import glob
    for idx in sorted(glob.glob("/sys/devices/system/cpu/cpu0/cache/index*")):
        try:
            with open(os.path.join(idx, "level")) as f:
                if f.read().strip() != "1":
                    continue
            with open(os.path.join(idx, "type")) as f:
                if f.read().strip() != "Data":
                    continue
            with open(os.path.join(idx, "size")) as f:
                size = f.read().strip().upper()     # e.g. "48K", "64K", "1M"
        except OSError:
            continue
        m = re.match(r"^(\d+)\s*([KM]?)$", size)
        if not m:
            return None
        mult = {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2)]
        return int(m.group(1)) * mult
    return None


def load(input_file):
    with open(input_file) as f:
        return json.load(f)


def collect(data):
    """Return {func: {attrs_key: [ (attrs, impls, rows) ]}} structure.

    For every result row we remember: the geometry attributes (all keys
    except the x key and timings), and per-implementation timing lists.
    """
    funcs = data.get("functions", {})
    out = {}
    for fname, fdata in funcs.items():
        ifuncs = fdata.get("ifuncs", [])
        groups = {}                 # attrs-key -> list of rows
        for row in fdata.get("results", []):
            if "timings" not in row or XKEY not in row:
                continue
            try:
                length = int(row[XKEY])
            except (TypeError, ValueError):
                continue
            attrs = []
            for k, v in row.items():
                if k in (XKEY, "timings"):
                    continue
                attrs.append((k, v))
            attrs.sort()
            key = tuple(attrs)
            groups.setdefault(key, []).append((length, row["timings"]))
        if groups:
            out[fname] = {"ifuncs": ifuncs, "groups": groups}
    return out


def attr_str(attrs):
    return ", ".join("%s=%s" % (k, v) for k, v in attrs)


def matches(attrs, patterns):
    if not patterns:
        return True
    text = attr_str(attrs).lower()
    return any(p.lower() in text for p in patterns)


def collect_series(groups, ifuncs, cache_size):
    """Aggregate (x -> mean ns) per implementation for one geometry."""
    series = {impl: {} for impl in ifuncs}
    for length, timings in groups:
        if length <= 0:
            continue
        for i, impl in enumerate(ifuncs):
            if i >= len(timings):
                continue
            ns = timings[i]
            if ns is None:
                continue
            try:
                ns = float(ns)
            except (TypeError, ValueError):
                continue
            if ns <= 0.0:
                continue
            bucket = series[impl].setdefault(length, [])
            bucket.append(ns)
    # median of duplicate samples, then throughput
    curves = {}
    for impl, buckets in series.items():
        xs, ys = [], []
        for length in sorted(buckets):
            vals = sorted(buckets[length])
            ns = vals[len(vals) // 2]
            xs.append(length)
            ys.append(length / ns)          # GB/s == bytes/ns
        if len(xs) >= 1 and max(xs) >= 1:
            curves[impl] = (xs, ys)
    return curves


def plot_one(func, attrs, curves, ifuncs, cache_size, cache_label,
             xscale, outpath):
    fig, ax = plt.subplots(figsize=(9, 5.5), dpi=100)
    color_of = {impl: PALETTE[i % len(PALETTE)]
                for i, impl in enumerate(ifuncs)}

    xs_all = []
    for impl in ifuncs:
        if impl not in curves:
            continue
        xs, ys = curves[impl]
        xs_all.extend(xs)
        c = color_of[impl]
        if len(xs) == 1:
            ax.plot(xs, ys, "o", color=c, label=impl)
        else:
            ax.plot(xs, ys, "-", color=c, lw=1.8, label=impl,
                    marker="o", markersize=3, markevery=0.1)

    if not xs_all:
        plt.close(fig)
        return False

    xmin, xmax = min(xs_all), max(xs_all)

    cache_artist = None
    if cache_size > 0 and xmin <= cache_size <= xmax:
        ax.axvline(cache_size, color="0.45", linestyle="--", lw=1.2,
                   zorder=0)
        cache_artist = matplotlib.lines.Line2D(
            [0], [0], color="0.45", linestyle="--", lw=1.2)

    if xscale == "log" or (xscale == "auto" and xmax >= 10 * xmin):
        ax.set_xscale("log")
    ax.set_xlabel(AXIS_LABEL)
    ax.set_ylabel(YLABEL)
    ax.set_title("%s — %s" % (func, attr_str(attrs)), fontsize=10)
    ax.grid(True, which="both", alpha=0.3)

    handles, labels = ax.get_legend_handles_labels()
    if cache_artist is not None:
        handles.append(cache_artist)
        labels.append("%s (L1)" % cache_label)
    ax.legend(handles, labels, fontsize=9, loc="best", ncol=min(3, len(handles)))

    fig.tight_layout()
    fig.savefig(outpath, format=os.path.splitext(outpath)[1][1:])
    plt.close(fig)
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Plot GB/s vs size from memlib benchout JSON. "
                    "One figure per alignment geometry, all implementations "
                    "overlaid.")
    ap.add_argument("input", help="benchout JSON file (results of mb run)")
    ap.add_argument("-o", "--outdir", default="plots",
                    help="output directory (created if needed) "
                         "(default: plots)")
    ap.add_argument("--fmt", choices=["png", "pdf", "svg"], default="png",
                    help="image format (default: png)")
    ap.add_argument("--dpi", type=int, default=150)
    ap.add_argument("--func", action="append", default=[],
                    help="only these functions (repeatable; default: all)")
    ap.add_argument("--match", action="append", default=[],
                    help="only geometries whose attr string contains this "
                         "substring, e.g. 'align1=0' (repeatable)")
    ap.add_argument("--cache-size", type=int, default=None,
                    help="L1 cache size in bytes; draw a vertical line if "
                         "the size range covers it (0 disables; default: "
                         "auto-detect from sysfs; line is omitted when the "
                         "cache size cannot be determined)")
    ap.add_argument("--xlog", action="store_true",
                    help="logarithmic x axis (default: auto when the size "
                         "range spans more than a decade)")
    ap.add_argument("--xlinear", action="store_true",
                    help="force linear x axis")
    ap.add_argument("--max-figs", type=int, default=0,
                    help="abort if more than N figures would be produced "
                         "(0 = unlimited)")
    args = ap.parse_args(argv)

    try:
        data = load(args.input)
    except (OSError, ValueError) as e:
        sys.stderr.write("error: cannot read %s: %s\n" % (args.input, e))
        return 2

    collected = collect(data)
    if not collected:
        sys.stderr.write("error: no usable results in %s\n" % args.input)
        return 2

    funcs = collected
    if args.func:
        funcs = {f: v for f, v in collected.items() if f in args.func}
        missing = [f for f in args.func if f not in collected]
        if missing:
            sys.stderr.write("warning: functions not present in input: %s\n"
                             % ", ".join(missing))

    if args.cache_size is None:
        args.cache_size = detect_l1d() or 0   # 0 = no marker (no fallback)
    os.makedirs(args.outdir, exist_ok=True)
    cache_label = human_size(args.cache_size)

    n_figs = 0
    for fname, fdata in sorted(funcs.items()):
        ifuncs = fdata["ifuncs"]
        for key in sorted(fdata["groups"]):
            attrs = key
            if not matches(attrs, args.match):
                continue
            n_figs += 1

    if n_figs == 0:
        sys.stderr.write("error: no figures match the given filters\n")
        return 2
    if args.max_figs and n_figs > args.max_figs:
        sys.stderr.write(
            "error: %d figures would be produced (limit --max-figs %d). "
            "Narrow down with --func/--match.\n" % (n_figs, args.max_figs))
        return 2

    written = 0
    for fname, fdata in sorted(funcs.items()):
        ifuncs = fdata["ifuncs"]
        for key in sorted(fdata["groups"]):
            attrs = key
            if not matches(attrs, args.match):
                continue
            curves = collect_series(fdata["groups"][key], ifuncs,
                                    args.cache_size)
            if not curves:
                continue
            base = "%s_%s" % (fname, attr_str(attrs))
            out = os.path.join(args.outdir,
                               "%s.%s" % (safe_name(base), args.fmt))
            ok = plot_one(fname, attrs, curves, ifuncs, args.cache_size,
                          cache_label,
                          "log" if args.xlog and not args.xlinear else
                          ("lin" if args.xlinear else "auto"), out)
            if ok:
                written += 1
                print(out)

    print("%d figure(s) written to %s" % (written, args.outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())

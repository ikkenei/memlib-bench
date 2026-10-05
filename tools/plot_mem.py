#!/usr/bin/env python3
"""plot_mem.py — throughput graphs (GB/s vs size) from memlib JSON output.

Reads a glibc-benchout JSON file produced by `mb run` (or by a single
benchmark driver) and renders one PNG/SVG/PDF figure per geometry, with
every implementation as a separate colored curve:

        GB/s = length_bytes / time_ns

Size labels on the x axis are shown as B, KB or MB depending on
magnitude.  When the tested sizes cover the L1 data-cache size (detected
from sysfs) a vertical dashed line is drawn at that spot.

Two figure layouts (`--mode`):

  combo (default)
      One figure per *full parameter combination*: every combination gets
      its own figure, because e.g. src=1,dst=1 is a different code path
      from src=1,dst=2, and the memset align/fill combinations are four
      different algorithms.  In each figure the curves are the
      implementations.  Example filenames:
        memcpy_src_0_dst_3_dir_1.png
        memmove_src_0_dst_7.png
        memset_align_0_fill_255.png
        memcmp_src_0_dst_3_result_-1.png

  param
      One figure per *parameter value*, medians taken over the remaining
      parameters - useful to see the sensitivity to one parameter:
        memcpy_src_0.png, memset_fill_255.png, memcmp_result_-1.png

Requires: matplotlib.  Python >= 3.6.

Usage:
    python3 tools/plot_mem.py results/latest.json -o plots
    python3 tools/plot_mem.py results/latest.json --func memcpy \\
            --match 'dst=0' --cache-size 65536
    python3 tools/plot_mem.py results/latest.json --mode param \\
            --param src --param fill
"""

import argparse
import glob
import hashlib
import json
import os
import re
import sys

try:
    import matplotlib
    matplotlib.use("Agg")           # headless: write files, no GUI
    import matplotlib.pyplot as plt
    from matplotlib.ticker import (FuncFormatter, LogLocator, NullLocator)
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

# The x axis (size) and the measurement array are not "parameters".
XKEY = "length"

# Canonical display order of the parameters (friendly names).
PARAM_ORDER = ["align1", "align2", "alignment", "char", "fill",
               "result", "dst > src", "dst>src"]

# Matrix attribute -> user-facing parameter name.
PARAM_NAMES = {
    "align1": "src",
    "align2": "dst",
    "alignment": "align",
    "char": "fill",
    "fill": "fill",
    "result": "result",
    "dst > src": "dir",
    "dst>src": "dir",
}


def human_size(n):
    n = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024.0:
            return "%.0f %s" % (n, unit)
        n /= 1024.0
    return "%.0f TiB" % n


def size_label(x, _pos=None):
    """X tick label: B / KB / MB depending on magnitude."""
    if x >= 1024 * 1024:
        return "%g MB" % (x / float(1024 * 1024))
    if x >= 1024:
        return "%g KB" % (x / 1024.0)
    return "%g B" % x


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


# ----------------------------------------------------------------------
# Input handling
# ----------------------------------------------------------------------

def load_rows(data):
    """{func: {"ifuncs": [...], "rows": [...]}} for all usable results."""
    out = {}
    for fname, fdata in data.get("functions", {}).items():
        ifuncs = fdata.get("ifuncs", [])
        rows = [r for r in fdata.get("results", [])
                if "timings" in r and XKEY in r]
        if rows and ifuncs:
            out[fname] = {"ifuncs": ifuncs, "rows": rows}
    return out


def param_dimensions(rows):
    """Ordered [(raw_attr, display_name)] seen in the result rows."""
    dims = []
    seen = set()
    for row in rows:
        for k in row:
            if k in (XKEY, "timings") or k in seen:
                continue
            seen.add(k)
            dims.append((k, PARAM_NAMES.get(k, k)))
    return dims


def param_rank(k):
    """Sort key giving the canonical parameter order."""
    try:
        return (0, PARAM_ORDER.index(k))
    except ValueError:
        return (1, k)


def display_attr(k):
    """User-facing parameter name for a raw attribute key."""
    return PARAM_NAMES.get(k, k)


def value_sort_key(v):
    try:
        return (0, float(v), "")
    except (TypeError, ValueError):
        return (1, 0.0, str(v))


def curves_for(rows, ifuncs):
    """Aggregate (median ns per length) into {impl: (xs, ys)} in GB/s."""
    buckets = [dict() for _ in ifuncs]
    for row in rows:
        try:
            length = int(row[XKEY])
        except (TypeError, ValueError):
            continue
        if length <= 0:
            continue
        timings = row["timings"]
        for i in range(min(len(ifuncs), len(timings))):
            try:
                ns = float(timings[i])
            except (TypeError, ValueError):
                continue
            if ns > 0.0:
                buckets[i].setdefault(length, []).append(ns)
    curves = {}
    for i, impl in enumerate(ifuncs):
        xs, ys = [], []
        for length in sorted(buckets[i]):
            vals = sorted(buckets[i][length])
            ns = vals[len(vals) // 2]
            xs.append(length)
            ys.append(length / ns)          # GB/s == bytes/ns
        if xs:
            curves[impl] = (xs, ys)
    return curves


# ----------------------------------------------------------------------
# Figure rendering
# ----------------------------------------------------------------------

def plot_one(title, curves, ifuncs, cache_size, cache_label, xscale,
             outpath):
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
        # Sizes are usually powers of two: base-2 log ticks read best.
        ax.set_xscale("log", base=2)
        ax.xaxis.set_major_locator(LogLocator(base=2, numticks=16))
        ax.xaxis.set_major_formatter(FuncFormatter(size_label))
        ax.xaxis.set_minor_locator(NullLocator())
    else:
        ax.xaxis.set_major_formatter(FuncFormatter(size_label))
    ax.set_xlabel("size")
    ax.set_ylabel("GB/s")
    ax.set_title(title, fontsize=10)
    ax.grid(True, which="both", alpha=0.3)

    handles, labels = ax.get_legend_handles_labels()
    if cache_artist is not None:
        handles.append(cache_artist)
        labels.append("%s (L1)" % cache_label)
    ax.legend(handles, labels, fontsize=9, loc="best",
              ncol=min(3, len(handles)))

    fig.tight_layout()
    fig.savefig(outpath, format=os.path.splitext(outpath)[1][1:])
    plt.close(fig)
    return True


# ----------------------------------------------------------------------
# Figure jobs
# ----------------------------------------------------------------------

def job_params(fname, ifuncs, rows, params):
    """One job per (parameter, value): curves = implementations."""
    dims = param_dimensions(rows)
    if params:
        wanted = set(params)
        dims = [d for d in dims if d[0] in wanted or d[1] in wanted]
    jobs = []
    for raw_key, display in dims:
        others = [d[1] for d in param_dimensions(rows) if d[0] != raw_key]
        values = sorted({row[raw_key] for row in rows}, key=value_sort_key)
        for val in values:
            sel = [r for r in rows if r[raw_key] == val]
            if not sel:
                continue
            title = "%s — %s=%s" % (fname, display, val)
            if others:
                title += "   (median over: %s)" % ", ".join(others)
            jobs.append({
                "title": title,
                "rows": sel,
                "ifuncs": ifuncs,
                "out": safe_name("%s_%s_%s" % (fname, display, val)),
                "pairs": [(display, str(val)), (raw_key, str(val))],
            })
    return jobs


def job_combo(fname, ifuncs, rows):
    """One job per full parameter combination (default layout).

    Every distinct combination of the non-length attributes gets its own
    figure: src=1,dst=1 and src=1,dst=2 are different code paths, as are
    the four align/fill combinations of memset.
    """
    groups = {}
    for row in rows:
        attrs = tuple(sorted(((k, v) for k, v in row.items()
                              if k not in (XKEY, "timings")),
                             key=lambda kv: param_rank(kv[0])))
        groups.setdefault(attrs, []).append(row)
    jobs = []
    for attrs in groups:
        pretty = ", ".join("%s=%s" % (display_attr(k), v) for k, v in attrs)
        raw = ", ".join("%s=%s" % (k, v) for k, v in attrs)
        name = "_".join("%s_%s" % (display_attr(k), v) for k, v in attrs)
        jobs.append({
            "title": "%s — %s" % (fname, pretty),
            "rows": groups[attrs],
            "ifuncs": ifuncs,
            "out": safe_name("%s_%s" % (fname, name)),
            "keys": [pretty, raw],
        })
    return jobs


def job_matches(job, patterns):
    """--match filter.

    Parameter jobs match structurally: `name=value` must equal one of the
    job's (display name | raw key, value) pairs, so 'src=0' does not match
    a `dir`/`dst > src` figure.  Geometry jobs keep substring matching on
    the full attribute string.
    """
    if "pairs" in job:
        pairs = [(n.lower(), v.lower()) for n, v in job["pairs"]]
        for pat in patterns:
            if "=" in pat:
                pn, pv = pat.split("=", 1)
                if (pn, pv) in pairs:
                    return True
            elif any(pat in n for n, _ in pairs):
                return True
        return False
    return any(p in k.lower() for k in job.get("keys", []) for p in patterns)


# ----------------------------------------------------------------------
# main
# ----------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Plot GB/s vs size from memlib benchout JSON.  By "
                    "default: one figure per parameter value (src, dst, "
                    "align, fill, result, dir), implementations overlaid "
                    "as colored curves.")
    ap.add_argument("input", help="benchout JSON file (results of mb run)")
    ap.add_argument("-o", "--outdir", default="plots",
                    help="output directory (created if needed) "
                         "(default: plots)")
    ap.add_argument("--mode", choices=["combo", "geometry",
                                        "param", "params"],
                    default="combo",
                    help="figure layout: 'combo' (default) = one figure "
                         "per full parameter combination "
                         "(src=0,dst=3,dir=1 ...); 'param' = one figure "
                         "per parameter value (median over the rest)")
    ap.add_argument("--fmt", choices=["png", "pdf", "svg"], default="png",
                    help="image format (default: png)")
    ap.add_argument("--dpi", type=int, default=150)
    ap.add_argument("--func", action="append", default=[],
                    help="only these functions (repeatable; default: all)")
    ap.add_argument("--param", action="append", default=[],
                    help="only these parameters, e.g. src, dst, align, "
                         "fill, result, dir (repeatable; default: all)")
    ap.add_argument("--match", action="append", default=[],
                    help="only figures whose parameter/geometry string "
                         "contains this substring, e.g. 'src=0' "
                         "(repeatable)")
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
        with open(args.input) as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        sys.stderr.write("error: cannot read %s: %s\n" % (args.input, e))
        return 2

    collected = load_rows(data)
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
    if not funcs:
        sys.stderr.write("error: no functions match --func\n")
        return 2

    if args.cache_size is None:
        args.cache_size = detect_l1d() or 0   # 0 = no marker (no fallback)
    cache_label = human_size(args.cache_size)

    # Build the list of figures first (so --max-figs can be checked).
    mode = "combo" if args.mode in ("combo", "geometry") else "param"
    jobs = []
    for fname, fdata in sorted(funcs.items()):
        if mode == "param":
            js = job_params(fname, fdata["ifuncs"], fdata["rows"],
                            args.param)
        else:
            js = job_combo(fname, fdata["ifuncs"], fdata["rows"])
        jobs.extend(js)

    patterns = [p.lower() for p in args.match]
    if patterns:
        jobs = [j for j in jobs if job_matches(j, patterns)]

    if not jobs:
        sys.stderr.write("error: no figures match the given filters\n")
        return 2
    if args.max_figs and len(jobs) > args.max_figs:
        sys.stderr.write(
            "error: %d figures would be produced (limit --max-figs %d). "
            "Narrow down with --func/--param/--match.\n"
            % (len(jobs), args.max_figs))
        return 2

    os.makedirs(args.outdir, exist_ok=True)
    xscale = ("log" if args.xlog and not args.xlinear else
              "lin" if args.xlinear else "auto")

    written = 0
    for j in jobs:
        curves = curves_for(j["rows"], j["ifuncs"])
        if not curves:
            continue
        out = os.path.join(args.outdir, "%s.%s" % (j["out"], args.fmt))
        if plot_one(j["title"], curves, j["ifuncs"], args.cache_size,
                    cache_label, xscale, out):
            written += 1
            print(out)

    print("%d figure(s) written to %s" % (written, args.outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())

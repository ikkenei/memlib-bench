#!/usr/bin/env python3
"""plot_mem.py — throughput graphs (GB/s vs size) from memlib JSON output.

Reads a glibc-benchout JSON file produced by `mb run` (or by a single
benchmark driver) and renders one PNG/SVG/PDF figure per geometry, with
every implementation as a separate colored curve:

        GB/s = length_bytes / time_ns

Figure layout (`--mode`):

  combo (default)   one figure per full parameter combination, because
                    src=1,dst=1 is a different code path from
                    src=1,dst=2 and the memset align/fill combinations
                    are four different algorithms.
                    Files: memcpy_src_0_dst_3_dir_1.png
  geometry          alias of `combo` (the combination of all non-length
                    attributes - the historical name).
  param             one figure per parameter value (src=0, fill=255,
                    result=-1, ...); the remaining parameters are
                    aggregated.  Files: memcpy_src_0.png
  params            alias of `param` (plural spelling).

Samples are combined with `--stats` (median/mean/min/max) and can be
shown with a shaded band (`--band`): min-max, p5-p95 or the 95% t
confidence interval.  The band is computed from the samples available
for one point: the repetitions (`--repeat N`) and, in `param` mode, the
aggregated parameter combinations.

Axes: the x axis starts at zero by default with round tick values (1/2/5
in B/KB/MB); `--xlog` switches to a base-2 log axis with power-of-two
ticks.  The y axis starts at zero; `--xlim`/`--ylim` set explicit ranges.

The L1 cache marker is only drawn when asked for: `--cache-size N` (bytes,
`0x` allowed) or `--cache-size auto` (detect from sysfs).

Requires: matplotlib.  Python >= 3.6.

Usage:
    python3 tools/plot_mem.py results/latest.json -o plots
    python3 tools/plot_mem.py results/latest.json --stats mean --band p5-p95
    python3 tools/plot_mem.py results/latest.json --func memcpy \\
            --match 'dst=0' --cache-size auto --ylim 0,120
"""

import argparse
import glob
import hashlib
import json
import math
import os
import re
import sys

try:
    import matplotlib
    matplotlib.use("Agg")           # headless: write files, no GUI
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch
    from matplotlib.ticker import FuncFormatter, LogLocator, NullLocator
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

# The x axis (size), the measurement array and the repetition/batch
# bookkeeping are not "parameters".
XKEY = "length"
META_KEYS = ("length", "timings", "run", "batch", "iters")

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

# Two-sided 95% t quantiles (small samples); the normal quantile beyond.
_T95 = {2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571, 7: 2.447,
        8: 2.365, 9: 2.306, 10: 2.262, 12: 2.201, 15: 2.131, 20: 2.086,
        25: 2.060, 30: 2.042}


# ----------------------------------------------------------------------
# Statistics
# ----------------------------------------------------------------------

def stat_value(vals, stat):
    """Central value of VALS: median (default), mean, min or max."""
    v = sorted(vals)
    if not v:
        return None
    if stat == "min":
        return v[0]
    if stat == "max":
        return v[-1]
    if stat == "mean":
        return sum(v) / float(len(v))
    return v[len(v) // 2]


def quantile(sorted_vals, p):
    """Linear-interpolated quantile of a sorted list (p in [0, 1])."""
    n = len(sorted_vals)
    if n == 0:
        return None
    if n == 1:
        return sorted_vals[0]
    idx = p * (n - 1)
    lo = int(math.floor(idx))
    hi = min(lo + 1, n - 1)
    frac = idx - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def t95(n):
    if n < 2:
        return None
    if n in _T95:
        return _T95[n]
    if n >= 30:
        return 1.96
    keys = [k for k in _T95 if k < n]
    return _T95[max(keys)] if keys else _T95[2]


def band_bounds(vals, band):
    """Lower/upper bound (in the sample unit) of the requested band."""
    if band == "none" or len(vals) < 2:
        return None
    s = sorted(vals)
    if band == "min-max":
        return s[0], s[-1]
    if band == "p5-p95":
        return quantile(s, 0.05), quantile(s, 0.95)
    if band == "ci95":
        n = len(s)
        m = sum(s) / float(n)
        var = sum((x - m) ** 2 for x in s) / float(n - 1)
        half = (t95(n) or 1.96) * math.sqrt(var / n)
        return max(0.0, m - half), m + half
    return None


# ----------------------------------------------------------------------
# Formatting helpers
# ----------------------------------------------------------------------

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


def nice_ticks(xmax, target=8):
    """Round tick positions (in bytes) from 0 up to XMAX.

    The step is a power of two of the display unit (B, KB or MB), which
    is the natural scale for buffer sizes: 0, 8, 16 ... 64 B or
    0, 16 KB, 32 KB ... 128 KB - never something like 39.0625 KB.
    """
    if xmax is None or xmax <= 0:
        return []
    unit = 1.0
    for candidate in (1024.0 * 1024.0, 1024.0, 1.0):
        if xmax / candidate >= 2.0:
            unit = candidate
            break
    top = xmax / unit
    raw = top / float(target)
    if raw <= 1.0:
        step_units = 1.0
    else:
        step_units = 2.0 ** math.ceil(math.log2(raw))
    step_bytes = max(1.0, step_units * unit)
    ticks, t, guard = [], 0.0, 0
    while t <= xmax + step_bytes * 0.5 and guard < 500:
        ticks.append(t)
        t += step_bytes
        guard += 1
    return ticks


def parse_range(text, what):
    """'LO' or 'LO,HI' -> (lo, hi); empty parts become None."""
    parts = str(text).split(",")
    if len(parts) > 2:
        sys.stderr.write("error: %s expects 'LO' or 'LO,HI'\n" % what)
        raise SystemExit(2)
    out = []
    for p in parts:
        p = p.strip()
        if p == "":
            out.append(None)
            continue
        try:
            v = float(p)
        except ValueError:
            sys.stderr.write("error: %s: bad number %r\n" % (what, p))
            raise SystemExit(2)
        out.append(v)
    while len(out) < 2:
        out.append(None)
    return out[0], out[1]


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", s)
    s = s.strip("_")
    if len(s) > 80:
        h = hashlib.sha1(s.encode("utf-8")).hexdigest()[:8]
        s = s[:68] + "_" + h
    return s or "figure"


def detect_l1d():
    """Read the L1 data-cache size from sysfs (Linux), in bytes.

    Only used with an explicit `--cache-size auto`; returns None when it
    cannot be determined.
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


def resolve_cache_size(spec):
    """None/0/off -> (0, None); 'auto' -> detect; N -> N bytes."""
    if spec is None:
        return 0, None
    s = str(spec).strip().lower()
    if s in ("", "0", "none", "off", "no"):
        return 0, None
    if s in ("auto", "l1"):
        n = detect_l1d()
        if not n:
            sys.stderr.write("warning: --cache-size auto: cannot detect the "
                             "L1 size; no cache marker drawn\n")
            return 0, None
        return n, "%s (L1)" % human_size(n)
    try:
        n = int(s, 0)
    except ValueError:
        sys.stderr.write("error: --cache-size expects bytes, 'auto' or 0 "
                         "(got %r)\n" % spec)
        raise SystemExit(2)
    return (n, human_size(n)) if n > 0 else (0, None)


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
            if k in META_KEYS or k in seen:
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


def curves_for(rows, ifuncs, stat, band):
    """Aggregate the rows into {impl: {"x", "y", "lo", "hi"}} in GB/s.

    For every size the samples (repetitions, and in `param` mode the
    aggregated parameter values) are reduced with STAT; the band, when
    requested, comes from the same samples.
    """
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
        c = {"x": [], "y": [], "lo": [], "hi": []}
        for length in sorted(buckets[i]):
            vals = buckets[i][length]
            ns = stat_value(vals, stat)
            if not ns or ns <= 0:
                continue
            c["x"].append(length)
            c["y"].append(length / ns)          # GB/s == bytes/ns
            bounds = band_bounds(vals, band)
            if bounds is not None:
                lo_ns, hi_ns = bounds
                c["lo"].append(length / hi_ns if hi_ns > 0 else 0.0)
                c["hi"].append(length / lo_ns if lo_ns > 0 else 0.0)
            else:
                c["lo"].append(None)
                c["hi"].append(None)
        if c["x"]:
            curves[impl] = c
    return curves


def runs_of(rows):
    """Number of repetitions in these rows (1 when not repeated)."""
    runs = 1
    for r in rows:
        try:
            runs = max(runs, int(r.get("run", 0)) + 1)
        except (TypeError, ValueError):
            pass
    return runs


# ----------------------------------------------------------------------
# Figure rendering
# ----------------------------------------------------------------------

def apply_axes(ax, xs_all, args, cache_bytes):
    xmin, xmax = min(xs_all), max(xs_all)
    xlo, xhi = parse_range(args.xlim, "--xlim") if args.xlim else (None, None)
    visible_max = xhi if (xhi is not None and xhi > 0) else xmax

    if args.xlog:
        # Powers of two only, so the labels stay round in KB/MB.
        ax.set_xscale("log", base=2)
        ax.xaxis.set_major_locator(LogLocator(base=2, subs=(1.0,),
                                              numticks=24))
        ax.xaxis.set_minor_locator(NullLocator())
        left = xlo if (xlo is not None and xlo > 0) else None
        if left is None:
            left = max(1.0, min(xs_all))
        ax.set_xlim(left=left, right=xhi)
    else:
        ax.set_xlim(left=0.0 if xlo is None else xlo, right=xhi)
        ticks = nice_ticks(visible_max)
        if ticks:
            ax.set_xticks(ticks)
    ax.xaxis.set_major_formatter(FuncFormatter(size_label))

    ylo, yhi = parse_range(args.ylim, "--ylim") if args.ylim else (None, None)
    ax.set_ylim(bottom=0.0 if ylo is None else ylo, top=yhi)

    ax.set_xlabel("size")
    ax.set_ylabel("GB/s")

    cache_artist = None
    if cache_bytes > 0 and xmin <= cache_bytes <= visible_max:
        ax.axvline(cache_bytes, color="0.45", linestyle="--", lw=1.2, zorder=0)
        cache_artist = Line2D([0], [0], color="0.45", linestyle="--", lw=1.2)
    return cache_artist


def plot_one(title, curves, ifuncs, args, cache_bytes, cache_label,
             band, outpath):
    fig, ax = plt.subplots(figsize=(9, 5.5), dpi=100)
    color_of = {impl: PALETTE[i % len(PALETTE)]
                for i, impl in enumerate(ifuncs)}

    xs_all = []
    band_handles = []
    for impl in ifuncs:
        if impl not in curves:
            continue
        c = curves[impl]
        xs, ys = c["x"], c["y"]
        xs_all.extend(xs)
        color = color_of[impl]
        if len(xs) == 1:
            ax.plot(xs, ys, "o", color=color, label=impl)
        else:
            ax.plot(xs, ys, "-", color=color, lw=1.8, label=impl,
                    marker="o", markersize=3, markevery=0.1)
        if band != "none" and any(v is not None for v in c["lo"]):
            lo = [v if v is not None else y for v, y in zip(c["lo"], ys)]
            hi = [v if v is not None else y for v, y in zip(c["hi"], ys)]
            ax.fill_between(xs, lo, hi, color=color, alpha=0.18,
                            linewidth=0, zorder=1)
            if len(band_handles) < 4:
                band_handles.append(Patch(facecolor=color, alpha=0.18,
                                          label="%s %s" % (impl, band)))

    if not xs_all:
        plt.close(fig)
        return False

    cache_artist = apply_axes(ax, xs_all, args, cache_bytes)
    ax.set_title(title, fontsize=10)
    ax.grid(True, which="both", alpha=0.3)

    handles, labels = ax.get_legend_handles_labels()
    handles += band_handles
    labels += [h.get_label() for h in band_handles]
    if band != "none" and not band_handles:
        handles.append(Patch(facecolor="0.6", alpha=0.3,
                             label="%s band" % band))
        labels.append("%s band" % band)
    if cache_artist is not None:
        handles.append(cache_artist)
        labels.append(cache_label)
    ax.legend(handles, labels, fontsize=9, loc="best",
              ncol=min(3, max(1, len(handles))))

    fig.tight_layout()
    fig.savefig(outpath, format=os.path.splitext(outpath)[1][1:])
    plt.close(fig)
    return True


# ----------------------------------------------------------------------
# Figure jobs
# ----------------------------------------------------------------------

def job_params(fname, ifuncs, rows, params, stat):
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
                title += "   (%s over: %s)" % (stat, ", ".join(others))
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
                              if k not in META_KEYS),
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

MODE_HELP = """figure layout (default: combo):
  combo     one figure per full parameter combination - every
            combination of the non-length attributes
            (src=0,dst=3,dir=1; memset align=0,fill=255; ...)
  geometry  alias of 'combo' (the combination is the "geometry" of the
            case); kept for compatibility with older commands
  param     one figure per parameter value (src=0, dst=7, fill=255,
            result=-1, dir=1, ...); the remaining parameters are
            aggregated with --stats (and shown as a --band spread)
  params    alias of 'param' (plural spelling)"""


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Plot GB/s vs size from memlib benchout JSON: one "
                    "figure per parameter combination (or per parameter "
                    "value), implementations overlaid as colored curves.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Statistics: in `combo` mode the samples of one point are "
               "the repetitions (--repeat); in `param` mode they are the "
               "repetitions plus the aggregated parameter combinations.")
    ap.add_argument("input", help="benchout JSON file (results of mb run)")
    ap.add_argument("-o", "--outdir", default="plots",
                    help="output directory (created if needed) "
                         "(default: plots)")
    ap.add_argument("--mode", choices=["combo", "geometry",
                                       "param", "params"],
                    default="combo", help=MODE_HELP)
    ap.add_argument("--fmt", choices=["png", "pdf", "svg"], default="png",
                    help="image format (default: png)")
    ap.add_argument("--dpi", type=int, default=150)
    ap.add_argument("--func", action="append", default=[],
                    help="only these functions (repeatable; default: all)")
    ap.add_argument("--param", action="append", default=[],
                    help="with --mode param: only these parameters, e.g. "
                         "src, dst, align, fill, result, dir (repeatable)")
    ap.add_argument("--match", action="append", default=[],
                    help="only figures whose parameter/geometry string "
                         "contains this substring, e.g. 'src=0' "
                         "(repeatable)")
    ap.add_argument("--stats", "--stat", dest="stats", default="median",
                    choices=["median", "mean", "min", "max"],
                    help="value plotted for a point (default: median)")
    ap.add_argument("--band", default="none",
                    choices=["none", "min-max", "p5-p95", "ci95"],
                    help="shaded band around the curves (default: none): "
                         "min-max range, 5th-95th percentile or the 95%% "
                         "t confidence interval")
    ap.add_argument("--xlim", default=None, metavar="LO[,HI]",
                    help="x axis range in bytes (default: 0 .. max size)")
    ap.add_argument("--ylim", default=None, metavar="LO[,HI]",
                    help="y axis range in GB/s (default: 0 .. data max)")
    ap.add_argument("--cache-size", default=None, metavar="N|auto",
                    help="draw a vertical cache marker: a byte count "
                         "(0x.. allowed) or 'auto' to read the L1 data "
                         "cache size from sysfs; no marker by default")
    ap.add_argument("--xlog", action="store_true",
                    help="logarithmic (base 2) x axis with power-of-two "
                         "ticks; the default is a linear axis from zero")
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

    cache_bytes, cache_label = resolve_cache_size(args.cache_size)

    # Build the list of figures first (so --max-figs can be checked).
    mode = "combo" if args.mode in ("combo", "geometry") else "param"
    jobs = []
    for fname, fdata in sorted(funcs.items()):
        if mode == "param":
            js = job_params(fname, fdata["ifuncs"], fdata["rows"],
                            args.param, args.stats)
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

    written = 0
    for j in jobs:
        curves = curves_for(j["rows"], j["ifuncs"], args.stats, args.band)
        if not curves:
            continue
        runs = runs_of(j["rows"])
        descriptor = args.stats
        if runs > 1:
            descriptor += " of %d runs" % runs
        if args.band != "none":
            descriptor += ", %s band" % args.band
        title = "%s\nGB/s, %s" % (j["title"], descriptor)
        out = os.path.join(args.outdir, "%s.%s" % (j["out"], args.fmt))
        if plot_one(title, curves, j["ifuncs"], args, cache_bytes,
                    cache_label, args.band, out):
            written += 1
            print(out)

    print("%d figure(s) written to %s" % (written, args.outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())

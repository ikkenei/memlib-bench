#!/usr/bin/env python3
"""mb — CLI for the memlib benchmark suite.

Benchmark and validate custom implementations of memcpy / memmove /
memset / memcmp against the system libc and a portable generic C
reference, using the glibc benchtests methodology (guard-page buffers,
alignment/length matrices, glibc-compatible JSON output).

Commands
--------
  mb build [--cross [PREFIX]] [--arch FLAGS] [--jobs N]
  mb list
  mb run   [OPTIONS] FUNC...
  mb check [OPTIONS] FUNC...
  mb plot        RESULT.json [OPTIONS]   # GB/s vs size (tools/plot_mem.py)
  mb plot-glibc  RESULT.json [OPTIONS]   # glibc plot_strings.py (timings)

FUNC is one of: memcpy memmove memset memcmp  (or "all").
"""

import argparse
import importlib.util
import json
import math
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")
IMPLDIR = os.path.join(BUILD, "impls")
RESULTS = os.path.join(ROOT, "results")
SRC = os.path.join(ROOT, "src")
IMPLS_SRC = os.path.join(ROOT, "impls")
TOOLS = os.path.join(ROOT, "tools")
SCHEMA = os.path.join(TOOLS, "benchout_strings.schema.json")

FUNCS = ["memcpy", "memmove", "memset", "memcmp"]
DRIVER = {f: os.path.join(BUILD, "bench_" + f) for f in FUNCS}

# Default comparison attributes used for the detailed (compare_strings)
# tables, matching what one would use with glibc's own tooling.
FUNC_ATTRS = {
    "memcpy":  "length,align1,align2",
    "memmove": "length,align1,align2",
    "memset":  "length,alignment,char",
    "memcmp":  "length,align1,align2,result",
}


def die(msg, code=2):
    sys.stderr.write("mb: error: %s\n" % msg)
    sys.exit(code)


def err(msg):
    sys.stderr.write("mb: %s\n" % msg)


def _make(argv):
    return subprocess.call(["make", "-C", ROOT] + list(argv))


def rel(path):
    """Path relative to the project root (as the Makefile spells targets)."""
    return os.path.relpath(path, ROOT)


def ensure_drivers(no_build=False):
    """Make sure the four benchmark drivers exist and are up to date."""
    targets = [rel(DRIVER[f]) for f in FUNCS]      # Makefile uses rel paths
    missing = [t for t in targets if not os.path.exists(os.path.join(ROOT, t))]
    if not missing:
        if no_build:
            return
        # make -q: 0 = up to date, 1 = needs rebuild, 2 = error
        rc = subprocess.call(["make", "-C", ROOT, "-q"] + targets,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        if rc == 0:
            return
        why = "out of date"
    else:
        why = "missing"
    if no_build:
        die("benchmark drivers are %s; run `mb build` "
            "(auto-build disabled by --no-build)" % why)
    err("benchmark drivers %s; rebuilding (make)" % why)
    if subprocess.call(["make", "-C", ROOT] + targets) != 0:
        die("failed to build the benchmark drivers")
    still = [t for t in targets if not os.path.exists(os.path.join(ROOT, t))]
    if still:
        die("benchmark drivers still missing: %s" % ", ".join(still))


# ----------------------------------------------------------------------
# Implementation resolution
# ----------------------------------------------------------------------

def source_of(spec):
    """Return the impls/ source file for a bare name, or None."""
    for ext in (".c", ".S"):
        p = os.path.join(IMPLS_SRC, spec + ext)
        if os.path.exists(p):
            return p
    return None


def _impl_stale(so_path, spec):
    """True when the impls/ source is newer than its .so."""
    stem = None
    if spec.endswith((".c", ".S")):
        stem = os.path.basename(spec)[:-2]
    elif "/" not in spec and not spec.endswith(".so"):
        stem = spec
    if stem is None:
        return False
    for ext in (".c", ".S"):
        src = os.path.join(IMPLS_SRC, stem + ext)
        if os.path.exists(src) and os.path.exists(so_path):
            if os.path.getmtime(src) > os.path.getmtime(so_path):
                return True
    return False


def resolve_impls(specs):
    """Map user specs to .so paths, building impls/ sources when stale.

    A spec may carry an explicit label:  PATH=NAME   (NAME overrides the
    display name derived from the file name).  The driver understands the
    same PATH[=LABEL] syntax.
    """
    out = []
    for spec in specs:
        label = None
        if "=" in spec:
            head, _, tail = spec.rpartition("=")
            if os.path.exists(head) or source_of(head) or \
               os.path.exists(os.path.join(IMPLDIR, head + ".so")):
                spec, label = head, tail
        # ... resolve `spec` to a .so path as before ...
        if "/" in spec:
            # explicit path: accept an existing .so or an impls/ source
            if spec.endswith(".so") and os.path.exists(spec):
                path = spec
            else:
                if spec.endswith((".c", ".S")) and os.path.exists(spec):
                    path = None      # build this source below
                else:
                    die("implementation not found: %s" % spec)
        elif spec.endswith(".so"):
            if os.path.exists(spec):
                path = spec
            else:
                die("implementation not found: %s" % spec)
        else:
            path = os.path.join(IMPLDIR, spec + ".so")
        if path is None:
            src = spec
            path = os.path.join(IMPLDIR, os.path.basename(src)[:-2] + ".so")
        if not os.path.exists(path) or _impl_stale(path, spec):
            src = None
            if "/" not in spec and not spec.endswith(".so"):
                src = source_of(spec)
            elif spec.endswith((".c", ".S")):
                src = spec
            if src is None:
                if not os.path.exists(path):
                    die("implementation %r not built (looked for %s); "
                        "run `mb build` first" % (spec, path))
            else:
                target = rel(os.path.join(IMPLDIR,
                                          os.path.basename(src)[:-2] + ".so"))
                if _make([target]) != 0:
                    die("failed to build %s (see make output)" % src)
        if not os.path.exists(path):
            die("impl .so still missing after build: %s" % path)
        out.append(path if label is None else "%s=%s" % (path, label))
    return out


# ----------------------------------------------------------------------
# Drivers
# ----------------------------------------------------------------------

def driver_cmd(fn, args, impls, check):
    cmd = [DRIVER[fn]]
    if check:
        cmd.append("--check")
    for opt, attr in (("--iters", "iters"), ("--budget", "budget"),
                      ("--max-len", "max_len"), ("--seed", "seed"),
                      ("--min-iters", "min_iters"),
                      ("--repeat", "repeat"),
                      ("--measure", "measure"), ("--batch", "batch"),
                      ("--iters-mode", "iters_mode"),
                      ("--epsilon", "epsilon"), ("--scaling", "scaling"),
                      ("--initial-iters", "initial_iters"),
                      ("--min-samples", "min_samples"),
                      ("--max-samples", "max_samples"),
                      ("--min-duration", "min_duration"),
                      ("--max-duration", "max_duration"),
                      ("--mismatch-at", "mismatch_at")):
        v = getattr(args, attr, None)
        if v:
            cmd += [opt, str(v)]
    if getattr(args, "quick", False):
        cmd.append("--quick")
    if getattr(args, "no_libc", False):
        cmd.append("--no-libc")
    if getattr(args, "generic", False):
        cmd.append("--generic")
    for p in impls:
        cmd += ["--impl", p]
    m = getattr(args, "matrix", None)
    if m:
        cmd += ["--matrix", m]
    return cmd


def run_function(fn, args, impls, check):
    """Run one driver.  Returns (rc, stdout_bytes)."""
    env = dict(os.environ)
    if getattr(args, "no_warmup", False):
        env["MB_NO_WARMUP"] = "1"
    p = subprocess.run(driver_cmd(fn, args, impls, check),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       env=env)
    if p.stderr:
        sys.stderr.write(p.stderr.decode("utf-8", "replace"))
    return p.returncode, p.stdout


def parse_funcs(names):
    if not names:
        return FUNCS[:]
    out = []
    for n in names:
        if n == "all":
            out.extend(FUNCS)
        elif n in FUNCS:
            out.append(n)
        else:
            die("unknown function %r (use one of: %s)"
                % (n, ", ".join(FUNCS)))
    # keep order, de-duplicate
    return list(dict.fromkeys(out))


# ----------------------------------------------------------------------
# JSON merge & validation
# ----------------------------------------------------------------------

def merge_json(payloads):
    timing = None
    functions = {}
    for fn, blob in payloads.items():
        if blob is None:
            continue
        if timing is None:
            timing = blob.get("timing_type", "hp_timing")
        fns = blob.get("functions")
        if not isinstance(fns, dict):
            die("driver output for %s has unexpected shape" % fn)
        for key, val in fns.items():
            if key in functions:
                err("duplicate function %r in outputs, keeping first" % key)
                continue
            functions[key] = val
    return {"timing_type": timing, "functions": functions}


def save_result(data, label):
    os.makedirs(RESULTS, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = "%s-%s.json" % (stamp, label)
    path = os.path.join(RESULTS, name)
    with open(path, "w") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    latest = os.path.join(RESULTS, "latest.json")
    try:
        if os.path.islink(latest) or os.path.exists(latest):
            os.remove(latest)
        os.symlink(name, latest)
    except OSError:
        pass
    return path


def validate_schema(data):
    try:
        import jsonschema
    except ImportError:
        return  # schema validation is best-effort
    with open(SCHEMA) as f:
        schema = json.load(f)
    try:
        jsonschema.validate(data, schema)
    except Exception as e:
        err("output does not validate against %s: %s" % (SCHEMA, e))


# ----------------------------------------------------------------------
# Tables
# ----------------------------------------------------------------------

def have_module(name):
    return importlib.util.find_spec(name) is not None


def pick_base(ifuncs, requested):
    if requested:
        if requested in ifuncs:
            return requested
        err("--base %r not among implementations %s; using first"
            % (requested, ", ".join(ifuncs)))
    if "libc" in ifuncs:
        return "libc"
    if "generic" in ifuncs:
        return "generic"
    return ifuncs[0]


def pct(v, base):
    if base <= 0:
        return ""
    d = (v - base) * 100.0 / base
    return "%+6.2f%%" % d


# Parameter names shared with tools/plot_mem.py.
PARAM_DISPLAY = {
    "align1": "src", "align2": "dst", "alignment": "align",
    "char": "fill", "fill": "fill", "result": "result",
    "dst > src": "dir", "dst>src": "dir",
}
PARAM_ORDER = ["align1", "align2", "alignment", "char", "fill",
               "result", "dst > src", "dst>src"]
# Attributes that are not part of a row's identity: the x axis, the
# measurements and the bookkeeping of repeats/batches.
SUMMARY_META = ("length", "timings", "run", "batch", "iters")
DEFAULT_REGIONS = "16,64,512,4096,65536"


def _param_rank(k):
    try:
        return (0, PARAM_ORDER.index(k))
    except ValueError:
        return (1, k)


def _fmt_size(n):
    if n >= 1024 * 1024 and n % (1024 * 1024) == 0:
        return "%dM" % (n // (1024 * 1024))
    if n >= 1024 and n % 1024 == 0:
        return "%dK" % (n // 1024)
    return "%d" % n


def _combo_of(row):
    return tuple(sorted(((k, v) for k, v in row.items()
                         if k not in SUMMARY_META),
                        key=lambda kv: _param_rank(kv[0])))


def _combo_label(combo):
    return " ".join("%s=%s" % (PARAM_DISPLAY.get(k, k), v)
                    for k, v in combo)


def _combo_sort_key(combo):
    return tuple((_param_rank(k), str(v)) for k, v in combo)


def _region_bounds(spec):
    try:
        bounds = sorted({int(x) for x in str(spec).replace(" ", "").split(",")
                         if x != ""})
    except ValueError:
        die("--regions expects comma-separated byte sizes, e.g. %s"
            % DEFAULT_REGIONS)
    if not bounds or bounds[0] < 1:
        die("--regions must contain positive byte sizes")
    return bounds


def _region_of(length, bounds):
    for i, b in enumerate(bounds):
        if length <= b:
            return i
    return len(bounds)


def _region_labels(bounds):
    labels = []
    prev = 0
    for b in bounds:
        lo = prev + 1
        labels.append(("\u2264%s" % _fmt_size(b)) if lo <= 1
                      else ("%s-%s" % (_fmt_size(lo), _fmt_size(b))))
        prev = b
    labels.append(">%s" % _fmt_size(bounds[-1]))
    return labels


def _central(vals, stat):
    v = sorted(vals)
    if stat == "min":
        return v[0]
    if stat == "max":
        return v[-1]
    if stat == "mean":
        return sum(v) / float(len(v))
    return v[len(v) // 2]


def _cov(vals):
    """Coefficient of variation in percent (needs >= 2 samples)."""
    if len(vals) < 2:
        return None
    m = sum(vals) / float(len(vals))
    if m <= 0:
        return None
    var = sum((x - m) ** 2 for x in vals) / float(len(vals))
    return (var ** 0.5) / m * 100.0


# Two-sided 95% t quantiles for small samples (else the normal 1.96).
_T95 = {2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571, 7: 2.447,
        8: 2.365, 9: 2.306, 10: 2.262, 12: 2.201, 15: 2.131, 20: 2.086,
        25: 2.060, 30: 2.042}


def _t95(n):
    if n < 2:
        return None
    if n in _T95:
        return _T95[n]
    if n >= 30:
        return 1.96
    keys = [k for k in _T95 if k < n]
    return _T95[max(keys)] if keys else _T95[2]


def _ci95(vals):
    """Relative half-width of the 95% confidence interval, in percent."""
    n = len(vals)
    t = _t95(n)
    if t is None:
        return None
    m = sum(vals) / float(n)
    if m <= 0:
        return None
    var = sum((x - m) ** 2 for x in vals) / float(n - 1)
    return t * (var ** 0.5) / (float(n) ** 0.5) / m * 100.0


def _collect_samples(func_data):
    """samples[(combo, length)][impl] = [ns, ...] plus metadata."""
    ifuncs = func_data.get("ifuncs", [])
    samples = {}
    lengths = set()
    combos = set()
    runs = 1
    for row in func_data.get("results", []):
        if "timings" not in row or "length" not in row:
            continue
        try:
            length = int(row["length"])
        except (TypeError, ValueError):
            continue
        if length <= 0:
            continue                        # no throughput for size 0
        combo = _combo_of(row)
        combos.add(combo)
        lengths.add(length)
        bucket = samples.setdefault((combo, length), {})
        for i, ns in enumerate(row["timings"][:len(ifuncs)]):
            try:
                ns = float(ns)
            except (TypeError, ValueError):
                continue
            if ns > 0:
                bucket.setdefault(i, []).append(ns)
        try:
            runs = max(runs, int(row.get("run", 0)) + 1)
        except (TypeError, ValueError):
            pass
    return ifuncs, samples, sorted(lengths), sorted(combos, key=_combo_sort_key), runs


def summary_by_combo(func_data, fn, args):
    """Rows: parameter combinations.  Columns: size regions (GB/s)."""
    ifuncs, samples, lengths, combos, runs = _collect_samples(func_data)
    if not ifuncs or not combos:
        return
    base = pick_base(ifuncs, getattr(args, "base", None))
    bounds = _region_bounds(getattr(args, "regions", None) or DEFAULT_REGIONS)
    labels = _region_labels(bounds)
    stat = getattr(args, "stats", None) or "median"
    match = getattr(args, "match", None)
    b_idx = ifuncs.index(base)

    by_region = {}
    for length in lengths:
        by_region.setdefault(_region_of(length, bounds), []).append(length)
    regions = [r for r in sorted(by_region) if r < len(labels)]
    if not regions:
        return

    # cell[(combo, impl, region)] = geo-mean GB/s; noise[...] = median CoV
    cell = {}
    noise = {}
    cis = {}
    for combo in combos:
        for i, impl in enumerate(ifuncs):
            for r in regions:
                rates, covs, civs = [], [], []
                for length in by_region[r]:
                    vals = samples.get((combo, length), {}).get(i)
                    if not vals:
                        continue
                    c = _central(vals, stat)
                    if c > 0:
                        rates.append(length / c)
                    cv = _cov(vals)
                    if cv is not None:
                        covs.append(cv)
                    ci = _ci95(vals)
                    if ci is not None:
                        civs.append(ci)
                if rates:
                    g = math.exp(sum(math.log(x) for x in rates) / len(rates))
                    cell[(combo, i, r)] = g
                if covs:
                    covs.sort()
                    noise[(combo, i, r)] = covs[len(covs) // 2]
                if civs:
                    civs.sort()
                    cis[(combo, i, r)] = civs[len(civs) // 2]

    header = [" " * 2 + "%-*s" % (max(14, max(len(n) for n in ifuncs) + 2),
                                  "combinations / GB/s")
              + "".join("%9s" % labels[r] for r in regions)]
    print("Function: %s" % fn)
    print("cells: geo-mean GB/s over the sizes in the region "
          "(statistic: %s%s; %% vs %s)"
          % (stat, " of %d runs" % runs if runs > 1 else "", base))
    print(header[0])
    shown = 0
    for combo in combos:
        label = _combo_label(combo)
        if match and match.lower() not in label.lower():
            continue
        shown += 1
        print("  %s" % label)
        for i, impl in enumerate(ifuncs):
            cells = []
            for r in regions:
                v = cell.get((combo, i, r))
                if v is None:
                    cells.append("%9s" % ".")
                elif i == b_idx:
                    cells.append("%9.2f" % v)
                else:
                    b = cell.get((combo, b_idx, r))
                    if b is None or b <= 0:
                        cells.append("%9.2f" % v)
                    else:
                        cells.append("%8.1f%%" % ((v - b) * 100.0 / b))
            print("    %-14s %s" % (impl, "".join(cells)))
    if shown == 0:
        print("  (no combinations match --match %r)" % match)

    if runs > 1:
        line = []
        for i, impl in enumerate(ifuncs):
            covs = sorted(v for (c, j, r), v in noise.items() if j == i)
            civs = sorted(v for (c, j, r), v in cis.items() if j == i)
            bits = []
            if covs:
                bits.append("CoV median %.1f%%, max %.1f%%"
                            % (covs[len(covs) // 2], covs[-1]))
            if civs:
                bits.append("95%% CI \u00b1%.1f%%" % civs[len(civs) // 2])
            if bits:
                line.append("%s: %s" % (impl, ", ".join(bits)))
        if line:
            print("  repeat statistics (%d runs, median over cells): %s"
                  % (runs, "; ".join(line)))

    if getattr(args, "gmean", False):
        print("  geo-mean over all regions:")
        for i, impl in enumerate(ifuncs):
            vals = [v for (c, j, r), v in cell.items() if j == i]
            if not vals:
                continue
            g = math.exp(sum(math.log(x) for x in vals) / len(vals))
            if i == b_idx:
                print("    %-14s %8.2f GB/s" % (impl, g))
            else:
                bvals = [v for (c, j, r), v in cell.items() if j == b_idx]
                b = math.exp(sum(math.log(x) for x in bvals) / len(bvals))
                print("    %-14s %+7.1f%%" % (impl, (g - b) * 100.0 / b))
    print()


def summary_by_size(func_data, fn, base, gmean_only, match=None):
    """Rows: tested sizes, aggregated over all parameter combinations."""
    ifuncs = func_data.get("ifuncs", [])
    results = func_data.get("results", [])
    if not results:
        return
    base = pick_base(ifuncs, base)
    idx = {n: i for i, n in enumerate(ifuncs)}

    by_len = {}
    runs = 1
    for row in results:
        if "timings" not in row:
            continue
        length = row.get("length", row.get("alignment", 0))
        by_len.setdefault(int(length), []).append(row["timings"])
        try:
            runs = max(runs, int(row.get("run", 0)) + 1)
        except (TypeError, ValueError):
            pass

    print("Function: %s" % fn)
    print("cells: median ns per call over all combinations "
          "(%s)" % ("median of %d runs" % runs if runs > 1 else "single run"))
    print("%9s %9s | %s" % ("length", "B/call",
                            " ".join("%14s" % n for n in ifuncs)))
    gsum = [0.0] * len(ifuncs)
    gcnt = 0
    for length in sorted(by_len):
        if match and match not in str(length):
            continue
        samples = by_len[length]
        med = []
        for i in range(len(ifuncs)):
            vals = sorted(s[i] for s in samples if i < len(s) and s[i] > 0)
            med.append(vals[len(vals) // 2] if vals else float("nan"))
        for i, v in enumerate(med):
            if not math.isnan(v) and v > 0:
                gsum[i] += math.log(v)
        gcnt += 1
        cells = []
        for i, v in enumerate(med):
            if math.isnan(v):
                cells.append("%14s" % "-")
            else:
                cell = "%10.2f" % v
                if ifuncs[i] != base and not math.isnan(med[idx[base]]):
                    cell += " %s" % pct(v, med[idx[base]])
                cells.append("%14s" % cell)
        print("%9d %9d | %s" % (length, length, " ".join(cells)))
    if gcnt and not gmean_only:
        print("-" * (22 + 15 * len(ifuncs)))
        cells = []
        for i, v in enumerate(gsum):
            gm = math.exp(v / gcnt) if not math.isnan(v) else float("nan")
            cell = "%10.2f" % gm
            if ifuncs[i] != base and not math.isnan(gm) and gm > 0:
                cell += " %s" % pct(gm, math.exp(gsum[idx[base]] / gcnt))
            cells.append("%14s" % cell)
        print("%9s %9s | %s" % ("geo-mean", "", " ".join(cells)))

    if runs > 1:
        line = []
        for i, impl in enumerate(ifuncs):
            covs, civs = [], []
            for samples_i in by_len.values():
                vals = sorted(s[i] for s in samples_i
                              if i < len(s) and s[i] > 0)
                if len(vals) < 2:
                    continue
                cv = _cov(vals)
                ci = _ci95(vals)
                if cv is not None:
                    covs.append(cv)
                if ci is not None:
                    civs.append(ci)
            bits = []
            if covs:
                covs.sort()
                bits.append("CoV median %.1f%%, max %.1f%%"
                            % (covs[len(covs) // 2], covs[-1]))
            if civs:
                civs.sort()
                bits.append("95%% CI \u00b1%.1f%%" % civs[len(civs) // 2])
            if bits:
                line.append("%s: %s" % (impl, ", ".join(bits)))
        if line:
            print("repeat statistics (%d runs, median over sizes): %s"
                  % (runs, "; ".join(line)))
    print()


def full_table(data, funcs, args):
    """Delegate to the vendored glibc compare_strings.py."""
    if not have_module("jsonschema"):
        err("'full' tables need the python 'jsonschema' module "
            "(pip install jsonschema); using the summary view instead")
        for fn in funcs:
            if fn in data.get("functions", {}):
                summary_table(data["functions"][fn], fn,
                              getattr(args, "base", None),
                              bool(getattr(args, "gmean", False)))
        return
    import tempfile
    fd, tmp = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump(data, f)
    attrs = getattr(args, "attrs", None)
    for fn in funcs:
        if fn not in data.get("functions", {}):
            continue
        a = attrs or FUNC_ATTRS.get(fn, "length")
        cmd = [sys.executable, os.path.join(TOOLS, "compare_strings.py"),
               "-a", a, "-i", tmp, "-s", SCHEMA]
        if getattr(args, "base", None):
            cmd += ["-b", args.base]
        if getattr(args, "gmean", False):
            cmd.append("--gmean")
        subprocess.run(cmd)
    os.remove(tmp)


def do_run(args):
    ensure_drivers(getattr(args, "no_build", False))
    funcs = parse_funcs(args.funcs)
    impls = resolve_impls(args.impl) if args.impl else []
    label = "-".join(funcs)
    if impls:
        label += "-" + ",".join(os.path.basename(p) for p in impls)

    payloads = {}
    failed = False
    for fn in funcs:
        sys.stderr.write("running %s benchmark...\n" % fn)
        rc, out = run_function(fn, args, impls, check=False)
        if rc != 0 or not out.strip():
            failed = True
            err("benchmark of %s failed (rc=%d); see message above"
                % (fn, rc))
            continue
        try:
            payloads[fn] = json.loads(out.decode("utf-8"))
        except ValueError:
            failed = True
            err("invalid JSON output from %s benchmark" % fn)
            continue

    if not payloads:
        die("no benchmark results produced")
    data = merge_json(payloads)
    validate_schema(data)

    outfile = getattr(args, "out", None)
    if outfile:
        path = os.path.abspath(outfile)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w") as f:
            json.dump(data, f, indent=1)
            f.write("\n")
        err("results written to %s" % path)
    else:
        path = save_result(data, label)
        err("results written to %s (also results/latest.json)" % path)

    table = getattr(args, "table", None) or "summary"
    if table != "none" and not getattr(args, "no_table", False):
        for fn in funcs:
            if fn in data.get("functions", {}):
                if table == "full":
                    full_table(data, [fn], args)
                elif getattr(args, "summary", "combo") == "size":
                    summary_by_size(data["functions"][fn], fn,
                                    getattr(args, "base", None),
                                    bool(getattr(args, "gmean", False)),
                                    getattr(args, "match", None))
                else:
                    summary_by_combo(data["functions"][fn], fn, args)
    return 1 if failed else 0


def do_check(args):
    ensure_drivers(getattr(args, "no_build", False))
    funcs = parse_funcs(args.funcs)
    impls = resolve_impls(args.impl) if args.impl else []
    failed = False
    for fn in funcs:
        sys.stderr.write("checking %s...\n" % fn)
        rc, out = run_function(fn, args, impls, check=True)
        if out:
            sys.stdout.write(out.decode("utf-8", "replace"))
        if rc != 0:
            failed = True
            err("%s check FAILED" % fn)
        else:
            err("%s check passed" % fn)
    return 1 if failed else 0


def do_list(args):
    print("Benchmark drivers (build/):")
    for fn in FUNCS:
        print("  %-8s %s" % (fn, "built" if os.path.exists(DRIVER[fn])
                             else "NOT BUILT"))
    print("\nImplementations:")
    for p in sorted(os.listdir(IMPLS_SRC)) if os.path.isdir(IMPLS_SRC) else []:
        stem = os.path.splitext(p)[0]
        so = os.path.join(IMPLDIR, stem + ".so")
        print("  %-28s .so: %s" % (p, "built" if os.path.exists(so)
                                   else "not built"))
    print("\nUse:  mb run <func...> [--impl <name|path> ...]")


def do_build(args):
    cmd = ["make", "-C", ROOT]
    if args.jobs:
        cmd += ["-j", str(args.jobs)]
    env = dict(os.environ)
    if args.cross:
        env["CROSS"] = args.cross if args.cross != "1" else "aarch64-linux-gnu-"
    if args.arch:
        env["MB_ARCH"] = args.arch
    r = subprocess.run(cmd, env=env)
    return r.returncode


def run_tool(tool, argv, modules):
    """Run a vendored/auxiliary python tool with argument pass-through."""
    for m in modules:
        if not have_module(m):
            die("this command needs the python module '%s' "
                "(pip install %s)" % (m, " ".join(modules)))
    path = os.path.join(TOOLS, tool)
    if not os.path.exists(path):
        die("tool not found: %s" % path)
    return subprocess.call([sys.executable, path] + list(argv))


def do_plot(args):
    """GB/s vs size graphs: tools/plot_mem.py (see `mb plot --help`)."""
    argv = list(args.args) or ["--help"]
    return run_tool("plot_mem.py", argv, ["matplotlib"])


def do_plot_glibc(args):
    """glibc-style timing plots: tools/plot_strings.py."""
    argv = list(args.args) or ["--help"]
    # plot_strings.py does not create its output directory, so do it here.
    try:
        for i, a in enumerate(argv):
            if a in ("-o", "--outdir") and i + 1 < len(argv):
                os.makedirs(argv[i + 1], exist_ok=True)
            elif a.startswith("--outdir="):
                os.makedirs(a.split("=", 1)[1], exist_ok=True)
    except OSError:
        pass
    return run_tool("plot_strings.py", argv, ["matplotlib", "jsonschema"])


def common_driver_options(p):
    p.add_argument("--impl", action="append", default=[],
                   help="shared object (path or impls/ basename); repeatable")
    p.add_argument("--no-libc", action="store_true", dest="no_libc",
                   help="exclude the libc baseline")
    p.add_argument("--generic", action="store_true", dest="generic",
                   help="include the generic C baseline (off by default)")
    p.add_argument("--quick", action="store_true",
                   help="short run (smaller iteration budget)")
    p.add_argument("--iters", type=int,
                   help="fixed inner-loop iterations per test")
    p.add_argument("--budget", type=float,
                   help="adaptive bytes budget per (impl, test), MiB")
    p.add_argument("--max-len", type=int,
                   help="cap tested lengths (also enables large sizes up to N)")
    p.add_argument("--seed", type=int, help="pattern seed")
    p.add_argument("--no-warmup", action="store_true", dest="no_warmup",
                   help="skip the frequency ramp-up loop")
    p.add_argument("-m", "--matrix", default=None,
                   help="matrix profile file for sizes/offsets "
                        "(see matrices/example.txt); ignored by mb check")
    p.add_argument("--no-build", action="store_true", dest="no_build",
                   help="do not rebuild drivers/implementations "
                        "automatically when they are out of date")



def build_parser():
    ap = argparse.ArgumentParser(
        prog="mb",
        description="Benchmark custom memcpy/memmove/memset/memcmp "
                    "implementations (glibc benchtests methodology).")
    sub = ap.add_subparsers(dest="cmd")

    b = sub.add_parser("build", help="run the make build")
    b.add_argument("--cross", nargs="?", const="1", default=None,
                   help="cross-compile: optional toolchain prefix "
                        "(default aarch64-linux-gnu-)")
    b.add_argument("--arch", default=None,
                   help="extra architecture flags, e.g. -march=armv8.2-a+sve")
    b.add_argument("-j", "--jobs", type=int, default=None)
    b.set_defaults(handler=do_build)

    l = sub.add_parser("list", help="list drivers and implementations")
    l.set_defaults(handler=do_list)

    r = sub.add_parser("run", help="benchmark functions and print a table")
    common_driver_options(r)
    r.add_argument("funcs", nargs="*",
                   help="functions to run (default: all)")
    r.add_argument("-o", "--out", default=None,
                   help="write merged JSON to this file")
    r.add_argument("--table", choices=["summary", "full", "none"],
                   default="summary",
                   help="terminal table style (default: summary)")
    r.add_argument("--attrs", default=None,
                   help="attributes for the full table (comma separated)")
    r.add_argument("-b", "--base", default=None,
                   help="baseline implementation for percentages")
    r.add_argument("--gmean", action="store_true",
                   help="print the geometric mean summary row/block")
    r.add_argument("--repeat", type=int, default=None,
                   help="measure every test N times; each run is stored in "
                        "the JSON with a 'run' attribute and the summary "
                        "reports repeat noise")
    r.add_argument("--summary", choices=["combo", "size"], default="combo",
                   help="summary layout: 'combo' (default) = rows are "
                        "parameter combinations and columns are size "
                        "regions; 'size' = one row per tested size")
    r.add_argument("--regions", default=None,
                   help="size-region boundaries for --summary combo in bytes "
                        "(default: %s)" % DEFAULT_REGIONS)
    r.add_argument("--stats", choices=["median", "min", "max", "mean"],
                   default="median",
                   help="statistic over repeated runs (default: median)")
    r.add_argument("--match", default=None,
                   help="only summary rows whose combination label "
                        "contains this substring")
    r.add_argument("--measure", choices=["hot", "offsets", "mixed"],
                   default=None,
                   help="measurement mode: hot (default) repeats each case; "
                        "offsets keeps the matrix sizes but randomizes the "
                        "offsets per call; mixed draws sizes and offsets "
                        "randomly (one result per batch)")
    r.add_argument("--batch", type=int, default=None,
                   help="calls per randomized batch in --measure "
                        "offsets/mixed (default 1024)")
    r.add_argument("--iters-mode", choices=["budget", "precision"],
                   default=None,
                   help="iteration policy: budget (default) or precision "
                        "(grow the iteration count until the estimate "
                        "settles within --epsilon)")
    r.add_argument("--epsilon", type=float, default=None,
                   help="precision target for --iters-mode precision "
                        "(default 0.01 = 1%%)")
    r.add_argument("--scaling", type=float, default=None,
                   help="iteration growth factor in precision mode "
                        "(default 1.4)")
    r.add_argument("--initial-iters", type=int, default=None,
                   help="first sample size in precision mode (default 1)")
    r.add_argument("--min-samples", type=int, default=None,
                   help="precision mode: minimum samples (default 4)")
    r.add_argument("--max-samples", type=int, default=None,
                   help="precision mode: maximum samples (default 1000)")
    r.add_argument("--min-duration", type=float, default=None,
                   help="precision mode: minimum time per measurement, s")
    r.add_argument("--max-duration", type=float, default=None,
                   help="precision mode: maximum time per measurement, s "
                        "(default 10)")
    r.add_argument("--mismatch-at", type=int, default=None,
                   help="memcmp: place the mismatch at byte N-1 "
                        "(default: matrix-defined)")
    r.add_argument("--no-table", action="store_true",
                   help="do not print any table")
    r.set_defaults(handler=do_run)

    c = sub.add_parser("check", help="verify correctness of implementations")
    common_driver_options(c)
    c.add_argument("funcs", nargs="*",
                   help="functions to check (default: all)")
    c.set_defaults(handler=do_check)

    p = sub.add_parser("plot", add_help=False,
                       help="GB/s vs size graphs (tools/plot_mem.py); "
                            "run `mb plot --help` for its options")
    p.add_argument("args", nargs=argparse.REMAINDER,
                   help="arguments forwarded to tools/plot_mem.py, e.g. "
                        "RESULT.json -o plots --param src --match 'dst=0'")
    p.set_defaults(handler=do_plot)

    g = sub.add_parser("plot-glibc", add_help=False,
                       help="glibc-style timing plots (vendored "
                            "plot_strings.py); `mb plot-glibc --help`")
    g.add_argument("args", nargs=argparse.REMAINDER,
                   help="arguments forwarded to tools/plot_strings.py")
    g.set_defaults(handler=do_plot_glibc)

    return ap


def main(argv=None):
    # Convenience: `mb memcpy --impl x` and `mb --impl x` mean `mb run ...`.
    # (Subcommands: build, list, run, check, plot, plot-glibc)
    argv = list(sys.argv[1:] if argv is None else argv)
    subcmds = ("build", "list", "run", "check", "plot", "plot-glibc")
    if argv and argv[0] not in subcmds and argv[0] not in ("-h", "--help"):
        argv.insert(0, "run")
    # plot commands forward their arguments verbatim (argparse REMAINDER
    # cannot capture "--help", so handle them before parsing).
    if argv and argv[0] == "plot":
        return do_plot(argparse.Namespace(args=argv[1:]))
    if argv and argv[0] == "plot-glibc":
        return do_plot_glibc(argparse.Namespace(args=argv[1:]))
    args = build_parser().parse_args(argv)
    if not getattr(args, "handler", None):
        build_parser().print_help()
        return 2
    try:
        return args.handler(args)
    except KeyboardInterrupt:
        err("interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())

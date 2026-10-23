#!/usr/bin/env python3
"""Summarize the zset-ab CI job (utils/zset_compare_benchmark.tcl runs).

Usage: zset_ab_summary.py <dir> <base-sha> <head-sha>

<dir> holds:
  time-base-<round>.tsv, time-head-<round>.tsv  server time per case
  zv-head-<round>.tsv                           head runs with zv-* cases
  ir-base/, ir-head/                            callgrind dumps (see load_ir)
  ir-base.tsv, ir-head.tsv                      the benchmark output of those runs
Prints a markdown report on stdout.
"""
import glob
import math
import os
import statistics
import sys


def load_times(pattern):
    runs = {}
    for path in sorted(glob.glob(pattern)):
        for line in open(path):
            if line.startswith("#") or line.startswith("case"):
                continue
            f = line.rstrip("\n").split("\t")
            runs.setdefault(f[0], []).append(float(f[1]))
    return {k: statistics.median(v) for k, v in runs.items()}


def load_ir(directory, tsv):
    """Instructions per case. The server ran under callgrind with
    --dump-before=echoCommand and the benchmark (--callgrind 1, one repeat)
    sent an ECHO before and after each case's payload, so dumps 2, 4, 6, ...
    hold the payloads of the cases in the order of the benchmark output."""
    labels = [line.split("\t")[0] for line in open(tsv) if not line.startswith(("#", "case"))]
    totals = {}
    for path in glob.glob(os.path.join(directory, "cg.out.*")):
        seq = path.rsplit(".", 1)[1]
        for line in open(path, errors="replace"):
            if line.startswith("totals:"):
                totals[int(seq)] = int(line.split()[1])
                break
    return {label: totals[2 * (i + 1)] for i, label in enumerate(labels) if 2 * (i + 1) in totals}


def geomean(values):
    values = [v for v in values if v > 0]
    return math.exp(sum(map(math.log, values)) / len(values)) if values else float("nan")


def main():
    d, base_sha, head_sha = sys.argv[1:4]
    base_t = load_times(os.path.join(d, "time-base-*.tsv"))
    head_t = load_times(os.path.join(d, "time-head-*.tsv"))
    base_ir = load_ir(os.path.join(d, "ir-base"), os.path.join(d, "ir-base.tsv"))
    head_ir = load_ir(os.path.join(d, "ir-head"), os.path.join(d, "ir-head.tsv"))
    cases = [c for c in base_t if c in head_t]

    print("## ZSET A/B: existing ZSET commands, base vs this branch")
    print(f"base `{base_sha}`  head `{head_sha}`")
    print()
    print("Server time: median of alternating rounds (us/command, from INFO commandstats).")
    print("Instructions: callgrind instruction count of each case's commands (N=3000).")
    print("`small` = listpack (64 members), `large` = btree.")
    print()
    print("| case | base us | head us | time ratio | base Mi | head Mi | instr. diff |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    ratios, ir_base_total, ir_head_total = [], 0, 0
    for c in cases:
        b, h = base_t[c], head_t[c]
        ratio = b / h if h > 0 else float("nan")
        ratios.append(ratio)
        if c in base_ir and c in head_ir:
            bi, hi = base_ir[c], head_ir[c]
            ir_base_total += bi
            ir_head_total += hi
            ir_cols = f"{bi / 1e6:.2f} | {hi / 1e6:.2f} | {100.0 * (hi / bi - 1):+.2f}%"
        else:
            ir_cols = "n/a | n/a | n/a"
        print(f"| {c} | {b:.3f} | {h:.3f} | {ratio:.2f}x | {ir_cols} |")
    print()
    print(f"Time: geometric mean speedup head vs base **{geomean(ratios):.3f}x** (>1 = head faster).")
    if ir_base_total:
        print(f"Instructions: total head vs base **{100.0 * (ir_head_total / ir_base_total - 1):+.3f}%**.")

    zv = load_times(os.path.join(d, "zv-head-*.tsv"))
    pairs = [(c[2:], zv[c], zv["zv-" + c[2:]]) for c in zv if c.startswith("z-") and "zv-" + c[2:] in zv]
    if pairs:
        print()
        print("## ZV (1 dimension) vs ZSET on this branch (median of 3 runs, us/command)")
        print()
        print("| case | ZSET us | ZV us | ZV speedup |")
        print("|---|---:|---:|---:|")
        for c, z, v in pairs:
            print(f"| {c} | {z:.3f} | {v:.3f} | {z / v:.2f}x |")
        print()
        print(f"Geometric mean ZV speedup over ZSET: **{geomean([z / v for _, z, v in pairs]):.3f}x**.")


if __name__ == "__main__":
    main()

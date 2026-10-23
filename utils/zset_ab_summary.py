#!/usr/bin/env python3
"""Summarize the zset-ab CI job (utils/zset_compare_benchmark.tcl runs).

Usage: zset_ab_summary.py <dir> <base-sha> <head-sha>

<dir> holds:
  time-base-<round>.tsv, time-head-<round>.tsv  server time per case
  zv-head-<round>.tsv                           head runs with zv-* cases
  ir-base/, ir-head/                            callgrind dumps, one per case
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


def load_ir(directory):
    """Instructions per case from callgrind dumps triggered after each case."""
    ir = {}
    for path in glob.glob(os.path.join(directory, "cg.out.*")):
        label = total = None
        for line in open(path, errors="replace"):
            if line.startswith("desc: Trigger:"):
                label = line.rsplit(" ", 1)[1].strip()
            elif line.startswith("totals:"):
                total = int(line.split()[1])
                break
        if label and total is not None:
            ir[label] = total
    return ir


def geomean(values):
    values = [v for v in values if v > 0]
    return math.exp(sum(map(math.log, values)) / len(values)) if values else float("nan")


def main():
    d, base_sha, head_sha = sys.argv[1:4]
    base_t = load_times(os.path.join(d, "time-base-*.tsv"))
    head_t = load_times(os.path.join(d, "time-head-*.tsv"))
    base_ir = load_ir(os.path.join(d, "ir-base"))
    head_ir = load_ir(os.path.join(d, "ir-head"))
    cases = [c for c in base_t if c in head_t]

    print("## ZSET A/B: existing ZSET commands, base vs this branch")
    print(f"base `{base_sha}`  head `{head_sha}`")
    print()
    print("Server time: median of alternating rounds (us/command, from INFO commandstats).")
    print("Instructions: callgrind instruction count per case (deterministic; setup included,")
    print("identical on both sides). `small` = listpack (64 members), `large` = btree.")
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

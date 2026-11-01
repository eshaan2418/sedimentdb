#!/usr/bin/env python3
"""Median ops/s (and us/op) per engine and benchmark from db_bench_raw.txt."""
import collections
import statistics
import sys

rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    if line.startswith("#") or not line.strip():
        continue
    parts = line.split()
    _, engine, bench = parts[0], parts[1], parts[2]
    ops_s = float(parts[parts.index("ops/s") - 1])
    rows[(bench, engine)].append(ops_s)

engines = ["sediment", "leveldb", "sqlite"]
benches = ["fillseq", "fillrandom", "overwrite", "readrandom", "readmissing", "readseq", "fillsync"]
print("| Benchmark | " + " | ".join(engines) + " | sediment / leveldb |")
print("|---|" + "---:|" * (len(engines) + 1))
for b in benches:
    med = {e: statistics.median(rows[(b, e)]) for e in engines if rows[(b, e)]}
    cells = [f"{med[e]:,.0f}" if e in med else "-" for e in engines]
    ratio = med["sediment"] / med["leveldb"] if "sediment" in med and "leveldb" in med else float("nan")
    print(f"| {b} | " + " | ".join(cells) + f" | {ratio:.2f}x |")
n = len(next(iter(rows.values())))
print(f"\nMedian ops/s over {n} runs.")

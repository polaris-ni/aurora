#!/usr/bin/env python3
# ============================================================================
# build_baseline.py - build/test timing baseline (observability, not a gate)
# ----------------------------------------------------------------------------
# Parses Ninja and CTest logs from a build directory and prints a timing
# snapshot for manual regression comparison (never fails the build):
#
#   1) .ninja_log          -> per-edge compile durations: summary (edge count,
#                             wall clock, aggregate edge time, effective
#                             parallelism), a duration histogram (exposes I/O
#                             contention tails such as the PCH-era 10-39s
#                             spikes), and the top-N slowest edges.
#   2) Testing/Temporary/LastTest.log
#                          -> per-test ctest durations: summary + top-N slowest
#                             tests (the parallel-ctest critical path).
#
# Exit code: always 0 (observability only; missing logs are reported, not fatal).
#
# Usage:
#   python tools/check/build_baseline.py                     # build/ defaults
#   python tools/check/build_baseline.py --build-dir build-msvc --top 20
#   python tools/check/build_baseline.py --json baseline.json
# ============================================================================
import argparse
import json
import os
import re
import sys


# ---------------------------------------------------------------------------
# .ninja_log parsing
# ---------------------------------------------------------------------------

def parse_ninja_log(path):
    """Parse .ninja_log lines into (output_path, duration_seconds) tuples.

    Format (ninja log v5/v6): a "# ninja log vN" header, then whitespace-
    separated records `start end mtime output cmd_hash`. The log is append-
    across-builds and compacted by ninja on exit, so entries may span more
    than one build. start/end are milliseconds (start/end magnitudes are far
    below epoch-ms, so only relative durations are meaningful; detect a
    hypothetical microsecond-scale log by magnitude).
    """
    edges = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) < 5:
                continue
            try:
                start, end = int(fields[0]), int(fields[1])
            except ValueError:
                continue
            if end <= start:
                continue
            edges.append((" ".join(fields[3:-1]), start, end))

    if not edges:
        return []

    # Unit guard: treat anomalously large timestamps as microseconds.
    scale = 1e6 if max(e[2] for e in edges) > 10_000_000_000_000 else 1e3
    return [(out, (end - start) / scale) for out, start, end in edges]


def histogram(durations):
    """Bucket durations (seconds) into fixed ranges that make contention tails
    (10s+ edges) visually obvious next to the sub-second majority."""
    bounds = [0.25, 0.5, 1.0, 2.0, 5.0, 10.0, float("inf")]
    labels = ["0-0.25s", "0.25-0.5s", "0.5-1s", "1-2s", "2-5s", "5-10s", "10s+"]
    counts = [0] * len(labels)
    for d in durations:
        for i, b in enumerate(bounds):
            if d < b:
                counts[i] += 1
                break
    return labels, counts


def report_ninja_log(path, top_n):
    edges = parse_ninja_log(path)
    if not edges:
        print(f"[ninja] {path}: no usable edges")
        return None

    durations = [d for _, d in edges]
    total = sum(durations)
    ordered = sorted(edges, key=lambda e: e[1], reverse=True)

    print(f"[ninja] {path}")
    print(f"  编译边数        : {len(edges)}")
    print(f"  边耗时合计      : {total:.1f}s (≈编译 CPU 量)")
    p95 = sorted(durations)[int(len(durations) * 0.95) - 1 if len(durations) >= 20 else len(durations) - 1]
    print(f"  p95 单边        : {p95:.2f}s   最慢单边: {ordered[0][1]:.2f}s")
    print(f"  wall clock      : 需构建命令计时（.ninja_log 不含全局起止），用 time cmake --build 记录")

    labels, counts = histogram(durations)
    print("  耗时分布        :")
    for label, count in zip(labels, counts):
        if count:
            print(f"    {label:>10s} : {count}")

    print(f"  top-{top_n} 慢边:")
    for out, d in ordered[:top_n]:
        print(f"    {d:8.2f}s  {out}")
    return {
        "edges": len(edges),
        "total_edge_seconds": round(total, 2),
        "p95_edge_seconds": round(p95, 3),
        "histogram": dict(zip(labels, counts)),
        "top": [{"seconds": round(d, 2), "output": out} for out, d in ordered[:top_n]],
    }


# ---------------------------------------------------------------------------
# ctest LastTest.log parsing
# ---------------------------------------------------------------------------

_TEST_NAME = re.compile(r"^\s*\d+/\d+\s+Testing:\s+(.+)$")
# Time line comes both with the "N/M" prefix (ctest stdout style) and without
# it (LastTest.log on this ctest version); accept either.
_TEST_TIME = re.compile(r"^\s*(?:\d+/\d+\s+)?Test\s+[Tt]ime\s*=\s*([0-9.]+)\s*sec")


def parse_ctest_log(path):
    """Parse ctest's Testing/Temporary/LastTest.log into (test_name, seconds)."""
    tests, current = [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = _TEST_NAME.match(line)
            if m:
                current = m.group(1).strip()
                continue
            m = _TEST_TIME.match(line)
            if m and current is not None:
                tests.append((current, float(m.group(1))))
                current = None
    return tests


def report_ctest_log(path, top_n):
    tests = parse_ctest_log(path)
    if not tests:
        print(f"[ctest] {path}: no usable entries")
        return None

    total = sum(t for _, t in tests)
    ordered = sorted(tests, key=lambda e: e[1], reverse=True)
    print(f"[ctest] {path}")
    print(f"  测试数          : {len(tests)}")
    print(f"  串行耗时合计    : {total:.1f}s（并行后的关键路径 ≈ 最慢单测）")
    print(f"  top-{top_n} 慢测:")
    for name, t in ordered[:top_n]:
        print(f"    {t:8.2f}s  {name}")
    return {
        "tests": len(tests),
        "total_serial_seconds": round(total, 2),
        "top": [{"seconds": t, "name": name} for name, t in ordered[:top_n]],
    }


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Build/test timing baseline (observability, not a gate)")
    parser.add_argument("--build-dir", default="build", help="CMake build directory (default: build)")
    parser.add_argument("--top", type=int, default=15, help="Number of slowest edges/tests to list (default: 15)")
    parser.add_argument("--ninja-log", default=None, help="Path to .ninja_log (default: <build-dir>/.ninja_log)")
    parser.add_argument("--ctest-log", default=None,
                        help="Path to ctest LastTest.log (default: <build-dir>/Testing/Temporary/LastTest.log)")
    parser.add_argument("--json", default=None, help="Also write the summary as JSON to this path")
    args = parser.parse_args()

    ninja_path = args.ninja_log or os.path.join(args.build_dir, ".ninja_log")
    ctest_path = args.ctest_log or os.path.join(args.build_dir, "Testing", "Temporary", "LastTest.log")

    result = {}
    if os.path.isfile(ninja_path):
        result["ninja"] = report_ninja_log(ninja_path, args.top)
    else:
        print(f"[ninja] {ninja_path}: not found (skipped)")

    print()
    if os.path.isfile(ctest_path):
        result["ctest"] = report_ctest_log(ctest_path, args.top)
    else:
        print(f"[ctest] {ctest_path}: not found (skipped; run ctest once to produce it)")

    if args.json and result:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print(f"\nJSON 基线已写入: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

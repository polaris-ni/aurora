#!/usr/bin/env bash
# coverage_report.sh - Aurora line coverage terminal summary (Linux/macOS, GCC gcov; no HTML).
#
# Windows counterpart: tools/coverage/coverage_report.ps1. Invoked by the CMake `coverage` target on
# the GCC branch, but also runnable standalone. Prereq: the build dir was configured with
# -DAURORA_ENABLE_COVERAGE=ON, fully built, and tests run (so `*.gcda` exist under the object dirs).
#
# Usage:
#   tools/coverage/coverage_report.sh [build_dir] [threshold]
#     build_dir    default build-cov
#     threshold    per-file pass line, default 90
#
# Difference from the ps1 version: this script aggregates "all target dirs" line counts via the
# `gcov -i -t` intermediate format before summing (a shared source file is counted as a union across
# the library and multiple test TUs), instead of per-.gcda overwriting the same-named .gcov.
#
# Output in three parts (all produced by tools/coverage/gcov_aggregate.py; the HTML is self-drawn by
# that script -- because the local gcov has no --html, the GCC and LLVM branches share the same-shaped
# HTML, see gcov_aggregate.py):
#   (A) overall line coverage + files below the threshold (ascending)
#   (B) full table CSV to <build_dir>/coverage.csv + HTML to <build_dir>/coverage.html
#   (C) public-header <-> test-file 1:1 mapping overview (project's conventional proxy metric)
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${1:-build-cov}
THRESHOLD=${2:-90}

command -v gcov >/dev/null 2>&1 || { echo "[coverage] gcov not found" >&2; exit 2; }
if [ ! -d "$BUILD_DIR" ]; then
    echo "[coverage] build dir does not exist: $BUILD_DIR (configure with -DAURORA_ENABLE_COVERAGE=ON and run tests first)" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- Collect all .gcda and emit each in intermediate format to one stream (file:/lcount: blocks are
# self-describing and can be concatenated directly) ----
find "$BUILD_DIR" -name '*.gcda' | sort > "$WORK/gcdas.txt"
count=$(wc -l < "$WORK/gcdas.txt")
if [ "$count" -eq 0 ]; then
    echo "[coverage] no .gcda -- run tests first (ctest --test-dir $BUILD_DIR)" >&2
    exit 2
fi

while IFS= read -r gcda; do
    objdir=$(dirname "$gcda")
    # Do not abort on failure: an individual object may lack its paired .gcno (external toolchain
    # artifacts, etc.); just skip it.
    gcov -i -t -o "$objdir" "$gcda" >> "$WORK/all.gcov" 2>/dev/null || true
done < "$WORK/gcdas.txt"

# ---- Aggregation + report (text / CSV / self-drawn HTML) is delegated entirely to gcov_aggregate.py ----
AGG="$SCRIPT_DIR/gcov_aggregate.py"
command -v python3 >/dev/null 2>&1 || { echo "[coverage] python3 not found (needed for aggregation/HTML)" >&2; exit 2; }
python3 "$AGG" "$WORK/all.gcov" --src-root "$SRC_ROOT" --threshold "$THRESHOLD" \
    --csv "$BUILD_DIR/coverage.csv" --html "$BUILD_DIR/coverage.html"

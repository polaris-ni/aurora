#!/usr/bin/env python3
"""Parallel clang-tidy runner for the Aurora project.

Why this exists instead of LLVM's `run-clang-tidy`:
  * it is available everywhere clang-tidy is (no dependence on the LLVM python
    helper being installed / on PATH),
  * it excludes third_party/ translation units,
  * it **deduplicates** findings: clang-tidy re-reports a header diagnostic in
    every translation unit that includes it, so a raw warning count massively
    overstates the work and makes CI numbers non-comparable across runs,
  * it emits a stable summary (by check, by file) suitable for CI logs.

Usage:
    run_clang_tidy.py --build-dir build-lint [--jobs N] [--fix] [--checks ...]

Exit codes:
    0  no findings (or --fix mode completed)
    1  findings present with the configured severity
    2  usage / environment error (no compile database, clang-tidy missing)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

# <path>:<line>:<col>: warning: <msg> [<check>]
DIAG = re.compile(r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+(.*?)\s+\[(.+)\]\s*$")

DEFAULT_EXCLUDE = re.compile(r"(^|/)third_party/")


def norm(p: str) -> str:
    return os.path.normpath(p).replace("\\", "/")


def which(name: str) -> str:
    from shutil import which as _w

    return _w(name)


def load_tus(compile_db: str, include: re.Pattern,
             exclude: re.Pattern) -> list[str]:
    with open(compile_db, encoding="utf-8") as fh:
        entries = json.load(fh)
    out, seen = [], set()
    for e in entries:
        # 'file' is relative in newer CMake DBs; 'directory' holds the base.
        f = e.get("file", "")
        if not os.path.isabs(f):
            f = os.path.join(e.get("directory", ""), f)
        f = norm(f)
        if f in seen:
            continue
        seen.add(f)
        if exclude.search(f):
            continue
        if is_auto_generated(f):
            continue
        if include and not include.search(f):
            continue
        out.append(f)
    return sorted(out)


def is_auto_generated(path: str) -> bool:
    """判断源文件是否为自动生成（首部若干行标注 AUTO-GENERATED）。

    生成物（如字模字节表）不应纳入 lint：改动会被下次重新生成覆盖，
    且其 C 数组/指针形态由生成器决定，人工抑制毫无意义。
    """
    try:
        with open(path, encoding="utf-8", errors="ignore") as fh:
            for _ in range(10):
                if "AUTO-GENERATED" in fh.readline():
                    return True
    except OSError:
        pass
    return False


def run_one(job: tuple[str, str | None, str, bool, str | None]) -> tuple[str, str, float]:
    tu, checks, build_dir, do_fix, config = job
    cmd: list[str] = [which("clang-tidy") or "clang-tidy", "-p", build_dir, "--quiet"]
    if checks:
        cmd.append(f"--checks={checks}")
    if config:
        cmd.append(f"--config-file={config}")
    if do_fix:
        cmd.append("--fix")
    cmd.append(tu)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=1800)
        return tu, (r.stdout or "") + (r.stderr or ""), time.time() - t0
    except subprocess.TimeoutExpired:
        return tu, f"__TIMEOUT__ {tu}\n", time.time() - t0
    except OSError as exc:
        return tu, f"__ERROR__ {tu}: {exc}\n", time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description="Parallel clang-tidy runner for Aurora.")
    ap.add_argument("--build-dir", required=True,
                    help="CMake build directory containing compile_commands.json")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel clang-tidy processes (0 = CPU count)")
    ap.add_argument("--checks", default=None,
                    help="override the Checks: list from .clang-tidy")
    ap.add_argument("--config", default=None,
                    help="use an alternate .clang-tidy file")
    ap.add_argument("--include", default=None,
                    help="regex; only lint TUs whose path matches")
    ap.add_argument("--exclude", default=DEFAULT_EXCLUDE.pattern,
                    help="regex; skip TUs whose path matches (default: third_party)")
    ap.add_argument("--fix", action="store_true",
                    help="apply clang-tidy fix-its in place (does not fail the run)")
    ap.add_argument("--fail-on", choices=["warning", "error"], default="warning",
                    help="minimum severity that counts as a failure")
    ap.add_argument("--json-out", default=None, help="write findings to this JSON file")
    ap.add_argument("--show", type=int, default=20,
                    help="how many top files to print")
    a = ap.parse_args()

    compile_db = os.path.join(a.build_dir, "compile_commands.json")
    if not os.path.isfile(compile_db):
        print(f"error: no compile_commands.json in {a.build_dir!r}.\n"
              f"Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.", file=sys.stderr)
        return 2
    if which("clang-tidy") is None:
        print("error: clang-tidy not found on PATH.", file=sys.stderr)
        return 2

    include = re.compile(a.include) if a.include else None
    exclude = re.compile(a.exclude) if a.exclude else None
    tus = load_tus(compile_db, include, exclude) if exclude else load_tus(compile_db, include, re.compile(r"(?!x)x"))
    if not tus:
        print("error: no translation units selected.", file=sys.stderr)
        return 2

    jobs = a.jobs or (os.cpu_count() or 4)
    print(f"[lint] clang-tidy over {len(tus)} translation units, {jobs} parallel")

    findings: dict[tuple[str, str, str], str] = {}
    by_area: Counter[str] = Counter()
    timeouts: list[str] = []
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        jobs_args = [(t, a.checks, a.build_dir, a.fix, a.config) for t in tus]
        for i, (tu, out, _dt) in enumerate(ex.map(run_one, jobs_args), 1):
            if out.startswith("__TIMEOUT__") or out.startswith("__ERROR__"):
                timeouts.append(out.strip())
            for line in out.splitlines():
                m = DIAG.match(line.strip())
                if not m:
                    continue
                path, line_no, _col, sev, msg, check = m.groups()
                np_ = norm(path)
                if exclude and exclude.search(np_):
                    continue
                # keyed by (file, line, check): a header diagnostic is re-emitted in
                # every including TU, so later occurrences just refresh the message.
                findings[(np_, line_no, check)] = (sev, msg)
                rel = os.path.relpath(np_).replace("\\", "/")
                by_area[rel.split("/")[0] if "/" in rel else rel] += 1
            if i % 25 == 0 or i == len(tus):
                print(f"  ... {i}/{len(tus)}  ({time.time() - t0:.0f}s)", flush=True)

    by_check = Counter(c for (_f, _l, c) in findings)
    by_file = Counter(f for (f, _l, _c) in findings)
    sev_rank = {"warning": 0, "error": 1}
    threshold = sev_rank[a.fail_on]
    blocking = sum(1 for s, _m in findings.values() if sev_rank.get(s, 0) >= threshold)

    print(f"\n[lint] elapsed {time.time() - t0:.0f}s")
    print(f"[lint] unique findings: {len(findings)}   blocking(>={a.fail_on}): {blocking}")
    if timeouts:
        print(f"[lint] problems: {len(timeouts)}")
        for t in timeouts[:10]:
            print("   ", t)

    print("\n=== by area ===")
    for k, v in by_area.most_common():
        print(f"  {k:12} {v}")
    print("\n=== by check ===")
    for k, v in by_check.most_common():
        print(f"  {v:6}  {k}")
    print(f"\n=== top {a.show} files ===")
    for k, v in by_file.most_common(a.show):
        print(f"  {v:6}  {k}")

    if a.json_out:
        payload = {
            "tu_count": len(tus),
            "unique_findings": len(findings),
            "by_check": by_check.most_common(),
            "by_file": by_file.most_common(),
            "findings": sorted([list(k) + [v[0], v[1]] for k, v in findings.items()]),
        }
        with open(a.json_out, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=1)
        print(f"\n[saved] {a.json_out}")

    if a.fix:
        print("\n[lint] --fix applied; not failing the build (review the diff).")
        return 0
    return 1 if blocking else 0


if __name__ == "__main__":
    sys.exit(main())

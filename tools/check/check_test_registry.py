#!/usr/bin/env python3
# ============================================================================
# check_test_registry.py - TEST-R6 case-level registry integrity gate
# ----------------------------------------------------------------------------
# Compares the case list emitted by `aurora_test_runner --list --format=cases`
# against the case macros statically extracted from the test sources.
#
# The framework derives Suite from __FILE__ (== file stem), so the risk this
# gate targets is "silent skip": a test TU whose registered cases never reach
# the runner output. Detected drift classes:
#   1) literal case (AURORA_TEST_CASE / AURORA_TEST_F) missing from --list
#   2) value-parameterized case (AURORA_TEST_P) without AURORA_INSTANTIATE_TEST_SUITE_P
#      for its fixture, or instantiated but never expanded in --list
#   3) typed case (AURORA_TYPED_TEST) without AURORA_TYPED_TEST_SUITE for its
#      fixture, or declared but never expanded in --list
#   4) --list contains a suite that maps to no test source file
#   5) test source file whose stem contributes zero cases in --list
#
# Exit code: 0 clean, 1 any drift found.
#
# Usage (single runner):
#   python3 tools/check/check_test_registry.py --runner <runner.exe> \
#       --tests-dir tests/unit --tests-dir tests/integration
# Sharded builds pass --runner repeatedly; the case lists are unioned before compare.
# ============================================================================
import argparse
import os
import re
import subprocess
import sys

RE_LITERAL_CASE = re.compile(r"\bAURORA_TEST_CASE\s*\(\s*([A-Za-z_]\w*)\s*\)")
RE_F_CASE = re.compile(r"\bAURORA_TEST_F\s*\(\s*(\w+)\s*,\s*([A-Za-z_]\w*)\s*\)")
RE_P_CASE = re.compile(r"\bAURORA_TEST_P\s*\(\s*(\w+)\s*,\s*([A-Za-z_]\w*)\s*\)")
RE_INSTANTIATE = re.compile(r"\bAURORA_INSTANTIATE_TEST_SUITE_P(?:_GEN)?\s*\(\s*\w+\s*,\s*(\w+)\s*,")
RE_TYPED_CASE = re.compile(r"\bAURORA_TYPED_TEST\s*\(\s*(\w+)\s*,\s*([A-Za-z_]\w*)\s*\)")
RE_TYPED_SUITE = re.compile(r"\bAURORA_TYPED_TEST_SUITE\s*\(\s*(\w+)\s*[,)]")


def strip_comments(text):
    """Remove // line comments and /* */ block comments so macro regexes only
    see real registrations (a commented-out AURORA_TEST_CASE must not count)."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def scan_source(path, stem):
    """Extract expected case descriptors from one test source file."""
    with open(path, encoding="utf-8", errors="replace") as f:
        code = strip_comments(f.read())
    expected = {
        "literal": [f"{stem}.{name}" for name in RE_LITERAL_CASE.findall(code)]
        + [f"{stem}.{fx}_{case}" for fx, case in RE_F_CASE.findall(code)],
        "p_cases": RE_P_CASE.findall(code),  # (fixture, case) pairs
        "instantiated_fixtures": set(RE_INSTANTIATE.findall(code)),
        "typed_cases": RE_TYPED_CASE.findall(code),  # (fixture, case) pairs
        "typed_suites": set(RE_TYPED_SUITE.findall(code)),
    }
    return expected


def list_actual_cases(runner):
    """Run `runner --list --format=cases`; return the list of `Suite.Case` names."""
    proc = subprocess.run([runner, "--list", "--format=cases"], capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", errors="replace"))
        raise SystemExit(f"[FAIL] {runner} --list exited with {proc.returncode}")
    return [line.strip().rstrip("\r") for line in proc.stdout.decode("utf-8", errors="replace").splitlines()
            if line.strip()]


def main():
    parser = argparse.ArgumentParser(description="TEST-R6 case-level registry integrity gate")
    parser.add_argument("--runner", action="append", required=True,
                        help="path to an aurora_test_runner binary (repeatable for sharded builds)")
    parser.add_argument("--tests-dir", action="append", required=True,
                        help="test source directory (repeatable); *.cpp children are scanned")
    args = parser.parse_args()

    actual = []
    for runner in args.runner:
        actual.extend(list_actual_cases(runner))
    actual_set = set(actual)
    actual_suites = {name.split(".", 1)[0] for name in actual if "." in name}

    expected_stems = set()
    problems = []
    for tests_dir in args.tests_dir:
        # 重写中间态下 tests/integration 可能尚不存在：跳过缺失目录（存在但为空仍照常扫描）。
        if not os.path.isdir(tests_dir):
            continue
        for entry in sorted(os.listdir(tests_dir)):
            if not entry.endswith(".cpp"):
                continue
            path = os.path.join(tests_dir, entry)
            stem = os.path.splitext(entry)[0]
            expected_stems.add(stem)
            src = scan_source(path, stem)

            for full in src["literal"]:
                if full not in actual_set:
                    problems.append(f"{path}: literal case '{full}' not in --list output (forgotten macro? "
                                    f"duplicate registration?)")
            for fixture, case in src["p_cases"]:
                if fixture not in src["instantiated_fixtures"]:
                    problems.append(f"{path}: AURORA_TEST_P({fixture}, {case}) has no "
                                    f"AURORA_INSTANTIATE_TEST_SUITE_P for '{fixture}' in this file - "
                                    f"the case never expands and silently never runs")
                elif not any(name.startswith(stem + ".") and f"{fixture}_{case}" in name for name in actual):
                    problems.append(f"{path}: parameterized case '{fixture}_{case}' never expanded in --list "
                                    f"(INSTANTIATE present but no matching case)")
            for fixture, case in src["typed_cases"]:
                if fixture not in src["typed_suites"]:
                    problems.append(f"{path}: AURORA_TYPED_TEST({fixture}, {case}) has no AURORA_TYPED_TEST_SUITE "
                                    f"declaration for '{fixture}' in this file - the case never expands and "
                                    f"silently never runs")
                elif not any(name.startswith(f"{stem}.{case}/") for name in actual):
                    problems.append(f"{path}: typed case '{case}' never expanded in --list "
                                    f"(TYPED_TEST_SUITE present but no matching case)")
            if not any(name.startswith(stem + ".") for name in actual):
                problems.append(f"{path}: suite '{stem}' contributes zero cases in --list "
                                f"(file registers no AURORA_TEST_* macro)")

    for suite in sorted(actual_suites - expected_stems):
        problems.append(f"--list contains suite '{suite}' that maps to no *.cpp under "
                        f"{', '.join(args.tests_dir)} (stray registration from a non-test TU?)")

    if not expected_stems:
        raise SystemExit(f"[FAIL] no test source scanned - missing directories? {', '.join(args.tests_dir)}")

    if problems:
        for problem in problems:
            print(f"[FAIL] {problem}")
        raise SystemExit(1)
    print(f"[OK] test registry case-level sync clean: {len(actual)} case(s) across "
          f"{len(actual_suites)} suite(s), {len(expected_stems)} test source file(s)")


if __name__ == "__main__":
    main()

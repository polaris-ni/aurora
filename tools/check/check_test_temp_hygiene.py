#!/usr/bin/env python3
# ============================================================================
# check_test_temp_hygiene.py - test temp-file discipline gate (C4)
# ----------------------------------------------------------------------------
# Spec: codespec/CODING_STANDARDS.md §3.4 (C4, 2026-09-11 定论)
#   "所有测试产生的临时文件一律走 aurora::testing::isolation::temp_dir()，
#    禁止自行调用 temp_directory_path()、裸写 /tmp、或往 cwd 写文件。"
#
# Scan points (tests/ only, .cpp/.h):
#   1) [blocking] std::filesystem::temp_directory_path() / fs::temp_directory_path()
#      — must go through isolation::temp_dir() instead.
#   2) [blocking] A bare /tmp (or \tmp) path literal (e.g. "/tmp/x", "C:/tmp/a.txt")
#      — hardcoded system temp dir, ignores per-case isolation.
#   3) [blocking] current_path() used as a write target (path construction via
#      `current_path() /` or `current_path() +`) — writes into cwd/repo root,
#      polluting the working tree instead of test_temp/.
#
# Exemption: a line containing the token "TEST_TEMP_EXEMPT" (case-insensitive),
# on the matched line or any of the 2 preceding lines, documents a legitimate
# exception and is silently allowed. The temp-dir machinery itself
# (tests/framework/isolation.{h,cpp}) is exempt wholesale (it cannot use
# temp_dir() — chicken-and-egg), but every other exception must be inline.
#
# Comments (// and /* */) are stripped before scanning, so commented-out
# mentions of the banned tokens are never flagged.
#
# Exit code: 1 when any un-exempted bypass is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_test_temp_hygiene.py [--root <aurora_root>]
# ============================================================================
import argparse
import os
import re
import sys

EXEMPT_TOKEN = "TEST_TEMP_EXEMPT"
SCAN_DIRS = ("tests",)
SCAN_EXTS = (".cpp", ".h")

# The temp-dir machinery itself cannot use temp_dir() (chicken-and-egg); exempt wholesale.
EXEMPT_FILES = {
    "tests/framework/isolation.cpp",
    "tests/framework/isolation.h",
}

TEMP_DIR_RE = re.compile(r"temp_directory_path\s*\(")
# Bare /tmp or \tmp inside a quoted path literal.
BARE_TMP_RE = re.compile(r'["\'][^"\']*(?:/|\\\\)tmp(?:/|\\\\)?[^\"\']*["\']')
# current_path() used as a write target (path construction).
CWD_WRITE_RE = re.compile(r"current_path\s*\(\s*\)\s*(?:/|\+)")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def strip_comments(text):
    """Remove /* */ (multi-line) and // line comments so commented mentions aren't flagged."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def is_exempt(raw_lines, idx):
    """True if the matched line or any of the 2 preceding RAW lines carries the exempt token.

    Must run on raw (un-stripped) lines: the exempt token lives in a `//` comment that
    strip_comments() would otherwise delete before scanning.
    """
    for j in range(max(0, idx - 2), idx + 1):
        if EXEMPT_TOKEN in raw_lines[j].upper():
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    violations = []
    files_scanned = 0

    for sd in SCAN_DIRS:
        base = os.path.join(root, sd)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in filenames:
                if not fn.endswith(SCAN_EXTS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, "/")
                if rel in EXEMPT_FILES:
                    continue
                files_scanned += 1
                with open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace") as f:
                    raw = f.read()
                raw_lines = raw.splitlines()
                lines = strip_comments(raw).splitlines()  # comment-stripped, for token matching
                for i, sline in enumerate(lines):
                    ln = i + 1
                    if is_exempt(raw_lines, i):
                        continue
                    if TEMP_DIR_RE.search(sline):
                        violations.append(
                            f"{rel}:{ln}: uses temp_directory_path() instead of isolation::temp_dir()")
                        continue
                    if BARE_TMP_RE.search(sline):
                        violations.append(
                            f"{rel}:{ln}: writes to a bare /tmp path literal instead of test_temp/")
                        continue
                    if CWD_WRITE_RE.search(sline):
                        violations.append(
                            f"{rel}:{ln}: writes into current_path() (cwd) instead of test_temp/")

    if violations:
        print("[FAIL] test temp-file hygiene violations (C4: use isolation::temp_dir(), not "
              "temp_directory_path()/bare /tmp/cwd writes):")
        for v in violations:
            print(f"  {v}")
        print("  legit exceptions must carry a 'TEST_TEMP_EXEMPT: <reason>' comment "
              "on the line or the one above")
        return 1

    print(f"[PASS] no test temp-file bypasses in {files_scanned} scanned files under tests/")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# ============================================================================
# check_version_consistency.py - version-consistency gate
# ----------------------------------------------------------------------------
# Check points:
#   1) [blocking] CHANGELOG.md's currentVersion (in the `aurora-changelog-meta`
#      HTML comment block near the top of the file) must equal the library's true
#      version (AURORA_VERSION_STRING in include/aurora/core/version.h).
#      The two were long kept in sync manually and sometimes CHANGELOG led or
#      lagged the library version; if they mismatch, version numbers copied
#      elsewhere at release (CLI/README/docs) would be based on the wrong baseline.
#   2) [non-blocking] CHANGELOG.md body prose (e.g. the "NNN standalone executable
#      tests" style wording in historical entries), if it disagrees with the count
#      actually measured in the repo, only warns, does not error (descriptive text
#      naturally goes stale with refactors and should not block CI).
#
# Exit code: 1 only when item 1 mismatches; otherwise 0.
#
# Usage:
#   python3 tools/check/check_version_consistency.py [--root <aurora_root>]
# ============================================================================
import argparse
import glob
import os
import re
import sys

# currentVersion lives in the machine-readable meta block at the top of CHANGELOG.md:
#   <!-- aurora-changelog-meta
#   currentVersion: 1.0.0-alpha.9
#   -->
META_CURRENT_VERSION_RE = re.compile(r"^currentVersion:\s*(\S+)", re.MULTILINE)
# Non-blocking descriptive check: wording like "188 standalone executable tests".
PROSE_TESTS_RE = re.compile(r"(\d+)\s*个独立可执行测试")


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt (still resolves correctly when the
    script lives under tools/check/)."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def read_library_version(include_root):
    """Reconstruct the actual value of AURORA_VERSION_STRING from version.h.

    AURORA_VERSION_STRING itself is a macro concatenation in version.h
    (AURORA_VERSION_NUMERIC "-" AURORA_VERSION_SUFFIX_STR), not a literal string, so instead read
    MAJOR/MINOR/PATCH and the optional SUFFIX and assemble them here.
    """
    path = os.path.join(include_root, "aurora", "core", "version.h")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        text = f.read()

    def _def(name, cast=int):
        m = re.search(r'#define\s+' + name + r'\s+([^\s]+)', text)
        if not m:
            return None
        try:
            return cast(m.group(1))
        except ValueError:
            return None

    major = _def("AURORA_VERSION_MAJOR")
    minor = _def("AURORA_VERSION_MINOR")
    patch = _def("AURORA_VERSION_PATCH")
    if major is None or minor is None or patch is None:
        return None
    ver = f"{major}.{minor}.{patch}"
    has_suffix = _def("AURORA_HAS_VERSION_SUFFIX")
    if has_suffix:
        ms = re.search(r'#define\s+AURORA_VERSION_SUFFIX_STR\s+"([^"]*)"', text)
        if ms and ms.group(1):
            ver += "-" + ms.group(1)
    return ver


def count_test_sources(root):
    return len(glob.glob(os.path.join(root, "tests", "*.cpp")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    include_root = os.path.join(root, "include")
    changelog = os.path.join(root, "CHANGELOG.md")

    # ---- Item 1: blocking version consistency ----
    lib_ver = read_library_version(include_root)
    if lib_ver is None:
        print(f"[ERR] cannot read library version: {include_root}/aurora/core/version.h", file=sys.stderr)
        return 2

    if not os.path.isfile(changelog):
        print(f"[ERR] CHANGELOG.md not found: {changelog}", file=sys.stderr)
        return 2

    with open(changelog, encoding="utf-8") as f:
        changelog_text = f.read()

    m = META_CURRENT_VERSION_RE.search(changelog_text)
    if not m:
        print("[FAIL] CHANGELOG.md is missing the currentVersion field "
              "(expected in the aurora-changelog-meta HTML comment block)")
        return 1
    current = m.group(1)
    if current != lib_ver:
        print(
            f"[FAIL] version mismatch: CHANGELOG.currentVersion={current} but library AURORA_VERSION_STRING={lib_ver}")
        return 1

    # ---- Item 2: non-blocking wording warning ----
    warnings = []
    for mm in PROSE_TESTS_RE.finditer(changelog_text):
        stated = int(mm.group(1))
        actual = count_test_sources(root)
        if stated != actual:
            warnings.append(
                f"CHANGELOG.md states \"{stated} standalone executable tests\", but tests/*.cpp actually has {actual}"
            )

    print(f"[PASS] versions match: currentVersion={current} == library {lib_ver}")
    if warnings:
        for w in warnings:
            print(
                f"[WARN] {w} (non-blocking: descriptive text naturally goes stale with refactors; sync when convenient)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

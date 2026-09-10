#!/usr/bin/env python3
# ============================================================================
# check_platform_macros.py - zero-raw-platform-macro gate (requirement #14)
# ----------------------------------------------------------------------------
# Spec: codespec/specification/06-app-platform.md §12.1
#   "平台 / 架构 / 位宽 / 编译器 / 能力分支一律使用 core/platform.h 的规范化目标宏
#    (AURORA_PLATFORM_* / AURORA_ARCH_* / AURORA_BIT_* / AURORA_COMPILER_* /
#    AURORA_CAP_*)，禁止直接书写 _WIN32 / __linux__ 等原生宏。例外：该头自身、
#    third_party/、CMake 脚本、_WIN32_WINNT 等 SDK 版本旋钮。"
#
# Check points:
#   1) [blocking] No raw platform/arch/bit-width macro may appear in a
#      preprocessor condition (#if/#ifdef/#ifndef/#elif, including line
#      continuations) under include/ or src/. The canonical-macro definition
#      site (include/aurora/core/platform.h) is exempt; SDK version knobs
#      (_WIN32_WINNT / _WIN32_IE) are not in the banned set by design.
#      Compiler-feature macros (__GNUC__ / __clang__ / _MSC_VER) are NOT
#      platform/arch/bit-width macros and stay allowed.
#   2) [report only] Density of canonical AURORA_PLATFORM_*/AURORA_ARCH_*/
#      AURORA_BIT_*/AURORA_BACKEND_*/AURORA_CAP_* conditional branches is
#      printed for trend tracking; it never fails the gate.
#
# Exit code: 1 when a banned raw macro is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_platform_macros.py [--root <aurora_root>]
# ============================================================================
import argparse
import os
import re
import sys

# Raw native macros banned from preprocessor conditions (token-exact match,
# so _WIN32_WINNT / _WIN32_IE SDK knobs are naturally NOT in this set).
BANNED_MACROS = {
    # Platform family
    "_WIN32", "_WIN64", "__linux__", "__APPLE__", "__MACH__", "__unix__",
    "__unix", "__FreeBSD__", "__NetBSD__", "__OpenBSD__", "__DragonFly__",
    "__ANDROID__", "__EMSCRIPTEN__", "__wasm__",
    # CPU architecture
    "_M_X64", "_M_AMD64", "__x86_64__", "__amd64__", "_M_IX86", "__i386__",
    "_M_ARM64", "__aarch64__", "_M_ARM", "__arm__", "__riscv", "__riscv_xlen",
    # Bit width
    "__SIZEOF_POINTER__",
}

CANONICAL_PREFIXES = ("AURORA_PLATFORM_", "AURORA_ARCH_", "AURORA_BIT_", "AURORA_BACKEND_", "AURORA_CAP_")

# Files exempt from the banned-macro rule (the canonical-macro definition
# site must itself inspect raw macros to define them).
EXEMPT_FILES = {
    "include/aurora/core/platform.h",
}

SCAN_DIRS = ("include", "src")
SCAN_EXTS = (".h", ".hpp", ".hh", ".inl", ".cpp", ".cc")
COND_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif)\b(.*)$")
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def iter_source_files(root):
    for sd in SCAN_DIRS:
        base = os.path.join(root, sd)
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in filenames:
                if fn.endswith(SCAN_EXTS):
                    yield os.path.join(dirpath, fn)


def logical_preproc_lines(text):
    """Yield (line_no, directive_plus_condition) with backslash continuations
    joined, so multi-line #if conditions are scanned as one logical line."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        start = i + 1
        m = COND_RE.match(line)
        if m:
            cond = m.group(2)
            while cond.rstrip().endswith("\\") and i + 1 < len(lines):
                cond = cond.rstrip()[:-1]
                i += 1
                cond += " " + lines[i]
            yield start, m.group(1), cond
        i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    violations = []
    canonical_hits = 0
    files_scanned = 0

    for path in sorted(iter_source_files(root)):
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        if rel in EXEMPT_FILES:
            continue
        files_scanned += 1
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        for line_no, _directive, cond in logical_preproc_lines(text):
            tokens = set(TOKEN_RE.findall(cond))
            bad = sorted(tokens & BANNED_MACROS)
            if bad:
                violations.append(
                    f"{rel}:{line_no}: raw platform/arch macro(s) in preprocessor condition: {', '.join(bad)}")
            canonical_hits += sum(1 for t in tokens if t.startswith(CANONICAL_PREFIXES))

    if violations:
        print("[FAIL] raw platform/arch/bit-width macros found in preprocessor conditions")
        print("       (spec: 06-app-platform.md §12.1 — use AURORA_PLATFORM_*/AURORA_ARCH_*/AURORA_BIT_*")
        print("        from core/platform.h; core/platform.h itself and SDK knobs are exempt):")
        for v in violations:
            print(f"  {v}")
        return 1

    print(f"[PASS] no raw platform/arch macros in {files_scanned} source files "
          f"(include/ + src/, platform.h exempt)")
    print(f"[INFO] canonical macro conditional branches (report only, not gated): {canonical_hits}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

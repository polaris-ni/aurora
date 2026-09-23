#!/usr/bin/env python3
"""clang-format gate runner for the Aurora project.

Why this exists instead of `clang-format --dry-run --Werror <files>`:
  * it scopes the check to **first-party tracked sources** only — third_party/
    carries its own vendored .clang-format files and must never be touched;
  * it is parallel, which matters at ~823 source files;
  * it prints a stable per-file / total summary suitable for CI logs instead of
    streaming hundreds of raw unified diffs;
  * it hard-codes the invocation form that is known to be correct.

Invocation correctness matters more than it looks: clang-format resolves the
`file` style by walking up from the **arguments'** directory. Feeding it a
relative `--assume-filename`, or running with a different cwd, silently falls
back to the built-in default style and produces conclusions that point the
opposite way. Hence: absolute path arguments + cwd pinned to the repository root.

Usage:
    run_clang_format.py [--fix] [--jobs N] [--include REGEX] [--clang-format PATH]

--clang-format lets the caller pin the executable instead of gambling on PATH. cmake passes
the binary it already validated (cmake/AuroraUtils.cmake: aurora_find_clang_format probes
candidates with `--dump-config --style=file` and rejects any build that cannot parse this
repo's .clang-format). That rejection is not hypothetical — the config uses enum values
newer than distro packages (`BinPackParameters: BinPack`), and an old clang-format exits 1
with `error: invalid boolean`, which would look like a repo-wide style failure.

Exit codes:
    0  every checked file already conforms (or --fix finished)
    1  at least one file differs from the style (check mode)
    2  usage / environment error (clang-format missing, no source files)
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

EXTS = ("*.cpp", "*.cc", "*.h", "*.hpp", "*.cxx")

# Skip vendored trees: they ship their own .clang-format and are not ours to restyle.
DEFAULT_EXCLUDE = re.compile(r"(^|/)third_party/|(^|/)build(-[a-z0-9]+)?/|^\.git/")


def find_root() -> str:
    """Locate the repository root from this script's location."""
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def source_files(root: str, include: re.Pattern | None) -> list[str]:
    """Enumerate first-party sources. Prefer git; fall back to a filesystem walk."""
    try:
        proc = subprocess.run(
            ["git", "ls-files", *EXTS], capture_output=True, text=True,
            encoding="utf-8", errors="replace", cwd=root,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            candidates = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        else:
            candidates = walk_sources(root)
    except (OSError, FileNotFoundError):
        candidates = walk_sources(root)

    out = []
    for rel in candidates:
        rel = rel.replace("\\", "/")
        if DEFAULT_EXCLUDE.search(rel):
            continue
        if include is not None and not include.search(rel):
            continue
        if os.path.exists(os.path.join(root, rel.replace("/", os.sep))):
            out.append(rel)
    return sorted(set(out))


def walk_sources(root: str) -> list[str]:
    """Fallback enumeration when git is unavailable (e.g. source tarball CI)."""
    suffixes = tuple(e.lstrip("*") for e in EXTS)
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "third_party") and not d.startswith("build")]
        for name in filenames:
            if name.endswith(suffixes):
                rel = os.path.relpath(os.path.join(dirpath, name), root).replace("\\", "/")
                out.append(rel)
    return out


def run_format(root: str, cf: str, rel: str, fix: bool) -> tuple[str, int, str]:
    abs_path = os.path.join(root, rel.replace("/", os.sep))
    args = [cf, "--style=file"]
    if fix:
        args.append("-i")
    args.append(abs_path)
    proc = subprocess.run(
        args, capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=root,
    )
    if proc.returncode != 0:
        return (rel, -1, (proc.stderr or "").strip().splitlines()[0][:140] if proc.stderr else "unknown error")
    if fix:
        # -i rewrites in place; re-check that the result is now stable (idempotent).
        verify = subprocess.run(
            [cf, "--style=file", abs_path], capture_output=True, text=True,
            encoding="utf-8", errors="replace", cwd=root,
        )
        current = read_text(abs_path)
        return (rel, 0, "" if (verify.stdout or "").replace("\r\n", "\n") == current else "NOT-IDEMPOTENT")
    current = read_text(abs_path)
    produced = (proc.stdout or "").replace("\r\n", "\n")
    if produced == current:
        return (rel, 0, "")
    import difflib

    old_lines = current.split("\n")
    new_lines = produced.split("\n")
    delta = sum(
        1
        for line in difflib.unified_diff(old_lines, new_lines, lineterm="", n=0)
        if line[:1] in "+-" and line[:3] not in ("+++", "---")
    )
    return (rel, max(delta, 1), "")


def read_text(path: str) -> str:
    with open(path, encoding="utf-8", newline="") as fh:
        return fh.read().replace("\r\n", "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="clang-format gate for first-party Aurora sources")
    parser.add_argument("--fix", action="store_true", help="rewrite files in place instead of only checking")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel clang-format processes")
    parser.add_argument("--include", default=None, help="restrict to paths matching this regex")
    parser.add_argument("--show", type=int, default=40, help="max offending files to list")
    parser.add_argument("--clang-format", default=None,
                        help="clang-format executable to use (default: first of clang-format / "
                             "clang-format.exe on PATH); callers pin it after validating the config")
    args = parser.parse_args()

    root = find_root()
    include = re.compile(args.include) if args.include else None

    cf = None
    from shutil import which as _which

    if args.clang_format:
        # Explicit path from the build system: trust it verbatim (a plain name like
        # "clang-format-22" still resolves through PATH).
        cf = args.clang_format if os.path.isabs(args.clang_format) else _which(args.clang_format)
        if not cf:
            sys.stderr.write("error: --clang-format %r not found\n" % args.clang_format)
            return 2
    else:
        for candidate in ("clang-format", "clang-format.exe"):
            cf = _which(candidate)
            if cf:
                break
    if not cf:
        sys.stderr.write("error: clang-format not found on PATH\n")
        return 2

    files = source_files(root, include)
    if not files:
        sys.stderr.write("error: no first-party source files found under %s\n" % root)
        return 2

    mode = "fix" if args.fix else "check"
    print("clang-format gate: %d first-party source files (mode=%s, jobs=%d)" % (len(files), mode, args.jobs))

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_format, root, cf, rel, args.fix) for rel in files]
        for i, fut in enumerate(futures, 1):
            results.append(fut.result())
            if i % 200 == 0:
                print("  ... %d/%d" % (i, len(files)), flush=True)

    failures = [r for r in results if r[1] < 0]
    offenders = sorted([r for r in results if r[1] > 0], key=lambda r: -r[1])
    note_worthy = [r for r in results if r[1] == 0 and r[2]]

    print()
    print("checked        : %d" % len(files))
    print("conforming     : %d" % (len(files) - len(offenders) - len(failures)))
    print("non-conforming : %d" % len(offenders))
    print("diff lines     : %d" % sum(r[1] for r in offenders))
    if failures:
        print("failures       : %d" % len(failures))
    if note_worthy:
        print("warnings       : %d" % len(note_worthy))

    if offenders:
        print()
        print("--- %s (top %d) ---" % ("rewritten, verify with git diff" if args.fix else "non-conforming files", args.show))
        for rel, delta, _note in offenders[: args.show]:
            print("  %6d  %s" % (delta, rel))
    if failures:
        print()
        print("--- clang-format errors ---")
        for rel, _delta, msg in failures[:20]:
            print("  %-52s %s" % (rel.split("/")[-1], msg))
    if note_worthy:
        print()
        print("--- notes ---")
        for rel, _delta, note in note_worthy[:20]:
            print("  %-52s %s" % (rel.split("/")[-1], note))

    if args.fix:
        print()
        print("='fix' completed; review 'git diff' before committing.")
        return 0
    return 1 if offenders or failures else 0


if __name__ == "__main__":
    sys.exit(main())

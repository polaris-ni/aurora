#!/usr/bin/env python3
"""
check_core_layer_boundary.py - guard the `core/` layer boundary.

Invariant guarded (ARCHITECTURE.md §2): the **public headers** of `core/` (the Foundation
layer) must not depend on any other aurora module. Everything else may depend on `core/`;
`core/` depends only on itself and on the standard library.

Why headers only (and not src/aurora/core/):
- The layer boundary is a compile-time interface contract. A header that reaches upward
  forces every downstream consumer of `core/` to drag in the upper module (include
  explosion + conceptual inversion: the foundation would know its consumers).
- Implementation files are deliberately out of scope: `src/aurora/core/diagnostics.cpp`
  formats `props_io` prop values and `src/aurora/core/image.cpp` delegates to the
  `image/` codec module — both are leaf-level, non-transitive consultation of upper-layer
  metadata that never leaks into the headers. They are recorded as known exceptions in
  ARCHITECTURE.md §2 and deliberately not gated.

Rules:
- Scan every `*.h` under `include/aurora/core/` (recursive).
- An `#include "aurora/<module>/..."` whose `<module>` is not `core` -> violation.
- An `#include "aurora/aurora.h"` / `"aurora/aurora_pch.h"` (umbrella/aggregate headers)
  -> violation: `core/` is the bottom of the graph and must never pull the whole library.
- A scan that finds zero core headers is a hard error, never a vacuous pass.

Usage:
  python3 tools/check/check_core_layer_boundary.py [--root <aurora_root>]

Exit codes: 0 = clean, 1 = violation(s), 2 = cannot scan / bad usage.
"""
import argparse
import os
import re
import sys

MODULE_INC_RE = re.compile(r'^\s*#\s*include\s+"aurora/([A-Za-z_][A-Za-z0-9_]*)/([^"]*)"')
UMBRELLA_RE = re.compile(r'^\s*#\s*include\s+"aurora/(aurora|aurora_pch)\.h"')

ALLOWED_MODULE = "core"


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt (still resolves correctly when the
    script lives under tools/check/)."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def iter_core_headers(root):
    base = os.path.join(root, "include", "aurora", "core")
    if not os.path.isdir(base):
        return []
    out = []
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in filenames:
            if name.endswith((".h", ".hpp", ".hh", ".inl")):
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Guard the core/ layer boundary.")
    ap.add_argument("--root", default=None, help="aurora repo root (defaults to auto-detect)")
    args = ap.parse_args()

    root = os.path.abspath(args.root) if args.root else repo_root_of(__file__)
    headers = iter_core_headers(root)
    if not headers:
        print(f"[FAIL] no headers found under {os.path.join(root, 'include', 'aurora', 'core')}")
        print("       refusing to report a vacuous pass")
        return 2

    violations = []  # (relpath, lineno, text, module)
    for path in headers:
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                lines = handle.read().splitlines()
        except OSError as exc:
            print(f"[FAIL] cannot read {rel}: {exc}")
            return 2
        for idx, line in enumerate(lines, 1):
            umbrella = UMBRELLA_RE.match(line)
            if umbrella:
                violations.append((rel, idx, line.strip(), "aurora (umbrella)"))
                continue
            match = MODULE_INC_RE.match(line)
            if match and match.group(1) != ALLOWED_MODULE:
                violations.append((rel, idx, line.strip(), match.group(1)))

    if violations:
        print(f"[FAIL] core/ layer boundary violated ({len(violations)} include(s) reaching upward):")
        for rel, idx, text, module in violations:
            print(f"  - {rel}:{idx}  -> module '{module}'")
            print(f"      {text}")
        print()
        print("  core/ (Foundation) must not depend on any other aurora module. Move the")
        print("  upward-reaching code into the upper module (see widget/a11y_tree.h for the")
        print("  precedent), or forward-declare and keep the definition out of core/.")
        return 1

    print(f"[PASS] core/ layer boundary clean: {len(headers)} headers, 0 cross-module includes.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

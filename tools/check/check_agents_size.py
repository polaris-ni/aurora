#!/usr/bin/env python3
"""
check_agents_size.py - keep the repository-root AGENTS.md inside its injection budget.

Why this gate exists
--------------------
AGENTS.md is the single entry point injected verbatim into every coding-agent session.
It carries only two duties: route to the authoritative document under codespec/, and list
the hard rules that must not be violated. When it outgrows the injection budget it is
silently truncated mid-file, and the part that gets dropped is always the tail -- the
document map and the hard rules, i.e. exactly the two duties it exists for. Observed on
this repository: at 23847 bytes the injection stopped at 11705 bytes (49%), so sections 4,
5 and 6 (51% of the file) never reached the model.

Rules
-----
- AGENTS.md must exist, be valid UTF-8, and expose at least one level-2 heading.
  A missing file, an undecodable file, or a scan that finds 0 sections is a hard error --
  never a vacuous pass (same rule as check_arch_module_map.py: a check that inspects
  nothing must fail, not succeed).
- Its size in UTF-8 bytes must not exceed the limit (default 8192 = 8 KiB).

Usage
-----
  python3 tools/check/check_agents_size.py [--root <aurora_root>] [--limit-bytes <n>]
"""
import argparse
import os
import re
import sys

DOC_REL = "AGENTS.md"
DEFAULT_LIMIT_BYTES = 8192
SEC_HEADING_RE = re.compile(r"^##\s+\S")


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt."""
    d = os.path.dirname(os.path.abspath(path))
    prev = None
    while d and d != prev:
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        prev = d
        d = os.path.dirname(d)
    return d


def section_sizes(lines):
    """Return [(heading, byte_size)] per level-2 section, in file order.

    The leading block before the first level-2 heading is reported under the empty name.
    Each block counts its own trailing newline, so the sum is the file size plus one.
    """
    out = []
    cur_name = ""
    cur_bytes = 0
    for line in lines:
        m = SEC_HEADING_RE.match(line)
        if m:
            out.append((cur_name, cur_bytes))
            cur_name = line[3:].strip()
            cur_bytes = 0
        cur_bytes += len(line.encode("utf-8")) + 1
    out.append((cur_name, cur_bytes))
    return out


def main():
    ap = argparse.ArgumentParser(description="Guard the AGENTS.md injection budget.")
    ap.add_argument("--root", default=None, help="aurora repository root (default: auto-detect)")
    ap.add_argument("--limit-bytes", type=int, default=DEFAULT_LIMIT_BYTES,
                    help=f"size limit in UTF-8 bytes (default {DEFAULT_LIMIT_BYTES})")
    args = ap.parse_args()

    root = os.path.abspath(args.root) if args.root else repo_root_of(__file__)
    doc = os.path.join(root, DOC_REL)

    if not os.path.isfile(doc):
        print(f"FAIL: {DOC_REL} not found under {root}")
        return 1
    try:
        with open(doc, "rb") as fh:
            raw = fh.read()
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        print(f"FAIL: {DOC_REL} is not valid UTF-8: {exc}")
        return 1

    lines = text.split("\n")
    sections = section_sizes(lines)
    if not any(name for name, _ in sections):
        # 0 level-2 sections means the scan inspected nothing: hard error, not a pass.
        print(f"FAIL: {DOC_REL} has no level-2 section heading - scan would be vacuous")
        return 1

    size = len(raw)
    print(f"{DOC_REL}: {size} bytes / limit {args.limit_bytes} bytes "
          f"({size * 100 // args.limit_bytes}% of budget, {len(sections)} blocks)")
    for name, nbytes in sections:
        label = name if name else "(preamble)"
        share = nbytes * 100 // size if size else 0
        print(f"  {label[:48]:<48} {nbytes:>6} bytes  {share:>3}%")

    if size > args.limit_bytes:
        over = size - args.limit_bytes
        print(f"FAIL: over budget by {over} bytes; move detail into codespec/ or shorten it")
        return 1

    print("PASS: within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
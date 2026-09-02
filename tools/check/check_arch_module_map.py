#!/usr/bin/env python3
"""
check_arch_module_map.py - validate header/source file references in the codespec ARCHITECTURE_RUNTIME §4 module map.

Rules:
- Only scan the content between "## 4. Module Map" and the next "## ".
- Extract all backtick-wrapped tokens ending in a source extension (.h/.hpp/.hh/.cpp/.cc/.cxx/.inl).
  - Type names/symbols (e.g. `Result<T>`, `Signal`, `Window`) have no extension and are excluded automatically.
  - Directory references (e.g. `core/`) have no extension and are excluded automatically.
- Resolution strategy:
  - Tokens containing "/" are treated as full paths relative to include/aurora/ (headers) or src/aurora/ (impl).
  - Bare filenames (no "/") are inferred from the directory prefix of the nearest table row's "path" cell;
    if they cannot be anchored, fall back to a full search under the corresponding root.
- Output per-entry STATUS (OK / MISSING / AMBIGUOUS / FALLBACK) and a summary.
- Exit code: MISSING or AMBIGUOUS present -> 1 (can be wired into CI); otherwise 0.

Usage:
  python3 tools/check/check_arch_module_map.py [--root <aurora_root>]
"""
import argparse
import os
import re
import sys


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt (still resolves correctly when the
    script lives under tools/check/)."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


DOC_REL = "codespec/ARCHITECTURE.md"
HEADER_EXT = {".h", ".hpp", ".hh", ".inl"}
IMPL_EXT = {".cpp", ".cc", ".cxx"}
SRC_EXT = HEADER_EXT | IMPL_EXT

TOKEN_RE = re.compile(r"`([^`]+)`")
FILE_RE = re.compile(r"^[\w./\\-]+$")  # backtick content must be valid path characters


def iter_section4_lines(lines):
    """Yield only the lines of the §4 section (including the §4 heading, stopping at the next "## ")."""
    in_sec = False
    for ln in lines:
        if ln.startswith("## 4."):
            in_sec = True
            yield ln
            continue
        if in_sec and ln.startswith("## ") and not ln.startswith("## 4."):
            return
        if in_sec:
            yield ln


def parse_table_row(line):
    """If this is a 3-column table row, return (module, path cell, full line text); otherwise None."""
    s = line.strip()
    if not s.startswith("|"):
        return None
    parts = [p.strip() for p in s.strip("|").split("|")]
    if len(parts) < 3:
        return None
    return parts[0], parts[1], line


def collect_refs(lines):
    """
    Returns refs: list of dict {token, base_dir, hint_dir, section_no, line_no, in_table}
    base_dir resolution priority (see resolve):
      - token inside a table row: use the row's "path" column directory (most precise).
      - token in free text / list item: prefer the body's hint directory (e.g. `(include/aurora/storage/)`),
        otherwise the nearest table row's module directory (inherited, e.g. §4.3 list items inherit window/).
      - top-level entry header (aurora.h etc.): check include/aurora/<tok> directly.
    """
    hint_re = re.compile(r"include/aurora/([A-Za-z_][\w/]*/)")  # capture a directory like storage/
    refs = []
    inherited_base = None  # nearest table row's module directory (inherited by free text)
    last_hint_dir = None  # nearest body hint directory
    sec_no = None
    line_no = 0
    for raw in lines:
        line_no += 1
        msec = re.match(r"^###\s+(4\.\d+)\s", raw)
        if msec:
            sec_no = msec.group(1)
        mhint = hint_re.search(raw)
        if mhint:
            last_hint_dir = mhint.group(1)
        row = parse_table_row(raw)
        row_base = None
        if row is not None:
            _, path_cell, _ = row
            pcell_tokens = TOKEN_RE.findall(path_cell)
            if pcell_tokens:
                pt = pcell_tokens[0]
                if pt.endswith("/") and "/" not in pt[:-1]:
                    row_base = pt
                    inherited_base = pt
        # Free text / list item: hint directory first, otherwise inherit the module directory
        eff_base = row_base if row is not None else (last_hint_dir or inherited_base)
        for tok in TOKEN_RE.findall(raw):
            if not FILE_RE.match(tok):
                continue
            ext = os.path.splitext(tok)[1].lower()
            if ext not in SRC_EXT:
                continue
            refs.append({
                "token": tok,
                "base_dir": row_base if row is not None else eff_base,
                "hint_dir": last_hint_dir,
                "sec": sec_no,
                "line": line_no,
                "in_table": row is not None,
            })
    return refs


def resolve(tok, base_dir, hint_dir, include_root, src_root):
    """Return (status, resolved_path).

    status semantics:
      OK        exists under the anchored directory / full path
      MISPLACED not found under the anchored directory (the row's "path" column) but found elsewhere
               -> doc has wrong module attribution (hard fail)
      FALLBACK  no anchored directory; resolved via fallback search (prefer making the doc explicit)
      MISSING   does not exist anywhere
      AMBIGUOUS fallback search hit multiple locations
    """
    ext = os.path.splitext(tok)[1].lower()
    is_impl = ext in IMPL_EXT
    if "/" in tok:
        cand = os.path.join((src_root if is_impl else include_root), tok)
        if os.path.isfile(cand):
            return "OK", os.path.relpath(cand)
        alt = os.path.join((include_root if is_impl else src_root), tok)
        if os.path.isfile(alt):
            return "OK", os.path.relpath(alt)
        return "MISSING", tok
    # Bare filename: prefer anchoring via the row's "path" column
    if base_dir:
        anchored = os.path.join((src_root if is_impl else include_root), base_dir, tok)
        if os.path.isfile(anchored):
            return "OK", os.path.relpath(anchored)
        # Not at the anchor -> full search; if it exists elsewhere it is "misplaced" (wrong doc module attribution)
        root = src_root if is_impl else include_root
        hits = [os.path.relpath(os.path.join(dp, tok))
                for dp, _, fn in os.walk(root) if tok in fn]
        if len(hits) == 1:
            return "MISPLACED", f"document written as `{base_dir}{tok}`, actually located at `{hits[0]}`"
        if len(hits) > 1:
            return "MISPLACED", f"document written as `{base_dir}{tok}`, actually at multiple locations: " + " | ".join(
                hits)
        return "MISSING", f"{base_dir}{tok}"
    # No in-row anchor: use the body hint directory (e.g. `(include/aurora/storage/)`)
    if hint_dir:
        anchored = os.path.join((src_root if is_impl else include_root), hint_dir, tok)
        if os.path.isfile(anchored):
            return "OK", os.path.relpath(anchored)
    # Top-level entry headers (aurora.h / aurora_fwd.h etc., directly under include/aurora/)
    top = os.path.join(include_root, tok)
    if os.path.isfile(top):
        return "OK", os.path.relpath(top)
    # Fallback: full search
    root = src_root if is_impl else include_root
    hits = [os.path.relpath(os.path.join(dp, tok))
            for dp, _, fn in os.walk(root) if tok in fn]
    if len(hits) == 1:
        return "FALLBACK", hits[0]
    if len(hits) > 1:
        return "AMBIGUOUS", " | ".join(hits)
    return "MISSING", tok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)
    doc_path = os.path.join(root, DOC_REL)
    include_root = os.path.join(root, "include", "aurora")
    src_root = os.path.join(root, "src", "aurora")

    if not os.path.isfile(doc_path):
        print(f"[ERR] document not found: {doc_path}", file=sys.stderr)
        return 2
    if not os.path.isdir(include_root):
        print(f"[ERR] header dir not found: {include_root}", file=sys.stderr)
        return 2

    with open(doc_path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    sec_lines = list(iter_section4_lines(lines))
    refs = collect_refs(sec_lines)

    # Dedupe (same token + same anchor reported once)
    seen = set()
    rows = []
    for r in refs:
        status, resolved = resolve(r["token"], r["base_dir"], r["hint_dir"], include_root, src_root)
        key = (r["token"], resolved, status)
        if key in seen:
            continue
        seen.add(key)
        rows.append((r, status, resolved))

    # Print report
    print(f"Module-map consistency check — {DOC_REL}")
    print(f"Scanned {len(rows)} file references (after dedup of fallback/ambiguous)\n")
    print(f"{'STATUS':<10} {'SEC':<6} {'TOKEN':<34} RESOLVED")
    print("-" * 90)
    n_ok = n_miss = n_amb = n_fb = n_mis = 0
    for r, status, resolved in rows:
        loc = f"[{r['sec'] or '?'}: L{r['line']}]"
        print(f"{status:<10} {loc:<16} {r['token']:<30} {resolved}")
        if status == "OK":
            n_ok += 1
        elif status == "MISSING":
            n_miss += 1
        elif status == "AMBIGUOUS":
            n_amb += 1
        elif status == "FALLBACK":
            n_fb += 1
        elif status == "MISPLACED":
            n_mis += 1

    print("-" * 90)
    print(f"OK={n_ok}  FALLBACK={n_fb}  MISPLACED={n_mis}  MISSING={n_miss}  AMBIGUOUS={n_amb}")
    if n_miss or n_amb or n_mis:
        print("\n[FAIL] missing/ambiguous/misplaced references exist; fix the docs or code.")
        return 1
    if n_fb:
        print(
            "\n[WARN] some references resolved via fallback search (docs did not give an explicit path prefix); prefer making them explicit.")
    print("\n[PASS] no missing/ambiguous/misplaced references.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

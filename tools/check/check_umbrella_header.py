#!/usr/bin/env python3
"""
check_umbrella_header.py - guard the integrity of the umbrella header.

`include/aurora/aurora.h` is the single entry point every consumer includes, yet it had no
gate at all until now. A `style(lint)!` clang-format sweep once merged two `#include` lines
into one and swallowed the second into a trailing `//` comment: the include became invisible
to the preprocessor while the file still compiled, because another header happened to pull
it in transitively. Nothing failed, nothing warned, and it stayed latent for months.

This gate has two layers.

Layer 1 - file health (self-contained, no manifest needed):
  R1  at most one `#include` directive per code line
  R2  no `#include` may sit after `//` on a code line (trailing-comment swallow)
  R3  no duplicate include path
  R4  every included path exists on disk
  R5  no Unicode private-use / replacement character (encoding-corruption fingerprint)
  R6  no BOM and no lone CR (line-ending mix)

Layer 2 - coverage contract (manifest driven, see umbrella_manifest.txt):
  R7  the direct-include set must not shrink relative to the `[direct]` baseline; a silently
      deleted include line is a regression. Growth is reported as a hint to sync the baseline.
  R8  every header under include/aurora/ must be classified - directly included, reachable
      from the direct set via transitive includes, or listed in `[exempt]` /
      `[exempt-review]`. An unclassified header fails the gate, forcing an explicit decision
      instead of letting a public header drift out of the umbrella unnoticed.

Reachability is computed on comment-stripped text: a `#include` written inside a doc comment
(as in the file's own header) is prose, not an edge of the include graph.

Usage:
  python3 tools/check/check_umbrella_header.py [--root <aurora_root>]
  python3 tools/check/check_umbrella_header.py --write-manifest   # refresh [direct] section

Exit code: 1 on any hard failure, 2 on usage / precondition error, 0 otherwise.
"""
import argparse
import fnmatch
import os
import re
import sys

UMBRELLA_REL = "include/aurora/aurora.h"
MANIFEST_REL = "tools/check/umbrella_manifest.txt"
INCLUDE_REL = "include"
PKG_PREFIX = "aurora/"

SECTION_ALIASES = {"direct": "direct", "exempt": "exempt", "exempt-review": "review"}

# `#include "aurora/..."` 形式的包含指令；锚定行首（允许缩进）与空白分隔。
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"(' + PKG_PREFIX + r'[^"]+)"')
INCLUDE_ANY_RE = re.compile(r'#\s*include')
COMMENT_SPAN_RE = re.compile(r"/\*.*?\*/", re.S)

# Unicode 私用区与替换字符：源码中不存在合法用途，出现即编码损坏。
_CORRUPT_RANGES = ((0xE000, 0xF8FF), (0xF0000, 0xFFFFD), (0x100000, 0x10FFFD))


def repo_root_of(path):
    """Walk up to the repo root containing CMakeLists.txt."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def strip_block_comments(text):
    """Replace /* ... */ spans with blanks, preserving line count and column positions."""
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    return COMMENT_SPAN_RE.sub(blank, text)


def is_corrupt_char(ch):
    cp = ord(ch)
    if cp == 0xFFFD:
        return True
    return any(lo <= cp <= hi for lo, hi in _CORRUPT_RANGES)


def split_line_comment(line):
    """Split a physical line into (code, comment) at the first `//`, ignoring `//` inside a
    string literal is unnecessary here: include lines carry no string literals with slashes."""
    pos = line.find("//")
    if pos < 0:
        return line, ""
    return line[:pos], line[pos:]


def extract_includes(text):
    """All aurora include paths of comment-stripped text, in order, duplicates preserved."""
    stripped = strip_block_comments(text)
    out = []
    for line in stripped.split("\n"):
        code, _ = split_line_comment(line)
        m = INCLUDE_RE.match(code)
        if m:
            out.append(m.group(1))
    return out


def analyse_layer1(raw_bytes):
    """File-health checks on the umbrella header itself. Returns (direct_paths, problems).

    problems is a list of (rule, line_no, message).
    """
    problems = []
    text = raw_bytes.decode("utf-8", errors="replace")

    # ---- R6: BOM / lone CR / mixed line endings ----
    if raw_bytes.startswith(b"\xef\xbb\xbf"):
        problems.append(("R6", 0, "file starts with a UTF-8 BOM"))
    crlf = raw_bytes.count(b"\r\n")
    lone_cr = raw_bytes.count(b"\r") - crlf
    bare_lf = raw_bytes.count(b"\n") - crlf
    if lone_cr:
        problems.append(("R6", 0, f"{lone_cr} lone CR byte(s) not part of CRLF"))
    if crlf and bare_lf:
        problems.append(("R6", 0, f"mixed line endings: {crlf} CRLF vs {bare_lf} LF"))

    # ---- R5: encoding-corruption fingerprint ----
    for idx, line in enumerate(text.split("\n"), 1):
        hits = [ch for ch in line if is_corrupt_char(ch)]
        if hits:
            codes = ", ".join("U+%04X" % ord(c) for c in hits[:4])
            problems.append(("R5", idx, f"private-use / replacement character(s): {codes}"))

    # ---- R1 / R2 / R3 / R4 ----
    code = strip_block_comments(text)
    direct = []
    for idx, line in enumerate(code.split("\n"), 1):
        stripped = line.strip()
        if not stripped:
            continue
        # 纯注释行：doc 示例里的 `#include "…"` 是说明文字，不参与解析（见文件头 §9 示例）。
        if stripped.startswith(("*", "//", "/*")):
            continue
        body, comment = split_line_comment(line)
        n_body = len(INCLUDE_ANY_RE.findall(body))
        n_comment = len(INCLUDE_ANY_RE.findall(comment))
        if n_body > 1:
            problems.append(("R1", idx, f"{n_body} include directives on one code line"))
        if n_comment and n_body >= 1:
            problems.append(("R2", idx, "include directive swallowed by a trailing // comment"))
        elif n_comment and n_body == 0 and INCLUDE_ANY_RE.search(comment):
            # 行内有 include 但完全落在注释里，且该行不是纯注释行 —— 同上，属吞并。
            problems.append(("R2", idx, "include directive fully inside a trailing // comment"))
        m = INCLUDE_RE.match(body)
        if m:
            direct.append(m.group(1))

    seen = {}
    for path in direct:
        seen[path] = seen.get(path, 0) + 1
    for path, n in sorted(seen.items()):
        if n > 1:
            problems.append(("R3", 0, f"duplicate include path ({n}x): {path}"))

    return direct, problems


def check_paths_exist(direct, include_root):
    problems = []
    for path in direct:
        if not os.path.isfile(os.path.join(include_root, path)):
            problems.append(("R4", 0, f"included path does not exist on disk: {path}"))
    return problems


def load_manifest(path):
    """Parse the manifest into {"direct": [(pat, reason)], "exempt": [...], "review": [...]}."""
    sections = {"direct": [], "exempt": [], "review": []}
    current = None
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                name = line[1:-1].strip()
                if name not in SECTION_ALIASES:
                    raise ValueError(f"unknown manifest section: [{name}]")
                current = SECTION_ALIASES[name]
                continue
            if current is None:
                raise ValueError(f"manifest entry outside of any section: {line}")
            if "->" in line:
                pat, reason = line.split("->", 1)
                sections[current].append((pat.strip(), reason.strip()))
            else:
                sections[current].append((line, ""))
    return sections


def scan_header_graph(include_root):
    """Map every header to its direct aurora includes, computed on comment-stripped text."""
    graph = {}
    for dirpath, _dirnames, filenames in os.walk(include_root):
        for name in filenames:
            if not name.endswith(".h"):
                continue
            full = os.path.join(dirpath, name)
            # relpath 已含 `aurora/` 段（相对 include/），与 INCLUDE_RE 捕获的路径同形。
            rel = os.path.relpath(full, include_root).replace("\\", "/")
            with open(full, encoding="utf-8", errors="replace") as handle:
                graph[rel] = extract_includes(handle.read())
    return graph


def closure_of(seeds, graph):
    """Transitive include closure of the seed set."""
    seen = set()
    stack = list(seeds)
    while stack:
        node = stack.pop()
        if node in seen:
            continue
        seen.add(node)
        for nxt in graph.get(node, ()):
            if nxt not in seen:
                stack.append(nxt)
    return seen


def matches_any(path, patterns):
    return any(fnmatch.fnmatch(path, pat) for pat, _ in patterns)


def write_manifest(path, direct):
    """Refresh only the [direct] section; the exempt sections stay hand-maintained."""
    try:
        sections = load_manifest(path)
    except (OSError, ValueError):
        sections = {"direct": [], "exempt": [], "review": []}
    lines = [
        "# Umbrella header baseline - include/aurora/aurora.h",
        "# Read by tools/check/check_umbrella_header.py. Format: `pattern -> reason`;",
        "#   `# ` starts a comment; blank lines are ignored; `*` in a pattern matches any run.",
        "#",
        "# [direct]        headers the umbrella must include directly (R7 fails on any removal).",
        "# [exempt]        headers deliberately kept out of the umbrella, with the design reason.",
        "# [exempt-review] unreachable public-looking headers awaiting a maintainer decision.",
        "",
        "[direct]",
    ]
    lines += sorted(direct)
    lines.append("")
    lines.append("[exempt]")
    for pat, reason in sections["exempt"]:
        lines.append(f"{pat} -> {reason}" if reason else pat)
    lines.append("")
    lines.append("[exempt-review]")
    for pat, reason in sections["review"]:
        lines.append(f"{pat} -> {reason}" if reason else pat)
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: derived from script path)")
    ap.add_argument("--manifest", default=None, help="manifest path (default: <root>/%s)" % MANIFEST_REL)
    ap.add_argument("--write-manifest", action="store_true",
                    help="refresh the [direct] baseline from the current umbrella header, then exit")
    args = ap.parse_args()

    root = args.root or repo_root_of(__file__)
    umbrella = os.path.join(root, UMBRELLA_REL)
    manifest = args.manifest or os.path.join(root, MANIFEST_REL)
    include_root = os.path.join(root, INCLUDE_REL)

    if not os.path.isfile(umbrella):
        print(f"[ERR] umbrella header not found: {umbrella}", file=sys.stderr)
        return 2
    if not os.path.isdir(include_root):
        print(f"[ERR] include dir not found: {include_root}", file=sys.stderr)
        return 2

    with open(umbrella, "rb") as handle:
        raw_bytes = handle.read()

    direct, problems = analyse_layer1(raw_bytes)
    if not direct:
        print(f"[ERR] no `#include \"{PKG_PREFIX}...\"` directive parsed from {UMBRELLA_REL};"
              f" refusing to pass on an empty scan.", file=sys.stderr)
        return 2
    problems += check_paths_exist(direct, include_root)

    if args.write_manifest:
        write_manifest(manifest, direct)
        print(f"[OK] refreshed [direct] baseline with {len(direct)} entries: {MANIFEST_REL}")
        return 0

    if not os.path.isfile(manifest):
        print(f"[ERR] manifest not found: {manifest}", file=sys.stderr)
        return 2
    try:
        sections = load_manifest(manifest)
    except ValueError as exc:
        print(f"[ERR] {MANIFEST_REL}: {exc}", file=sys.stderr)
        return 2

    direct_set = set(direct)
    baseline = {pat for pat, _ in sections["direct"]}

    # ---- R7: the direct set must not shrink ----
    lost = sorted(baseline - direct_set)
    added = sorted(direct_set - baseline)

    # ---- R8: every header must be classified ----
    graph = scan_header_graph(include_root)
    reachable = closure_of(direct_set, graph)
    self_key = os.path.relpath(umbrella, include_root).replace("\\", "/")
    unclassified = []
    review_hits = []
    for path in sorted(graph):
        if path == self_key:
            continue  # 伞头不包含自身
        if path in direct_set or path in reachable:
            continue
        if matches_any(path, sections["exempt"]):
            continue
        if matches_any(path, sections["review"]):
            review_hits.append(path)
            continue
        unclassified.append(path)

    # ---- report ----
    print("Umbrella header integrity - %s" % UMBRELLA_REL)
    print(f"  direct includes   : {len(direct)} (baseline {len(baseline)})")
    print(f"  reachable closure : {len(reachable)}")
    print(f"  headers on disk   : {len(graph)}")
    print(f"  exempt / review   : {len(sections['exempt'])} rule(s) / {len(sections['review'])} rule(s)")
    print()

    for rule, line_no, msg in problems:
        loc = f"L{line_no}" if line_no else "-"
        print(f"  [FAIL] {rule} {loc:<6} {msg}")
    for path in lost:
        print(f"  [FAIL] R7 -      direct include lost vs baseline: {path}")
    for path in unclassified:
        print(f"  [FAIL] R8 -      unclassified header (not direct, not reachable, not exempt): {path}")

    hard_fail = bool(problems or lost or unclassified)

    if review_hits:
        print()
        print("  [WARN] R8 unreachable headers awaiting a maintainer decision:")
        for path in review_hits:
            print(f"         {path}")
    if added:
        print()
        print("  [WARN] R7 direct set grew beyond the baseline; run --write-manifest to sync:")
        for path in added:
            print(f"         {path}")

    print()
    if hard_fail:
        print("[FAIL] umbrella header integrity violated.")
        return 1
    print("[PASS] umbrella header intact.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

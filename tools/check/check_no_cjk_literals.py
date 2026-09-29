#!/usr/bin/env python3
# ============================================================================
# check_no_cjk_literals.py - no-CJK-in-string-literals gate (LIT)
# ----------------------------------------------------------------------------
# Spec: codespec/CODING_STANDARDS.md §14
#   注释可用中文书写，但**字符串字面量**里的中文会经 stdout / stderr / Inspector /
#   CLI / LSP 抵达控制台，在 Windows GBK 等窄代码页控制台上必然乱码（同一份文案在
#   不同代码页下呈现为问号或方块）。故：注释外不得出现中日韩字符，一律用英文表达。
#
# Rule LIT-1 (blocking): a CJK character (Han / kana / CJK punctuation /
#   fullwidth forms) inside a C++ or Python string or character literal, inside a
#   C++ raw string R"(...)", or inside a CMake quoted / bracket argument; comments
#   are stripped first, so Chinese prose in `//`, `/* */`, `///`, `#` never trips
#   the rule.
#   Scanned areas: include/ src/ examples/ tests/ tools/ (*.h *.hpp *.hh *.cpp
#   *.cc *.cxx *.c *.inl *.py) plus cmake/*.cmake and CMakeLists.txt (the latter's
#   strings reach `cmake --configure` / build output, and its cache-entry
#   descriptions reach cmake-gui).
#
# Rule LIT-2 (blocking): an EXEMPT_FILES entry that matches nothing (stale
#   whitelist), so the list cannot rot after the code moves on.
#
# Exemptions for LIT-1:
#   1. Inline marker: the token `CJK-LITERAL` (case-insensitive) on the finding
#      line or any of the 3 preceding raw lines, carrying a reason:
#         // CJK-LITERAL: cjk-fixture - asserts Han shaping through the font engine
#      For a multi-line raw string the marker sits next to its opening `R"(...)`.
#   2. File-level EXEMPT_FILES below (whole file is CJK product/fixture data).
#      Legit uses are narrow: text *rendered on screen* to demo CJK shaping, test
#      data that *must* be Han to prove the CJK pipeline, locale output that *is*
#      the feature (zh/ja date units), comments embedded in shader source handed
#      to the GPU. Console/stderr prose is never legit.
#   Python docstrings (module/class/function prose) count as comments, not output.
#
# Diagnostics print the offending literal with non-ASCII escaped as \uXXXX so the
# gate's own output stays ASCII - a gate that mojibakes cannot be read.
#
# Exit code: 1 when any un-exempted finding or stale whitelist entry exists;
#            0 when clean. Machine-readable worklist via --json.
#
# Usage:
#   python3 tools/check/check_no_cjk_literals.py [--root <dir>]
#       [--json <path>] [--limit N] [--files-with-cjk] [--show-text]
# ============================================================================
import argparse
import json
import os
import re
import sys

# CJK ranges that mark Chinese/Japanese/Korean *text* (or the fullwidth/CJK
# punctuation used with it). Em dash (U+2014), arrows (U+2192), middle dot and
# friends are General Punctuation used legitimately in English prose: NOT here.
CJK_RANGES = (
    (0x3000, 0x303F),  # CJK punctuation: 、。〈〉《》「」【】
    (0x3040, 0x30FF),  # Hiragana + Katakana
    (0x3400, 0x4DBF),  # CJK Extension A
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs
    (0xF900, 0xFAFF),  # CJK Compatibility Ideographs
    (0xFF01, 0xFF60),  # Fullwidth forms: ，！？（）：；
    (0xFF61, 0xFFDC),  # Halfwidth katakana / hangul
)
CJK_RE = re.compile("[" + "".join("%s-%s" % (chr(lo), chr(hi)) for lo, hi in CJK_RANGES) + "]")

SCAN_DIRS = ("include", "src", "examples", "tests", "tools")
CPP_EXTS = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".c", ".inl")
PY_EXTS = (".py",)
CMAKE_EXTS = (".cmake",)
# cmake 侧另扫两处：cmake/ 全部模块 + 根 CMakeLists.txt
CMAKE_DIRS = ("cmake",)
CMAKE_ROOT_FILES = ("CMakeLists.txt",)
CMAKE_EXTS = (".cmake",)
CMAKE_FILES = ("CMakeLists.txt",)  # repo root + any scanned dir

EXEMPT_TOKEN = "CJK-LITERAL"
INLINE_LOOKBACK = 3
_PRUNE_DIRS = {"third_party", "build", "build-prof", "build-inspector", ".git", "node_modules"}

# Whole-file exemptions: relpath -> reason. Kept empty on purpose: exemptions
# belong next to the data they justify (inline marker), and LIT-2 would flag an
# entry the moment its file is cleaned.
EXEMPT_FILES = {}


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def iter_files(root):
    """Yield (relpath, abspath, lang) for every scanned source file."""
    seen = set()
    for sd in SCAN_DIRS:
        base = os.path.join(root, sd)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if d not in _PRUNE_DIRS)
            for fn in sorted(filenames):
                ext = os.path.splitext(fn)[1]
                if ext not in CPP_EXTS and ext not in PY_EXTS:
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                seen.add(rel)
                yield (rel, full, "py" if ext in PY_EXTS else "cpp")
    for sd in CMAKE_DIRS:
        base = os.path.join(root, sd)
        if os.path.isdir(base):
            for fn in sorted(os.listdir(base)):
                full = os.path.join(base, fn)
                if not os.path.isfile(full) or os.path.splitext(fn)[1] not in CMAKE_EXTS:
                    continue
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel in seen:
                    continue
                seen.add(rel)
                yield (rel, full, "cmake")
    for fn in CMAKE_ROOT_FILES:
        full = os.path.join(root, fn)
        rel = fn
        if os.path.isfile(full) and rel not in seen:
            seen.add(rel)
            yield (rel, full, "cmake")


# --------------------------------------------------------------------------
# Literal scanners: each returns [(start_line, kind, content, prose)] where
# `prose` is True for documentation-only strings (Python docstrings).
# --------------------------------------------------------------------------

def scan_cpp(text):
    """Yield string / char / raw-string literals that are not inside comments."""
    out = []
    i, n, line = 0, len(text), 1
    in_block = False
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if in_block:
            if c == "*" and text[i + 1:i + 2] == "/":
                in_block = False
                i += 2
            else:
                i += 1
            continue
        if c == "/" and text[i + 1:i + 2] == "/":
            j = text.find("\n", i)
            i = n if j == -1 else j
            continue
        if c == "/" and text[i + 1:i + 2] == "*":
            in_block = True
            i += 2
            continue
        if c == "R" and text[i + 1:i + 2] == '"':
            d = text.find("(", i + 2)
            eol = text.find("\n", i)
            if d == -1 or (eol != -1 and d > eol):
                i += 1
                continue
            delim = text[i + 2:d]
            close = ")" + delim + '"'
            start = d + 1
            end = text.find(close, start)
            end = n if end == -1 else end
            content = text[start:end]
            out.append((line, "raw", content, False))
            line += content.count("\n")
            i = end + len(close)
            continue
        if c == "'":
            prev = text[i - 1] if i else ""
            if prev.isalnum() or prev in "_'":
                # digit separator (60'000) - not a character literal start
                i += 1
                continue
        if c in "\"'":
            q = c
            j = i + 1
            buf = []
            while j < n:
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                if text[j] == q or text[j] == "\n":
                    break
                buf.append(text[j])
                j += 1
            out.append((line, "str" if q == '"' else "chr", "".join(buf), False))
            i = j + 1
            continue
        i += 1
    return out


_PY_PREFIX_RE = re.compile(r"[rbfuRBFU]{0,3}$")


def scan_py(text):
    """Yield Python string literals outside comments, flagging docstrings as prose.

    A literal is prose (module/class/function docstring) only when it opens a
    statement: bracket depth 0 and nothing but whitespace before it on its
    physical line. A Chinese docstring is documentation, like a comment; the same
    text inside `print(...)` is console output and gets flagged.
    """
    out = []
    i, n, line = 0, len(text), 1
    depth = 0
    line_has_code = False
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            line_has_code = False
            continue
        if c in " \t":
            i += 1
            continue
        if c == "#":
            j = text.find("\n", i)
            i = n if j == -1 else j
            continue
        if c in "([{":
            depth += 1
            line_has_code = True
            i += 1
            continue
        if c in ")]}":
            depth = max(0, depth - 1)
            line_has_code = True
            i += 1
            continue
        if c in "\"'":
            pm = _PY_PREFIX_RE.search(text[max(0, i - 3):i])
            prefix = pm.group(0) if pm else ""
            q = c
            triple = text[i:i + 3] == q * 3
            body_start = i + (3 if triple else 1)
            if triple:
                closer = q * 3
                end = body_start
                content = ""
                while True:
                    end = text.find(closer, end)
                    if end == -1:
                        content = text[body_start:n]
                        end = n
                        break
                    if text[end - 1:end] == "\\" and "r" not in prefix.lower():
                        end += 1
                        continue
                    content = text[body_start:end]
                    break
                is_docstring = depth == 0 and not line_has_code
                out.append((line, "triple", content, is_docstring))
                line += content.count("\n")
                i = end + 3
                line_has_code = True
                continue
            j = body_start
            buf = []
            while j < n and text[j] != q and text[j] != "\n":
                if text[j] == "\\":
                    buf.append(text[j:j + 2])
                    j += 2
                    continue
                buf.append(text[j])
                j += 1
            out.append((line, "str", "".join(buf), depth == 0 and not line_has_code))
            i = j + 1
            line_has_code = True
            continue
        line_has_code = True
        i += 1
    return out


_CMAKE_BRACKET_RE = re.compile(r"\[(=*)\[")


def scan_cmake(text):
    """Yield CMake quoted / bracket arguments, plus bare (unquoted) code text.

    `#` starts a line comment and `#[[ ... ]]` a bracket comment, both only when
    outside a quoted argument - so the 900+ Chinese comment lines in cmake/ stay
    invisible here, while a Chinese `CACHE PATH "..."` description or
    `aurora_log("...")` (both reach the configure console) is flagged.
    """
    out = []
    i, n, line = 0, len(text), 1
    buf = []

    def flush():
        if buf:
            out.append((line, "bare", "".join(buf), False))
            del buf[:]

    while i < n:
        c = text[i]
        if c == "\n":
            flush()
            line += 1
            i += 1
            continue
        if c == "#":
            if text[i + 1:i + 3] == "[[":
                close = text.find("]]", i + 3)
                close = n if close == -1 else close + 2
                line += text.count("\n", i, close)
                i = close
                continue
            flush()
            j = text.find("\n", i)
            i = n if j == -1 else j
            continue
        if c == "[":
            m = _CMAKE_BRACKET_RE.match(text, i)
            if m:
                close = "]" + m.group(1) + "]"
                start = m.end()
                end = text.find(close, start)
                end = n if end == -1 else end
                flush()
                content = text[start:end]
                out.append((line, "bracket", content, False))
                line += content.count("\n")
                i = end + len(close)
                continue
        if c == '"':
            flush()
            j = i + 1
            chars = []
            while j < n:
                if text[j] == "\\" and text[j + 1:j + 2] in ('"', "\\"):
                    chars.append(text[j + 1])
                    j += 2
                    continue
                if text[j] == '"':
                    break
                chars.append(text[j])
                j += 1
            content = "".join(chars)
            out.append((line, "str", content, False))
            line += content.count("\n")
            i = j + 1
            continue
        buf.append(c)
        i += 1
    flush()
    return out


# --------------------------------------------------------------------------

def escape_non_ascii(s):
    return "".join(ch if (ord(ch) < 0x80 and ch != "\\") else
                   ("\\n" if ch == "\n" else "\\u%04x" % ord(ch)) for ch in s)


def first_cjk_snippet(content, width=56):
    m = CJK_RE.search(content)
    if not m:
        return ""
    lo = max(0, m.start() - width // 4)
    return escape_non_ascii(content[lo:m.end() + width].replace("\n", " "))


_SCANNERS = {"cpp": scan_cpp, "py": scan_py, "cmake": scan_cmake}


def collect(root):
    findings = []
    stats = {"files": 0, "cpp": 0, "py": 0, "cmake": 0}
    for rel, full, lang in iter_files(root):
        stats["files"] += 1
        stats[lang] += 1
        try:
            with open(full, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            print("[WARN] cannot read %s: %s" % (rel, exc))
            continue
        raw_lines = text.splitlines()
        for ln, kind, content, prose in _SCANNERS[lang](text):
            if prose or not CJK_RE.search(content):
                continue
            lo = max(1, ln - INLINE_LOOKBACK)
            upto = min(ln, len(raw_lines))
            if any(EXEMPT_TOKEN in raw_lines[k].upper() for k in range(lo - 1, upto)):
                continue
            findings.append({
                "file": rel,
                "line": ln,
                "kind": kind,
                "preview": first_cjk_snippet(content),
                "text": content if kind != "raw" else escape_non_ascii(content)[:200],
                "cjk_count": len(CJK_RE.findall(content)),
            })
    return findings, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root")
    ap.add_argument("--json", default=None, help="write findings as JSON to this path")
    ap.add_argument("--limit", type=int, default=60, help="max findings printed (0 = all)")
    ap.add_argument("--files-with-cjk", action="store_true", help="print per-file finding counts only")
    ap.add_argument("--show-text", action="store_true", help="print literal text (escapes non-ASCII)")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    findings, stats = collect(root)

    # LIT-2: stale file-level whitelist entries; whitelisted files drop out of LIT-1.
    stale = []
    exempt_hits = 0
    for rel, reason in sorted(EXEMPT_FILES.items()):
        kept = [f for f in findings if f["file"] == rel]
        if not kept:
            stale.append("LIT-2 %s: EXEMPT_FILES entry matches nothing (reason: %s)" % (rel, reason))
            continue
        exempt_hits += len(kept)
        findings = [f for f in findings if f["file"] != rel]
    findings.sort(key=lambda f: (f["file"], f["line"]))

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"findings": findings, "stale_whitelist": stale, "stats": stats},
                      fh, ensure_ascii=True, indent=1)

    if args.files_with_cjk:
        per = {}
        for f in findings:
            per[f["file"]] = per.get(f["file"], 0) + 1
        for rel, cnt in sorted(per.items(), key=lambda kv: (-kv[1], kv[0])):
            print("%5d %s" % (cnt, rel))

    if findings or stale:
        if not args.files_with_cjk:
            shown = findings if args.limit <= 0 else findings[:args.limit]
            for f in shown:
                extra = ('  text="%s"' % f["text"]) if args.show_text else ""
                print("LIT-1 %s:%d [%s] ...%s...%s" % (f["file"], f["line"], f["kind"], f["preview"], extra))
            if len(findings) > len(shown):
                print("... %d more (use --limit 0 or --json for the full list)" % (len(findings) - len(shown)))
        for s in stale:
            print("FAIL %s" % s)
        if not args.files_with_cjk:
            print("[FAIL] LIT-1: %d CJK literal(s) in %d file(s) outside comments; "
                  "translate to English, or mark CJK-dependent data with an inline "
                  "`CJK-LITERAL: <reason>` comment (CODING_STANDARDS.md)" %
                  (len(findings), len({f["file"] for f in findings})))
        return 1

    print("[PASS] no CJK in string literals: %d files scanned (%d C++, %d Python, "
          "%d CMake), %d whitelisted file(s)" %
          (stats["files"], stats["cpp"], stats["py"], stats["cmake"], exempt_hits))
    return 0


if __name__ == "__main__":
    sys.exit(main())

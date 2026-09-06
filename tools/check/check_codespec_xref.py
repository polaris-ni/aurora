#!/usr/bin/env python3
# ============================================================================
# codespec cross-reference guard
# ----------------------------------------------------------------------------
# 守护 codespec/ 下文档的内部一致性，只做「确定性可判定」的规则，避免狼来了效应：
#   R1 相对链接目标存在；带 #锚点 时锚点在目标文件中存在（GitHub 风格 slug 比对）；
#   R2 章节号格式：markdown 标题若以序号开头，必须是纯数字点分层级（1 / 1.1 / 1.1.1），
#      禁止中英文序号（第一章 / 一、/ Section One 等）；
#   R3 章节号在单文件内同层级不重复、不跳号；
#   R4 反引号包裹的路径必须真实存在（就近解析：同目录 → codespec/ → 仓库根）；
#   R5 SPECIFICATIONS.md 特性表（#1–#24）「规格落点」列中的链接逐行可达。
#
# 排除范围（防误报）：
#   - fenced code block（``` / ~~~）内的链接与路径不校验（多为示例占位）；
#   - 外链（http/https/mailto）、页内锚点（#xxx）、纯锚点不校验；
#   - 明显占位符（含 < > * {} 或以 example/foo/placeholder 命名）不校验。
#
# 白名单：首版内置存量豁免（见 WHITELIST，逐项注明原因），**只拦增量**。
#
# Usage:
#   python tools/check/check_codespec_xref.py [repo_root]
# ============================================================================
import os
import re
import sys

# ---- 白名单：存量豁免，逐项注明原因；新规则只拦增量 -------------------------
WHITELIST = {
    # (rule, file_rel, detail) -> reason
}

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
PATH_RE = re.compile(r"`([^`\s]+)`")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
NUM_PREFIX_RE = re.compile(r"^(\d+(?:\.\d+)*)[\s、.]+")
CJK_NUM_RE = re.compile(r"^[第卷][一二三四五六七八九十百千]+[章節节]|^[一二三四五六七八九十]+[、.]")
PLACEHOLDER_RE = re.compile(r"[<>*{}]|\b(example|foo|bar|placeholder|your_|xxx)\b", re.IGNORECASE)


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def slugify(text):
    """GitHub-style anchor slug: lowercase, drop punctuation, spaces -> dashes."""
    text = text.strip().lower()
    text = re.sub(r"[^\w\u4e00-\u9fff\- ]", "", text)
    return text.replace(" ", "-")


def strip_fences(lines):
    """Return [(lineno, line)] with fenced code blocks removed."""
    out, in_fence, fence = [], False, ""
    for i, line in enumerate(lines, start=1):
        match = FENCE_RE.match(line)
        if match:
            if not in_fence:
                in_fence, fence = True, match.group(1)
                continue
            if match.group(1) == fence:
                in_fence, fence = False, ""
                continue
        if not in_fence:
            out.append((i, line))
    return out


def headings_of(lines):
    """[(lineno, level, number_or_None, raw_text, slug)] for headings outside code fences."""
    result = []
    for lineno, line in strip_fences(lines):
        match = HEADING_RE.match(line)
        if not match:
            continue
        level = len(match.group(1))
        text = match.group(2).strip()
        number = None
        num_match = NUM_PREFIX_RE.match(text)
        if num_match:
            number = num_match.group(1)
        result.append((lineno, level, number, text, slugify(text)))
    return result


def resolve_path(base_dir, target, repo, extra_dirs=()):
    """Resolve a doc-relative path: same dir -> codespec/ -> repo root -> extra_dirs.
    Returns path or None."""
    candidates = [
        os.path.normpath(os.path.join(base_dir, target)),
        os.path.normpath(os.path.join(repo, "codespec", target)),
        os.path.normpath(os.path.join(repo, target)),
    ]
    candidates += [
        os.path.normpath(os.path.join(repo, d, target)) for d in extra_dirs
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    return None


def check_link_targets(rel, lines, repo, problems):
    """R1: relative link targets and anchors must exist."""
    base_dir = os.path.dirname(os.path.join(repo, rel))
    for lineno, line in strip_fences(lines):
        for target in LINK_RE.findall(line):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            path_part, _, anchor = target.partition("#")
            if not path_part:
                continue  # pure in-page anchor
            resolved = resolve_path(base_dir, path_part, repo)
            if resolved is None:
                problems.append(("R1", rel, lineno, f"link target missing: {target}"))
                continue
            if anchor:
                try:
                    with open(resolved, encoding="utf-8") as handle:
                        target_lines = handle.read().splitlines()
                except OSError:
                    continue
                slugs = {h[4] for h in headings_of(target_lines)}
                if anchor not in slugs:
                    problems.append(
                        ("R1", rel, lineno, f"anchor '#{anchor}' not found in {path_part}")
                    )


def check_heading_numbers(rel, lines, problems):
    """R2/R3: numeric section numbering, no CJK/English ordinals, no dup/gap per level."""
    heads = headings_of(lines)
    for lineno, _level, number, text, _slug in heads:
        if number is None and CJK_NUM_RE.match(text):
            problems.append(
                ("R2", rel, lineno, f"non-numeric section ordinal: {text[:40]}")
            )

    # R3: duplicates and gaps, evaluated per (level, parent prefix)
    seen = {}
    for lineno, level, number, _text, _slug in heads:
        if number is None:
            continue
        parent = ".".join(number.split(".")[:-1])
        key = (level, parent)
        seen.setdefault(key, []).append((number, lineno))

    for (level, parent), items in seen.items():
        numbers = [n for n, _ in items]
        dupes = {n for n in numbers if numbers.count(n) > 1}
        for number, lineno in items:
            if number in dupes:
                problems.append(("R3", rel, lineno, f"duplicate section number: {number}"))
        # gaps: only when the sequence is a clean 1..N run at this level
        tails = sorted({int(n.split(".")[-1]) for n in numbers if re.fullmatch(r"[\d.]+", n)})
        if len(tails) > 1 and tails == list(range(tails[0], tails[-1] + 1)):
            continue  # contiguous: fine
        if len(tails) > 1 and tails[-1] - tails[0] + 1 != len(tails):
            problems.append(
                ("R3", rel, items[0][1],
                 f"section numbers skip: {' '.join(str(t) for t in tails)}")
            )


def check_backtick_paths(rel, lines, repo, problems):
    """R4: backticked **paths** must exist (same dir -> codespec/ -> repo root -> include/ -> src/).

    Bare filenames (e.g. `types.h`, `ARCHITECTURE.md`) are treated as symbol / module mentions
    rather than path references — the docs list module inventories that way — so they are not
    checked. Only references containing a path separator are validated, which keeps the rule
    deterministic and false-positive free.
    """
    base_dir = os.path.dirname(os.path.join(repo, rel))
    for lineno, line in strip_fences(lines):
        for candidate in PATH_RE.findall(line):
            if PLACEHOLDER_RE.search(candidate):
                continue
            if not re.search(r"\.(md|cpp|h|hpp|json|toml|py|cmake|txt|tsv|sh)$", candidate):
                continue
            if "/" not in candidate and "\\" not in candidate:
                continue  # bare filename: symbol mention, not a path reference
            # Docs cite headers both as include/<path> and as aurora-relative (widget/text.h),
            # so both include/ and include/aurora/ (and their src/ counterparts) are candidates.
            extra = ("include", "src", "include/aurora", "src/aurora", "third_party")
            if resolve_path(base_dir, candidate, repo, extra_dirs=extra) is None:
                problems.append(("R4", rel, lineno, f"backticked path missing: {candidate}"))


def check_spec_table(rel, lines, repo, problems):
    """R5: SPECIFICATIONS.md feature table rows (#1-#24) must have a reachable spec link."""
    if os.path.basename(rel) != "SPECIFICATIONS.md":
        return
    base_dir = os.path.dirname(os.path.join(repo, rel))
    headings = headings_of(lines)
    for lineno, line in strip_fences(lines):
        if not re.match(r"^\s*\|\s*\d{1,2}\s*\|", line):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 4:
            continue
        try:
            number = int(cells[0])
        except ValueError:
            continue
        if not 1 <= number <= 24:
            continue
        if not LINK_RE.search(cells[-1]):
            # 「本文 §N」is a legitimate in-document reference (the landing point lives in this
            # very file); accept it when the cited section actually exists here.
            self_ref = re.search(r"本文\s*§\s*([\d.]+)", cells[-1])
            if self_ref:
                wanted = self_ref.group(1)
                existing = {h[2] for h in headings if h[2]}
                if wanted in existing:
                    continue
                problems.append(
                    ("R5", rel, lineno, f"feature #{number} cites missing local section §{wanted}")
                )
                continue
            problems.append(("R5", rel, lineno, f"feature #{number} has no spec link"))
            continue
        for target in LINK_RE.findall(cells[-1]):
            if target.startswith(("http://", "https://")):
                continue
            path_part = target.partition("#")[0]
            if path_part and resolve_path(base_dir, path_part, repo) is None:
                problems.append(
                    ("R5", rel, lineno, f"feature #{number} spec link missing: {target}")
                )


def main() -> int:
    repo = sys.argv[1] if len(sys.argv) > 1 else repo_root_of(__file__)
    codespec = os.path.join(repo, "codespec")
    if not os.path.isdir(codespec):
        print(f"[FAIL] codespec directory not found: {codespec}")
        return 2

    docs = []
    for current, _dirs, files in os.walk(codespec):
        for name in sorted(files):
            if name.endswith(".md"):
                full = os.path.join(current, name)
                docs.append(os.path.relpath(full, repo))

    problems = []
    for rel in docs:
        with open(os.path.join(repo, rel), encoding="utf-8") as handle:
            lines = handle.read().splitlines()
        check_link_targets(rel, lines, repo, problems)
        check_heading_numbers(rel, lines, problems)
        check_backtick_paths(rel, lines, repo, problems)
        check_spec_table(rel, lines, repo, problems)

    # Apply whitelist: only block regressions (incremental).
    remaining = []
    for rule, rel, lineno, detail in problems:
        if (rule, rel, detail) in WHITELIST or (rule, rel, "*") in WHITELIST:
            continue
        remaining.append((rule, rel, lineno, detail))

    if not remaining:
        print(f"[OK] codespec xref clean: {len(docs)} docs, 0 problems.")
        return 0

    print(f"[FAIL] codespec xref: {len(remaining)} problem(s) across {len(docs)} docs:")
    by_rule = {}
    for rule, _rel, _lineno, _detail in remaining:
        by_rule[rule] = by_rule.get(rule, 0) + 1
    for rule in sorted(by_rule):
        print(f"  {rule}: {by_rule[rule]}")
    print("")
    for rule, rel, lineno, detail in remaining[:60]:
        print(f"  {rule} {rel}:{lineno}  {detail}")
    if len(remaining) > 60:
        print(f"  ... and {len(remaining) - 60} more")
    print("\n  Fix the above, or add a justified entry to WHITELIST (存量豁免只用于既有问题).")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
check_change_proposals.py - guard the change proposal contract of codespec/changes/*/proposal.md.

Why this gate exists: a change proposal is the only artifact that makes a design decision
**reviewable before code lands**. Without a parseable shape, a proposal degrades into prose
that no one can diff, no one can list, and no one can hold anyone to. The contract is enforced
here rather than by reviewer memory, so that "we agreed on this" survives past the session that
produced it.

The rule set deliberately mirrors `check_manual_test_format.py`: same fixed-field table, same
ascending-id discipline, same stale-whitelist detection. Two artifacts, one shape.

Rules guarded:
- Directory name is kebab-case (`[a-z0-9]+(-[a-z0-9]+)*`); it is the stable handle a proposal is
  referred to by, so it must be safe to type in a path, a commit message and a URL.
- Every proposal directory holds a `proposal.md`.
- Level-1 heading is `# CHG-<NNN> <title>`; `<NNN>` is three digits.
- Change ids are unique, and ascend in directory-name order (a gap only ever means an abandoned
  proposal, which stays on record rather than being renumbered).
- Metadata table carries exactly the five fields, in fixed order, each non-empty.
- 变更编号 equals its heading id; 提出日期 is YYYY-MM-DD; 当前状态 is inside the domain.
- 关联需求 is `无` or a `SPEC.*` id that really exists in the SPECIFICATIONS.md feature table
  (a proposal pointing at a requirement nobody declared is a proposal nobody can verify).
- The four sections are present, in fixed order, each non-empty.
- 回写落点 names at least one real `codespec/...` path -- a write-back target that does not exist
  is a broken promise, not a plan.
- 验收判据 carries at least one concrete entry and no placeholder.

Usage:
  python3 tools/check/check_change_proposals.py [--root <aurora_root>] [dir ...]

Exit codes: 0 = clean, 1 = violation(s), 2 = cannot scan / bad usage.
"""
import argparse
import os
import re
import sys

# Windows 控制台默认 cp1252/GBK，无法编码本脚本诊断输出里的中文，故强制 UTF-8 stdio；
# 重定向流（文件 / None）不可 reconfigure，try/except 兜住。
try:
    sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
except (AttributeError, OSError):
    pass

CHANGES_DIR = ("codespec", "changes")
PROPOSAL_NAME = "proposal.md"
DIR_NAME_RE = re.compile(r'^[a-z0-9]+(?:-[a-z0-9]+)*$')
HEADING_RE = re.compile(r'^#\s+(CHG-\d{3})\s+(\S.*)$')
ID_RE = re.compile(r'^CHG-(\d{3})$')
FIELD_ROW_RE = re.compile(r'^\|\s*([^|]+?)\s*\|\s*(.*?)\s*\|\s*$')
DATE_RE = re.compile(r'^\d{4}-\d{2}-\d{2}$')
SPEC_ID_RE = re.compile(r'^SPEC\.[A-Z0-9-]+(?:\.[A-Z0-9-]+)+\.\d{1,3}$')
SECTION_RE = re.compile(r'^##\s+(.+?)\s*$')
BACKTICK_PATH_RE = re.compile(r'`([A-Za-z0-9_./-]+\.(?:md|toml|json|py|h|cpp))`')

# CJK-LITERAL: doc-schema - the metadata field names are the parse contract of the Chinese
# proposal tables; translating them would stop the parser from matching any proposal.
# Each pair is (document field name, ASCII diagnostic name) -- diagnostics stay pure ASCII so a
# GBK console can still render the gate output.
FIELDS = [
    # CJK-LITERAL: doc-schema - metadata field name of the Chinese proposal table, matched verbatim
    ("变更编号", "change-id"),
    # CJK-LITERAL: doc-schema - metadata field name of the Chinese proposal table, matched verbatim
    ("提出日期", "proposed-date"),
    # CJK-LITERAL: doc-schema - metadata field name of the Chinese proposal table, matched verbatim
    ("当前状态", "status"),
    # CJK-LITERAL: doc-schema - metadata field name of the Chinese proposal table, matched verbatim
    ("关联需求", "linked-requirement"),
    # CJK-LITERAL: doc-schema - metadata field name of the Chinese proposal table, matched verbatim
    ("影响面", "impact-surface"),
]
FIELD_BY_CJK = dict(FIELDS)
# 反查表：诊断与代码里一律用 ASCII 名指代字段，本表是唯一的 CJK -> ASCII 桥。
# 使用点写 `values.get(FIELD_BY_ASCII["change-id"], "")` 而非直接写出该中文字面量，
# 这样中文字面量只在上面的 FIELDS 一处出现（诊断输出已全 ASCII，契约仍按文档原文匹配）。
FIELD_BY_ASCII = {ascii_name: cjk for cjk, ascii_name in FIELDS}

# CJK-LITERAL: doc-schema - section headings authors write in the proposals, matched verbatim.
SECTIONS = [
    # CJK-LITERAL: doc-schema - section heading authors write in the proposals, matched verbatim
    ("动机", "motivation"),
    # CJK-LITERAL: doc-schema - section heading authors write in the proposals, matched verbatim
    ("变更内容", "change-content"),
    # CJK-LITERAL: doc-schema - section heading authors write in the proposals, matched verbatim
    ("验收判据", "acceptance-criteria"),
    # CJK-LITERAL: doc-schema - section heading authors write in the proposals, matched verbatim
    ("回写落点", "write-back-targets"),
]
SECTION_BY_CJK = dict(SECTIONS)
SECTION_BY_ASCII = {ascii_name: cjk for cjk, ascii_name in SECTIONS}

# CJK-LITERAL: doc-schema - status tokens authors write in the proposals, matched verbatim.
# Keys are the document spelling; values are the ASCII alias used in diagnostics.
STATUS_DOMAIN = {
    # CJK-LITERAL: doc-schema - status token authors write in the proposals, matched verbatim
    "已提议": "proposed",
    # CJK-LITERAL: doc-schema - status token authors write in the proposals, matched verbatim
    "实施中": "in-progress",
    # CJK-LITERAL: doc-schema - status token authors write in the proposals, matched verbatim
    "已归档": "archived",
    # CJK-LITERAL: doc-schema - status token authors write in the proposals, matched verbatim
    "已放弃": "abandoned",
}
# CJK-LITERAL: regex-semantic - matches the legal cell value 无 ("no linked requirement")
NO_REQUIREMENT = "无"
PLACEHOLDERS = {"-", "--", "TBD", "N/A", "none", "待定", "暂无"}
PLACEHOLDERS_ASCII = {"-", "--", "TBD", "N/A", "none"}

# ---- 白名单：存量豁免，逐项注明原因；规则只拦增量 ----------------------------
# 每条为 (提案相对路径, 命中片段, 豁免原因)。命中片段须能定位到该条诊断文本。
# 新增条目须写清「为何不直接修文档」；条目一旦不再命中任何诊断即判红灯，防止清单腐烂。
EXEMPT: list = []


def check_reverse_tables(errors):
    """Verify the ASCII -> CJK reverse tables round-trip back to the forward tables.

    The parse sites index the document by ASCII name (``FIELD_BY_ASCII["change-id"]``),
    so a reverse table that is not a strict inverse makes a parse site silently read the
    wrong field or section. That failure is not always observable from the diagnostics --
    reading the acceptance-criteria section while pointed at a non-empty write-back-targets
    section still yields a non-empty body -- so the invariant is asserted here instead of
    being left to a case that may or may not exist. Diagnostics stay pure ASCII.
    """
    for forward_name, forward, reverse in (("FIELDS", FIELDS, FIELD_BY_ASCII),
                                           ("SECTIONS", SECTIONS, SECTION_BY_ASCII)):
        expected = {ascii_name: cjk for cjk, ascii_name in forward}
        if len(forward) != len(expected):
            seen, dupes = set(), set()
            for _cjk, ascii_name in forward:
                if ascii_name in seen:
                    dupes.add(ascii_name)
                seen.add(ascii_name)
            errors.append(f"internal: {forward_name} has duplicate ASCII names "
                          f"({', '.join(sorted(dupes))}) -- reverse lookup would be ambiguous")
            continue
        if reverse != expected:
            broken = sorted(k for k in expected if reverse.get(k) != expected[k])
            errors.append(f"internal: {forward_name} reverse table is not an inverse of "
                          f"{forward_name} (broken: {', '.join(broken)})")


def apply_exemptions(errors, scanned):
    """Drop whitelisted stock findings; a stale whitelist entry is itself a failure.

    `scanned` is the set of proposal paths actually read: single-directory invocations must not
    report the other proposals' exemptions as stale.
    """
    kept, hits = [], [0] * len(EXEMPT)
    for error in errors:
        matched = None
        for position, (doc, needle, _reason) in enumerate(EXEMPT):
            if error.startswith(doc) and needle in error:
                matched = position
                break
        if matched is None:
            kept.append(error)
        else:
            hits[matched] += 1
    stale = [f"{doc}: whitelist entry matching `{needle}` caught no diagnostic and is now stale"
             for (doc, needle, _reason), count in zip(EXEMPT, hits)
             if count == 0 and doc in scanned]
    return kept, stale


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def parse_table(lines):
    """Read the two-column metadata table; return (values-by-CJK-key, key order)."""
    values, order = {}, []
    for line in lines:
        match = FIELD_ROW_RE.match(line)
        if not match:
            continue
        key, value = match.group(1).strip(), match.group(2).strip()
        if re.match(r'^:?-+$', key):
            continue  # 表格分隔行
        if key not in FIELD_BY_CJK:
            continue
        values[key] = value
        order.append(key)
    return values, order


def section_bodies(lines):
    """Split the document into `## <heading>` bodies; return (CJK heading order, body by name)."""
    heads, bodies, current = [], {}, None
    for line in lines:
        match = SECTION_RE.match(line)
        if match:
            heading = match.group(1).strip()
            current = heading if heading in SECTION_BY_CJK else None
            if current:
                heads.append(heading)
                bodies[current] = []
            continue
        if current:
            bodies[current].append(line)
    return heads, bodies


def requirement_ids(root):
    """Requirement IDs listed in SPECIFICATIONS.md's feature table (None if unreadable)."""
    path = os.path.join(root, "codespec", "SPECIFICATIONS.md")
    if not os.path.isfile(path):
        return None
    ids = set()
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = re.match(
                r"^\s*\|\s*`?(SPEC\.[A-Z0-9-]+(?:\.[A-Z0-9-]+)+\.\d{1,3})(?!\d)`?\s*\|", line)
            if match:
                ids.add(match.group(1))
    return ids


def check_proposal(root, path, errors):
    """Validate one proposal.md; return its change id (or None when unparseable)."""
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    directory = os.path.basename(os.path.dirname(path))
    if not DIR_NAME_RE.match(directory):
        errors.append(f"{rel}: directory name `{directory}` is not kebab-case "
                      "(lower-case letters and digits joined by single hyphens)")
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        errors.append(f"{rel}: cannot read ({exc})")
        return None

    heading_id = None
    for line_no, text in enumerate(lines, 1):
        match = HEADING_RE.match(text)
        if match:
            heading_id, title = match.group(1), match.group(2).strip()
            if not title:
                errors.append(f"{rel}:{line_no} heading `{match.group(1)}` carries no title")
            break
    if heading_id is None:
        errors.append(f"{rel}: no level-1 heading `# CHG-<NNN> <title>` found")
        return None

    values, order = parse_table(lines)
    for cjk, ascii_name in FIELDS:
        if cjk not in values:
            errors.append(f"{rel} [{heading_id}]: missing field {ascii_name}")
    if order and order[:len(FIELDS)] != [cjk for cjk, _ in FIELDS] \
            and all(cjk in values for cjk, _ in FIELDS):
        errors.append(f"{rel} [{heading_id}]: metadata field order must be "
                      f"{' / '.join(a for _, a in FIELDS)}, got "
                      f"{' / '.join(FIELD_BY_CJK[k] for k in order)}")
    for cjk, ascii_name in FIELDS:
        if cjk in values and not values[cjk]:
            errors.append(f"{rel} [{heading_id}]: field {ascii_name} is empty")

    # 变更编号
    declared = values.get(FIELD_BY_ASCII["change-id"], "")
    if declared and declared != heading_id:
        errors.append(f"{rel} [{heading_id}]: change-id field `{declared}` does not match "
                      f"the heading id")

    # 提出日期
    date = values.get(FIELD_BY_ASCII["proposed-date"], "")
    if date and not DATE_RE.match(date):
        errors.append(f"{rel} [{heading_id}]: proposed-date `{date}` is not YYYY-MM-DD")

    # 当前状态
    status = values.get(FIELD_BY_ASCII["status"], "")
    if status and status not in STATUS_DOMAIN:
        allowed = " / ".join(sorted(set(STATUS_DOMAIN.values())))
        errors.append(f"{rel} [{heading_id}]: status is outside the allowed domain ({allowed})")

    # 关联需求
    linked = values.get(FIELD_BY_ASCII["linked-requirement"], "")
    if linked and linked != NO_REQUIREMENT:
        if not SPEC_ID_RE.match(linked):
            errors.append(f"{rel} [{heading_id}]: linked-requirement `{linked}` is neither "
                          f"`{NO_REQUIREMENT}` nor a `SPEC.*` id")
        else:
            known = requirement_ids(root)
            if known is not None and linked not in known:
                errors.append(f"{rel} [{heading_id}]: linked-requirement `{linked}` is not in "
                              f"the SPECIFICATIONS.md feature table")

    heads, bodies = section_bodies(lines)
    for cjk, ascii_name in SECTIONS:
        if cjk not in bodies:
            errors.append(f"{rel} [{heading_id}]: missing section {ascii_name}")
    if heads and heads != [cjk for cjk, _ in SECTIONS]:
        got = [SECTION_BY_CJK.get(h, h) for h in heads if h in SECTION_BY_CJK]
        errors.append(f"{rel} [{heading_id}]: sections must appear in order "
                      f"{' / '.join(a for _, a in SECTIONS)}, got {' / '.join(got)}")
    for cjk, ascii_name in SECTIONS:
        body = bodies.get(cjk)
        if body is not None and not "".join(body).strip():
            errors.append(f"{rel} [{heading_id}]: section {ascii_name} is empty")

    # 验收判据：至少一条实质条目，且不得是占位符
    criteria_body = bodies.get(SECTION_BY_ASCII["acceptance-criteria"], [])
    criteria = [line.strip() for line in criteria_body if line.strip()]
    criteria = [line for line in criteria if not re.match(r'^[|:-]+$', line)]
    if not criteria:
        errors.append(f"{rel} [{heading_id}]: acceptance-criteria carries no entry "
                      "(a proposal without a checkable criterion cannot be reviewed)")
    elif all(any(line == p for p in PLACEHOLDERS) for line in criteria):
        errors.append(f"{rel} [{heading_id}]: acceptance-criteria holds only placeholders")

    # 回写落点：至少一条真实存在的 codespec 路径
    targets = []
    for line in bodies.get(SECTION_BY_ASCII["write-back-targets"], []):
        for candidate in BACKTICK_PATH_RE.findall(line):
            targets.append(candidate)
    if not targets:
        errors.append(f"{rel} [{heading_id}]: write-back-targets names no `codespec/...` path "
                      "(every change must say which document it writes back to)")
    for candidate in targets:
        if not candidate.startswith("codespec/"):
            errors.append(f"{rel} [{heading_id}]: write-back target `{candidate}` is outside "
                          f"codespec/ (AGENTS.md hard rule 5)")
        elif not os.path.isfile(os.path.join(root, *candidate.split("/"))):
            errors.append(f"{rel} [{heading_id}]: write-back target `{candidate}` does not exist")

    return heading_id


def main() -> int:
    ap = argparse.ArgumentParser(description="Guard the change proposal contract.")
    ap.add_argument("--root", default=None, help="aurora repo root (defaults to auto-detect)")
    ap.add_argument("dirs", nargs="*", help="specific codespec/changes/* directories to scan")
    args = ap.parse_args()

    root = os.path.abspath(args.root) if args.root else repo_root_of(__file__)
    base = os.path.join(root, *CHANGES_DIR)
    if not os.path.isdir(base):
        print(f"[PASS] change proposals OK: no {''.join(CHANGES_DIR)} directory yet, nothing to guard.")
        return 0
    if args.dirs:
        targets = [os.path.abspath(d) for d in args.dirs]
    else:
        targets = []
        for name in sorted(os.listdir(base)):
            candidate = os.path.join(base, name)
            if os.path.isdir(candidate):
                targets.append(candidate)

    errors = []
    # 解析表自洽先行：FIELDS/SECTIONS 的 ASCII->CJK 反查表若不是严格逆映射，解析点会静默读到
    # 错字段（甚至在键缺失时抛 KeyError）。这是脚本自身的不变量，与文档无关，故不进 EXEMPT
    # （EXEMPT 按文档路径前缀匹配，白名单化只会把它悄悄吃掉）；判红后直接返回，不扫描任何提案。
    check_reverse_tables(errors)
    if errors:
        for item in errors:
            print(f"[FAIL] {item}")
        print("  The parse tables themselves are inconsistent; fix the tables in this script")
        print("  before reading any proposal.")
        return 1

    seen = []
    for directory in targets:
        path = os.path.join(directory, PROPOSAL_NAME)
        rel_dir = os.path.relpath(directory, root).replace(os.sep, "/")
        if not os.path.isfile(path):
            errors.append(f"{rel_dir}: missing {PROPOSAL_NAME}")
            continue
        change_id = check_proposal(root, path, errors)
        if change_id:
            seen.append((rel_dir, int(ID_RE.match(change_id).group(1)), change_id))

    # 编号唯一 + 按目录名字典序升序（与人工用例的编号升序约束同构）
    ids = [change_id for _, _, change_id in seen]
    if len(set(ids)) != len(ids):
        dup = sorted({i for i in ids if ids.count(i) > 1})
        errors.append(f"{'/'.join(CHANGES_DIR)}: duplicate change ids: {', '.join(dup)}")
    for previous, current in zip(seen, seen[1:]):
        if current[1] <= previous[1]:
            errors.append(f"{'/'.join(CHANGES_DIR)}: change ids must ascend in directory order: "
                          f"{previous[2]} ({previous[0]}) -> {current[2]} ({current[0]})")

    scanned = {os.path.relpath(os.path.join(d, PROPOSAL_NAME), root).replace(os.sep, "/")
               for d in targets}
    kept, stale = apply_exemptions(errors, scanned)
    exempted = len(errors) - len(kept)

    if kept or stale:
        if stale:
            print(f"[FAIL] {len(stale)} whitelist exemption(s) are stale "
                  "(the matching violation no longer occurs, delete the entry):")
            for item in stale:
                print(f"  - {item}")
        if kept:
            print(f"[FAIL] change proposal contract violations: {len(kept)}")
            for item in kept:
                print(f"  - {item}")
        print()
        print("  See codespec/changes/README.md for the proposal schema: field names, order and")
        print("  value domains are all part of the parse contract.")
        print("  Fix the proposals rather than loosening this gate, unless the contract itself")
        print("  changes (then update every proposal and this script together).")
        return 1

    print(f"[PASS] change proposal contract OK: {len(seen)} proposal(s) / "
          f"ids unique and ascending / metadata, sections and write-back targets consistent"
          + (f" ({exempted} whitelisted)" if exempted else "") + ".")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
check_manual_test_format.py - guard the fixed field format of codespec/manual-test/*.md.

Why this gate exists: the manual-test case files are written to be **parsed by programs**
(dependency topology, failure-point attribution, pass-rate rollup). Every field is therefore
constrained by a single regex (see each file's section 1.5-1.7). Free-form drift in one file
silently breaks the parser for all of them, so the contract is enforced here instead of by
reviewer memory.

Rules guarded (field names / order / value domains):
- Case heading `#### TC-<MODULE>-<NNN> <title>`; `<MODULE>` is the uppercased module directory
  name taken from the file name (`01-core.md` -> `CORE`), numeric file prefix stripped.
- Case ids unique, first is 001, strictly ascending (a gap only ever means a retired id).
- Exactly the six fields, in fixed order, each non-empty: 用例编号 / 测试目的 / 前置条件 /
  依赖用例 / 操作步骤 / 预期结果.
- 用例编号 equals its heading id.
- 依赖用例 is `无` or `TC-<MODULE>-<NNN>(, TC-<MODULE>-<NNN>)*`; every same-module dependency
  must exist and precede the dependent case, and the listed order must be ascending.
- 操作步骤 entries are numbered from 1 with no gaps; a pure-execution step ends with
  （纯执行，无预期结果）.
- 预期结果 numbering is a strictly ascending subset of the step numbers, and covers exactly the
  non-pure steps -- i.e. no renumbering, no compression, no orphan entry.
- Section 3 record table: one row per declared case (same ids, same order), 8 columns, result
  within PASS/FAIL/BLOCKED/SKIP, date and failure-step formats honoured, failure steps must be
  a subset of that case's 预期结果 numbers, non-executed rows fully empty, and no `-`/`TBD`/
  `N/A` placeholders anywhere.

Usage:
  python3 tools/check/check_manual_test_format.py [--root <aurora_root>] [file.md ...]

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

CASE_HEADING_RE = re.compile(r'^####\s+(TC-[A-Z0-9]+-\d{3})\s+\S')
CASE_ID_RE = re.compile(r'^TC-([A-Z0-9]+)-(\d{3})$')
# CJK-LITERAL: doc-schema - the six field names are the parse contract of the Chinese case tables;
# translating them would stop the parser from matching any document.
FIELD_ORDER = ["用例编号", "测试目的", "前置条件", "依赖用例", "操作步骤", "预期结果"]
FIELD_RE = re.compile(r'^\|\s*([^|]+?)\s*\|\s*(.*?)\s*\|\s*$')
ENTRY_RE = re.compile(r'^(\d+)\.\s')
# CJK-LITERAL: doc-schema - suffix mark authors write in the docs to flag a pure-execution step
PURE_EXEC_MARK = "（纯执行，无预期结果）"
# CJK-LITERAL: regex-semantic - matches the legal cell value `无` ("no dependency") written in the docs
DEP_RE = re.compile(r'^(?:无|TC-[A-Z0-9]+-\d{3}(?:,\sTC-[A-Z0-9]+-\d{3})*)$')
DATE_RE = re.compile(r'^\d{4}-\d{2}-\d{2}$')
STEPS_RE = re.compile(r'^\d+(?:,\s\d+)*$')
RESULTS = {"PASS", "FAIL", "BLOCKED", "SKIP"}
# CJK-LITERAL: doc-schema - rejected placeholder authors leave in Chinese cells ("no dependency" spelled out)
PLACEHOLDERS = {"-", "--", "TBD", "N/A", "none", "无依赖"}
RECORD_COLS = ["case id", "execution date", "executor", "result", "failed step", "observed behaviour",
               "defect id", "remark"]

# ---- 白名单：存量豁免，逐项注明原因；规则只拦增量 ----------------------------
# 每条为 (文档相对路径, 命中片段, 豁免原因)。命中片段须能定位到该条诊断文本。
# 新增条目须写清「为何不直接修文档」；条目一旦不再命中任何诊断即判红灯，防止清单腐烂。
EXEMPT: list = []



def apply_exemptions(errors, scanned):
    """Drop whitelisted stock findings; a stale whitelist entry is itself a failure.

    `scanned` is the set of document paths actually read: single-file invocations must not
    report the other documents' exemptions as stale.
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


def module_of_filename(name):
    stem = os.path.splitext(os.path.basename(name))[0]
    return re.sub(r'^\d+[-_]*', '', stem).upper()


def split_entries(cell):
    """Split a multi-line cell into numbered entries; unnumbered chunks continue the previous one."""
    entries = []
    for chunk in cell.split("<br>"):
        chunk = chunk.strip()
        if not chunk:
            continue
        match = ENTRY_RE.match(chunk)
        if match:
            entries.append([int(match.group(1)), chunk])
        elif entries:
            entries[-1][1] += " " + chunk
        else:
            entries.append([None, chunk])
    return entries


def parse_case(region):
    """Read the field rows of one case table; return (fields, order, pure, expected)."""
    fields = {}
    order = []
    for line in region:
        match = FIELD_RE.match(line)
        if not match:
            continue
        key, value = match.group(1).strip(), match.group(2).strip()
        if re.match(r'^:?-+$', key):
            continue  # 表格分隔行
        if key not in FIELD_ORDER:
            # 非二列行（记录表 / 其他说明表）按行形状排除；含已知字段名的行始终按字段处理，
            # 以免值里出现竖线时静默漏读，伪装成「缺字段」。
            if line.strip().count("|") != 3:
                continue
            # CJK-LITERAL: doc-schema - header cell name of the case table, compared verbatim
            if key == "项目":
                continue  # 用例表头 `| 项目 | 内容 |`
        fields[key] = value
        order.append(key)
    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    pure = {num for num, text in split_entries(fields.get("操作步骤", ""))
            if num is not None and text.rstrip().endswith(PURE_EXEC_MARK)}
    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    expected = [num for num, _ in split_entries(fields.get("预期结果", "")) if num is not None]
    return fields, order, pure, expected


def check_case(rel, heading_line, heading_id, case_module, fields, order, pure, expected, errors):
    def fail(what):
        errors.append(f"{rel}:{heading_line} [{heading_id}] {what}")

    for name in FIELD_ORDER:
        if name not in fields:
            fail(f"missing field {name} (misspelled name, or a pipe inside the value deformed the row)")
    if order and order[:len(FIELD_ORDER)] != FIELD_ORDER and all(n in fields for n in FIELD_ORDER):
        fail(f"field order must be {' / '.join(FIELD_ORDER)}, got {' / '.join(order)}")
    for name in FIELD_ORDER:
        if name in fields and not fields[name]:
            fail(f"field {name} is empty")
    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    if fields.get("用例编号") and fields["用例编号"] != heading_id:
        fail(f"case id field `{fields['用例编号']}` does not match the heading `{heading_id}`")
    match = CASE_ID_RE.match(heading_id)
    if match and match.group(1) != case_module:
        fail(f"module segment {match.group(1)} does not match {case_module} derived from the file name")

    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    steps = [num for num, _ in split_entries(fields.get("操作步骤", "")) if num is not None]
    if steps:
        if steps[0] != 1:
            fail(f"operation steps must be numbered from 1, got {steps[0]}")
        for previous, current in zip(steps, steps[1:]):
            if current != previous + 1:
                fail(f"operation step numbers are not contiguous: {previous} -> {current}")
    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    for text in (fields.get("操作步骤", "").split("<br>")):
        text = text.strip()
        marker = PURE_EXEC_MARK in text
        numbered = ENTRY_RE.match(text)
        if marker and not numbered:
            fail(f"pure-execution step has no number: {text[:24]}")

    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    if any(num is None for num, _ in split_entries(fields.get("预期结果", ""))):
        fail("an expected-result entry has no number")
    if expected != sorted(expected) or len(set(expected)) != len(expected):
        fail(f"expected-result numbers must strictly ascend without repeats, got {expected}")
    orphans = [num for num in expected if num not in steps]
    if orphans:
        fail(f"expected-result numbers {orphans} have no matching operation step")
    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    renumbered = [num for num, _ in split_entries(fields.get("预期结果", ""))
                  if num is not None and num in pure]
    if renumbered:
        fail(f"pure-execution step {renumbered} must not carry an expected result")
    missing = [num for num in steps if num not in pure and num not in expected]
    if missing:
        fail(f"non pure-execution step {missing} has no expected result")

    # CJK-LITERAL: doc-schema - field key of the Chinese case table, looked up verbatim
    dep = fields.get("依赖用例", "")
    if dep and not DEP_RE.match(dep):
        fail(f"dependency value is not in the allowed form: `{dep}`")
    # CJK-LITERAL: doc-schema - 无 is the literal token the docs use for an empty dependency list
    return [token.strip() for token in dep.split(",")] if dep and dep != "无" else []


def check_document(root, path, cases, errors):
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    case_module = module_of_filename(path)
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        errors.append(f"{rel}: cannot read ({exc})")
        return
    headings = [(idx + 1, text) for idx, text in enumerate(lines) if text.startswith("#### TC-")]
    if not headings:
        errors.append(f"{rel}: no `#### TC-` case declaration found")
        return
    declared = []
    for position, (line_no, text) in enumerate(headings):
        heading_line = text.rstrip()
        bare = text[5:].strip()
        id_match = CASE_HEADING_RE.match(text)
        if not id_match:
            errors.append(f"{rel}:{line_no} case heading must be `#### TC-<MODULE>-<NNN> <title>`: {bare[:40]}")
            continue
        heading_id = id_match.group(1)
        # 区域止于下一条任意标题（含 §3 执行记录表），避免把别的表读成字段。
        stop = len(lines)
        for later_line, later_text in enumerate(lines[line_no:], line_no + 1):
            if later_text.startswith("#"):
                stop = later_line - 1
                break
        fields, order, pure, expected = parse_case(lines[line_no:stop])
        deps = check_case(rel, line_no, heading_id, case_module, fields, order, pure, expected, errors)
        numbers = CASE_ID_RE.match(heading_id)
        declared.append({"id": heading_id, "seq": int(numbers.group(2)), "deps": deps,
                         "expected": expected, "module": case_module})
    ids = [case["id"] for case in declared]
    if len(set(ids)) != len(ids):
        dup = sorted({i for i in ids if ids.count(i) > 1})
        errors.append(f"{rel}: duplicate case ids: {', '.join(dup)}")
    if declared and declared[0]["seq"] != 1:
        errors.append(f"{rel}: first case id should be {case_module}-001")
    for previous, current in zip(declared, declared[1:]):
        if current["seq"] <= previous["seq"]:
            errors.append(f"{rel}: cases must be declared in ascending id order: "
                          f"{previous['id']} -> {current['id']}")
    known = set(ids)
    for case in declared:
        seen = []
        for dep in case["deps"]:
            if dep not in known:
                if dep.split("-")[1] != case["module"]:
                    continue  # 跨模块依赖由 other-document 校验，此处只核格式
                errors.append(f"{rel}: {case['id']} depends on the non-existent case {dep}")
            seq = CASE_ID_RE.match(dep)
            if seq and dep in known and int(seq.group(2)) >= case["seq"]:
                errors.append(f"{rel}: prerequisite {dep} of {case['id']} is not declared before it")
            if seq and dep in known:
                seen.append(int(seq.group(2)))
        if seen != sorted(seen):
            errors.append(f"{rel}: dependencies of {case['id']} are not listed in ascending id order")
    check_record_table(rel, lines, declared, errors)
    cases.extend(dict(case, doc=rel) for case in declared)


def check_record_table(rel, lines, declared, errors):
    rows = [(idx + 1, text) for idx, text in enumerate(lines)
            if re.match(r'^\|\s*TC-[A-Z0-9]+-\d{3}\s*\|', text)]
    if not rows:
        errors.append(f"{rel}: execution record table missing "
                      "(no table row starting with a case id in section 3)")
        return
    by_id = {case["id"]: case for case in declared}
    listed = []
    for line_no, text in rows:
        cells = [cell.strip() for cell in text.strip().strip("|").split("|")]
        case_id = cells[0]
        prefix = f"{rel}:{line_no} [record table {case_id}]"
        if len(cells) != len(RECORD_COLS):
            errors.append(f"{prefix} should have {len(RECORD_COLS)} columns, got {len(cells)}")
            continue
        if case_id not in by_id:
            errors.append(f"{prefix} records a case that no document declares")
            continue
        listed.append(case_id)
        date, tester, result, steps, phenomenon, defect, remark = cells[1:]
        for name, value in zip(RECORD_COLS[1:], cells[1:]):
            if value in PLACEHOLDERS:
                errors.append(f"{prefix} column {name} holds the placeholder `{value}`, "
                              "leave it empty when the case was not executed")
        if result and result not in RESULTS:
            errors.append(f"{prefix} result `{result}` is outside {'/'.join(sorted(RESULTS))}")
        if date and not DATE_RE.match(date):
            errors.append(f"{prefix} execution date `{date}` is not YYYY-MM-DD")
        expected_ids = by_id[case_id]["expected"]
        if steps and not STEPS_RE.match(steps):
            errors.append(f"{prefix} failed step numbers `{steps}` do not match the expected format")
        if not result:
            for name, value in zip(["execution date", "executor", "failed step", "observed behaviour",
                                    "defect id", "remark"],
                                   [date, tester, steps, phenomenon, defect, remark]):
                if value:
                    errors.append(f"{prefix} case not executed, column {name} must stay empty")
            continue
        if not (date and tester):
            errors.append(f"{prefix} result filled but execution date or executor is missing")
        if result == "FAIL":
            if not steps:
                errors.append(f"{prefix} FAIL requires the failed step numbers")
            else:
                for num in [int(n) for n in steps.split(",")]:
                    if num not in expected_ids:
                        errors.append(f"{prefix} failed step {num} is not an expected-result number "
                                      f"of this case {expected_ids}")
            if not phenomenon:
                errors.append(f"{prefix} FAIL requires the observed behaviour")
        elif steps:
            errors.append(f"{prefix} result {result} must not carry failed step numbers")
    missing_rows = [case["id"] for case in declared if case["id"] not in listed]
    if missing_rows:
        errors.append(f"{rel}: execution record table lacks rows for: {', '.join(missing_rows)}")
    if listed != [case["id"] for case in declared]:
        extra = [i for i in listed if i not in [c["id"] for c in declared]]
        if not extra:
            errors.append(f"{rel}: record table rows must follow the case declaration order of section 2")


def main() -> int:
    ap = argparse.ArgumentParser(description="Guard the manual-test case format.")
    ap.add_argument("--root", default=None, help="aurora repo root (defaults to auto-detect)")
    ap.add_argument("files", nargs="*", help="specific codespec/manual-test/*.md files to scan")
    args = ap.parse_args()

    root = os.path.abspath(args.root) if args.root else repo_root_of(__file__)
    if args.files:
        targets = [os.path.abspath(f) for f in args.files]
    else:
        base = os.path.join(root, "codespec", "manual-test")
        if not os.path.isdir(base):
            print(f"[FAIL] no manual-test directory under {base}")
            return 2
        targets = sorted(os.path.join(base, name)
                         for name in os.listdir(base) if name.endswith(".md"))

    cases, errors = [], []
    for path in targets:
        check_document(root, path, cases, errors)

    raw, ids = list(errors), {case["id"] for case in cases}
    for case in cases:
        for dep in case["deps"]:
            if dep not in ids:
                raw.append(f"{case['doc']}: {case['id']} depends on {dep}, which no scanned document declares")

    kept, stale = apply_exemptions(raw, {os.path.relpath(p, root).replace(os.sep, "/")
                                         for p in targets})
    exempted = len(raw) - len(kept)

    if kept or stale:
        if stale:
            print(f"[FAIL] {len(stale)} whitelist exemption(s) are stale "
                  "(the matching violation no longer occurs, delete the entry):")
            for item in stale:
                print(f"  - {item}")
        if kept:
            print(f"[FAIL] manual-test case format violations: {len(kept)}")
            for item in kept:
                print(f"  - {item}")
        print()
        print("  See section 1.5-1.7 of each file for the case field schema: field names, order and")
        print("  value domains are all part of the parse contract.")
        print("  Fix the documents rather than loosening this gate, unless the contract itself changes")
        print("  (then update every document and script together).")
        return 1

    print(f"[PASS] manual-test case format OK: {len(targets)} docs / {len(cases)} cases / "
          f"dependencies and record tables consistent"
          + (f" ({exempted} whitelisted)" if exempted else "") + ".")
    return 0


if __name__ == "__main__":
    sys.exit(main())

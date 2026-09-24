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
FIELD_ORDER = ["用例编号", "测试目的", "前置条件", "依赖用例", "操作步骤", "预期结果"]
FIELD_RE = re.compile(r'^\|\s*([^|]+?)\s*\|\s*(.*?)\s*\|\s*$')
ENTRY_RE = re.compile(r'^(\d+)\.\s')
PURE_EXEC_MARK = "（纯执行，无预期结果）"
DEP_RE = re.compile(r'^(?:无|TC-[A-Z0-9]+-\d{3}(?:,\sTC-[A-Z0-9]+-\d{3})*)$')
DATE_RE = re.compile(r'^\d{4}-\d{2}-\d{2}$')
STEPS_RE = re.compile(r'^\d+(?:,\s\d+)*$')
RESULTS = {"PASS", "FAIL", "BLOCKED", "SKIP"}
PLACEHOLDERS = {"-", "--", "TBD", "N/A", "none", "无依赖"}
RECORD_COLS = ["用例编号", "执行日期", "执行人", "结果", "失败步骤号", "实际现象", "缺陷编号", "备注"]

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
    stale = [f"{doc}: 豁免条目 `{needle}` 未命中任何诊断，白名单已失效"
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
            if key == "项目":
                continue  # 用例表头 `| 项目 | 内容 |`
        fields[key] = value
        order.append(key)
    pure = {num for num, text in split_entries(fields.get("操作步骤", ""))
            if num is not None and text.rstrip().endswith(PURE_EXEC_MARK)}
    expected = [num for num, _ in split_entries(fields.get("预期结果", "")) if num is not None]
    return fields, order, pure, expected


def check_case(rel, heading_line, heading_id, case_module, fields, order, pure, expected, errors):
    def fail(what):
        errors.append(f"{rel}:{heading_line} [{heading_id}] {what}")

    for name in FIELD_ORDER:
        if name not in fields:
            fail(f"缺少字段「{name}」（或字段名拼写不符 / 值中含竖线致行形变）")
    if order and order[:len(FIELD_ORDER)] != FIELD_ORDER and all(n in fields for n in FIELD_ORDER):
        fail(f"字段顺序应为 {' / '.join(FIELD_ORDER)}，实际为 {' / '.join(order)}")
    for name in FIELD_ORDER:
        if name in fields and not fields[name]:
            fail(f"字段「{name}」为空")
    if fields.get("用例编号") and fields["用例编号"] != heading_id:
        fail(f"用例编号字段 `{fields['用例编号']}` 与标题 `{heading_id}` 不符")
    match = CASE_ID_RE.match(heading_id)
    if match and match.group(1) != case_module:
        fail(f"模块段 {match.group(1)} 与文件名推出的 {case_module} 不符")

    steps = [num for num, _ in split_entries(fields.get("操作步骤", "")) if num is not None]
    if steps:
        if steps[0] != 1:
            fail(f"操作步骤应自 1 起，实为 {steps[0]}")
        for previous, current in zip(steps, steps[1:]):
            if current != previous + 1:
                fail(f"操作步骤编号不连续：{previous} -> {current}")
    for text in (fields.get("操作步骤", "").split("<br>")):
        text = text.strip()
        marker = PURE_EXEC_MARK in text
        numbered = ENTRY_RE.match(text)
        if marker and not numbered:
            fail(f"纯执行步骤缺编号：{text[:24]}")

    if any(num is None for num, _ in split_entries(fields.get("预期结果", ""))):
        fail("预期结果存在无编号条目")
    if expected != sorted(expected) or len(set(expected)) != len(expected):
        fail(f"预期结果编号须严格升序且不重复，实为 {expected}")
    orphans = [num for num in expected if num not in steps]
    if orphans:
        fail(f"预期结果编号 {orphans} 无对应操作步骤")
    renumbered = [num for num, _ in split_entries(fields.get("预期结果", ""))
                  if num is not None and num in pure]
    if renumbered:
        fail(f"纯执行步骤 {renumbered} 不应产生预期结果")
    missing = [num for num in steps if num not in pure and num not in expected]
    if missing:
        fail(f"非纯执行步骤 {missing} 缺对应预期结果")

    dep = fields.get("依赖用例", "")
    if dep and not DEP_RE.match(dep):
        fail(f"依赖用例取值不合规：`{dep}`")
    return [token.strip() for token in dep.split(",")] if dep and dep != "无" else []


def check_document(root, path, cases, errors):
    rel = os.path.relpath(path, root).replace(os.sep, "/")
    case_module = module_of_filename(path)
    try:
        with open(path, encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError as exc:
        errors.append(f"{rel}: 无法读取（{exc}）")
        return
    headings = [(idx + 1, text) for idx, text in enumerate(lines) if text.startswith("#### TC-")]
    if not headings:
        errors.append(f"{rel}: 未发现任何 `#### TC-` 用例声明")
        return
    declared = []
    for position, (line_no, text) in enumerate(headings):
        heading_line = text.rstrip()
        bare = text[5:].strip()
        id_match = CASE_HEADING_RE.match(text)
        if not id_match:
            errors.append(f"{rel}:{line_no} 用例标题须形如 `#### TC-<MODULE>-<NNN> 标题`：{bare[:40]}")
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
        errors.append(f"{rel}: 用例编号重复：{'、'.join(dup)}")
    if declared and declared[0]["seq"] != 1:
        errors.append(f"{rel}: 首条用例编号应为 {case_module}-001")
    for previous, current in zip(declared, declared[1:]):
        if current["seq"] <= previous["seq"]:
            errors.append(f"{rel}: 用例须按编号升序声明：{previous['id']} -> {current['id']}")
    known = set(ids)
    for case in declared:
        seen = []
        for dep in case["deps"]:
            if dep not in known:
                if dep.split("-")[1] != case["module"]:
                    continue  # 跨模块依赖由 other-document 校验，此处只核格式
                errors.append(f"{rel}: {case['id']} 依赖了不存在的用例 {dep}")
            seq = CASE_ID_RE.match(dep)
            if seq and dep in known and int(seq.group(2)) >= case["seq"]:
                errors.append(f"{rel}: {case['id']} 的前置用例 {dep} 编号未在其之前")
            if seq and dep in known:
                seen.append(int(seq.group(2)))
        if seen != sorted(seen):
            errors.append(f"{rel}: {case['id']} 的依赖用例未按编号升序排列")
    check_record_table(rel, lines, declared, errors)
    cases.extend(dict(case, doc=rel) for case in declared)


def check_record_table(rel, lines, declared, errors):
    rows = [(idx + 1, text) for idx, text in enumerate(lines)
            if re.match(r'^\|\s*TC-[A-Z0-9]+-\d{3}\s*\|', text)]
    if not rows:
        errors.append(f"{rel}: 缺执行记录表（§3 未找到以用例编号起始的表格行）")
        return
    by_id = {case["id"]: case for case in declared}
    listed = []
    for line_no, text in rows:
        cells = [cell.strip() for cell in text.strip().strip("|").split("|")]
        case_id = cells[0]
        prefix = f"{rel}:{line_no} [记录表 {case_id}]"
        if len(cells) != len(RECORD_COLS):
            errors.append(f"{prefix} 应为 {len(RECORD_COLS)} 列，实为 {len(cells)} 列")
            continue
        if case_id not in by_id:
            errors.append(f"{prefix} 记录了文档中不存在的用例")
            continue
        listed.append(case_id)
        date, tester, result, steps, phenomenon, defect, remark = cells[1:]
        for name, value in zip(RECORD_COLS[1:], cells[1:]):
            if value in PLACEHOLDERS:
                errors.append(f"{prefix} 「{name}」用了占位符 `{value}`，未执行应留空")
        if result and result not in RESULTS:
            errors.append(f"{prefix} 结果取值 `{result}` 不在 {'/'.join(sorted(RESULTS))}")
        if date and not DATE_RE.match(date):
            errors.append(f"{prefix} 执行日期 `{date}` 非 YYYY-MM-DD")
        expected_ids = by_id[case_id]["expected"]
        if steps and not STEPS_RE.match(steps):
            errors.append(f"{prefix} 失败步骤号 `{steps}` 格式不符")
        if not result:
            for name, value in zip(["执行日期", "执行人", "失败步骤号", "实际现象", "缺陷编号", "备注"],
                                   [date, tester, steps, phenomenon, defect, remark]):
                if value:
                    errors.append(f"{prefix} 未执行的用例「{name}」应留空")
            continue
        if not (date and tester):
            errors.append(f"{prefix} 结果已填但执行日期/执行人缺失")
        if result == "FAIL":
            if not steps:
                errors.append(f"{prefix} FAIL 必须登记失败步骤号")
            else:
                for num in [int(n) for n in steps.split(",")]:
                    if num not in expected_ids:
                        errors.append(f"{prefix} 失败步骤号 {num} 不是该用例的预期结果编号 {expected_ids}")
            if not phenomenon:
                errors.append(f"{prefix} FAIL 必须登记实际现象")
        elif steps:
            errors.append(f"{prefix} 结果 {result} 不应填失败步骤号")
    missing_rows = [case["id"] for case in declared if case["id"] not in listed]
    if missing_rows:
        errors.append(f"{rel}: 执行记录表缺用例行：{'、'.join(missing_rows)}")
    if listed != [case["id"] for case in declared]:
        extra = [i for i in listed if i not in [c["id"] for c in declared]]
        if not extra:
            errors.append(f"{rel}: 执行记录表行序应与 §2 用例声明顺序一致")


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
                raw.append(f"{case['doc']}: {case['id']} 依赖用例 {dep} 在全部文档中不存在")

    kept, stale = apply_exemptions(raw, {os.path.relpath(p, root).replace(os.sep, "/")
                                         for p in targets})
    exempted = len(raw) - len(kept)

    if kept or stale:
        if stale:
            print(f"[FAIL] 白名单存量豁免失效 {len(stale)} 条（对应违规已不再出现，请删条目）：")
            for item in stale:
                print(f"  - {item}")
        if kept:
            print(f"[FAIL] 人工测试用例格式违规 {len(kept)} 处：")
            for item in kept:
                print(f"  - {item}")
        print()
        print("  用例字段范式见各文件 §1.5-§1.7：字段名/顺序/取值域均为解析契约的一部分。")
        print("  修文档而非放宽本门禁，除非确要变更契约（须同步全部文档与脚本）。")
        return 1

    print(f"[PASS] 人工测试用例格式合规：{len(targets)} 份文档 / {len(cases)} 条用例 / "
          f"依赖与执行记录表一致" + (f"（存量豁免 {exempted} 条）" if exempted else "") + "。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

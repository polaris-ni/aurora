#!/usr/bin/env python3
# ============================================================================
# check_no_theme_color_literals.py - 主题硬编码色清偿与豁免登记门禁
# ----------------------------------------------------------------------------
# Spec: 主题债务清零判据。控件层（include/aurora/widget + src/aurora/widget）与 CSD 预设表里的
#   数字色构造式 `Color{...}` / `Color(...)` 必须归零；仍允许存在的每一处只能来自
#   `tools/include/theme_literal_exemptions.h` 的逐条豁免（文件 + 精确次数 + 理由），
#   该表同时被 gen_api 发射进 `aurora_api.json` 的 `theme_tokens.exemptions.files`。
#   同一枚脚本还判两处文档与表的一致性：
#     1) `theme_tokens.groups[].consumed`（属性行是否消费该组令牌）与
#        `codespec/specification/07-environment-modifier.md` §5.1「属性行未消费组」等集；
#     2) `theme_tokens.exemptions.files` 与豁免登记表逐条相等（json 过期即红）。
#
# 判据（编号进违例文案，便于变异自证逐条对号）：
#   L1 未登记：扫描范围内出现数字色构造式但文件不在豁免表。
#   L2 计数漂移：豁免登记次数 != 实际出现次数（按文件放行是不允许的）。
#   L3 悬空登记：豁免表指向不存在的文件。
#   L4 重复登记：同一路径在豁免表出现两次（多重口径）。
#   L5 交互态魔数：控件层出现 `shaded(...)` 调用（交互态改由绘制路径叠加 `roles.hover` /
#      `roles.pressed`，乘算魔数归零）。
#   C1 消费位缺失/漂移：schema 的 groups 缺 `consumed`，或与 §5.1 标注的组集合不等。
#   E1 豁免表与 schema 漂移：`theme_tokens.exemptions.files` 与登记表不等。
#
# 计数口径与判据原文一致：只认 `Color{...}` / `Color(...)` 构造式；`colors::AURORA_*`、
# `_rgb` / `_rgba` 字面量算子与 `palette[...]` 间接取用不计。注释先剥离（字符串保留，
# 故描述符里的示例串仍被计入——它是字面量构造式的另一种出现面）。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_no_theme_color_literals.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import json
import os
import re
import sys

import props_scan

EXEMPTION_HEADER = "tools/include/theme_literal_exemptions.h"
SCHEMA = "aurora_api.json"
SPEC = "codespec/specification/07-environment-modifier.md"
SCOPE_DIRS = ("include/aurora/widget", "src/aurora/widget")
SOURCE_EXTS = (".h", ".hpp", ".inl", ".cpp", ".cc")
COLOR_LITERAL_RE = re.compile(r"\bColor\s*[({]\s*(?:0x[0-9A-Fa-f]+|\d+)")
# 豁免条目形态：`{path, count, reason}`，成员可带可选的指定初始化器前缀（`.path = ...`）；
# 理由允许相邻字符串字面量折行书写。两种写法解析结果一致，登记表按 C++20 指定初始化器书写。
ENTRY_RE = re.compile(
    r'\{\s*(?:\.\s*\w+\s*=\s*)?"([^"]+)"\s*,\s*(?:\.\s*\w+\s*=\s*)?(\d+)\s*,\s*'
    r'(?:\.\s*\w+\s*=\s*)?((?:"(?:[^"\\]|\\.)*"\s*)+)\}',
    re.S,
)
STRING_PIECE_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
# 文档标记：§5.1 的「属性行未消费组」短语与全角冒号。转义书写以保持本文件 ASCII
# （LIT-1 判据不收非注释字符串里的 CJK，而这两个串是判据必需的文档锚点数据）。
CONSUMED_MARKER = "\u5c5e\u6027\u884c\u672a\u6d88\u8d39\u7ec4"
FULLWIDTH_COLON = "\uff1a"


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def scope_files(root):
    """扫描范围内的源码文件（仓库相对路径，去重保序）。"""
    seen = []
    for rel_dir in SCOPE_DIRS:
        base = os.path.join(root, rel_dir.replace("/", os.sep))
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in sorted(filenames):
                if not fn.endswith(SOURCE_EXTS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), root).replace(os.sep, "/")
                if rel not in seen:
                    seen.append(rel)
    return seen


def parse_exemptions(root, violations):
    """解析豁免登记表 -> [(path, count, reason)]；格式不符按违例记。"""
    path = os.path.join(root, EXEMPTION_HEADER.replace("/", os.sep))
    if not os.path.isfile(path):
        violations.append(f"L3 missing exemption header: {EXEMPTION_HEADER}")
        return []
    text = props_scan.strip_comments(read_text(path))
    entries = []
    for match in ENTRY_RE.finditer(text):
        reason = "".join(STRING_PIECE_RE.findall(match.group(3)))
        entries.append((match.group(1), int(match.group(2)), reason))
    if not entries:
        violations.append(f"L3 exemption header declares no entry: {EXEMPTION_HEADER}")
    return entries


def literal_counts(root, files):
    """按文件统计数字色构造式与 shaded 调用；返回 (counts, shaded_hits)。"""
    counts = {}
    shaded = []
    for rel in files:
        text = props_scan.strip_comments(read_text(os.path.join(root, rel.replace("/", os.sep))))
        counts[rel] = len(COLOR_LITERAL_RE.findall(text))
        for match in re.finditer(r"\.\s*shaded\s*\(", text):
            shaded.append(f"{rel}:{props_scan.line_of(text, match.start())}")
    return counts, shaded


def unconsumed_groups(schema, violations):
    """schema 侧 consumed=false 的组名集合；缺键按 C1 记违例。"""
    block = schema.get("theme_tokens")
    if not isinstance(block, dict) or not isinstance(block.get("groups"), list):
        violations.append("C1 schema has no theme_tokens.groups block")
        return None
    names = set()
    for group in block["groups"]:
        if not isinstance(group, dict) or "consumed" not in group or "field" not in group:
            violations.append("C1 theme_tokens group row lacks field/consumed")
            return None
        if group["consumed"] is False:
            names.add(group["field"])
    return names


def doc_unconsumed(root, violations):
    """文档侧 §5.1「属性行未消费组」行反引号里的组名集合。"""
    path = os.path.join(root, SPEC.replace("/", os.sep))
    if not os.path.isfile(path):
        violations.append(f"C1 missing spec file: {SPEC}")
        return None
    for line in read_text(path).splitlines():
        if CONSUMED_MARKER not in line:
            continue
        tail = line.rsplit(FULLWIDTH_COLON, 1)[-1]
        return set(re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", tail))
    violations.append(f"C1 spec has no '{CONSUMED_MARKER}' line")
    return None


def schema_exemptions(schema, violations):
    """schema 侧豁免行 -> {path: (count, reason)}。"""
    block = schema.get("theme_tokens")
    if not isinstance(block, dict) or not isinstance(block.get("exemptions"), dict):
        violations.append("E1 schema has no theme_tokens.exemptions block")
        return None
    rows = block["exemptions"].get("files")
    if not isinstance(rows, list):
        violations.append("E1 schema theme_tokens.exemptions.files is not a list")
        return None
    out = {}
    for row in rows:
        if not isinstance(row, dict) or not {"path", "count", "reason"} <= set(row):
            violations.append("E1 schema exemption row lacks path/count/reason")
            return None
        if row["path"] in out:
            violations.append(f"E1 duplicate schema exemption: {row['path']}")
        out[row["path"]] = (row["count"], row["reason"])
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description="Judge theme color literals and exemption registration.")
    parser.add_argument("--root", default=props_scan.repo_root_of(__file__), help="aurora repository root")
    parser.add_argument("--verbose", action="store_true", help="print every scanned file")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    violations = []

    # ---- 豁免登记表 ----
    entries = parse_exemptions(root, violations)
    registered = {}
    for path, count, reason in entries:
        if path in registered:
            violations.append(f"L4 duplicate exemption: {path}")
            continue
        registered[path] = (count, reason)
        if not os.path.isfile(os.path.join(root, path.replace("/", os.sep))):
            violations.append(f"L3 exemption points at a missing file: {path}")
        if not reason:
            violations.append(f"L3 exemption has no reason: {path}")

    # ---- 扫描面 = 控件层 ∪ 豁免登记路径 ----
    files = scope_files(root)
    for path in registered:
        if path not in files:
            files.append(path)
    counts, shaded = literal_counts(root, files)

    # L1 / L2
    total = 0
    for path in files:
        actual = counts.get(path, 0)
        if path not in registered:
            if actual:
                violations.append(f"L1 unregistered color literals: {path} has {actual}")
        else:
            want = registered[path][0]
            total += actual
            if actual != want:
                violations.append(f"L2 count drift: {path} registered {want}, found {actual}")
    # L5
    for hit in shaded:
        violations.append(f"L5 interaction-magic shaded() call remains at {hit}")

    # ---- 文档与 schema 一致性 ----
    schema = None
    schema_path = os.path.join(root, SCHEMA.replace("/", os.sep))
    if not os.path.isfile(schema_path):
        violations.append(f"E1 missing schema file: {SCHEMA}")
    else:
        try:
            schema = json.loads(read_text(schema_path))
        except json.JSONDecodeError as exc:
            violations.append(f"E1 schema is not valid JSON: {exc}")
    if schema is not None:
        want_unconsumed = unconsumed_groups(schema, violations)
        doc_groups = doc_unconsumed(root, violations)
        if want_unconsumed is not None and doc_groups is not None and want_unconsumed != doc_groups:
            violations.append(
                "C1 consumed drift: schema unconsumed=" + sorted(want_unconsumed).__repr__()
                + " spec unconsumed=" + sorted(doc_groups).__repr__())
        rows = schema_exemptions(schema, violations)
        if rows is not None:
            for path, (count, reason) in registered.items():
                if path not in rows:
                    violations.append(f"E1 exemption missing from schema: {path}")
                elif rows[path][0] != count:
                    violations.append(f"E1 exemption count differs in schema: {path} table {count}, schema {rows[path][0]}")
                elif rows[path][1] != reason:
                    violations.append(f"E1 exemption reason differs in schema: {path}")
            for path in rows:
                if path not in registered:
                    violations.append(f"E1 stale schema exemption: {path}")

    if args.verbose:
        for path in files:
            print(f"  {counts.get(path, 0):4d}  {path}")

    if violations:
        for item in violations:
            print(f"FAIL {item}")
        print(f"FAIL {len(violations)} violation(s)")
        return 1
    print(f"OK scanned {len(files)} file(s); {len(registered)} exemption(s), {total} registered literal(s); "
          f"shaded() calls 0")
    print("OK theme_tokens.consumed and exemptions agree with spec section 5.1 and the registry")
    return 0


if __name__ == "__main__":
    sys.exit(main())

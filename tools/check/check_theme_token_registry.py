#!/usr/bin/env python3
# ============================================================================
# check_theme_token_registry.py - 主题令牌登记表与实际字段的一致门禁
# ----------------------------------------------------------------------------
# Spec: 属性单一声明口径「回落点三形态」的活令牌档：`.fallback<&ColorRoles::primary>()` 把令牌以
#   **成员指针**写进表行，schema 里的 `default_expr` 短名由该组「成员指针 -> 短名」对照表反查。
#   于是这张表同时是三处的真源：编译期回落点、schema 发射名、MCP/CLI/LSP 的可枚举令牌清单。
#   它一旦与实际字段漂移（改字段名没改表、加字段没登记、删字段留下幽灵条目、短名前缀写错），
#   轻则 schema 少一枚令牌，重则发射出一个运行期反查不到落点的名字 —— 而 100% 场景下编译与测试
#   都不报错（成员指针仍合法，只是指向的字段改了名）。五枚登记表头此刻都写着「表与实际字段的
#   一致性由令牌登记表门禁校验」，本门禁就是那句话的兑现。
#
# 五条判据（编号进违例文案，便于变异自证逐条对号）：
#   T1 集合相等：某组的公开标量字段集合 == 该组 `AURORA_TOKENS` 里的成员指针集合（缺登记 / 幽灵
#      条目 / 同枚成员登记两次 各算一条违例）。
#   T2 名字规则：短名必须恰为 `<theme.h 里该组的前缀>.<字段名>`（前缀写错、字段改名没跟 各算一条）。
#   T3 类型对号：theme.h 里该组声明的 `value_type` 必须等于字段的实际声明类型（`Color` / `float` /
#      `Duration`）—— 清单里的类型名是消费侧的判型依据，漂了就误导生成器。
#   T4 反向覆盖：`include/aurora/theming/` 下任何登记了 `AURORA_TOKENS` 的组，必须出现在
#      `theme_token_groups()` 的发射清单里（新组只登记不发射，schema 就永远看不见它）。
#   T5 登记序：表内条目次序必须等于字段声明次序（`theme_tokens` 块的 tokens 数组按登记序发，
#      次序漂了逐字节比对无意义）。
#
# 解析件复用同目录 `props_scan`（注释剥离 / 括号配对 / 类体定位），不另起一份词法。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_theme_token_registry.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import os
import re
import sys

import props_scan

THEMING_DIR = os.path.join("include", "aurora", "theming")
GROUP_ROW_RE = re.compile(
    r"theme_token_group_row<(\w+)>\(\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*\)")
FIELD_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_:]*)\s+([a-z][a-z0-9_]*)$")
STRUCT_HEAD_RE = re.compile(r"\bstruct\s+(\w+)(?:\s[^;{]*)?\{")
ENTRY_RE = re.compile(r"\{&(\w+)::(\w+)\s*,\s*\"([^\"]*)\"\s*\}")


def read(root, rel):
    """注释剥离后的文件文本；缺失返回 None。"""
    path = os.path.join(root, rel.replace(os.sep, "/"))
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return props_scan.strip_comments(handle.read())


def struct_body(text, group):
    """取 `struct <group> { ... }` 的类体文本。"""
    for match in STRUCT_HEAD_RE.finditer(text):
        if match.group(1) != group:
            continue
        body, _start, _line = props_scan.class_body(text, match)
        if body is not None:
            return body
    return None


def split_statements(body):
    """按顶层分号切语句（跳过圆/方/花括号内的分号）。"""
    statements = []
    depth = 0
    current = []
    for ch in body:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == ";" and depth == 0:
            statements.append("".join(current))
            current = []
            continue
        current.append(ch)
    return statements


def public_scalar_fields(body):
    """声明序的公开标量字段：[(字段名, 类型名)]。

    只认「一个类型名 + 一个小写名」两段的语句头：初始化式（`= 4.0F` / `{Duration::from_ms(..)}`）
    先按首个顶层 `=` 或 `{` 截掉，故 `operator==`、`static constexpr auto AURORA_TOKENS`、函数成员
    这些三段以上的头自然落选。属性面里这些组只有纯数据字段，多余形态出现即按「不是字段」处理。
    """
    fields = []
    for statement in split_statements(body):
        head = statement.strip()
        if not head:
            continue
        for sep in ("=", "{"):
            idx = find_top_level(head, sep)
            if idx >= 0:
                head = head[:idx]
        head = head.strip()
        match = FIELD_RE.match(head)
        if not match:
            continue
        fields.append((match.group(2), match.group(1)))
    return fields


def find_top_level(text, ch):
    """第一个不在任何括号内的 `ch` 下标；没有返回 -1。

    判序要紧：`ch` 本身可能就是括号字符（找 `{` 时若先做深度累加，那一处永远扫不到）。
    """
    depth = 0
    for index, char in enumerate(text):
        if char == ch and depth == 0:
            return index
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
    return -1


def registry_entries(body, group):
    """表里的条目：[(成员名, 短名)]（登记序）。"""
    marker = "AURORA_TOKENS"
    idx = body.find(marker)
    if idx < 0:
        return None
    brace = body.find("(", idx)
    if brace < 0:
        return None
    inner, _ = props_scan.matching(body, brace, "(", ")")
    if inner is None:
        return None
    return [(m.group(2), m.group(3)) for m in ENTRY_RE.finditer(inner) if m.group(1) == group]


def emitted_groups(root, problems):
    """theme.h 里 `theme_token_group_row<Group>("OwnerText", "field", "prefix", "value_type")` 清单。"""
    text = read(root, os.path.join(THEMING_DIR, "theme.h"))
    if text is None:
        problems.append("T0 theme.h not found")
        return []
    rows = []
    for match in GROUP_ROW_RE.finditer(text):
        rows.append({"group": match.group(1), "owner_text": match.group(2), "field": match.group(3),
                     "prefix": match.group(4), "value_type": match.group(5)})
    return rows


def header_for(root, group):
    """组类型名 -> 其登记表头相对路径（同目录蛇形名，特例 `ColorRoles` -> `color_roles.h`）。"""
    candidate = re.sub(r"(?<!^)(?=[A-Z])", "_", group).lower() + ".h"
    rel = os.path.join(THEMING_DIR, candidate)
    return rel if os.path.isfile(os.path.join(root, rel.replace(os.sep, "/"))) else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=None)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    root = args.root or props_scan.repo_root_of(__file__)

    problems = []
    rows = emitted_groups(root, problems)
    seen_groups = {row["group"] for row in rows}

    total_tokens = 0
    for row in rows:
        group = row["group"]
        rel = header_for(root, group)
        if rel is None:
            problems.append("T0 group '%s' emitted but no registry header found (expected %s)" % (group, group))
            continue
        text = read(root, rel)
        body = struct_body(text, group)
        if body is None:
            problems.append("T0 struct '%s' not found in %s" % (group, rel))
            continue
        fields = public_scalar_fields(body)
        entries = registry_entries(body, group)
        if entries is None:
            problems.append("T0 '%s' has no AURORA_TOKENS table in %s" % (group, rel))
            continue

        field_names = [name for name, _ in fields]
        entry_members = [member for member, _short in entries]

        missing = sorted(set(field_names) - set(entry_members))
        ghost = sorted(set(entry_members) - set(field_names))
        duplicates = sorted({m for m in entry_members if entry_members.count(m) > 1})
        for name in missing:
            problems.append("T1 %s: field '%s' is not registered in AURORA_TOKENS" % (group, name))
        for name in ghost:
            problems.append("T1 %s: table entry '&%s::%s' names no field" % (group, group, name))
        for name in duplicates:
            problems.append("T1 %s: member '%s' registered more than once" % (group, name))

        by_field = dict(fields)
        for member, short in entries:
            field = by_field.get(member)
            if field is None:
                continue  # T1 已报
            expected = "%s.%s" % (row["prefix"], member)
            if short != expected:
                problems.append("T2 %s: '&%s::%s' registered as '%s', rule says '%s'"
                                % (group, group, member, short, expected))
            if field != row["value_type"]:
                problems.append("T3 %s: field '%s' has type '%s' but theme.h emits value_type '%s'"
                                % (group, member, field, row["value_type"]))

        ordered_present = [m for m in entry_members if m in field_names]
        expected_order = [n for n in field_names if n in set(entry_members)]
        if ordered_present != expected_order:
            problems.append("T5 %s: table order %s != declaration order %s"
                            % (group, ordered_present, expected_order))

        if not missing and not ghost and not duplicates:
            total_tokens += len(entries)

    # T4 反向覆盖：theming 目录里登记了表却没进发射清单的组。
    for name in sorted(os.listdir(os.path.join(root, THEMING_DIR))):
        if not name.endswith(".h") or name == "theme.h":
            continue
        text = read(root, os.path.join(THEMING_DIR, name))
        if text is None or "AURORA_TOKENS" not in text:
            continue
        for match in STRUCT_HEAD_RE.finditer(text):
            group = match.group(1)
            body = struct_body(text, group)
            if body is None or "AURORA_TOKENS" not in body:
                continue
            if group not in seen_groups:
                problems.append("T4 %s: group '%s' registers AURORA_TOKENS but theme_token_groups() omits it"
                                % (name, group))

    if problems:
        print("[FAIL] theme token registry drifted from the real fields:")
        for item in problems:
            print("  - %s" % item)
        return 1

    print("[OK] theme token registry matches the fields: %d tokens in %d groups."
          % (total_tokens, len(rows)))
    if args.verbose:
        for row in rows:
            print("  %s -> %s.* (%s)" % (row["group"], row["prefix"], row["value_type"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

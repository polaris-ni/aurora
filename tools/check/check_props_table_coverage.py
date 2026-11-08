#!/usr/bin/env python3
# ============================================================================
# check_props_table_coverage.py - 属性权威表覆盖率与「每行恰一件」门禁
# ----------------------------------------------------------------------------
# Spec: 属性单一声明口径的第 9 条（纯文本判定）与验收判据第 1 / 2 / 10 条。
#   判定对象 = 已完成迁移的 `XxxProps` 类，即「继承 `AbstractProps<自身>`」的那一类；
#   未迁移的公有字段聚合体按现状放行（迁移期两种形态并存，判据不得反过来咬未迁的控件）。
#
# 十条判据（编号进违例文案，便于变异自证逐条对号）：
#   C1  三方计数：私有字段数 == `x_row()` 行数 == `rows()` 里被点名的行数。
#   C2  词根对应：`x_row()` ↔ `prop_row<&C::x_>("x")` ↔ 私有字段 `x_` ↔ 读件 `x` ↔ 写件 `set_x`
#       —— 词根、JSON 键名、字段名三者必须字面相等（「词根 = 属性键名」是本口径的地基）。
#   C3  回落二选一：每行必须命中 `.fallback(...)` / `.fallback<...>()` 或 `.required()` 之一，
#       都不写即红（不给回落点正是本轮要消灭的分叉），两者都写亦红。回落点的合法拼法只有机制层
#       那三件重载对应的两种（`.fallback<&Scale::member>()` 令牌档 / `.fallback(&Owner::provider)`
#       提供者档）：给裸值的实参没有对应重载，编译红即判红；写法不正的实参同样判红。
#   C4  读面每行恰一件：由该行回落点形态唯一决定读件形状 ——
#       常量档（无 ctx 的库内默认提供者）与必填行 ⇒ 必须有 `x()` 且不得有 `x(ctx)`；
#       作用域依赖档（活令牌档 `.fallback<&Scale::m>()` 与派生档：提供者形参含 `BuildContext`）
#       ⇒ 必须有 `x(ctx)` 且不得有无参 `x()`。两个方向的漏写与多写各判一次红。
#   C5  写面转调对号：每行至少一枚 `set_x`；体内只允许 `assign(x_row(), ...)` / `rebind(x_row(), ...)`
#       / 同名词根重载转调，且 `rebind` 只许出现在来源行（字段形状是句柄：只读 `ReadOnlyState<T>`
#       或可写 `State<T>`，与机制层 `PropIsHandle` 的两条特化同口径）上。
#   C6  表外名字不得进公共面：`XxxProps` 的公共函数名只允许「行规则件」
#       （`x_row` / `x` / `set_x`）+ 回落点提供者（`x_default`）+ 通用件白名单 `{clear, rows}`；
#       `x_source()` 一类句柄读件、`font()` 一类合成读件、`set_border(c, w)` 一类组合便利写件
#       任一声明即红——它们不进表、不进 descriptor、不进序列化，是模型从属性键推不出来的名字。
#   C7  来源性不作旁注：行上出现 `.source(...)` 即红（该事实由字段形状推导，重复声明必漂）。
#   C8  撤表态无具名件：`XxxProps` 公共面出现任何 `reset_<...>()` 声明即红，
#       退回回落点只有 `clear(row)` 一条通道（库内非属性类的 `reset_*` 不在判定面内）。
#   C9  表序即 schema 序：`rows()` 里的点名次序必须等于 `x_row()` 的声明次序。
#   C10 派生档必带显示串：派生档行须有 `.shows(...)` 或 `.shows_token<...>()`，否则发射面给不出默认值表示。
#
# 解析件（去注释、类体定位、成员与字段扫描）取自同目录 `props_scan`：本门禁与
# `check_widget_bind_pattern` / `check_no_optional_in_public_signature` 判的是同一张权威表
# 的三个侧面（表内自洽 / 控件接线 / 签名不外泄可空态）。「什么算一行」「公共面到哪为止」
# 必须是同一个答案——两份各自实现的解析器迟早给出两个答案，而两个答案都绿不代表口径成立。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_props_table_coverage.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import re
import sys

import props_scan

PUBLIC_FACE_WHITELIST = ("clear", "rows")


def extract_row_calls(row_body):
    """从 `x_row()` 函数体取 `prop_row<&C::field_>("key") ...` 及其链式实参。"""
    m = re.search(r"prop_row\s*(<[^;]*?>)?\s*\(\s*&\s*\w+::(\w+)\s*\)\s*\(\s*\"([^\"]*)\"\s*\)", row_body)
    if not m:
        m2 = re.search(r"prop_row<&\s*\w+::(\w+)\s*>\s*\(\s*\"([^\"]*)\"\s*\)", row_body)
        if m2:
            return m2.group(1), m2.group(2), row_body
        return None, None, row_body
    return m.group(2), m.group(3), row_body


def check_props_class(rel, class_name, body, members, fields, findings, stats):
    # 违例报的是文件行号：`props_scan.parse_members` 已按类体起始行换算过 `Member.line`
    rows = {}
    order = []
    providers = {m.name: m for m in members if m.is_static}

    for m in members:
        if not m.name.endswith("_row"):
            continue
        root = m.name[:-4]
        if not m.is_static:
            findings.append(f"{rel}:{m.line}: [C1] `{m.name}` must be a static row accessor")
        field, key, row_body = extract_row_calls(m.body)
        rows[root] = {"root": root, "line": m.line, "field": field, "key": key, "body": row_body}
        order.append(root)

    rows_member = next((m for m in members if m.name == "rows"), None)
    named = []
    if rows_member:
        named = re.findall(r"\b(\w+)_row\s*\(\s*\)", rows_member.body)

    # --- C1 三方计数 ---
    field_names = [n for n, _ in fields]
    if len(field_names) != len(rows):
        findings.append(f"{rel}: [C1] {class_name}: {len(field_names)} private fields vs {len(rows)} `*_row()` "
                        "accessors -- every field needs exactly one row")
    if named and sorted(named) != sorted(rows):
        missing = sorted(set(rows) - set(named))
        extra = sorted(set(named) - set(rows))
        if missing:
            findings.append(f"{rel}: [C1] {class_name}: rows not named in `rows()`: {', '.join(missing)}")
        if extra:
            findings.append(f"{rel}: [C1] {class_name}: `rows()` names undeclared rows: {', '.join(extra)}")
    if len(named) != len(set(named)):
        dupes = sorted({n for n in named if named.count(n) > 1})
        findings.append(f"{rel}: [C1] {class_name}: duplicated in `rows()`: {', '.join(dupes)}")

    # --- C9 表序 ---
    if named and len(named) == len(order) and named != order:
        findings.append(f"{rel}: [C9] {class_name}: `rows()` order must match `*_row()` declaration order "
                        f"(schema key order); declared {order}, listed {named}")

    # --- C2 词根对应 ---
    for root, row in rows.items():
        if row["field"] is None:
            findings.append(f"{rel}:{row['line']}: [C2] {class_name}::{root}_row(): no "
                            "`prop_row<&Class::field_>(\"key\")` pattern found")
            continue
        if row["field"] != root + "_":
            findings.append(f"{rel}:{row['line']}: [C2] {class_name}: row root `{root}` binds field "
                            f"`{row['field']}` -- field name must be `<root>_`")
        if row["key"] != root:
            findings.append(f"{rel}:{row['line']}: [C2] {class_name}: row root `{root}` declares JSON key "
                            f"`{row['key']}` -- root and key must be the same word")
        if row["field"] not in field_names:
            findings.append(f"{rel}:{row['line']}: [C1] {class_name}: row `{root}` binds undeclared field "
                            f"`{row['field']}`")

    # --- 行的回落形态 ---
    for root, row in rows.items():
        expr = row["body"]
        has_token = bool(re.search(r"\.fallback\s*<", expr))
        has_paren = bool(re.search(r"\.fallback\s*\(", expr))
        has_provider = re.search(r"\.fallback\s*\(\s*&\s*\w+::(\w+)\s*\)", expr)
        # 值字面量：机制层三件回落入口（`fallback<auto TokenPtr>` / `fallback(Bare (*)())` /
        # `fallback(Bare (*)(const Owner &, const BuildContext &))`）没有一件收值，编译直接红。
        has_literal = bool(re.search(r'\.fallback\s*\(\s*(?:[-+]?[0-9]|["{]|true\b|false\b|nullptr\b)', expr))
        has_required = bool(re.search(r"\.required\s*\(", expr))
        forms = sum([has_token, bool(has_provider)])
        if forms == 0 and not has_required and not has_paren:
            findings.append(f"{rel}:{row['line']}: [C3] {class_name}: row `{root}` declares neither "
                            "`.fallback(...)` nor `.required()`")
        if forms and has_required:
            findings.append(f"{rel}:{row['line']}: [C3] {class_name}: row `{root}` declares both a fallback "
                            "and `.required()`")
        # 判定面不得窄于机制面：曾经这里把 `.fallback(<裸值>)` 认作一档合法的「值形态常量档」，还顺着
        # C4 要求它配无参读件——而机制层根本没有收值的重载，门禁绿、编译红。裸值与写法不正的实参
        # 各判一次红（全库 133 处回落点实测只有 23 枚令牌档 / 110 枚提供者档两种拼法）。
        if has_paren and not has_provider:
            if has_literal:
                findings.append(f"{rel}:{row['line']}: [C3] {class_name}: row `{root}` passes a value literal to "
                                "`.fallback(...)`; the mechanism layer has no overload taking a value -- "
                                "use `.fallback<&Scale::member>()`, `.fallback(&Owner::provider)`, or `.required()`")
            else:
                findings.append(f"{rel}:{row['line']}: [C3] {class_name}: row `{root}` writes `.fallback(...)` whose "
                                "argument is not the canonical `&Owner::provider` spelling -- the two provider "
                                "overloads are selected by their signature, so the call site must name the owner")
        if re.search(r"\.source\s*\(", expr):
            findings.append(f"{rel}:{row['line']}: [C7] {class_name}: row `{root}` writes a `.source(...)` "
                            "side-note; source-ness is derived from the field shape")
        scoped = None
        form = None
        if has_token:
            scoped, form = True, "token"
        elif has_provider:
            provider = providers.get(has_provider.group(1))
            form = "provider"
            scoped = None if provider is None else provider.has_ctx_param()
        elif has_required:
            scoped, form = False, "required"
        row["scoped"] = scoped
        row["form"] = form
        # C10 只钉派生档（带 ctx 的提供者）：活令牌档由令牌反查表出 default_expr，无 ctx 的库内默认
        # 提供者可直接经同一 codec 格式化，这两档都不必写显示串；带 ctx 的派生提供者没有实例可读默认值，
        # 必须写。显示串两种写法皆合法：
        # `.shows(<串>)` 给非令牌派生（`auto` / `none` 一类），`.shows_token<&X::y>()` 给只读已登记
        # 令牌的派生（名由登记表反查、引号发射时补）——判据认「有显示串」，不认它的来源写法。
        if form == "provider" and scoped is True and not re.search(
                r"\.(shows\s*\(|shows_token<)", expr):
            findings.append(f"{rel}:{row['line']}: [C10] {class_name}: derived-form row `{root}` needs "
                            "`.shows(...)` or `.shows_token<...>()` -- the emitter has no instance to "
                            "read a default from")

    # --- C4 读面每行恰一件 ---
    readers = {}
    writers = {}
    for m in members:
        if m.is_static or m.access != "public":
            continue
        if m.name.endswith("_row") or m.name in PUBLIC_FACE_WHITELIST:
            continue
        if m.name.startswith("set_"):
            writers.setdefault(m.name[4:], []).append(m)
        else:
            readers.setdefault(m.name, []).append(m)

    for root, row in rows.items():
        got = readers.get(root, [])
        if not got:
            want = f"{root}(const BuildContext &)" if row.get("scoped") else f"{root}()"
            findings.append(f"{rel}: [C4] {class_name}: row `{root}` has no reader (expected `{want}`)")
        shapes = {bool(m.has_ctx_param()) for m in got}
        if len(got) > 1 and len(shapes) > 1:
            findings.append(f"{rel}: [C4] {class_name}: row `{root}` declares both `x()` and `x(ctx)` -- "
                            "one row, one answer")
        if len(shapes) == 1 and row.get("scoped") is not None:
            if row["scoped"] is True and not shapes.pop():
                findings.append(f"{rel}: [C4] {class_name}: row `{root}` falls back to a scope-dependent "
                                "provider but its reader takes no BuildContext")
            elif row["scoped"] is False and shapes.pop():
                findings.append(f"{rel}: [C4] {class_name}: row `{root}` has a constant fallback but its "
                                "reader takes BuildContext")

    # --- C5 写面转调对号 ---
    for root, row in rows.items():
        got = writers.get(root, [])
        if not got:
            findings.append(f"{rel}: [C5] {class_name}: row `{root}` has no `set_{root}(...)` writer")
        field_decl = next((d for n, d in fields if n == row.get("field")), None)
        is_source_row = False
        if field_decl:
            line_text = body.splitlines()[field_decl - 1]
            is_source_row = props_scan.is_source_field_decl(line_text)
        for m in got:
            calls = re.findall(r"\b(assign|rebind)\s*\(\s*(\w+)_row\s*\(\s*\)", m.body)
            delegates = re.findall(r"\bset_" + re.escape(root) + r"\s*\(", m.body)
            if not calls and not delegates:
                findings.append(f"{rel}:{m.line}: [C5] {class_name}::set_{root}: body neither calls "
                                f"`assign`/`rebind` nor delegates to a same-root overload")
            for kind, called_root in calls:
                if called_root != root:
                    findings.append(f"{rel}:{m.line}: [C5] {class_name}::set_{root} targets row "
                                    f"`{called_root}` -- writer root must equal the row root")
                if kind == "rebind" and not is_source_row:
                    findings.append(f"{rel}:{m.line}: [C5] {class_name}::set_{root} calls `rebind` but the "
                                    "field shape is not a handle (`ReadOnlyState` / `State`) row")

    # --- C6 表外名字 / C8 reset_ ---
    known_static_suffix = "_default"
    for m in members:
        if m.access != "public":
            continue
        name = m.name
        if name.endswith("_row") or name in PUBLIC_FACE_WHITELIST or name in rows:
            continue
        if name.startswith("set_") and name[4:] in rows:
            continue
        if m.is_static and name.endswith(known_static_suffix) and name[:-len(known_static_suffix)] in rows:
            continue
        if re.match(r"^reset_[a-z]\w*$", name):
            findings.append(f"{rel}:{m.line}: [C8] {class_name}: named opt-out `{name}()` -- the only public "
                            "way back to a fallback is `clear(row)`")
        elif name.endswith("_source"):
            findings.append(f"{rel}:{m.line}: [C6] {class_name}: handle reader `{name}()` is off-table -- "
                            "source-ness is derived, not exposed")
        elif name.startswith("set_"):
            findings.append(f"{rel}:{m.line}: [C6] {class_name}: composite convenience writer `{name}()` is "
                            "off-table -- chain two row writers instead")
        else:
            findings.append(f"{rel}:{m.line}: [C6] {class_name}: off-table name `{name}()` in the public "
                            "face -- the reader of a row is `<root>()`/`<root>(ctx)` and nothing else")

    stats["classes"] += 1
    stats["rows"] += len(rows)


def scan_text(rel, text, findings, stats):
    stats["declared"] += len(props_scan.PROPS_DECL_RE.findall(text))
    for m in props_scan.MIGRATED_DECL_RE.finditer(text):
        name = m.group(1)
        body, _brace, line_offset = props_scan.class_body(text, m)
        if body is None:
            findings.append(f"{rel}: [C1] `{name}` derives AbstractProps but its class body is missing "
                            "or is not brace-balanced")
            continue
        stats["migrated"] += 1
        spans = props_scan.parse_access_spans(body)
        check_props_class(rel, name, body, props_scan.parse_members(body, line_offset),
                          props_scan.parse_fields(body, spans), findings, stats)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    ap.add_argument("--verbose", action="store_true", help="print the per-class census before the verdict")
    args = ap.parse_args()
    root = args.root or props_scan.repo_root_of(__file__)

    findings = []
    files_scanned = 0
    stats = {"declared": 0, "migrated": 0, "classes": 0, "rows": 0}

    for path, rel in props_scan.header_files(root):
        files_scanned += 1
        scan_text(rel, props_scan.read_stripped(path), findings, stats)

    if args.verbose:
        print(f"[info] scanned {files_scanned} headers; migrated {stats['migrated']} / declared "
              f"{stats['declared']} Props classes; {stats['rows']} property rows total")

    if findings:
        print(f"[FAIL] property table coverage violations ({len(findings)} findings, "
              "criteria C1-C10 of the single-declaration contract):")
        for v in findings:
            print(f"  {v}")
        print("  every migrated Props class must satisfy: one private field per row, one row per key, "
              "fallback XOR required, exactly one reader per row whose shape follows the fallback form, "
              "one chained writer per row, and no off-table name in the public face")
        return 1

    if stats["migrated"] == 0:
        print("[FAIL] no Props class inherits AbstractProps -- the gate is spinning on an empty set "
              "(a migrated widget must be registered before this check can hold)")
        return 1
    print(f"[PASS] {stats['migrated']} migrated Props class(es), {stats['rows']} property row(s) fully "
          f"covered ({stats['declared']} Props declarations in total; the rest still use public fields "
          "and are pending migration)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

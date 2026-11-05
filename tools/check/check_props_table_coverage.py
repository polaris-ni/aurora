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
#       都不写即红（不给回落点正是本轮要消灭的分叉），两者都写亦红。
#   C4  读面每行恰一件：由该行回落点形态唯一决定读件形状 ——
#       常量档（值形态 / 无 ctx 的库内默认提供者）与必填行 ⇒ 必须有 `x()` 且不得有 `x(ctx)`；
#       作用域依赖档（活令牌档 `.fallback<&Scale::m>()` 与派生档：提供者形参含 `BuildContext`）
#       ⇒ 必须有 `x(ctx)` 且不得有无参 `x()`。两个方向的漏写与多写各判一次红。
#   C5  写面转调对号：每行至少一枚 `set_x`；体内只允许 `assign(x_row(), ...)` / `rebind(x_row(), ...)`
#       / 同名词根重载转调，且 `rebind` 只许出现在来源行（字段形状含 `ReadOnlyState`）上。
#   C6  表外名字不得进公共面：`XxxProps` 的公共函数名只允许「行规则件」
#       （`x_row` / `x` / `set_x`）+ 回落点提供者（`x_default`）+ 通用件白名单 `{clear, rows}`；
#       `x_source()` 一类句柄读件、`font()` 一类合成读件、`set_border(c, w)` 一类组合便利写件
#       任一声明即红——它们不进表、不进 descriptor、不进序列化，是模型从属性键推不出来的名字。
#   C7  来源性不作旁注：行上出现 `.source(...)` 即红（该事实由字段形状推导，重复声明必漂）。
#   C8  撤表态无具名件：`XxxProps` 公共面出现任何 `reset_<...>()` 声明即红，
#       退回回落点只有 `clear(row)` 一条通道（库内非属性类的 `reset_*` 不在判定面内）。
#   C9  表序即 schema 序：`rows()` 里的点名次序必须等于 `x_row()` 的声明次序。
#   C10 派生档必带显示串：派生档行须有 `.shows(...)`，否则发射面给不出默认值表示。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_props_table_coverage.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import os
import re
import sys

SCAN_DIRS = ("include",)
PUBLIC_FACE_WHITELIST = ("clear", "rows")
# 未迁移的 `XxxProps` 声明形态：不继承 AbstractProps 的聚合体（机制基类 `AbstractProps` 自身除外）。
PROPS_DECL_RE = re.compile(r"^\s*(?:class|struct)\s+(?!AbstractProps\b)(\w*Props)\b", re.M)
MIGRATED_DECL_RE = re.compile(r"(?:class|struct)\s+(\w*Props)\s*(?:final\s*)?:\s*public\s+AbstractProps\s*<\s*\1\s*>")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def strip_comments(text):
    """去注释保行结构：注释字符替换为空格，字符串与字符字面量原样保留（键名在其中）。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif c in "\"'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def matching(text, open_idx, open_ch, close_ch):
    """返回 (内容, 闭括号后的下标)；open_idx 指向 open_ch。找不到闭括号返回 (None, open_idx)。"""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i], i + 1
        i += 1
    return None, i


def line_of(text, idx):
    return text.count("\n", 0, idx) + 1


def angle_stripped(sig):
    """去掉尖括号内内容，便于判断「参数表之外是否还有括号」。"""
    out, depth, i = [], 0, 0
    while i < len(sig):
        c = sig[i]
        if c == "<":
            depth += 1
        elif c == ">" and depth:
            depth -= 1
        elif c == "," and depth:
            pass
        elif depth == 0:
            out.append(c)
        i += 1
    return "".join(out)


class Member:
    __slots__ = ("name", "params", "ret", "body", "is_static", "access", "line")

    def __init__(self, name, params, ret, body, is_static, access, line):
        self.name = name
        self.params = params
        self.ret = ret
        self.body = body
        self.is_static = is_static
        self.access = access
        self.line = line

    def has_ctx_param(self):
        return "BuildContext" in self.params


def parse_members(body_text, _unused=None):
    """扫类体内的函数成员：`auto <name>(<params>) [const] [-> ret] { ... }`。"""
    members = []
    spans = parse_access_spans(body_text)
    for m in re.finditer(r"\bauto\s+(\w+)\s*\(", body_text):
        name = m.group(1)
        open_paren = m.end() - 1
        params, after_paren = matching(body_text, open_paren, "(", ")")
        if params is None:
            continue
        # 参数表之后到函数体：可能带 `const`、`noexcept`、尾置返回类型
        rest = body_text[after_paren:]
        brace = rest.find("{")
        semi = rest.find(";")
        if brace < 0:
            continue
        tail = angle_stripped(rest[:brace])
        ret_m = re.search(r"->\s*([^{};]+?)\s*$", tail)
        ret = ret_m.group(1).strip() if ret_m else ""
        prefix = body_text[:m.start()]
        is_static = bool(re.search(r"\bstatic\b[^\n]*$", prefix))
        access = access_for(spans, m.start())
        if semi >= 0 and semi < brace:
            # 声明而非定义（纯虚 / 外联实现）：只登记名字，体为空
            fbody = ""
        else:
            fbody, _ = matching(body_text, after_paren + brace, "{", "}")
            if fbody is None:
                continue
        members.append(Member(name, params.strip(), ret, fbody, is_static, access,
                              line_of(body_text, m.start())))
    return members


def parse_access_spans(body_text):
    """返回 [(offset, access)]，按 `public:` / `private:` / `protected:` 出现次序。"""
    spans = [(0, "public")]
    for m in re.finditer(r"^(\s*)(public|private|protected)\s*:", body_text, re.M):
        spans.append((m.start(), m.group(2)))
    return spans


def access_for(spans, offset):
    cur = "public"
    for pos, kind in spans:
        if pos <= offset:
            cur = kind
        else:
            break
    return cur


def parse_fields(body_text, spans):
    """私有数据成员：以 `name_;` 或 `name_ = ...;` 收尾、且落在 private 区的行（本库一行一枚）。"""
    fields = []
    lines = body_text.splitlines()
    for idx, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped.endswith(";") or stripped.startswith("friend"):
            continue
        if "(" in angle_stripped(stripped.split("=")[0]):
            continue
        fm = re.search(r"\b([a-z][a-z0-9_]*)_\s*(?:=\s*.*)?;$", stripped)
        if not fm:
            continue
        offset = sum(len(l) + 1 for l in lines[:idx - 1])
        if access_for(spans, offset) != "private":
            continue
        fields.append((fm.group(1) + "_", idx))
    return fields


def extract_row_calls(row_body):
    """从 `x_row()` 函数体取 `prop_row<&C::field_>("key") ...` 及其链式实参。"""
    m = re.search(r"prop_row\s*(<[^;]*?>)?\s*\(\s*&\s*\w+::(\w+)\s*\)\s*\(\s*\"([^\"]*)\"\s*\)", row_body)
    if not m:
        m2 = re.search(r"prop_row<&\s*\w+::(\w+)\s*>\s*\(\s*\"([^\"]*)\"\s*\)", row_body)
        if m2:
            return m2.group(1), m2.group(2), row_body
        return None, None, row_body
    return m.group(2), m.group(3), row_body


def check_props_class(rel, text, class_name, body, body_start, findings, stats):
    spans = parse_access_spans(body)
    members = parse_members(body)
    fields = parse_fields(body, spans)
    # 违例报的是文件行号：类体内的相对行号加上类体起始行
    line_base = text.count("\n", 0, body_start)
    for m in members:
        m.line += line_base

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
        has_provider = re.search(r"\.fallback\s*\(\s*&\s*\w+::(\w+)\s*\)", expr)
        has_value = bool(re.search(r"\.fallback\s*\((?!\s*&)", expr))
        has_required = bool(re.search(r"\.required\s*\(", expr))
        forms = sum([has_token, bool(has_provider), has_value])
        if forms == 0 and not has_required:
            findings.append(f"{rel}:{row['line']}: [C3] {class_name}: row `{root}` declares neither "
                            "`.fallback(...)` nor `.required()`")
        if forms and has_required:
            findings.append(f"{rel}:{row['line']}: [C3] {class_name}: row `{root}` declares both a fallback "
                            "and `.required()`")
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
        elif has_value:
            scoped, form = False, "value"
        elif has_required:
            scoped, form = False, "required"
        row["scoped"] = scoped
        row["form"] = form
        # C10 只钉派生档（带 ctx 的提供者）：活令牌档由令牌反查表出 default_expr，
        # 值形态与库内默认提供者可直接经同一 codec 格式化，均不需要显示串。
        if form == "provider" and scoped is True and not re.search(r"\.shows\s*\(", expr):
            findings.append(f"{rel}:{row['line']}: [C10] {class_name}: derived-form row `{root}` needs "
                            "`.shows(...)` -- the emitter has no instance to read a default from")

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
            is_source_row = "ReadOnlyState" in line_text
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
                                    "field shape is not a handle (`ReadOnlyState`) row")

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


def scan_file(path, rel, findings, stats):
    with open(path, encoding="utf-8", errors="replace") as f:
        raw = f.read()
    text = strip_comments(raw)
    declared = PROPS_DECL_RE.findall(text)
    if declared:
        stats["declared"] += len(declared)
    for m in MIGRATED_DECL_RE.finditer(text):
        name = m.group(1)
        brace = text.find("{", m.end())
        if brace < 0:
            findings.append(f"{rel}: [C1] `{name}` derives AbstractProps but no class body found")
            continue
        body, _ = matching(text, brace, "{", "}")
        if body is None:
            findings.append(f"{rel}: [C1] `{name}` class body is not brace-balanced")
            continue
        stats["migrated"] += 1
        check_props_class(rel, text, name, body, brace, findings, stats)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    ap.add_argument("--verbose", action="store_true", help="print the per-class census before the verdict")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    findings = []
    files_scanned = 0
    stats = {"declared": 0, "migrated": 0, "classes": 0, "rows": 0}

    for sd in SCAN_DIRS:
        base = os.path.join(root, sd)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in sorted(filenames):
                if not fn.endswith((".h", ".hpp", ".inl")):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                files_scanned += 1
                scan_file(path, rel, findings, stats)

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

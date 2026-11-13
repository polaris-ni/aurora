#!/usr/bin/env python3
# ============================================================================
# check_widget_bind_pattern.py - 控件接线仪式（唯一手写件）的门禁
# ----------------------------------------------------------------------------
# Spec: 属性单一声明口径第 7 条与第 9 条「每个公有继承 Props 的控件，其构造尾部必须
#   出现一次 `bind(*this)`」；基类次序判据取自「落码事实」其十四①（基类移动面先于兄弟
#   属性子对象执行，故 Props 必须是**最后一个**基类，否则 `adopt()` 与 `take_over()` 的
#   补半顺序颠倒，整体移动即失联）。
#
# 判定面 = 公有继承了**已迁移** Props（即继承 `AbstractProps<自身>` 的那一类）的控件类。
#   未迁移的 `XxxProps` 还是公有字段聚合体，根本没有 `bind` 这件仪式，判据不得反过来咬它们。
#
# 四条判据（编号进违例文案，便于变异自证逐条对号）：
#   B1 基类次序：属性基类必须是基类清单的**最后一项**（写在 `Widget` 之前即红）。
#   B2 继承可见性：属性基类必须**公有**继承（私有/保护继承让链式写件在调用点失去可见性）。
#   B3 一次接线：每枚「非转调」构造的体内必须**恰好**出现一次 `bind(*this)`；
#      漏写即红，`= default` 的构造同样红（默认实现里不可能有接线）；
#      头里只有声明的构造改查类外定义（`src/` 同名的 `Class::Class` 体），两处都找不到即红。
#      转调同类的构造（`: Button(...)`）豁免——接线由被转调者兑现，本件重复接线反而多余。
#   B4 接线实参：出现 `bind(` 而实参不是 `*this` 即红（接错宿主 = 标脏打到别的控件）。
#
# 解析件取自同目录 `props_scan`，与 `check_props_table_coverage` 同口径。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_widget_bind_pattern.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import re
import sys

import props_scan

BIND_CALL_RE = re.compile(r"\bbind\s*\(([^)]*)\)")
SINGLE_BIND = "bind(*this)"


def repo_text(root):
    """一次性读入公共头与实现（去注释），供两遍扫描复用。"""
    headers = []
    for path, rel in props_scan.header_files(root):
        headers.append((rel, props_scan.read_stripped(path)))
    sources = []
    for path, rel in props_scan.source_files(root):
        sources.append((rel, props_scan.read_stripped(path)))
    return headers, sources


def migrated_props_names(headers):
    names = set()
    for _rel, text in headers:
        names.update(props_scan.MIGRATED_DECL_RE.findall(text))
    return names


def find_ctor_bodies(class_name, sources):
    """在实现文件里找该类的类外构造定义，返回 [(相对路径, 参数表, 初始化清单, 体, 行号)]。"""
    found = []
    for rel, text in sources:
        for params, init, body, line in props_scan.out_of_line_ctors(text, class_name):
            found.append((rel, params, init, body, line))
    return found


def check_ctor(class_name, ctor, rel, findings, stats, out_of_line):
    """B3 / B4：一枚构造的接线判据。`out_of_line` 为该类的类外定义清单（可能为空）。"""
    if ctor.delegates(class_name):
        stats["delegating"] += 1
        return
    if ctor.defaulted == "delete":
        return
    if ctor.defaulted == "default":
        findings.append(f"{rel}:{ctor.line}: [B3] {class_name}: defaulted constructor cannot call "
                        "`bind(*this)` -- give it a body that wires the host")
        return
    bodies = []
    if ctor.declared_only:
        matches = [entry for entry in out_of_line if entry[1] == ctor.params]
        if not matches:
            findings.append(f"{rel}:{ctor.line}: [B3] {class_name}: constructor has no body in the header and "
                            "none in src/ -- the wiring ritual cannot be verified")
            return
        for src_rel, _params, _init, body, line in matches:
            bodies.append((f"{src_rel}:{line}", body))
    else:
        bodies.append((f"{rel}:{ctor.line}", ctor.body))
    for where, body in bodies:
        calls = BIND_CALL_RE.findall(body)
        if len(calls) != 1:
            stats["unbound"] += 1 if not calls else 0
            findings.append(f"{where}: [B3] {class_name}: constructor body calls `bind` {len(calls)} times "
                            f"-- exactly one `{SINGLE_BIND}` is required (the only handwritten ritual of the "
                            "single-declaration contract)")
            return
        stats["bound"] += 1
        arg = calls[0].strip()
        if arg != "*this":
            findings.append(f"{where}: [B4] {class_name}: `bind({arg})` -- the host argument must be `*this`, "
                            "otherwise dirty marks land on another widget")


def check_widget(rel, class_name, bases, body, line_offset, migrated, sources, findings, stats):
    stats["widgets"] += 1
    props_index = [i for i, (_access, name) in enumerate(bases) if name in migrated]
    if len(props_index) > 1:
        findings.append(f"{rel}: [B1] {class_name}: inherits {len(props_index)} migrated Props classes -- "
                        "one widget carries exactly one property table")
    for idx in props_index:
        access, name = bases[idx]
        if access != "public":
            findings.append(f"{rel}: [B2] {class_name}: inherits `{name}` with `{access}` access -- the "
                            "chained writers must stay visible at the call site")
        if idx != len(bases) - 1:
            findings.append(f"{rel}: [B1] {class_name}: `{name}` is not the last base class -- base subobjects "
                            "are moved before member subobjects, so the host rebind order inverts and a moved "
                            "widget silently loses its link")
    out_of_line = find_ctor_bodies(class_name, sources)
    for ctor in props_scan.parse_ctors(body, class_name, line_offset):
        stats["ctors"] += 1
        check_ctor(class_name, ctor, rel, findings, stats, out_of_line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    ap.add_argument("--verbose", action="store_true", help="print the per-widget census before the verdict")
    args = ap.parse_args()
    root = args.root or props_scan.repo_root_of(__file__)

    headers, sources = repo_text(root)
    migrated = migrated_props_names(headers)
    if not migrated:
        print("[FAIL] no Props class inherits AbstractProps -- there is nothing to wire, the gate is "
              "spinning on an empty set")
        return 1

    findings = []
    stats = {"widgets": 0, "ctors": 0, "bound": 0, "delegating": 0, "unbound": 0}
    for rel, text in headers:
        for m in props_scan.WIDGET_INHERITS_PROPS_RE.finditer(text):
            class_name = m.group(1)
            bases = props_scan.base_list(m.group(2))
            if not any(name in migrated for _access, name in bases):
                continue
            body, _brace, line_offset = props_scan.class_body(text, m)
            if body is None:
                findings.append(f"{rel}: [B3] `{class_name}` class body is missing or not brace-balanced")
                continue
            check_widget(rel, class_name, bases, body, line_offset, migrated, sources, findings, stats)

    if args.verbose:
        print(f"[info] migrated Props: {', '.join(sorted(migrated))}")
        print(f"[info] widgets on that face: {stats['widgets']}; constructors judged: {stats['ctors']} "
              f"(bound {stats['bound']}, delegating {stats['delegating']}, missing bind {stats['unbound']})")

    if findings:
        print(f"[FAIL] widget wiring-pattern violations ({len(findings)} findings, criteria B1-B4 of the "
              "single-declaration contract):")
        for v in findings:
            print(f"  {v}")
        print("  every widget that inherits a migrated Props class must inherit it publicly as its last base "
              "class, and every non-delegating constructor must end with exactly one `bind(*this)`")
        return 1

    if stats["widgets"] == 0:
        print("[FAIL] no widget publicly inherits a migrated Props class -- the gate is spinning on an "
              "empty set (migrate a widget before this check can hold)")
        return 1
    print(f"[PASS] {stats['widgets']} widget(s) on the migrated property face, {stats['ctors']} constructor(s) "
          f"verified ({stats['bound']} bind, {stats['delegating']} delegate to a binding constructor)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# ============================================================================
# check_no_optional_in_public_signature.py - 可空态不外泄门禁
# ----------------------------------------------------------------------------
# Spec: 属性单一声明口径第 2 条末段与第 9 条 —— 「每个属性行必须给回落点，全部非必填字段
#   随之可空——值型 `std::optional<T>`、来源型 `std::optional<ReadOnlyState<T>>`（可写来源同形：
#   `std::optional<State<T>>`）；**公共签名一律裸 T / 裸句柄，`std::optional` 不再外泄**」。
#   这条把「optional 只活在存储层」变成机械判据，顺带拦住 `ReadOnlyState<std::optional<T>>`
#   这一类双重空态（现状 `ReadOnlyState` 的旧包装形态，迁移后必须归零）。
#
# 判定面 = 已迁移 Props 类（继承 `AbstractProps<自身>` 的那一类）的**公共成员函数签名**
#   （读件 / 写件 / 回落点提供者 / 行规则件）。范围收窄的理由有两条，都写在验收口径里：
#   ① 库内另有 164 处与本契约无关的合法 `std::optional` 签名（如 `Button::baseline_distance`
#      返回 `std::optional<float>`——非属性钩子，「没有基线」本就是可空语义），全量扫公共头
#      会把它们一次性判红，等于用门禁逼库改无关接口；
#   ② 本契约只约束「属性面」，而属性面的公共签名恰好就是已迁移类的这批成员。
#   非属性接口本就承载可空语义的，走 `EXEMPTIONS` 逐处登记（`类::函数` 为键，附理由）；
#   登记而未命中即红——白名单失效必须响，不能静默留着。
#
# 四条判据（编号进违例文案，便于变异自证逐条对号）：
#   O1 公共签名（参数或返回值）里出现 `std::optional` 即红。
#   O2 句柄作读面：句柄形状（只读 `ReadOnlyState<...>` / 可写 `State<...>`，与机制层
#      `PropIsHandle` 的两条特化同口径）出现在**返回值**里即红——句柄只许作为换源写件的参数
#      进公共面（`set_label(ReadOnlyState<LocalizedString>)`、`set_value(State<std::string>)`），
#      来源型的读面由框架订阅，不发句柄。
#   O3 双重空态：`ReadOnlyState<std::optional<T>>` / `std::optional<ReadOnlyState<T>>`，
#      以及可写来源的同形嵌套 `State<std::optional<T>>` / `std::optional<State<T>>`，任一即红
#      （单独出文案，因为它是第 2 条点名的现状形态，修法与其他 O1 不同：拆成裸句柄 + 未表态位）。
#   O4 机制类型不外泄：`PropRow<...>` 只允许作为 `*_row()` 的返回值出现；进参数表或进其他
#      成员的返回值即红（表行对象是机制层的中间形态，用户经它选行等于绕过逐属性具名件）。
#
# 解析件取自同目录 `props_scan`，与另两枚属性面门禁同口径。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_no_optional_in_public_signature.py [--root <root>] [--verbose]
# ============================================================================
import argparse
import re
import sys

import props_scan

OPTIONAL_RE = re.compile(r"\bstd::optional\s*<")
# 句柄形状与属性面门禁同源：只读来源 `ReadOnlyState<T>` 与可写来源 `State<T>` 同判。
HANDLE_RE = props_scan.SOURCE_HANDLE_RE
ROW_RE = re.compile(r"\bPropRow\s*<")
# 双重空态的两种嵌套方向（第 2 条点名的现状形态）。
NESTED_RE = re.compile(r"\b(?:ReadOnlyState|State)\s*<\s*(?:std\s*::\s*)?optional\b"
                       r"|\boptional\s*<\s*(?:ReadOnlyState|State)\b")

# 非属性接口的可空豁免登记表：键为 `类名::函数名`，值为理由（必须写明为何本就承载可空语义）。
# 现状为空——已迁移属性面里没有任何合法的可空签名；日后若某枚控件的**非属性**公共件需要
# `std::optional`，它不在本门禁判定面内，无须登记；登记项一旦命中不到即红。
EXEMPTIONS = {}


def check_member(rel, class_name, member, findings, stats):
    """O1..O4：一枚公共成员的签名判据（`*_row()` 只豁免 O2 / O4 的返回值侧）。"""
    if f"{class_name}::{member.name}" in EXEMPTIONS:
        stats["exemption_hits"].add(f"{class_name}::{member.name}")
        return
    ret = member.ret
    params = member.params
    is_row = member.name.endswith("_row")
    if NESTED_RE.search(ret) or NESTED_RE.search(params):
        findings.append(f"{rel}:{member.line}: [O3] {class_name}::{member.name}: doubly-emptied wrapper in a "
                        "public signature -- a handle row is `State<T>` / `ReadOnlyState<T>` bare, emptiness "
                        "lives in the stated bit, not in a nested optional")
        return
    for where, text in (("parameter", params), ("return type", ret)):
        if OPTIONAL_RE.search(text):
            findings.append(f"{rel}:{member.line}: [O1] {class_name}::{member.name}: `std::optional` leaks in "
                            f"the {where} -- public signatures take bare T; optionality is the storage layer's "
                            "business (the stated bit answers `present(row)`)")
    if HANDLE_RE.search(ret) and not is_row:
        findings.append(f"{rel}:{member.line}: [O2] {class_name}::{member.name}: returns a `State` / "
                        "`ReadOnlyState` handle -- handles enter the public face only as a re-source writer's "
                        "argument")
    if ROW_RE.search(ret) and not is_row:
        findings.append(f"{rel}:{member.line}: [O4] {class_name}::{member.name}: returns a `PropRow` outside a "
                        "row accessor -- callers select rows by property name, never by row object")
    if ROW_RE.search(params):
        findings.append(f"{rel}:{member.line}: [O4] {class_name}::{member.name}: takes a `PropRow` argument -- "
                        "row objects stay inside the mechanism layer")
    stats["members"] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    ap.add_argument("--verbose", action="store_true", help="print the per-class census before the verdict")
    args = ap.parse_args()
    root = args.root or props_scan.repo_root_of(__file__)

    findings = []
    stats = {"classes": 0, "members": 0, "exemption_hits": set()}
    for path, rel in props_scan.header_files(root):
        text = props_scan.read_stripped(path)
        for class_name, body, line_offset in props_scan.migrated_props_classes(text):
            stats["classes"] += 1
            for member in props_scan.parse_members(body, line_offset):
                if member.access != "public":
                    continue
                check_member(rel, class_name, member, findings, stats)

    for key in sorted(set(EXEMPTIONS) - stats["exemption_hits"]):
        findings.append(f"[O1] exemption entry `{key}` matches nothing -- the registry is stale, drop it "
                        "or fix the key")

    if args.verbose:
        print(f"[info] {stats['classes']} migrated Props class(es), {stats['members']} public signature(s) "
              f"clean, {len(stats['exemption_hits'])} exemption(s) hit")

    if findings:
        print(f"[FAIL] optionality leaks into the public property signature face ({len(findings)} findings, "
              "criteria O1-O4 of the single-declaration contract):")
        for v in findings:
            print(f"  {v}")
        print("  property writers take bare T or a bare handle; readers return bare T; `std::optional` "
              "stays in the private storage, `PropRow` stays in the row accessors")
        return 1

    if stats["classes"] == 0:
        print("[FAIL] no Props class inherits AbstractProps -- the gate is spinning on an empty set "
              "(a migrated widget must be registered before this check can hold)")
        return 1
    print(f"[PASS] {stats['classes']} migrated Props class(es), {stats['members']} public property "
          f"signature(s) free of `std::optional`, handles and row objects ({len(stats['exemption_hits'])} "
          "exemption(s) in force)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

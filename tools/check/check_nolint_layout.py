#!/usr/bin/env python3
# ============================================================================
# check_nolint_layout.py - clang-tidy NOLINT 指令排版门禁
# ----------------------------------------------------------------------------
# Spec: codespec/CODING_STANDARDS.md §5.2（NOLINT 抑制规则）规则 1、规则 2
#   规则 1：指令与代码之间不得插入任何行——包括折到下一行的理由文字。
#   规则 2：NOLINT 令牌只能出现在真指令里，散文里抄令牌会形成意外豁免。
#
# 为什么需要独立门禁（clang-tidy 自己**不会**报）：
#   NOLINTNEXTLINE 的作用范围严格等于「指令所在物理行 + 1」，它不看语义、不做跨行合并。
#   而 .clang-format（ColumnLimit: 120）会把过长的整行注释在列宽处折断，于是
#       // NOLINTNEXTLINE(check) 一长段理由………………（超过 120 列）
#       // ……被折下来的后半段              ← 指令现在罩住的是这一行注释
#       被豁免的代码                        ← 真正的告警行，无人拦截
#   这条组合让豁免**静默失效**：被命名的 check 处于关闭态时没有任何迹象，一旦开启，
#   告警直接漏出。实测本仓一次成型 234 处（涉及 98 文件），其中 194 处正落在真实告警行上。
#
# Scan points（受版控 C/C++ 源文件：include/ src/ tests/ tools/ examples/）:
#   1) [blocking] NOLINTNEXTLINE 之后紧邻的物理行是注释行或空行 → 豁免落空（规则 1）。
#   2) [blocking] 注释体内 NOLINT 家族令牌不在注释起始位置 → 散文抄令牌，形成对下一行的
#      全量意外豁免（规则 2）。
#   3) [blocking] NOLINTNEXTLINE / NOLINTBEGIN / NOLINTEND 挂在有代码的行尾 → 指令实际
#      作用于下一行，作者意图几乎必然是本行（同属规则 1 的错位形态）。
#
# 不在本门禁范围内的两件事：
#   * 裸 `// NOLINT`（无 check 列表）：§5.2 规则 1 只禁**新增**，仓内既有若干属存量，
#     由评审把关，脚本不追溯。
#   * NOLINTBEGIN/NOLINTEND 未配对：clang-tidy 以 `clang-tidy-nolint` **硬错误**中断该
#     翻译单元，lint 门禁已经拦住，无需重复实现。
#
# Exemption: 行内或上两行含令牌 "LAYOUT_EXEMPT"（不含 NOLINT 子串，避免自触发规则 2）时，
#   该行跳过检查，并须写明为何是合法排版。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_nolint_layout.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import os
import re
import sys

EXEMPT_TOKEN = "LAYOUT_EXEMPT"
SCAN_DIRS = ("include", "src", "tests", "tools", "examples")
SCAN_EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx")

# NOLINT / NOLINTNEXTLINE / NOLINTBEGIN / NOLINTEND，随后是否带 (check 列表)
TOKEN_RE = re.compile(r"NOLINT(?:NEXTLINE|BEGIN|END)?(\s*\([^)]*\))?")
# 指令之前只允许的「注释符号」——`///`、`//`、`*`、空白。越过它出现散文即规则 2 的范畴。
COMMENT_PUNCT_RE = re.compile(r"^[/*\s]*")


def repo_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def is_comment_line(line):
    """整行只有注释（`//` 起头，含 `///`）；块注释内的指令本仓未使用，按代码行处理。"""
    return line.strip().startswith("//")


def comment_punct_end(line):
    """注释符号前缀（`//` / `///` / `*` 与空白）的结束偏移——真指令必须从这里起头。"""
    return COMMENT_PUNCT_RE.match(line).end()


def is_exempt(lines, idx):
    return any(EXEMPT_TOKEN in lines[j].upper() for j in range(max(0, idx - 2), idx + 1))


def check_file(rel, lines, violations, stats):
    for i, line in enumerate(lines):
        toks = list(TOKEN_RE.finditer(line))
        if not toks:
            continue
        stats["directives"] += len(toks)
        if is_exempt(lines, i):
            continue
        comment_only = is_comment_line(line)
        body = comment_punct_end(line) if comment_only else None

        # --- 规则 2：令牌必须在注释正文的起始位置，否则是散文抄令牌 ---
        if comment_only:
            if body is None or toks[0].start() != body:
                violations.append(
                    f"{rel}:{i + 1}: [规则2] 注释散文里抄了 NOLINT 令牌（{toks[0].group(0).strip()}"
                    "），它会对下一物理行形成全量豁免；指代豁免请写「紧邻式/区间式豁免」")
                continue
            if len(toks) > 1:
                violations.append(
                    f"{rel}:{i + 1}: [规则2] 一条注释里出现多个 NOLINT 令牌，第二个起会被解析成"
                    "独立指令；理由文字勿再抄令牌")
                continue
        else:
            # 只有 NOLINTNEXTLINE 挂在代码行尾才是错位：它的作用范围是**下一行**。
            # NOLINTBEGIN / NOLINTEND 挂在行尾（如 `}  // NOLINTEND(...)`）是区间式的规范写法，不报。
            nxtline = [t for t in toks if t.group(0).strip().startswith("NOLINTNEXTLINE")]
            if nxtline:
                violations.append(
                    f"{rel}:{i + 1}: [规则1] {nxtline[0].group(0).strip()} 挂在代码行尾，它作用于**下一行**"
                    "而非本行；本行豁免改用 NOLINT(...)，下一行豁免请把指令独占一行上移")
                continue

        kind = toks[0].group(0).strip()
        if not kind.startswith("NOLINTNEXTLINE"):
            continue
        if not comment_only:
            continue
        # --- 规则 1：NEXTLINE 与代码之间不得插入注释行 / 空行 ---
        if i + 1 >= len(lines):
            violations.append(f"{rel}:{i + 1}: [规则1] NOLINTNEXTLINE 后已无代码行（文件/块尾），豁免落空")
            continue
        nxt = lines[i + 1]
        if nxt.strip() == "":
            violations.append(f"{rel}:{i + 1}: [规则1] NOLINTNEXTLINE 与代码之间夹了空行，豁免落空")
        elif is_comment_line(nxt):
            violations.append(
                f"{rel}:{i + 1}: [规则1] NOLINTNEXTLINE 的下一物理行仍是注释（多为 clang-format 在 120 列"
                "处折断的理由文字），豁免罩住了注释而非代码；把理由整段移到指令**之前**")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    ap.add_argument("--verbose", action="store_true", help="逐条打印后再汇总")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    violations = []
    files_scanned = 0
    stats = {"directives": 0}

    for sd in SCAN_DIRS:
        base = os.path.join(root, sd)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for fn in sorted(filenames):
                if not fn.endswith(SCAN_EXTS):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                files_scanned += 1
                with open(path, encoding="utf-8", errors="replace") as f:
                    lines = f.read().splitlines()
                check_file(rel, lines, violations, stats)

    if violations:
        print("[FAIL] NOLINT 指令排版违规（CODING_STANDARDS.md §5.2 规则 1/2：豁免静默失效）：")
        for v in violations:
            print(f"  {v}")
        print(f"  共 {len(violations)} 处；合法例外须在该行或其上两行写 'LAYOUT_EXEMPT: <原因>'")
        return 1

    print(f"[PASS] {stats['directives']} 条 NOLINT 指令排版全部落在代码行上（扫描 {files_scanned} 文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

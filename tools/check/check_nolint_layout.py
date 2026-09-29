#!/usr/bin/env python3
# ============================================================================
# check_nolint_layout.py - clang-tidy NOLINT 指令排版门禁
# ----------------------------------------------------------------------------
# Spec: codespec/CODING_STANDARDS.md §5.2（NOLINT 抑制规则）编号列表第 1—3 条
#   规则 1：指令与代码之间不得插入任何行——包括折到下一行的理由文字。
#   规则 2：NOLINT 令牌只能出现在真指令里，散文里抄令牌会形成意外豁免。
#   规则 3：指令的 (check 列表) 必须在同一物理行内闭合，跨行即被读成空列表。
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
#   4) [blocking] 指令的 `(check 列表)` 跨物理行书写 → clang-tidy 解析成**空列表**，于是
#      区间/该行豁免从「只列出表中检查」静默放大为「豁免全部检查」。实测（2026-09-23，
#      探针见 §5.2 的复验记录）：同一张表跨行写时连未列出的
#      readability-identifier-naming 也失踪，单行写才只豁免列出的几项；本仓曾因此让 4 个
#      大区间（painter.cpp / painter_simd.inl / system_tray_win32.cpp / win32_ua.cpp，最大
#      跨 2000 行）覆盖的 TU 呈「零告警假象」，改对名单后浮出 795 条存量。
#      修法不许动 .clang-format：CommentPragmas 与 ReflowComments 两条路都实测过，都会把
#      无关文件的注释缩进改掉（分别 5 与 15 个文件非合规），故改为「名单单行 + 通配压缩到
#      120 列内」——通配是**按名匹配**（`*-zzz-*` 形制的负例实测不误豁免），不会退化成空表。
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
# `.inl` 一并扫描：它同样进 clang-tidy（经宿主 TU 展开）与豁免体系，漏扫会留下规则 3 的盲区。
SCAN_EXTS = (".h", ".hpp", ".cpp", ".cc", ".cxx", ".inl")

# NOLINT / NOLINTNEXTLINE / NOLINTBEGIN / NOLINTEND，随后是否带 (check 列表)
TOKEN_RE = re.compile(r"NOLINT(?:NEXTLINE|BEGIN|END)?(\s*\([^)]*\))?")
# 规则 3 用：只定位「指令后紧跟的开括号」，再在同一物理行内找闭括号——找不到即为跨行写法。
OPEN_PAREN_RE = re.compile(r"NOLINT(?:NEXTLINE|BEGIN|END|FIX-MESSAGE|FIX)?\s*\(")
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
        # --- 规则 3：`(check 列表)` 必须在本物理行内闭合 ---
        for m in OPEN_PAREN_RE.finditer(line):
            if ")" not in line[m.end():]:
                violations.append(
                    f"{rel}:{i + 1}: [Rule 3] {m.group(0).strip()} check list is not closed on this "
                    "line (multi-line form); clang-tidy parses it as an empty list, which exempts "
                    "**all** checks; collapse the whole list onto one physical line")
                break
        comment_only = is_comment_line(line)
        body = comment_punct_end(line) if comment_only else None

        # --- 规则 2：令牌必须在注释正文的起始位置，否则是散文抄令牌 ---
        if comment_only:
            if body is None or toks[0].start() != body:
                violations.append(
                    f"{rel}:{i + 1}: [Rule 2] the comment prose copies a NOLINT token "
                    f"({toks[0].group(0).strip()}); it forms a blanket exemption on the next "
                    "physical line; to name an exemption write 'adjacent/range-style exemption'")
                continue
            if len(toks) > 1:
                violations.append(
                    f"{rel}:{i + 1}: [Rule 2] one comment contains multiple NOLINT tokens; from the "
                    "second on they are parsed as independent directives; do not restate tokens in the reason")
                continue
        else:
            # 只有 NOLINTNEXTLINE 挂在代码行尾才是错位：它的作用范围是**下一行**。
            # NOLINTBEGIN / NOLINTEND 挂在行尾（如 `}  // NOLINTEND(...)`）是区间式的规范写法，不报。
            nxtline = [t for t in toks if t.group(0).strip().startswith("NOLINTNEXTLINE")]
            if nxtline:
                violations.append(
                    f"{rel}:{i + 1}: [Rule 1] {nxtline[0].group(0).strip()} hangs at the end of a code "
                    "line; it applies to the **next line**, not this one; to exempt this line use "
                    "NOLINT(...), to exempt the next line keep the directive on its own line above")
                continue

        kind = toks[0].group(0).strip()
        if not kind.startswith("NOLINTNEXTLINE"):
            continue
        if not comment_only:
            continue
        # --- 规则 1：NEXTLINE 与代码之间不得插入注释行 / 空行 ---
        if i + 1 >= len(lines):
            violations.append(
                f"{rel}:{i + 1}: [Rule 1] no code line follows NOLINTNEXTLINE (end of file/block); "
                "the exemption is void")
            continue
        nxt = lines[i + 1]
        if nxt.strip() == "":
            violations.append(
                f"{rel}:{i + 1}: [Rule 1] a blank line sits between NOLINTNEXTLINE and the code; "
                "the exemption is void")
        elif is_comment_line(nxt):
            violations.append(
                f"{rel}:{i + 1}: [Rule 1] the physical line after NOLINTNEXTLINE is still a comment "
                "(often the reason text wrapped at 120 columns by clang-format); the exemption covers "
                "the comment instead of the code; move the whole reason block to **before** the directive")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora repo root (default: two levels above this script)")
    ap.add_argument("--verbose", action="store_true", help="print each finding before the summary")
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
        print("[FAIL] NOLINT directive layout violations (CODING_STANDARDS.md 5.2 rules 1/2: "
              "exemptions silently void):")
        for v in violations:
            print(f"  {v}")
        print(f"  {len(violations)} total; a legitimate exception must write "
              "'LAYOUT_EXEMPT: <reason>' on that line or the two lines above")
        return 1

    print(f"[PASS] all {stats['directives']} NOLINT directives land on code lines (scanned {files_scanned} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# ============================================================================
# check_no_theme_display_literals.py - 令牌显示串「只由表生成」的门禁
# ----------------------------------------------------------------------------
# Spec: 属性单一声明口径的活令牌档：schema 里的 `default_expr` 是带引号的令牌短名（`"theme.primary"`），
#   真源是各组「成员指针 -> 短名」对照表。控件侧手抄这串字面量的历史代价已经付过一次：同一档在不同
#   控件里被抄成裸串与转义串两种拼法，落到 schema 里就是两种 `default_expr`；令牌改名时手抄串不会
#   编译红，只留下一条反查不到落点的陈旧名。现在发射通道齐了（活令牌档 `.fallback<&X::y>()`、派生档
#   `.shows_token<&X::y>()`、未迁控件的 `theme_token_expr<&X::y>()`），控件侧不再需要手抄 —— 本门禁把
#   「不再需要」钉成「不许再有」。
#
# 判据（编号进违例文案，便于变异自证逐条对号）：
#   S1 三种拼法各自计数、各自归零：裸串 `"theme.x"` / 转义串 `"\"theme.x\""` / raw-string
#      `R"("theme.x")"`（带定界符的 `R"delim(...)"` 同档）。分开点名是因为历史漂移只发生在其中一档
#      （转义档是手抄的主流拼法），一把尺子量三种拼法会看不出新拼法的引入。
#   S2 登记表头豁免是**结构性**的：登记了 `AURORA_TOKENS` 的头里，命中必须落在表数组字面量之内
#      （表条目本身就是真源）；同文件别处出现即红。
#   S3 `include/aurora/theming/theme.h` 的豁免同样结构性：命中必须落在 `theme_token_groups()` 体内
#      （那里声明的是五组的前缀，不是逐枚令牌名）。
#   S4 其余任何命中须就地标 `THEME-LITERAL: <理由>`（口径同 `CJK-LITERAL`）；标了却没命中的陈旧
#      标记同样红。
#   S5 登记表自身的名案用例（`NAME_CASE_FILES`）允许裸串字面量——期望值必须由测试给出，经表求值即
#      同义反复；但那里同样不许出现转义 / raw-string 拼法（手抄拼法的分叉面与文件性质无关）。
#
# 注释不计：扫描前经 `props_scan.strip_comments` 去注释保行结构，文档块里的 `theme.*` 散文不误判；
# 标记只认原始行（注释剥离后标记本身会消失）。
#
# 输出必须全 ASCII（ctest 侧代码页约束）。
#
# Exit code: 1 when any violation is found; otherwise 0.
#
# Usage:
#   python3 tools/check/check_no_theme_display_literals.py [--root <aurora_root>] [--verbose]
# ============================================================================
import argparse
import os
import re
import sys

import props_scan

SCANNED_DIRS = ("include", "src", "tools", "tests", "examples")
CODE_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".cxx")
HIT_RE = re.compile(r'"theme\.')
MARKER_RE = re.compile(r"THEME-LITERAL:\s*(\S.*)")
# raw-string 档的开引号形如 `R"(` / `R"delim(`（定界符不许含换行），故命中前缀要吃到行首。
RAW_PREFIX_RE = re.compile(r'R"([^(\n]*)\($')
OPEN_TO_CLOSE = {"(": ")", "{": "}"}

# 登记表的名案用例：断言的**内容**就是「这组这枚成员登记成这个名字」，期望值若也经表求得即同义反复。
# 因此这些文件按名豁免，但只许裸串拼法（S5）。
NAME_CASE_FILES = frozenset({"tests/unit/utest_theme_scale_tokens.cpp"})


def classify(line):
    """这一行里每枚令牌字符串字面量的拼法（按命中开引号之前的形态定档，互斥）。"""
    kinds = []
    for match in HIT_RE.finditer(line):
        index = match.start()
        if RAW_PREFIX_RE.search(line[:index]):
            kinds.append("raw-string")
        elif index >= 1 and line[index - 1] == "\\":
            kinds.append("escaped")
        else:
            kinds.append("bare")
    return kinds


def span_after(text, marker, open_ch):
    """`marker` 之后第一个 `open_ch` 与其配对闭括号之间的 [begin, end)；找不到返回 None。"""
    index = text.find(marker)
    if index < 0:
        return None
    brace = text.find(open_ch, index)
    if brace < 0:
        return None
    inner, after = props_scan.matching(text, brace, open_ch, OPEN_TO_CLOSE[open_ch])
    if inner is None:
        return None
    return brace, after


def line_ranges(text):
    """每行在文本里的 [start, end)（与 `strip_comments` 保行结构的前提相配）。"""
    ranges = []
    start = 0
    for index, char in enumerate(text):
        if char == "\n":
            ranges.append((start, index))
            start = index + 1
    ranges.append((start, len(text)))
    return ranges


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=None)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    root = args.root or props_scan.repo_root_of(__file__)

    problems = []
    counts = {"bare": 0, "escaped": 0, "raw-string": 0}
    exempt = {"registry": 0, "theme_h": 0, "marked": 0, "name_case": 0}
    markers = 0
    scanned_files = 0

    for top in SCANNED_DIRS:
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in ("build", "third_party")]
            for name in sorted(filenames):
                if not name.endswith(CODE_SUFFIXES):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="replace") as handle:
                    raw_lines = handle.read().splitlines()
                text = props_scan.strip_comments("\n".join(raw_lines))
                ranges = line_ranges(text)
                scanned_files += 1
                # 两类结构性豁免按文件性质择一：theme.h 只认 `theme_token_groups()` 函数体（S3），
                # 否则它自身遍历 `Group::AURORA_TOKENS` 的那行会让登记表档（S2）在错的头文件里生效。
                is_theme_h = rel.endswith("aurora/theming/theme.h")
                table_span = None if is_theme_h else (
                    span_after(text, "AURORA_TOKENS", "(") if "AURORA_TOKENS" in text else None)
                groups_span = span_after(text, "auto theme_token_groups()", "{") if is_theme_h else None

                for number, (start, _end) in enumerate(ranges, 1):
                    kinds = classify(text[start:_end])
                    marker = MARKER_RE.search(raw_lines[number - 1]) if number <= len(raw_lines) else None
                    if marker:
                        markers += 1
                    if not kinds:
                        if marker:
                            problems.append("S4 stale marker, no token literal on the line: %s:%d" % (rel, number))
                        continue
                    for kind in kinds:
                        counts[kind] += 1
                    # 违例按所在文件的结构性豁免面归属判据：登记表头落表外即 S2，theme.h 落函数体外即
                    # S3，其余是普通控件侧手抄 S1（编号进文案，变异自证才能逐条对号）。
                    rule = "S1"
                    if table_span:
                        rule = "S2"
                    elif groups_span:
                        rule = "S3"
                    if rel in NAME_CASE_FILES:
                        for kind in kinds:
                            if kind != "bare":
                                problems.append("S5 name-case file carries the '%s' spelling at %s:%d"
                                                % (kind, rel, number))
                        exempt["name_case"] += sum(1 for kind in kinds if kind == "bare")
                        continue
                    if table_span and table_span[0] <= start < table_span[1]:
                        exempt["registry"] += len(kinds)
                        continue
                    if groups_span and groups_span[0] <= start < groups_span[1]:
                        exempt["theme_h"] += len(kinds)
                        continue
                    if marker:
                        exempt["marked"] += len(kinds)
                        continue
                    for kind in kinds:
                        problems.append("%s %s token literal hand-copied at %s:%d -> derive it from the "
                                        "registry (theme_token_expr / .shows_token / .fallback): '%s'"
                                        % (rule, kind, rel, number, text[start:_end].strip()[:88]))

    if problems:
        print("[FAIL] theme token display strings are still hand-copied in source:")
        for item in problems:
            print("  - %s" % item)
        print("  files=%d counts bare=%d escaped=%d raw=%d | exempt registry=%d theme_h=%d marked=%d "
              "name_case=%d"
              % (scanned_files, counts["bare"], counts["escaped"], counts["raw-string"],
                 exempt["registry"], exempt["theme_h"], exempt["marked"], exempt["name_case"]))
        return 1

    print("[OK] no hand-copied theme token literals: files=%d, table names bare=%d escaped=%d "
          "raw-string=%d, covered by registry tables (%d) / theme_token_groups (%d) / markers (%d) / "
          "name-case files (%d)."
          % (scanned_files, counts["bare"], counts["escaped"], counts["raw-string"],
             exempt["registry"], exempt["theme_h"], exempt["marked"], exempt["name_case"]))
    if args.verbose:
        print("  markers in tree: %d" % markers)
    return 0


if __name__ == "__main__":
    sys.exit(main())

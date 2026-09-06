#!/usr/bin/env python3
# ============================================================================
# code <-> doc sync guard (lite)
# ----------------------------------------------------------------------------
# 只做「低误报、确定性可判定」的文档与代码一致性项，签名级比对列为 backlog：
#   R1 代码注释中的 `架构 §N` 引用 → codespec/ARCHITECTURE.md 必须存在该章节；
#   R2 代码注释中的 `规格 §N` 引用 → 需求 #N 必须存在于 SPECIFICATIONS.md 特性表（#1–#24）；
#   R3 tests/unit/utest_*.cpp 与 tests/integration/itest_*.cpp 头部的
#      「目标单元 / 目标组合」声明路径必须真实存在（防注释路径烂掉）。
#
# 首版刻意不强制「每个测试文件必须有头部声明」——存量尚有文件未补，强制会把 28 个
# 既有文件一次性打成红灯，违背「只拦增量」原则；待存量补齐后再收紧。
#
# 白名单：首版内置存量豁免（见 WHITELIST，逐项注明原因），只拦增量。
#
# Usage:
#   python tools/check/check_code_doc_sync.py [repo_root]
# ============================================================================
import os
import re
import sys

# ---- 白名单：存量豁免，逐项注明原因 ----------------------------------------
# 测试头部写「(待补)」表示作者明确标注 TODO，非路径漂移；列为存量豁免，不拦增量。
WHITELIST = {
    ("R3", "tests/unit/utest_aurora_lsp.cpp", "(待补)"): "头部目标单元标注为待补（TODO），非路径漂移",
    ("R3", "tests/unit/utest_default_construct.cpp", "(待补)"): "头部目标单元标注为待补（TODO），非路径漂移",
}

ARCH_REF_RE = re.compile(r"架构\s*§\s*([\d.]+)")
SPEC_REF_RE = re.compile(r"规格\s*§\s*([\d.]+)")
# 只校验「目标单元 / 目标源单元」（路径声明）；「目标组合」是语义描述（如
# "widget + navigation + window 三模块"），不是路径，不参与校验。
TARGET_RE = re.compile(r"目标(?:单元|源单元)[:：]\s*([^\s`]+)")
PLACEHOLDER_RE = re.compile(r"[（(<*]待补|TODO|TBD|xxx", re.IGNORECASE)
HEADING_NUM_RE = re.compile(r"^(#{1,6})\s+(\d+(?:\.\d+)*)[\s、.]")

CODE_DIRS = ("include", "src", "tools")
SOURCE_EXT = (".h", ".hpp", ".cpp")


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def iter_source_files(repo):
    for directory in CODE_DIRS:
        base = os.path.join(repo, directory)
        if not os.path.isdir(base):
            continue
        for current, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ("build", ".git")]
            for name in sorted(files):
                if name.endswith(SOURCE_EXT):
                    yield os.path.relpath(os.path.join(current, name), repo)


def section_numbers(md_path):
    """Numeric section numbers present in a markdown file."""
    if not os.path.isfile(md_path):
        return None
    numbers = set()
    with open(md_path, encoding="utf-8") as handle:
        for line in handle:
            match = HEADING_NUM_RE.match(line)
            if match:
                numbers.add(match.group(2))
    return numbers


def feature_numbers(spec_path):
    """Requirement numbers (#1-#24) listed in the SPECIFICATIONS feature table."""
    if not os.path.isfile(spec_path):
        return None
    numbers = set()
    with open(spec_path, encoding="utf-8") as handle:
        for line in handle:
            match = re.match(r"^\s*\|\s*(\d{1,2})\s*\|", line)
            if match:
                value = int(match.group(1))
                if 1 <= value <= 24:
                    numbers.add(value)
    return numbers


def check_code_comment_refs(repo, problems):
    """R1/R2: `架构 §N` and `规格 §N` references in code comments must resolve."""
    arch_numbers = section_numbers(os.path.join(repo, "codespec", "ARCHITECTURE.md"))
    spec_numbers = feature_numbers(os.path.join(repo, "codespec", "SPECIFICATIONS.md"))

    for rel in iter_source_files(repo):
        with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as handle:
            for lineno, line in enumerate(handle, start=1):
                if not line.lstrip().startswith(("//", "///", "*", "/*")):
                    continue
                for number in ARCH_REF_RE.findall(line):
                    if arch_numbers is not None and number not in arch_numbers:
                        problems.append(("R1", rel, lineno, f"架构 §{number} not found in ARCHITECTURE.md"))
                for raw in SPEC_REF_RE.findall(line):
                    try:
                        number = int(float(raw))
                    except ValueError:
                        continue
                    if spec_numbers is not None and number not in spec_numbers:
                        problems.append(("R2", rel, lineno, f"规格 §{number} is not a known requirement #N"))


def resolve_target(repo, target):
    """Resolve a declared target path: repo-root relative, or aurora-relative (widget/text.h).

    Test headers cite targets both as include/aurora/... and as aurora-relative paths, mirroring
    how the docs cite headers; both must be accepted to avoid false positives.
    """
    candidates = (target,
                  os.path.join("include", "aurora", target),
                  os.path.join("src", "aurora", target),
                  os.path.join("include", target),
                  os.path.join("src", target))
    return any(os.path.exists(os.path.join(repo, c)) for c in candidates)


def check_test_headers(repo, problems):
    """R3: declared target path in test headers must exist."""
    for directory, prefix in (("tests/unit", "utest_"), ("tests/integration", "itest_")):
        base = os.path.join(repo, directory)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if not (name.startswith(prefix) and name.endswith(".cpp")):
                continue
            rel = os.path.join(directory, name).replace("\\", "/")
            with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as handle:
                head = "".join(handle.readlines()[:12])
            match = TARGET_RE.search(head)
            if not match:
                continue  # 首版不强制要求必须有头部声明（存量尚有未补文件）
            target = match.group(1)
            if PLACEHOLDER_RE.search(target):
                problems.append(("R3", rel, 1, target))  # whitelisted unless new
                continue
            if not resolve_target(repo, target):
                problems.append(("R3", rel, 1, target))


def main() -> int:
    repo = sys.argv[1] if len(sys.argv) > 1 else repo_root_of(__file__)
    if not os.path.isdir(os.path.join(repo, "codespec")):
        print(f"[FAIL] not an Aurora repo root: {repo}")
        return 2

    problems = []
    check_code_comment_refs(repo, problems)
    check_test_headers(repo, problems)

    remaining = []
    for rule, rel, lineno, detail in problems:
        rel = rel.replace("\\", "/")
        if (rule, rel, detail) in WHITELIST:
            continue
        remaining.append((rule, rel, lineno, detail))

    if not remaining:
        print("[OK] code-doc sync clean: 0 problems.")
        return 0

    print(f"[FAIL] code-doc sync: {len(remaining)} problem(s):")
    by_rule = {}
    for rule, _rel, _lineno, _detail in remaining:
        by_rule[rule] = by_rule.get(rule, 0) + 1
    for rule in sorted(by_rule):
        print(f"  {rule}: {by_rule[rule]}")
    print("")
    for rule, rel, lineno, detail in remaining[:60]:
        print(f"  {rule} {rel}:{lineno}  {detail}")
    if len(remaining) > 60:
        print(f"  ... and {len(remaining) - 60} more")
    print("\n  Fix the above, or add a justified entry to WHITELIST (存量豁免只用于既有问题).")
    return 1


if __name__ == "__main__":
    sys.exit(main())

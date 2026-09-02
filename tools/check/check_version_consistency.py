#!/usr/bin/env python3
# ============================================================================
# check_version_consistency.py — 版本一致性门禁
# ----------------------------------------------------------------------------
# 校验点：
#   1) [阻断] codespec/CHANGELOG.json 的 currentVersion 必须等于库的真实版本
#      （include/aurora/core/version.h 的 AURORA_VERSION_STRING）。
#      二者长期靠人手对齐，曾出现 CHANGELOG 领先/落后库版本的情况；不一致则
#      发布时搬运到别处的版本号（CLI/README/文档）会基于错误基准。
#   2) [非阻断] CHANGELOG.json 正文（如历史条目里「NNN 个独立可执行测试」等口径描述）
#      若与仓库实际测得的数量不符，仅告警不报错（描述性文字会随重构自然过时，
#      不应阻断 CI；例如早期写「188 个独立可执行测试」，实际 tests/*.cpp 已增至 191）。
#
# 退出码：仅当第 1 项不一致时为 1；否则 0（第 2 项无论是否命中均返回 0）。
#
# 用法：
#   python3 tools/check_version_consistency.py [--root <aurora_root>]
# ============================================================================
import argparse
import glob
import json
import os
import re
import sys


def repo_root_of(path):
    """向上查找含 CMakeLists.txt 的仓库根（脚本位于 tools/check/ 时仍可正确定位）。"""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def read_library_version(include_root):
    """从 version.h 还原 AURORA_VERSION_STRING 的实际值。

    AURORA_VERSION_STRING 本身在 version.h 中是宏拼接
    （AURORA_VERSION_NUMERIC "-" AURORA_VERSION_SUFFIX_STR），非字面串，
    故改为读取 MAJOR/MINOR/PATCH 与可选 SUFFIX 后自行拼装。
    """
    path = os.path.join(include_root, "aurora", "core", "version.h")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        text = f.read()

    def _def(name, cast=int):
        m = re.search(r'#define\s+' + name + r'\s+([^\s]+)', text)
        if not m:
            return None
        try:
            return cast(m.group(1))
        except ValueError:
            return None

    major = _def("AURORA_VERSION_MAJOR")
    minor = _def("AURORA_VERSION_MINOR")
    patch = _def("AURORA_VERSION_PATCH")
    if major is None or minor is None or patch is None:
        return None
    ver = f"{major}.{minor}.{patch}"
    has_suffix = _def("AURORA_HAS_VERSION_SUFFIX")
    if has_suffix:
        ms = re.search(r'#define\s+AURORA_VERSION_SUFFIX_STR\s+"([^"]*)"', text)
        if ms and ms.group(1):
            ver += "-" + ms.group(1)
    return ver


def count_test_sources(root):
    return len(glob.glob(os.path.join(root, "tests", "*.cpp")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora 仓库根目录（默认脚本上级两级）")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)

    include_root = os.path.join(root, "include")
    changelog = os.path.join(root, "CHANGELOG.json")

    # ---- 第 1 项：阻断级版本一致 ----
    lib_ver = read_library_version(include_root)
    if lib_ver is None:
        print(f"[ERR] 无法读取库版本：{include_root}/aurora/core/version.h", file=sys.stderr)
        return 2

    if not os.path.isfile(changelog):
        print(f"[ERR] 找不到 CHANGELOG.json：{changelog}", file=sys.stderr)
        return 2

    with open(changelog, encoding="utf-8") as f:
        changelog_text = f.read()
    try:
        changelog_doc = json.loads(changelog_text)
    except json.JSONDecodeError as e:
        print(f"[ERR] CHANGELOG.json 解析失败：{e}", file=sys.stderr)
        return 2

    current = changelog_doc.get("currentVersion")
    if not current:
        print("[FAIL] CHANGELOG.json 缺少 currentVersion 字段")
        return 1
    if current != lib_ver:
        print(f"[FAIL] 版本不一致：CHANGELOG.currentVersion={current} 但库 AURORA_VERSION_STRING={lib_ver}")
        return 1

    # ---- 第 2 项：非阻断口径告警 ----
    warnings = []
    # 匹配形如「188 个独立可执行测试」的口径描述（数字 + 个独立可执行测试）。
    for m in re.finditer(r"(\d+)\s*个独立可执行测试", changelog_text):
        stated = int(m.group(1))
        actual = count_test_sources(root)
        if stated != actual:
            warnings.append(
                f"CHANGELOG.json 描述「{stated} 个独立可执行测试」，但 tests/*.cpp 实际有 {actual} 个"
            )
    # 如后续文档出现其它「N 个用例/测试」类口径，可在此追加比对规则。

    print(f"[PASS] 版本一致：currentVersion={current} == 库 {lib_ver}")
    if warnings:
        for w in warnings:
            print(f"[WARN] {w}（非阻断：描述性文字随重构自然过时，请择机同步）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

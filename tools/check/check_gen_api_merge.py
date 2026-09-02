#!/usr/bin/env python3
# ============================================================================
# gen_api 合并不截断回归检查
# ----------------------------------------------------------------------------
# 直接调用真实构建产物（gen_error_codes / gen_debug_api），验证：
#   1) 现有 aurora_api.json 损坏时，生成器以非零退出且**不写文件**（不截断）；
#   2) merge-only 场景下，生成器只写自己的段，widgets/enums/error_codes 等其它段保留。
#
# 用法：
#   python tools/check/check_gen_api_merge.py <build_dir> [repo_root]
#   （build_dir 默认 "build"；repo_root 默认脚本所在仓库根）
# ============================================================================
import json
import os
import shutil
import subprocess
import sys
import tempfile


def repo_root_of(path):
    """向上查找含 CMakeLists.txt 的仓库根（脚本位于 tools/check/ 时仍可正确定位）。"""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def run(exe, *args, cwd):
    return subprocess.run([exe, *args], cwd=cwd, capture_output=True, text=True)


def main() -> int:
    repo = sys.argv[2] if len(sys.argv) > 2 else repo_root_of(__file__)
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    build_abs = build if os.path.isabs(build) else os.path.join(repo, build)

    # 跨平台可执行名：Windows 下构建产物带 .exe 后缀，其余平台（Linux/macOS/gcov）没有。
    _suffix = ".exe" if os.name == "nt" else ""
    gen_err = os.path.join(build_abs, "gen_error_codes" + _suffix)
    gen_dbg = os.path.join(build_abs, "gen_debug_api" + _suffix)
    errors_toml = os.path.join(repo, "codespec", "errors.toml")
    debug_toml = os.path.join(repo, "codespec", "debug_api.toml")

    missing = [p for p in (gen_err, gen_dbg) if not os.path.exists(p)]
    if missing:
        print(
            f"[FAIL] 生成器未构建：{missing}\n       先 `cmake --build {build} --target gen_error_codes gen_debug_api`")
        return 2

    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        # ---- 测试 1：gen_error_codes 遇损坏现有文件应拒绝写（不截断） ----
        api1 = os.path.join(tmp, "a1.json")
        # 真正无法解析的内容（结构不完整，nlohmann operator>> 会抛 parse_error）
        with open(api1, "w", encoding="utf-8") as f:
            f.write('{"widgets":[1,2],')  # 未闭合，解析失败
        before = open(api1, "rb").read()
        r1 = run(gen_err, errors_toml, os.path.join(tmp, "x.gen.h"),
                 os.path.join(tmp, "x.md"), api1, cwd=tmp)
        after = open(api1, "rb").read()
        if r1.returncode == 0:
            failures.append("gen_error_codes 遇损坏文件未拒绝（exit=0）")
        if after != before:
            failures.append("gen_error_codes 在拒绝时仍改写了文件（截断风险）")

        # ---- 测试 2：gen_debug_api 遇损坏现有文件应拒绝写（不截断） ----
        api2 = os.path.join(tmp, "a2.json")
        with open(api2, "w", encoding="utf-8") as f:
            f.write('{"widgets":[1,2],')  # 未闭合，解析失败
        before = open(api2, "rb").read()
        r2 = run(gen_dbg, debug_toml, api2, cwd=tmp)
        after = open(api2, "rb").read()
        if r2.returncode == 0:
            failures.append("gen_debug_api 遇损坏文件未拒绝（exit=0）")
        if after != before:
            failures.append("gen_debug_api 在拒绝时仍改写了文件（截断风险）")

        # ---- 测试 3：merge-only 保留其它段 ----
        api3 = os.path.join(tmp, "a3.json")
        with open(api3, "w", encoding="utf-8") as f:
            json.dump({"widgets": [1, 2], "enums": [3], "debug": []}, f)
        r3 = run(gen_dbg, debug_toml, api3, cwd=tmp)
        if r3.returncode != 0:
            failures.append(f"gen_debug_api merge 失败：{r3.stderr.strip()}")
        else:
            doc = json.load(open(api3, encoding="utf-8"))
            if doc.get("widgets") != [1, 2] or doc.get("enums") != [3]:
                failures.append("gen_debug_api merge 丢失了 widgets/enums 段")
            if not isinstance(doc.get("debug"), list) or len(doc["debug"]) == 0:
                failures.append("gen_debug_api merge 未写入 debug 段")

    if failures:
        print("[FAIL] gen_api 合并回归检查未通过：")
        for f in failures:
            print("  - " + f)
        return 1
    print("[PASS] gen_api 合并不截断回归检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Parallel clang-tidy runner for the Aurora project.

Why this exists instead of LLVM's `run-clang-tidy`:
  * it is available everywhere clang-tidy is (no dependence on the LLVM python
    helper being installed / on PATH),
  * it excludes third_party/ translation units,
  * it **deduplicates** findings: clang-tidy re-reports a header diagnostic in
    every translation unit that includes it, so a raw warning count massively
    overstates the work and makes CI numbers non-comparable across runs,
  * it emits a stable summary (by check, by file) suitable for CI logs.

Usage:
    run_clang_tidy.py --build-dir build-lint [--jobs N] [--fix] [--checks ...]
    run_clang_tidy.py --build-dir build-wasm --emscripten   # 浏览器编译库（见下）

Why `--emscripten`:
  门禁的覆盖面 = 该编译库里的 TU 与被这些 TU 编译到的代码路径。Emscripten 专属 TU
  （`tools/verify/wasm_*`）与 `#ifdef AURORA_BACKEND_WASM` / `AURORA_ENABLE_AUDIO_WEBAUDIO`
  包住的分支在 native 编译库里根本不参与分析——同一文件在两种配置下的告警面并不相同。
  但 em++ 是驱动包装器，clang-tidy 无法直接吃它生成的编译库：一是 PCH 由 emsdk 自带
  clang 生成，版本稍差即判 "invalid or out-of-date precompiled header"；二是
  `__EMSCRIPTEN__` 与 `include/compat` 之类的垫片路径由驱动内部注入。故此处把编译库
  重写成 native clang-tidy 能消费的形态（换三元组、指 sysroot、弃 PCH、补驱动宏），
  写进 <build-dir>/tidy_emscripten/ 再走原有并行/去重/汇总流程。

Exit codes:
    0  no findings, and every selected TU compiled (or --fix mode completed)
    1  findings present with the configured severity, or some TU failed to compile / timed out
       (a TU that fails to compile emits **no** `[check]` diagnostics, so "0 findings" would be
       a coverage collapse rather than a clean bill — hence it is a hard failure, not a pass)
    2  usage / environment error (no compile database, clang-tidy missing, broken rewrite)

Measuring a check that `.clang-tidy` currently excludes (取数用，不是门禁跑法):
    复制 .clang-tidy、删掉对应的 `  -<check>,` 一行，再 `--config <副本>` 全量跑。读结果前记两点：
    ① 输出的条数是**净新增**——已写 `NOLINT(<check>)` 的点位被 clang-tidy 自行消化、不进 JSON，
    故「开启该 check 的总成本 = 净新增 + 存量抑制数」，只看 JSON 会低估；
    ② `--config` 走 `--config-file=`，**整体替换**仓库配置（`CheckOptions` / `HeaderFilterRegex`
    必须原样带上），否则量出来的是另一套口径。副本放构建目录即可，勿往 tools/check/ 堆一次性脚本。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

# <path>:<line>:<col>: warning: <msg> [<check>]
DIAG = re.compile(r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+(.*?)\s+\[(.+)\]\s*$")

# 编译器前端诊断（不含带 [check] 的 tidy 诊断）：带路径与不带路径（如
# "fatal error: too many errors emitted"、"unable to handle compilation"）两类都要抓。
FRONT_ERROR = re.compile(r"^(?:(.*?):(\d+):\d+:\s+)?(?:fatal\s+)?error:\s+(.*)$")

DEFAULT_EXCLUDE = re.compile(r"(^|/)third_party/")

# ---- Emscripten 编译库改写（--emscripten）------------------------------------
# 三元组与驱动注入项：em++ 会在真实 argv 里补 `__EMSCRIPTEN__` 与垫片头目录
# （`include/compat` 放 xlocale.h 之类），clang 只按三元组给 `__wasm32__` 等宏，
# 少了它们 libc++ 会走错分支（locale_base_api.h 直接报 'xlocale.h' file not found）。
EM_TRIPLE = "wasm32-unknown-emscripten"
EM_DROP_WITH_ARG = {"-include-pch", "-o", "-Xclang", "-c"}
EM_DROP_BARE = {"-Winvalid-pch", "-fpch-instantiate-templates", "-fcolor-diagnostics",
                "-fansi-escape-codes"}
# 值为目录的选项：重写后要校验目录存在（见 validate_entry）。路径写错的后果不是报错，
# 而是「该 TU 编译不过」——编译不产出带 [check] 的诊断，门禁会静默显示 0 告警。
# 值为文件的选项（-imacros/-include）可能是相对名，由 clang 在 include 路径里搜，故不校验。
EM_DIR_OPT_PREFIXES = ("-I", "-isystem", "-iquote", "-idirafter")


def argv_from_command(cmd: str) -> list[str]:
    """把编译库里的 `command` 字符串还原成 argv 列表。

    为什么不用 `shlex.split(posix=True)`：Windows 下 Ninja 写出的 command 走的是
    `CommandLineToArgvW` 那套转义规则（`\"` 是转义引号，**其余反斜杠一律字面量**），
    而 posix 模式把所有反斜杠都当转义符吃掉——实测盘符绝对路径被拆成「盘符与目录粘连、
    分隔符丢失」的废路径，路径全部失效。反斜杠路径失效后 clang-tidy 编译不过，
    而「编译不过」不产出带 `[check]` 的诊断，于是门禁只看到 0 条告警：覆盖面塌了
    却报绿。posix=False 又走另一极端（转义引号原样留下，宏值带反斜杠）。故自实现。
    """
    toks: list[str] = []
    cur, i, n = [], 0, len(cmd)
    in_quote = False
    has = False  # 是否曾开始构造当前 token（区分「空参数」与「分隔空白」）
    while i < n:
        c = cmd[i]
        if c == "\\":  # 反斜杠只在紧邻引号时才有转义含义
            j = i
            while j < n and cmd[j] == "\\":
                j += 1
            if j < n and cmd[j] == '"':
                cur.extend(["\\"] * ((j - i) // 2))
                if (j - i) % 2:  # 奇数个：最后一个转义引号，字面量并入
                    cur.append('"')
                    has = True
                else:  # 偶数个：引号本身是开/合
                    in_quote = not in_quote
                    has = True
                i = j + 1
                continue
            cur.extend(["\\"] * (j - i))
            has = True
            i = j
            continue
        if c == '"':
            in_quote = not in_quote
            has = True
            i += 1
            continue
        if c.isspace() and not in_quote:
            if has:
                toks.append("".join(cur))
                cur, has = [], False
            i += 1
            continue
        cur.append(c)
        has = True
        i += 1
    if has:
        toks.append("".join(cur))
    return toks


def emscripten_sysroot() -> tuple[str, str]:
    """由 PATH 上的 em++ 反推 Emscripten 根目录与 sysroot。

    不写死本机路径（`check_no_hardcoded_paths` 门禁也不允许）：取不到即报错退出，
    让「没装 emsdk」成为一种可辨识的环境缺失，而不是静默少测一轮。
    """
    em = which("em++") or which("em++.exe")
    if em is None:
        raise RuntimeError("em++ not found on PATH (source emsdk/activate first)")
    root = norm(os.path.dirname(os.path.realpath(em)))
    sysroot = f"{root}/cache/sysroot"
    if not os.path.isdir(sysroot):
        raise RuntimeError(f"emscripten sysroot missing: {sysroot}")
    return root, sysroot


def validate_entry(entry: dict) -> list[str]:
    """校验重写后的 argv：返回问题描述列表（空即通过）。

    只查「会让 TU 编译不过」的硬伤，不做风格审查：源文件必须在，且绝对路径形态的
    include 目录必须存在。相对形态的 include 项由 clang 自行搜索，不在此校验。
    """
    problems: list[str] = []
    if not os.path.isfile(entry["file"]):
        problems.append(f"source missing: {entry['file']}")
    args = entry["arguments"]
    for tok in args:
        path = None
        for pfx in EM_DIR_OPT_PREFIXES:
            if tok.startswith(pfx) and len(tok) > len(pfx):
                path = tok[len(pfx):]
        if path and os.path.isabs(path) and not os.path.isdir(path):
            problems.append(f"include dir missing: {path}")
    return problems


def rewrite_emscripten_db(build_dir: str) -> str:
    """把 <build_dir>/compile_commands.json 重写为 native clang-tidy 可消费的编译库。

    返回新编译库所在目录（供 -p 使用）。PCH 生成 TU 整条丢弃——它不是被测对象，
    且其产物由 emsdk 自带 clang 生成，本机 clang-tidy 版本稍有差异即拒读。

    条数守恒 + argv 存在性校验都在此处完成：本函数一旦把某条 TU 静默丢掉或写坏，
    该 TU 的告警就永久缺席，而门禁仍显示绿灯，故把「塌覆盖」升格为环境错误（exit 2）。
    """
    _root, sysroot = emscripten_sysroot()
    inc = f"{sysroot}/include"
    head = ["clang++", f"--target={EM_TRIPLE}", f"--sysroot={sysroot}", "-D__EMSCRIPTEN__=1"]
    # 垫片/标准库目录按 sysroot 实存形态取用：`include/<三元组>` 只存在于部分 emsdk
    # 布局（本机 sysroot 便无此目录），写死会让 validate_entry 判为「include 目录缺失」
    # 而拒跑——宁缺毋滥：缺一个目录至多少几条告警，写错一个目录则整轮 TU 编译不过。
    for cand in (f"{inc}/c++/v1", f"{inc}/{EM_TRIPLE}", f"{inc}/compat", inc):
        if os.path.isdir(cand):
            head.append(f"-isystem{cand}")
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as fh:
        db = json.load(fh)
    out, dropped = [], 0
    for e in db:
        src = e.get("file", "")
        if not os.path.isabs(src):
            src = os.path.join(e.get("directory", ""), src)
        src = norm(src)
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        if "cmake_pch" in src or "-emit-pch" in cmd:
            dropped += 1
            continue
        toks = argv_from_command(cmd)
        args, i = list(head), 1  # [0] 是 em++ 驱动路径，clang-tidy 用自己
        while i < len(toks):
            t = toks[i]
            if t in EM_DROP_WITH_ARG:
                i += 2
                continue
            if t in EM_DROP_BARE:
                i += 1
                continue
            if norm(t) == src:  # 原命令里的源文件位置参数：末尾统一补规范化的 src
                i += 1
                continue
            args.append(t)
            i += 1
        args.append(src)
        out.append({"directory": e.get("directory", ""), "file": src, "arguments": args})
    if dropped + len(out) != len(db):
        raise RuntimeError(f"entry count not conserved: {len(db)} != {dropped} + {len(out)}")
    if not out:
        raise RuntimeError("rewrite produced 0 translation units")
    bad: list[str] = []
    for e in out:
        probs = validate_entry(e)
        if probs:
            bad.append(f"{e['file']}: " + "; ".join(probs))
    if bad:
        raise RuntimeError("rewritten db looks broken (would silently lose coverage):\n    "
                           + "\n    ".join(bad[:10])
                           + (f"\n    ... {len(bad)} entries affected" if len(bad) > 10 else ""))
    target = os.path.join(build_dir, "tidy_emscripten")
    os.makedirs(target, exist_ok=True)
    with open(os.path.join(target, "compile_commands.json"), "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    print(f"[lint] emscripten db: {len(out)} TUs ({dropped} PCH entries dropped)", flush=True)
    return target



def norm(p: str) -> str:
    return os.path.normpath(p).replace("\\", "/")


def which(name: str) -> str:
    from shutil import which as _w

    return _w(name)


def load_tus(compile_db: str, include: re.Pattern,
             exclude: re.Pattern) -> list[str]:
    with open(compile_db, encoding="utf-8") as fh:
        entries = json.load(fh)
    out, seen = [], set()
    for e in entries:
        # 'file' is relative in newer CMake DBs; 'directory' holds the base.
        f = e.get("file", "")
        if not os.path.isabs(f):
            f = os.path.join(e.get("directory", ""), f)
        f = norm(f)
        if f in seen:
            continue
        seen.add(f)
        if exclude.search(f):
            continue
        if is_auto_generated(f):
            continue
        if include and not include.search(f):
            continue
        out.append(f)
    return sorted(out)


def is_auto_generated(path: str) -> bool:
    """判断源文件是否为自动生成（首部若干行标注 AUTO-GENERATED）。

    生成物（如字模字节表）不应纳入 lint：改动会被下次重新生成覆盖，
    且其 C 数组/指针形态由生成器决定，人工抑制毫无意义。
    """
    try:
        with open(path, encoding="utf-8", errors="ignore") as fh:
            for _ in range(10):
                if "AUTO-GENERATED" in fh.readline():
                    return True
    except OSError:
        pass
    return False


def run_one(job: tuple[str, str | None, str, bool, str | None]) -> tuple[str, str, float]:
    tu, checks, build_dir, do_fix, config = job
    cmd: list[str] = [which("clang-tidy") or "clang-tidy", "-p", build_dir, "--quiet"]
    if checks:
        cmd.append(f"--checks={checks}")
    if config:
        cmd.append(f"--config-file={config}")
    if do_fix:
        cmd.append("--fix")
    cmd.append(tu)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=1800)
        return tu, (r.stdout or "") + (r.stderr or ""), time.time() - t0
    except subprocess.TimeoutExpired:
        return tu, f"__TIMEOUT__ {tu}\n", time.time() - t0
    except OSError as exc:
        return tu, f"__ERROR__ {tu}: {exc}\n", time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description="Parallel clang-tidy runner for Aurora.")
    ap.add_argument("--build-dir", required=True,
                    help="CMake build directory containing compile_commands.json")
    ap.add_argument("--emscripten", action="store_true",
                    help="build-dir 是 Emscripten 构建目录：先把其编译库重写成 native "
                         "clang-tidy 可消费的形态（wasm 三元组 + sysroot，弃 PCH），"
                         "使浏览器专属 TU 与 #ifdef AURORA_BACKEND_WASM 分支进入覆盖面")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel clang-tidy processes (0 = CPU count)")
    ap.add_argument("--checks", default=None,
                    help="override the Checks: list from .clang-tidy")
    ap.add_argument("--config", default=None,
                    help="use an alternate .clang-tidy file")
    ap.add_argument("--include", default=None,
                    help="regex; only lint TUs whose path matches")
    ap.add_argument("--exclude", default=DEFAULT_EXCLUDE.pattern,
                    help="regex; skip TUs whose path matches (default: third_party)")
    ap.add_argument("--fix", action="store_true",
                    help="apply clang-tidy fix-its in place (does not fail the run)")
    ap.add_argument("--fail-on", choices=["warning", "error"], default="warning",
                    help="minimum severity that counts as a failure")
    ap.add_argument("--json-out", default=None, help="write findings to this JSON file")
    ap.add_argument("--show", type=int, default=20,
                    help="how many top files to print")
    a = ap.parse_args()

    compile_db = os.path.join(a.build_dir, "compile_commands.json")
    if not os.path.isfile(compile_db):
        print(f"error: no compile_commands.json in {a.build_dir!r}.\n"
              f"Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON.", file=sys.stderr)
        return 2
    if which("clang-tidy") is None:
        print("error: clang-tidy not found on PATH.", file=sys.stderr)
        return 2

    mode = ""
    if a.emscripten:
        # 先重写编译库，再走完全相同的并行 / 去重 / 汇总流程——两种配置的口径必须可比。
        try:
            a.build_dir = rewrite_emscripten_db(a.build_dir)
        except RuntimeError as exc:
            print(f"error: --emscripten: {exc}", file=sys.stderr)
            return 2
        compile_db = os.path.join(a.build_dir, "compile_commands.json")
        mode = " [emscripten db]"

    include = re.compile(a.include) if a.include else None
    exclude = re.compile(a.exclude) if a.exclude else None
    tus = load_tus(compile_db, include, exclude) if exclude else load_tus(compile_db, include, re.compile(r"(?!x)x"))
    if not tus:
        print("error: no translation units selected.", file=sys.stderr)
        return 2

    jobs = a.jobs or (os.cpu_count() or 4)
    # 门禁只对本仓库源码负责。`.clang-tidy` 的 HeaderFilterRegex 能滤掉绝大多数头文件告警，
    # 但 clang-analyzer 的 optin 检查（如 core.EnumCastOutOfRange）会对系统头报路径敏感诊断而
    # 绕过该过滤——观测到 MSVC STL 的 `xfilesystem_abi.h` 即属此类。系统头既不可修、又会随
    # 工具链升级漂移，故在计数前按「是否位于仓库根之下」再过滤一次。
    repo_root = norm(os.path.abspath(".")) + "/"
    print(f"[lint] clang-tidy over {len(tus)} translation units, {jobs} parallel{mode}")

    findings: dict[tuple[str, str, str], str] = {}
    by_area: Counter[str] = Counter()
    timeouts: list[str] = []
    # 编译失败（前端 error，无 [check] 标签）单独记账：这类 TU 一条告警都不会产出，
    # 「0 findings」于是既可能是真干净、也可能是覆盖面塌了。两者必须可区分。
    broken: dict[str, str] = {}
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        jobs_args = [(t, a.checks, a.build_dir, a.fix, a.config) for t in tus]
        for i, (tu, out, _dt) in enumerate(ex.map(run_one, jobs_args), 1):
            if out.startswith("__TIMEOUT__") or out.startswith("__ERROR__"):
                timeouts.append(out.strip())
            for line in out.splitlines():
                m = DIAG.match(line.strip())
                if not m:
                    e = FRONT_ERROR.match(line.strip())
                    if e and tu not in broken:
                        loc = f"{norm(e.group(1))}:{e.group(2)}: " if e.group(1) else ""
                        broken[tu] = f"{loc}{e.group(3)}"
                    continue
                path, line_no, _col, sev, msg, check = m.groups()
                np_ = norm(path)
                if exclude and exclude.search(np_):
                    continue
                abs_p = np_ if os.path.isabs(np_) else norm(os.path.join(os.getcwd(), np_))
                if not abs_p.lower().startswith(repo_root.lower()):
                    continue  # 仓库外（系统头 / 工具链头）诊断不计入门禁
                # keyed by (file, line, check): a header diagnostic is re-emitted in
                # every including TU, so later occurrences just refresh the message.
                findings[(np_, line_no, check)] = (sev, msg)
                try:
                    rel = os.path.relpath(np_).replace("\\", "/")
                except ValueError:
                    # 诊断路径在另一盘符（如系统头在 C:，仓库在 D:），无法求相对路径
                    rel = np_
                by_area[rel.split("/")[0] if "/" in rel else rel] += 1
            if i % 25 == 0 or i == len(tus):
                print(f"  ... {i}/{len(tus)}  ({time.time() - t0:.0f}s)", flush=True)

    by_check = Counter(c for (_f, _l, c) in findings)
    by_file = Counter(f for (f, _l, _c) in findings)
    sev_rank = {"warning": 0, "error": 1}
    threshold = sev_rank[a.fail_on]
    blocking = sum(1 for s, _m in findings.values() if sev_rank.get(s, 0) >= threshold)

    print(f"\n[lint] elapsed {time.time() - t0:.0f}s")
    print(f"[lint] unique findings: {len(findings)}   blocking(>={a.fail_on}): {blocking}")
    if broken:
        print(f"\n[lint] FATAL: {len(broken)}/{len(tus)} translation units failed to compile "
              f"— 它们的告警不在统计内，本轮覆盖面不完整（勿当作「已清零」）")
        for tu_, msg in list(broken.items())[:10]:
            print(f"    {norm(tu_)}: {msg}")
        if len(broken) > 10:
            print(f"    ... 另有 {len(broken) - 10} 个 TU")
    if timeouts:
        print(f"[lint] problems: {len(timeouts)}")
        for t in timeouts[:10]:
            print("   ", t)

    print("\n=== by area ===")
    for k, v in by_area.most_common():
        print(f"  {k:12} {v}")
    print("\n=== by check ===")
    for k, v in by_check.most_common():
        print(f"  {v:6}  {k}")
    print(f"\n=== top {a.show} files ===")
    for k, v in by_file.most_common(a.show):
        print(f"  {v:6}  {k}")

    if a.json_out:
        payload = {
            "tu_count": len(tus),
            "unique_findings": len(findings),
            "broken_tus": sorted([[f, m] for f, m in broken.items()]),
            "by_check": by_check.most_common(),
            "by_file": by_file.most_common(),
            "findings": sorted([list(k) + [v[0], v[1]] for k, v in findings.items()]),
        }
        with open(a.json_out, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=1)
        print(f"\n[saved] {a.json_out}")

    if a.fix:
        print("\n[lint] --fix applied; not failing the build (review the diff).")
        return 0
    # 编译失败的 TU 即便本轮 0 告警也必须红灯：绿灯的含义是「这些 TU 干净」，不是「没跑到」。
    if broken or timeouts:
        return 1
    return 1 if blocking else 0


if __name__ == "__main__":
    sys.exit(main())

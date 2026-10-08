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

Why `--shard=i/n` (分片跑法，CI 门禁用它):
  单遍 clang-tidy 在 4 核 CI runner 上实测 498 TU / 5172s（86 分钟），而 native 门禁要跑
  DEBUG=OFF 与 DEBUG=ON 两遍——串行就是 3 小时量级，runner 侧再快也快不过核数。分片把同一份
  已排序 TU 清单按 `tus[i::n]` 切成 n 份、每份一个 job 并跑，墙钟随 n 近线性下降而**总核时不变**
  （门禁买的是覆盖面，不是把活儿砍掉）。
  ⚠️ 切分必须在 `sorted()` 之后做，且分片号只进 JSON/日志、不参与任何筛选语义：清单一旦随
  新增文件移位，「这轮少了哪几条」就无从比对。两两不相交的是 **TU 清单**，不是告警清单——头文件
  告警会在每个包含它的 TU 里重复上报，故同一条可能出现在多个分片的 `lint-findings.json` 里；
  聚合时并联各片 `findings` 后须再按 (文件, 行, check) 去重一次，片内计数也绝不能当全量读。
  判「全绿」必须 n 个分片全绿，任一片红即整门红——缺片等于缺覆盖面。
  `--print-tus` 只打印本片 TU 清单便退出，用于离线核验上面那条前提（各片两两不相交、并集 = 全量）。

Measuring a check that `.clang-tidy` currently excludes (取数用，不是门禁跑法):
    复制 .clang-tidy、删掉对应的 `  -<check>,` 一行，再 `--config <副本>` 全量跑。读结果前记两点：
    ① 输出的条数是**净新增**——已写 `NOLINT(<check>)` 的点位被 clang-tidy 自行消化、不进 JSON，
    故「开启该 check 的总成本 = 净新增 + 存量抑制数」，只看 JSON 会低估；
    ② `--config` 走 `--config-file=`，**整体替换**仓库配置（`CheckOptions` / `HeaderFilterRegex`
    必须原样带上），否则量出来的是另一套口径。副本放构建目录即可，勿往 tools/check/ 堆一次性脚本。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

# 编译库读取 / argv 还原 / 取依赖的底层件与 `select_lint_tus.py` 共用同一份实现：
# 两处若各写一套，选集判定的「受影响 TU」与缓存指纹覆盖的 TU 会分叉，而分叉的症状是
# 门禁少跑 TU 却仍报绿。本脚本以「脚本自身所在目录」入路径，供 CMake 目标与 CI 直接调用。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lint_db import (  # noqa: E402  (path bootstrap above must run first)
    DEFAULT_EXCLUDE,
    argv_from_command,
    is_auto_generated,
    load_db,
    load_tus,
    norm,
    which,
)

# <path>:<line>:<col>: warning: <msg> [<check>]
DIAG = re.compile(r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+(.*?)\s+\[(.+)\]\s*$")

# 编译器前端诊断（不含带 [check] 的 tidy 诊断）：带路径与不带路径（如
# "fatal error: too many errors emitted"、"unable to handle compilation"）两类都要抓。
FRONT_ERROR = re.compile(r"^(?:(.*?):(\d+):\d+:\s+)?(?:fatal\s+)?error:\s+(.*)$")

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



def parse_shard(spec: str) -> tuple[int, int]:
    """把 `--shard` 的 `i/n` 解析成 (片号, 片数)，非法即 argparse 报错退出。

    参数写错的后果是「某片 TU 谁都没跑」，而缺片在门禁日志里长得和「都跑过且干净」一样，
    故此处宁可在解析阶段硬失败，也不允许 i>=n 或 n<1 静默通过。
    """
    def fail(msg: str) -> None:
        raise argparse.ArgumentTypeError(f"--shard expects '<index>/<total>': {msg}")

    parts = spec.split("/")
    if len(parts) != 2:
        fail(f"got {spec!r}")
    try:
        idx, total = (int(p) for p in parts)
    except ValueError:
        fail(f"non-integer in {spec!r}")
    if total < 1:
        fail(f"total must be >= 1, got {total}")
    if not 0 <= idx < total:
        fail(f"index must be in [0, {total}), got {idx}")
    return idx, total


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


def _sha256_file(path: str) -> str:
    """文件内容指纹；读不到（被删 / 无权限）一律记为 `missing` 并参与指纹。

    刻意不返回 None：缺文件也是「这个 TU 此刻的形态」的一部分，把它排除在指纹之外，
    等于让「改了却读不到」的 TU 命中上一次的缓存。
    """
    try:
        with open(path, "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()
    except OSError:
        return "missing"


class ResultCache:
    """Per-TU clang-tidy result cache: key = content fingerprint, value = raw output.

    指纹覆盖六项：clang-tidy 版本、编译器版本、`.clang-tidy` 内容、该 TU 的编译 argv、
    TU 自身内容、以及该 TU 的依赖（由 `--deps` 给出，选集脚本产出）逐文件内容。
    缺任何一项都不缓存——指纹一旦退化成「只看 TU 自身」，改了它包含的头文件却命中
    上一次结果，就是本缓存唯一能造出来的假绿。

    ⚠️ 第二条硬边界：**编译失败与超时的 TU 绝不入缓存**。这类 TU 一条带 `[check]` 的
    诊断都不产出，「0 告警」于是既可能是真干净、也可能是覆盖面塌了；把这种结果写进
    缓存，等于把一次偶发塌方固化成永久绿灯。故存储动作排在完成解析、确认该 TU 既未
    broken 也未超时之后（见 `main()`）。
    """

    def __init__(self, root: str, deps: dict[str, list[str]], db_argv: dict[str, list[str]],
                 tidy_version: str, config_text: str) -> None:
        self.root = root
        self.deps = deps
        self.db_argv = db_argv
        self.tidy_version = tidy_version
        self.config_text = config_text
        self.hits = 0
        self.misses = 0
        self.stored = 0
        self._compiler_versions: dict[str, str] = {}

    def _compiler_version(self, compiler: str) -> str:
        if compiler not in self._compiler_versions:
            try:
                r = subprocess.run([compiler, "--version"], capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=60)
                self._compiler_versions[compiler] = (r.stdout or "")[:400]
            except (OSError, subprocess.TimeoutExpired):
                self._compiler_versions[compiler] = "unknown"
        return self._compiler_versions[compiler]

    def key_for(self, tu: str) -> str | None:
        """返回该 TU 的指纹；依赖清单里没有它时返回 None（调用方按「不可缓存」处理）。"""
        deps = self.deps.get(tu)
        if deps is None:
            return None
        argv = self.db_argv.get(tu, [])
        h = hashlib.sha256()
        for part in (self.tidy_version, self.config_text, "\x00".join(argv)):
            h.update(part.encode("utf-8", "replace"))
            h.update(b"\x00")
        h.update(self._compiler_version(argv[0] if argv else "").encode("utf-8", "replace"))
        h.update(_sha256_file(tu).encode("ascii"))
        for dep in sorted(deps):
            h.update(dep.encode("utf-8", "replace"))
            h.update(_sha256_file(dep).encode("ascii"))
        return h.hexdigest()

    def _path(self, key: str) -> str:
        return os.path.join(self.root, key[:2], key + ".json")

    def load(self, tu: str) -> str | None:
        """命中返回缓存的原始输出，未命中（或不可缓存）返回 None。"""
        key = self.key_for(tu)
        if key is None:
            return None
        try:
            with open(self._path(key), encoding="utf-8") as fh:
                payload = json.load(fh)
        except (OSError, ValueError):
            return None
        if payload.get("tu") != tu:
            return None
        self.hits += 1
        return payload.get("out", "")

    def store(self, tu: str, out: str) -> None:
        key = self.key_for(tu)
        if key is None:
            return
        path = self._path(key)
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump({"tu": tu, "key": key, "out": out}, fh)
            os.replace(tmp, path)  # 原子落盘：并发分片不会读到写了一半的条目
            self.stored += 1
        except OSError as exc:
            print(f"[lint] cache: could not store {tu}: {exc}", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Parallel clang-tidy runner for Aurora.")
    ap.add_argument("--build-dir", required=True,
                    help="CMake build directory containing compile_commands.json")
    ap.add_argument("--emscripten", action="store_true",
                    help="the build-dir is an Emscripten build directory: first rewrite its compile "
                         "database into a form native clang-tidy can consume (wasm triple + sysroot, drop PCH), "
                         "so browser-only TUs and #ifdef AURORA_BACKEND_WASM branches enter the coverage")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel clang-tidy processes (0 = CPU count)")
    ap.add_argument("--shard", type=parse_shard, default=None, metavar="I/N",
                    help="run only shard I of N: take [I::N] of the sorted TU list. "
                         "The CI gate uses it to spread one pass's full TU set across jobs, "
                         "shortening wall-clock without cutting coverage")
    ap.add_argument("--print-tus", action="store_true",
                    help="print the TUs selected for this shard, then exit 0 "
                         "(verify shard disjointness and union completeness; does not run tidy)")
    ap.add_argument("--checks", default=None,
                    help="override the Checks: list from .clang-tidy")
    ap.add_argument("--config", default=None,
                    help="use an alternate .clang-tidy file")
    ap.add_argument("--include", default=None,
                    help="regex; only lint TUs whose path matches")
    ap.add_argument("--tu-list", default=None, metavar="FILE",
                    help="newline-separated TU paths to lint (exact match, not a regex). "
                         "Produced by tools/check/select_lint_tus.py; every entry must exist in "
                         "the compile database, otherwise the run aborts (exit 2) instead of "
                         "silently analysing a smaller set")
    ap.add_argument("--deps", default=None, metavar="FILE",
                    help="JSON map TU -> dependency paths, used only for cache fingerprints")
    ap.add_argument("--cache-dir", default=None, metavar="DIR",
                    help="reuse per-TU results across runs: a TU whose fingerprint "
                         "(tidy/compiler version, config, argv, own content, dependencies) is "
                         "unchanged replays its previous output instead of being re-analysed. "
                         "Requires --deps; TUs missing from it are never cached")
    ap.add_argument("--exclude", default=DEFAULT_EXCLUDE.pattern,
                    help="regex; skip TUs whose path matches (default: see DEFAULT_EXCLUDE)")
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
    if which("clang-tidy") is None and not a.print_tus:
        # `--print-tus` 只求清单，不动 tidy：核验分片切分时机器上未必装了工具链，
        # 而这类核验恰恰需要在门禁同源的编译库上做。
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
    if a.tu_list:
        # 显式清单（选集脚本的产物）**逐项校验**必须落在本编译库里：清单路径与库内条目对不上时
        # 最常见的成因是「换了构建目录还拿着上一轮的清单」，而它的症状是 clang-tidy 静默少跑
        # 一批 TU 却给出绿灯。宁可当场拒跑。
        try:
            with open(a.tu_list, encoding="utf-8") as fh:
                listed = [ln.strip() for ln in fh if ln.strip()]
        except OSError as exc:
            print(f"error: cannot read --tu-list {a.tu_list!r}: {exc}", file=sys.stderr)
            return 2
        unknown = [t for t in listed if t not in set(tus)]
        if unknown:
            print(f"error: --tu-list has {len(unknown)} entries absent from this compile database "
                  f"(stale list for another build dir?); first: {unknown[:3]}", file=sys.stderr)
            return 2
        tus = sorted(set(listed))
    if not tus:
        print("error: no translation units selected.", file=sys.stderr)
        return 2
    shard_desc = ""
    tu_total = len(tus)  # 切分前的全量条数：分片产物据此自证「这片是全量的哪一份」
    if a.shard:
        idx, total = a.shard
        # 切分只在 sorted 清单上做一次 `tus[idx::total]`：顺序唯一确定「谁归哪片」，故同一份
        # 编译库上重复取片稳定、各片两两不相交、并集恰为全量。切片数取模而非按目录分组，是为了
        # 让相邻同目录（往往同样重）的 TU 摊到不同片上，而不是全压在同一片里。
        tus = tus[idx::total]
        shard_desc = f" [shard {idx}/{total}]"
        # 空片是环境错误不是「本片干净」：CI 矩阵若把片数调到超过 TU 数，缺片必须红。
        if not tus:
            print(f"error: shard {idx}/{total} selected 0 of the translation units.", file=sys.stderr)
            return 2
    if a.print_tus:
        print("\n".join(tus))
        return 0

    jobs = a.jobs or (os.cpu_count() or 4)
    # 门禁只对本仓库源码负责。`.clang-tidy` 的 HeaderFilterRegex 能滤掉绝大多数头文件告警，
    # 但 clang-analyzer 的 optin 检查（如 core.EnumCastOutOfRange）会对系统头报路径敏感诊断而
    # 绕过该过滤——观测到 MSVC STL 的 `xfilesystem_abi.h` 即属此类。系统头既不可修、又会随
    # 工具链升级漂移，故在计数前按「是否位于仓库根之下」再过滤一次。
    repo_root = norm(os.path.abspath(".")) + "/"

    cache = None
    if a.cache_dir:
        if not a.deps:
            print("error: --cache-dir requires --deps (per-TU dependency lists, as produced by "
                  "tools/check/select_lint_tus.py); without them a fingerprint cannot see header "
                  "changes and the cache would manufacture false greens.", file=sys.stderr)
            return 2
        try:
            with open(a.deps, encoding="utf-8") as fh:
                deps = json.load(fh)
        except (OSError, ValueError) as exc:
            print(f"error: cannot read --deps {a.deps!r}: {exc}", file=sys.stderr)
            return 2
        db_argv = {f: args for f, args, _d in load_db(compile_db)}
        tidy_bin = which("clang-tidy") or "clang-tidy"
        try:
            tidy_ver = subprocess.run([tidy_bin, "--version"], capture_output=True, text=True,
                                      encoding="utf-8", errors="replace", timeout=120).stdout
        except (OSError, subprocess.TimeoutExpired):
            tidy_ver = "unknown"
        config_text = ""
        try:
            with open(a.config or ".clang-tidy", encoding="utf-8") as fh:
                config_text = fh.read()
        except OSError:
            pass
        cache = ResultCache(a.cache_dir, deps, db_argv, tidy_ver, config_text)

    cached_out: dict[str, str] = {}
    to_run: list[str] = list(tus)
    if cache is not None:
        to_run = []
        for tu in tus:
            hit = cache.load(tu)
            if hit is None:
                cache.misses += 1
                to_run.append(tu)
            else:
                cached_out[tu] = hit
        print(f"[lint] cache: {cache.hits} hit / {cache.misses} miss "
              f"({len(cached_out)}/{len(tus)} replayed without re-analysis)", flush=True)

    # 打印**本轮总覆盖面**（含缓存复放）而不只是真跑的条数：读日志的人要判断的是
    # 「这一遍到底覆盖了几个 TU」，写 `0` 会被读成「这一遍没跑」。
    print(f"[lint] clang-tidy over {len(tus)} translation units "
          f"({len(to_run)} analysed, {len(cached_out)} replayed), {jobs} parallel{mode}{shard_desc}")

    findings: dict[tuple[str, str, str], str] = {}
    # 每条告警的**首发 TU**：头文件告警会在每个包含它的 TU 里重复上报，去重后只剩一条，
    # 于是「谁把它拉进分析的」这个判因必需的信息就丢了——`clang-analyzer` 一类只有沿某条
    # 具体调用路径才会命中的检查尤其如此（复现只能从某个 TU 起手）。ex.map 按输入顺序产出，
    # 而输入是 sorted() 的 TU 清单，故「首发」= 报出它的最小路径 TU，跨轮次确定可比。
    found_by: dict[tuple[str, str, str], str] = {}
    by_area: Counter[str] = Counter()
    timeouts: list[str] = []
    # 编译失败（前端 error，无 [check] 标签）单独记账：这类 TU 一条告警都不会产出，
    # 「0 findings」于是既可能是真干净、也可能是覆盖面塌了。两者必须可区分。
    broken: dict[str, str] = {}
    t0 = time.time()
    outputs: dict[str, str] = dict(cached_out)
    if to_run:
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            jobs_args = [(t, a.checks, a.build_dir, a.fix, a.config) for t in to_run]
            for i, (tu, out, _dt) in enumerate(ex.map(run_one, jobs_args), 1):
                outputs[tu] = out
                if i % 25 == 0 or i == len(to_run):
                    print(f"  ... {i}/{len(to_run)}  ({time.time() - t0:.0f}s)", flush=True)
    # 解析一律按 sorted 的 TU 清单顺序进行：缓存复放与真跑的产物在此合流，故去重后记下的
    # 「首发 TU」与全量跑法同一口径（`found_by` 的跨轮次可比性依赖这一点）。
    for tu in tus:
        out = outputs.get(tu, "")
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
            if (np_, line_no, check) not in found_by:
                found_by[(np_, line_no, check)] = norm(tu)
            findings[(np_, line_no, check)] = (sev, msg)
            try:
                rel = os.path.relpath(np_).replace("\\", "/")
            except ValueError:
                # 诊断路径在另一盘符（如系统头在 C:，仓库在 D:），无法求相对路径
                rel = np_
            by_area[rel.split("/")[0] if "/" in rel else rel] += 1
    if cache is not None:
        for tu in to_run:
            out = outputs.get(tu, "")
            if tu in broken or out.startswith("__TIMEOUT__") or out.startswith("__ERROR__"):
                continue  # 编译失败 / 超时的 TU 绝不入缓存：见 ResultCache 的类注释
            cache.store(tu, out)
        print(f"[lint] cache: stored {cache.stored}; "
              f"replayed {cache.hits} of {len(tus)} (elapsed {time.time() - t0:.0f}s)", flush=True)

    by_check = Counter(c for (_f, _l, c) in findings)
    by_file = Counter(f for (f, _l, _c) in findings)
    sev_rank = {"warning": 0, "error": 1}
    threshold = sev_rank[a.fail_on]
    blocking = sum(1 for s, _m in findings.values() if sev_rank.get(s, 0) >= threshold)

    print(f"\n[lint] elapsed {time.time() - t0:.0f}s")
    print(f"[lint] unique findings: {len(findings)}   blocking(>={a.fail_on}): {blocking}")
    if broken:
        print(f"\n[lint] FATAL: {len(broken)}/{len(tus)} translation units failed to compile "
              f"- their warnings are not tallied; this run's coverage is incomplete (do not treat as 'already clean')")
        for tu_, msg in list(broken.items())[:10]:
            print(f"    {norm(tu_)}: {msg}")
        if len(broken) > 10:
            print(f"    ... {len(broken) - 10} more TUs")
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
            # 分片跑法下这份 JSON 只是**其中一片**：tu_count / unique_findings 都是片内计数，
            # 直接当全量读会低估。shard 字段把口径写进产物，聚合脚本据此判断「n 片齐了没」。
            "shard": list(a.shard) if a.shard else None,
            "tu_count": len(tus),
            "tu_total": tu_total,
            "unique_findings": len(findings),
            "broken_tus": sorted([[f, m] for f, m in broken.items()]),
            "by_check": by_check.most_common(),
            "by_file": by_file.most_common(),
            # 每条 = [文件, 行, check, severity, 消息, 首发 TU]
            "findings": sorted([list(k) + [v[0], v[1], found_by[k]] for k, v in findings.items()]),
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

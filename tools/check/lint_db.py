#!/usr/bin/env python3
"""编译数据库公共层：`run_clang_tidy.py` 与 `select_lint_tus.py` 的唯一底层件。

抽出这一层的理由不是「代码复用」，而是**口径同源**：TU 清单的筛选规则（排除
third_party、跳过生成物）、编译库 `command` 的 argv 还原规则、以及「某个 TU 实际
读了哪些头文件」的取依赖手法，各自只有一份实现。两处使用者对这三件事的依赖是
硬性的——选集脚本据依赖判断「本次改动牵动哪些 TU」，runner 据同一批依赖算内容
指纹做结果缓存；若两边各写一套，「被选中的 TU 集合」与「被缓存键覆盖的 TU 集合」
就会悄悄分叉，而分叉的症状恰是门禁最怕的那一种：少跑了 TU，日志却仍然干净。

本模块不注册为 CTest 门禁（不提供 `main()`），只被上面两个脚本导入。
"""

from __future__ import annotations

import json
import os
import re
import subprocess

# 排除项在两处生效：`load_tus` 筛翻译单元、runner 主循环筛诊断所属文件。
# ① `third_party/`：三方代码不按本仓风格检查。
# ② `tests/support/fake_gl.h`——GL 驱动桩：整份文件是按 `GLFn` 函数表逐个填的桩，
#    56 条告警里 48 条是 `readability-named-parameter`（桩不读参数，命名只会误导读者）、
#    5 条是指针算术（按字节铺 GL 数据）。这类检查没有「按名字豁免」的选项，逐点 NOLINT
#    就是 56 处指令，且该文件永不该被本仓风格约束——排除比豁免划算。
#    ⚠️ 逐文件点名，不用目录通配：新增豁免须显式登记，避免整目录被静默放行。
# 反面判据（勿照抄本条）：`gl_core.h`(49) 与 `atspi_protocol.h`(74) 也在这份名单的候选里
# 跑过一轮，但它们的告警几乎全是 `readability-identifier-naming`，而该检查有按类别的
# `*IgnoredRegexp`（clang-tidy 22 实测有效，探针见 BUILD_OPTIONS.md §4.5）——用命名豁免能
# 保住这两个文件的其余覆盖面，故不改用「整文件排除」这种永久盲点。
DEFAULT_EXCLUDE = re.compile(
    r"(^|/)third_party/"
    r"|tests/support/fake_gl\.h$"
)

# 预处理取依赖时要从编译命令里剔除的选项。
# `-o <文件>`：否则预处理会顺手写出产物；`-c`：只预处理不编译由 `-MM` 本身保证，留着无害但
# 与 `-o` 成对出现时易混淆，一并去掉；`-MF`/`-MT`/`-MQ`：依赖输出的重定向项，会覆盖我们要读的
# stdout 或改写目标名；`-include-pch <文件>` 与 `-Xclang <参数>` 是成对的驱动项，前者指向的 PCH
# 未必与当前 clang 版本相符（详见 run_clang_tidy.py 的 `--emscripten` 段）。
DEPS_DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ", "-include-pch", "-Xclang"}
DEPS_DROP_BARE = {"-c", "-MD", "-MMD", "-Winvalid-pch", "-fpch-instantiate-templates"}

# 取依赖要靠编译器自己的 `-MM`。MSVC 的 `cl.exe` 没有该选项（对应的是 `/showIncludes`），
# 遇到它一律返 None，由调用方按「取不到依赖 = 该 TU 入选」的保守口径处理。
_UNSUPPORTED_DEPS_COMPILERS = {"cl.exe", "cl", "clang-cl.exe", "clang-cl"}


def norm(p: str) -> str:
    return os.path.normpath(p).replace("\\", "/")


def which(name: str) -> str:
    from shutil import which as _w

    return _w(name)


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


def load_db(compile_db: str) -> list[tuple[str, list[str], str]]:
    """读编译库，返回 `[(TU 绝对路径, argv, 工作目录)]`。

    argv 取自 `arguments`；只有 `command` 字符串时经 `argv_from_command` 还原。
    工作目录一并带回：编译库里的相对路径（`-I`、源文件）都以它为基准，
    取依赖时必须在该目录下执行，否则相对 include 会解析到别处去。
    """
    with open(compile_db, encoding="utf-8") as fh:
        entries = json.load(fh)
    out: list[tuple[str, list[str], str]] = []
    for e in entries:
        f = e.get("file", "")
        directory = e.get("directory", "")
        if not os.path.isabs(f):
            f = os.path.join(directory, f)
        args = e.get("arguments")
        if not args:
            args = argv_from_command(e.get("command") or "")
        out.append((norm(f), list(args), directory))
    return out


def load_tus(compile_db: str, include: re.Pattern | None,
             exclude: re.Pattern | None) -> list[str]:
    """编译库里的 TU 清单（已去重、已排除、已排序）。

    排序不是排版偏好：分片（见 run_clang_tidy.py 的 `--shard`）在 `tus[i::n]` 上做，
    只有顺序唯一确定，「谁归哪一片」才跨轮次稳定、各片并集才恰为全量。
    """
    out, seen = [], set()
    for f, _args, _dir in load_db(compile_db):
        if f in seen:
            continue
        seen.add(f)
        if exclude is not None and exclude.search(f):
            continue
        if is_auto_generated(f):
            continue
        if include is not None and not include.search(f):
            continue
        out.append(f)
    return sorted(out)


def deps_command(args: list[str]) -> list[str] | None:
    """把某条编译命令改造成「只输出依赖列表」的预处理命令。

    返回 None 表示该编译器不支持 `-MM`（见 `_UNSUPPORTED_DEPS_COMPILERS`），
    调用方须按保守口径处理——宁可多选，不可漏选。
    """
    if not args:
        return None
    if os.path.basename(args[0]).lower() in _UNSUPPORTED_DEPS_COMPILERS:
        return None
    out, i = [args[0]], 1
    while i < len(args):
        t = args[i]
        if t in DEPS_DROP_WITH_ARG:
            i += 2
            continue
        if t in DEPS_DROP_BARE:
            i += 1
            continue
        out.append(t)
        i += 1
    out.append("-MM")
    return out


def preprocess_deps(args: list[str], directory: str,
                    timeout: float = 180.0) -> list[str] | None:
    """取一个 TU 实际读到的头文件列表（绝对路径、已归一化、已去重）。

    为什么用编译器的 `-MM` 而不是 ninja 的 `ninja -t deps`：后者只认**已经构建过**
    的目标，而 `demos` 是 EXCLUDE_FROM_ALL——它们不在构建图里，却实实在在在编译库里、
    也实实在在会因为改一个公共头而产生新告警（本机实测：keycode.h 的
    `readability-string-compare` 正是从 `demo_google_play.cpp` 这个 TU 报出来的）。
    用构建图选集会正好漏掉这一类 TU，故依赖只能来自编译库自身。

    失败一律返 None（超时 / 非 MSVC 编译器 / 编译器缺失），由调用方决定保守口径：
    本模块的契约是「取不到依赖 = 该 TU 入选」，因为漏选的代价是静默失去覆盖面。
    """
    cmd = deps_command(args)
    if cmd is None:
        return None
    try:
        r = subprocess.run(cmd, cwd=directory or None, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    # `-MM` 的输出是 make 规则：`target: dep dep \` 续行。去掉续行、去掉 `target:` 项，
    # 余下即依赖；相对路径以编译库里的 directory 为基准补齐。
    deps, seen = [], set()
    for tok in r.stdout.replace("\\\n", " ").split():
        if tok.endswith(":"):
            continue
        p = tok if os.path.isabs(tok) else os.path.join(directory, tok)
        p = norm(p)
        if p in seen:
            continue
        seen.add(p)
        deps.append(p)
    return deps

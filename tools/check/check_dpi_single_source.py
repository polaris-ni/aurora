#!/usr/bin/env python3
"""check_dpi_single_source.py - Win32 DPI 换算点单源门禁。

守住两条不变量（对应 `specification/08-tooling.md` §8.2 那条「帧 / 逻辑 / scale 三方记账发散」的
历史缺口，本脚本把它的复发变成机器可判）：

  1. **换算点唯二**：`Win32Host::Impl` 内 `scale` 的乘除只出现在 `to_physical` / `to_logical`
     两个私有函数里；其它任何地方裸写 `* scale` / `/ scale` 都算违规。
  2. **读点唯一**：除 `refresh_scale()`（它委托 `detail::read_dpi`）之外，宿主内不得出现第二个
     DPI 读取点（`GetDpiForWindow` / `GetDpiForSystem` / `GetDpiForMonitor` / `GetDeviceCaps`）。

为什么做成脚本而不是一次性人工核对：`to_physical` / `to_logical` 之外的任何一处裸写换算都会让
「帧 / 逻辑 / scale」三方各走一条路径，而这类分叉在 100% DPI 的 CI 上**恒不显形**（scale 恒 1.0），
只能靠静态检查在合入前拦住。

用法：
    python3 tools/check/check_dpi_single_source.py [--root <aurora_root>]
退出码：0 通过；1 有违规。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

HOST_REL = "src/aurora/window/win32_host.cpp"

# 允许的换算点：这两个函数的函数体内可以出现 * scale / / scale。
CONVERSION_FUNCS = ("Win32Host::Impl::to_physical", "Win32Host::Impl::to_logical")

# scale 的裸写换算。两种形态都要抓：
#   * `scale * x` / `scale / x`（scale 紧贴运算符）；
#   * `x * pimpl_->scale` / `x / obj.scale`（运算符与 scale 之间隔着成员访问）——只认紧贴的那种
#     会漏掉这一形态，而它恰恰是最常出现的写法。
# `scale_handler` / `scale_factor` 之类被 `\b` 排除。
SCALE_MATH_RE = re.compile(
    r"(?<![.\w])scale\s*[*/]"  # scale 紧贴运算符
    r"|[*/]\s*[\w>._\-]*\bscale\b"  # 运算符后经成员访问再取 scale
)

# DPI 读取点。
DPI_READ_RE = re.compile(r"GetDpiForWindow|GetDpiForSystem|GetDpiForMonitor|GetDeviceCaps")

# 唯一允许的读点。
DPI_READ_SITES = ("Win32Host::Impl::refresh_scale",)


def strip_comments_and_strings(line: str) -> str:
    """去掉行注释与字符串字面量，只留代码本体（够用的近似，不需要完整词法分析）。"""
    out = line
    # 行注释：本文件内不出现含 `//` 的字符串字面量，按首个 `//` 切即可。
    pos = out.find("//")
    if pos >= 0:
        out = out[:pos]
    return out


def function_spans(lines: list[str]) -> dict[str, tuple[int, int]]:
    """按大括号配平切出 `Win32Host::Impl::<name>` 的函数体区间（行号 1-based，含签名行）。"""
    spans: dict[str, tuple[int, int]] = {}
    depth = 0
    start = 0
    name = ""
    for idx, raw in enumerate(lines, start=1):
        code = strip_comments_and_strings(raw)
        if start == 0:
            m = re.search(r"Win32Host::Impl::(\w+)\s*\(", code)
            if m is not None:
                name = "Win32Host::Impl::" + m.group(1)
                start = idx
        if start != 0:
            depth += code.count("{") - code.count("}")
            if depth <= 0 and code.count("}") > 0:
                spans[name] = (start, idx)
                start = 0
                name = ""
                depth = 0
    return spans


def main() -> int:
    parser = argparse.ArgumentParser(description="Win32 DPI single-source gate")
    parser.add_argument("--root", default=".", help="Aurora repository root")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    host = root / HOST_REL
    if not host.exists():
        print(f"[FAIL] host source not found: {host}")
        return 1

    lines = host.read_text(encoding="utf-8", errors="replace").splitlines()
    spans = function_spans(lines)

    # --- 1. 换算点唯二 ---
    allowed: list[tuple[int, int]] = []
    for fn in CONVERSION_FUNCS:
        if fn in spans:
            allowed.append(spans[fn])
    if len(allowed) != len(CONVERSION_FUNCS):
        missing = [f for f in CONVERSION_FUNCS if f not in spans]
        print(f"[FAIL] conversion entry points not found in {HOST_REL}: {missing}")
        return 1

    violations: list[str] = []
    for idx, raw in enumerate(lines, start=1):
        code = strip_comments_and_strings(raw)
        if not SCALE_MATH_RE.search(code):
            continue
        if any(lo <= idx <= hi for lo, hi in allowed):
            continue
        violations.append(f"{HOST_REL}:{idx}: {raw.strip()}")

    # --- 2. 读点唯一 ---
    read_allowed: list[tuple[int, int]] = []
    for fn in DPI_READ_SITES:
        if fn in spans:
            read_allowed.append(spans[fn])
    if len(read_allowed) != len(DPI_READ_SITES):
        missing = [f for f in DPI_READ_SITES if f not in spans]
        print(f"[FAIL] DPI read site not found in {HOST_REL}: {missing}")
        return 1

    for idx, raw in enumerate(lines, start=1):
        code = strip_comments_and_strings(raw)
        if not DPI_READ_RE.search(code):
            continue
        if any(lo <= idx <= hi for lo, hi in read_allowed):
            continue
        violations.append(f"{HOST_REL}:{idx}: {raw.strip()}  (DPI read outside refresh_scale)")

    if violations:
        print(f"[FAIL] DPI single-source violated: {len(violations)} finding(s)")
        for v in violations:
            print(f"  - {v}")
        print("       Conversions must go through to_physical / to_logical; DPI must be read only in")
        print("       refresh_scale(). See specification/08-tooling.md 8.2.")
        return 1

    print(f"[OK] DPI single-source clean: conversions confined to "
          f"{' / '.join(f.split('::')[-1] for f in CONVERSION_FUNCS)}, "
          f"DPI read confined to {', '.join(f.split('::')[-1] for f in DPI_READ_SITES)} "
          f"({HOST_REL})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

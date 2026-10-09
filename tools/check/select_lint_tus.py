#!/usr/bin/env python3
"""按本次改动挑出「受牵动的翻译单元」，供 `run_clang_tidy.py --tu-list` 消费。

门禁的覆盖面 = 它实际分析过的 TU。全量跑一遍 native 编译库是 498~554 个 TU / 86 分钟
（4 vCPU runner），两遍（DEBUG=OFF/ON）加浏览器口径就是三个 86 分钟——于是 CI 把它摊成
8 + 4 个分片作业并跑。分片只改排布、不减覆盖面，但**每个 PR 仍然要为该 PR 没碰过的代码
付一次全量代价**。本脚本买的是另一半：改一个文件只需重跑「读到它的那些 TU」。

判定办法是**编译库自身的 include 闭包**，不是构建图：

    ninja -t deps 只认**已经构建过**的目标，而 demos 是 EXCLUDE_FROM_ALL ——
    它们不在构建图里，却实实在在在编译库里、也实实在在会因为改一个公共头而产生新告警。
    本机实测：`include/aurora/event/keycode.h` 的一条 `readability-string-compare`
    正是从 `examples/app/google_play/demo_google_play.cpp` 这个 TU 报出来的，
    按构建图选集会正好漏掉它。故依赖只能来自编译库里那条编译命令本身（`-MM`）。

三条「宁可多选、不可漏选」的兜底：
  ① 取依赖失败（超时 / 编译器缺失 / 不支持 `-MM`）的 TU 一律入选；
  ② 改动里出现**已不存在**的文件（多半是删头）→ 退化为全量：删除后的依赖闭包算不出来，
     按闭包选会得到空集，那正是「没活儿被读成干净」的形态；
  ③ 改动命中「全局配置」（`.clang-tidy` / CMake 模块 / 本脚本与 runner 自身）→ 全量，
     因为这类文件改的是**分析口径本身**，选任何子集都是错的。

反向的守卫是本脚本唯一会退出码 2 的情形：**改动里存在本编译库认得的 C/C++ 文件，
却一个 TU 都没选中**。那在逻辑上不可能（认得 = 是 TU 或被某个 TU 包含），一旦发生说明
选集链路坏了，必须红，不能让它以「0 待跑」的形态流进 CI。

用法：
    python3 tools/check/select_lint_tus.py --build-dir build --changed changed.txt --out-dir build/lint-selection
    python3 tools/check/select_lint_tus.py --build-dir build --git-diff HEAD~1        # 直接取 git 改动

退出码：0 = 已产出清单（可能是空集，见 `selection.json` 的 `mode`）；2 = 环境/用法错误或
上面那条守卫触发。
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lint_db import (  # noqa: E402  (path bootstrap above must run first)
    DEFAULT_EXCLUDE,
    load_db,
    norm,
    preprocess_deps,
)

# C/C++ 家族：改这些才谈得上「要不要重跑 lint」。
SOURCE_EXTS = (".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".c", ".inl")

# 全局触发：改动它们等于改了「怎么分析」，选任何子集都是错的 → 全量。
# ⚠️ 逐项点名，不用目录通配：加进来的每一项都得说清「为什么它影响所有 TU」。
GLOBAL_TRIGGERS = (
    ".clang-tidy",             # 检查集与 CheckOptions：改一条等于换一套判据
    "compile_flags.txt",       # 非 CMake 消费方（clangd / 手工 tidy）的旗标来源
    "CMakeLists.txt",          # 编译旗标、TU 集合本身
    "tools/check/run_clang_tidy.py",   # runner：换实现等于换结果
    "tools/check/lint_db.py",          # 编译库公共层：换 TU / 依赖口径等于换覆盖面
    "tools/check/select_lint_tus.py",  # 本脚本：换选集口径等于换覆盖面
)


def repo_root() -> str:
    """仓库根：优先 `git rev-parse --show-toplevel`，退化到当前目录。"""
    try:
        r = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=60)
        if r.returncode == 0 and r.stdout.strip():
            return norm(r.stdout.strip())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return norm(os.path.abspath("."))


def git_changed(ref: str, root: str) -> list[str]:
    """取 `git diff --name-only <ref>` 的改动清单（仓库相对、正斜杠）。

    `core.quotepath=off`：非 ASCII 路径默认被 git 转义成八进制字面量，
    拿去做路径比较会全部失配——失配的表现是「改动了却没选中」，最坏的一种静默。
    """
    cmd = ["git", "-c", "core.quotepath=off", "diff", "--name-only", ref]
    try:
        r = subprocess.run(cmd, cwd=root, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"git diff failed: {exc}") from exc
    if r.returncode != 0:
        raise RuntimeError(f"git diff {ref} failed: {(r.stderr or '').strip()[:200]}")
    return [ln.strip().strip('"') for ln in r.stdout.splitlines() if ln.strip()]


def read_changed_file(path: str) -> list[str]:
    with open(path, encoding="utf-8") as fh:
        return [ln.strip().strip('"') for ln in fh if ln.strip()]


def is_global_trigger(rel: str) -> bool:
    if rel in GLOBAL_TRIGGERS:
        return True
    if rel.startswith("cmake/") and rel.endswith(".cmake"):
        return True
    return rel.endswith("CMakeLists.txt") or rel.endswith(".cmake")


def main() -> int:
    ap = argparse.ArgumentParser(description="Select the translation units affected by a change set.")
    ap.add_argument("--build-dir", required=True,
                    help="CMake build directory containing compile_commands.json")
    ap.add_argument("--changed", default=None, metavar="FILE",
                    help="newline-separated list of changed paths (repo-relative)")
    ap.add_argument("--git-diff", default=None, metavar="REF",
                    help="take the change set from `git diff --name-only <REF>` instead")
    ap.add_argument("--out-dir", default=None,
                    help="where to write selection.json / deps.json / tus_<i>.txt "
                         "(default: <build-dir>/lint-selection)")
    ap.add_argument("--shards", type=int, default=4,
                    help="split the selection into this many lists (default: 4)")
    ap.add_argument("--min-tu-per-shard", type=int, default=40,
                    help="below this many TUs per shard, use fewer shards instead of "
                         "handing empty lists to idle CI jobs (default: 40)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel dependency probes (0 = CPU count)")
    a = ap.parse_args()

    if bool(a.changed) == bool(a.git_diff):
        print("error: pass exactly one of --changed / --git-diff.", file=sys.stderr)
        return 2
    compile_db = os.path.join(a.build_dir, "compile_commands.json")
    if not os.path.isfile(compile_db):
        print(f"error: no compile_commands.json in {a.build_dir!r}.", file=sys.stderr)
        return 2

    root = repo_root()
    out_dir = a.out_dir or os.path.join(a.build_dir, "lint-selection")
    os.makedirs(out_dir, exist_ok=True)

    if a.git_diff:
        try:
            changed = git_changed(a.git_diff, root)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    else:
        changed = read_changed_file(a.changed)
    changed = sorted({norm(c) for c in changed})

    # ---- TU 宇宙：与 runner 同一套筛选（third_party / 生成物），另弃 PCH 条目 ----
    db = load_db(compile_db)
    exclude = DEFAULT_EXCLUDE
    universe, tu_argv, tu_dir = [], {}, {}
    seen = set()
    for f, args, directory in db:
        if f in seen:
            continue
        seen.add(f)
        if exclude.search(f) or "cmake_pch" in f:
            continue
        universe.append(f)
        tu_argv[f] = args
        tu_dir[f] = directory
    universe = sorted(universe)
    if not universe:
        print("error: compile database has no usable translation units.", file=sys.stderr)
        return 2

    triggers = [c for c in changed if is_global_trigger(c)]
    missing = [c for c in changed if not os.path.exists(os.path.join(root, c))]
    cpp_changed = [c for c in changed if c.endswith(SOURCE_EXTS)]

    mode, reason = "subset", "dependency closure"
    if triggers:
        mode, reason = "full", "global analysis config changed: " + ", ".join(triggers[:3])
    elif missing:
        mode, reason = "full", "change set contains deleted paths (closure unknowable): " + ", ".join(missing[:3])
    elif not cpp_changed:
        mode, reason = "none", "no C/C++ files in the change set"

    selected: list[str] = []
    deps_failed: list[str] = []
    deps_map: dict[str, list[str]] = {}
    if mode != "none":
        # 全量模式下也照跑一遍依赖探测：那份 `deps.json` 是结果缓存的指纹来源，
        # 少了它 master 的全量遍就永远享受不到跨轮次复用（而它恰是最贵的那一遍）。
        changed_abs = {norm(os.path.join(root, c)) for c in cpp_changed}
        jobs = a.jobs or (os.cpu_count() or 4)

        def probe(tu: str) -> tuple[str, list[str] | None]:
            return tu, preprocess_deps(tu_argv[tu], tu_dir[tu])

        with ThreadPoolExecutor(max_workers=jobs) as ex:
            for i, (tu, deps) in enumerate(ex.map(probe, universe), 1):
                if deps is None:
                    # 取不到依赖 = 无从判断它是否受影响 → 宁可多选
                    deps_failed.append(tu)
                    selected.append(tu)
                    continue
                if mode == "full" or tu in changed_abs or (changed_abs & set(deps)):
                    selected.append(tu)
                    # 只留仓库内的依赖：系统头随工具链漂移，进指纹只会让缓存永不命中
                    deps_map[tu] = sorted(d for d in deps if d.startswith(root + "/"))
                if i % 100 == 0 or i == len(universe):
                    print(f"  ... probed {i}/{len(universe)}", flush=True)
        selected = sorted(set(selected))

    # ---- 守卫：认得的 C/C++ 改动却一个 TU 都没选中 = 选集链路坏了 ----
    known = set(universe)
    for deps in deps_map.values():
        known |= set(deps)
    known_rel = {norm(os.path.relpath(p, root)) for p in known if p.startswith(root + "/")}
    applicable = [c for c in cpp_changed if c in known_rel]
    unmatched = [c for c in cpp_changed if c not in known_rel]
    if mode != "none" and applicable and not selected:
        print("error: change set touches C/C++ files known to this compile database, "
              "yet 0 translation units were selected - the selection link is broken "
              "(must not be reported as clean).", file=sys.stderr)
        return 2
    if mode == "subset" and not selected:
        mode, reason = "none", "no translation unit in this database reads the changed files"

    # ---- 分片：子集小到不值时就少用几片，空片交给 CI 显式 idle ----
    n = len(selected)
    eff = max(1, min(a.shards, math.ceil(n / a.min_tu_per_shard))) if n else 0
    per_shard = []
    for i in range(a.shards):
        part = selected[i::eff] if (eff and i < eff) else []
        per_shard.append(len(part))
        with open(os.path.join(out_dir, f"tus_{i}.txt"), "w", encoding="utf-8") as fh:
            fh.write("".join(t + "\n" for t in part))

    payload = {
        "mode": mode,
        "reason": reason,
        "build_dir": norm(a.build_dir),
        "changed_total": len(changed),
        "changed_cpp": cpp_changed,
        "applicable_cpp": applicable,
        "unmatched_cpp": unmatched,
        "global_triggers": triggers,
        "deleted_paths": missing,
        "tu_total": len(universe),
        "selected_count": n,
        "deps_failed": sorted(norm(d) for d in deps_failed),
        "shards": a.shards,
        "effective_shards": eff,
        "per_shard": per_shard,
    }
    with open(os.path.join(out_dir, "selection.json"), "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=1)
    with open(os.path.join(out_dir, "deps.json"), "w", encoding="utf-8") as fh:
        json.dump(deps_map, fh, indent=1)

    print(f"[select] mode={mode} ({reason})")
    print(f"[select] changed={len(changed)} cpp={len(cpp_changed)} "
          f"applicable={len(applicable)} unmatched={len(unmatched)}")
    if unmatched:
        print(f"[select] note: {len(unmatched)} changed C/C++ files are neither a TU nor an "
              f"include of any TU in this database (other platform/config); first: {unmatched[:3]}")
    print(f"[select] selected {n}/{len(universe)} TUs -> {eff}/{a.shards} shards {per_shard}")
    print(f"[select] wrote {out_dir}/selection.json, deps.json, tus_*.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# ============================================================================
# aurora_api.json schema-drift guard
# ----------------------------------------------------------------------------
# 守护「代码真实 API」与「提交的 aurora_api.json」不漂移：
#   直接调用真实构建产物 gen_api_tools（与 AuroraTools.cmake 的 aurora_api_json 目标同源），
#   以 `-` 参数取得「纯代码视角」JSON —— 该模式不做 merge，输出不含既有文件里的
#   error_codes / debug 段（二者分别由 gen_error_codes / gen_debug_api 维护），
#   因此只比对生成器自己负责的 widgets / enums 两段。
#
# 比对项：
#   1) widgets 的 type 集合（新增控件未刷新 json / 已删控件仍在 json 残留）；
#   2) 每个共同 type 的属性键集合（prop_descriptors[].name + default_props + props）；
#   3) enums 的名称集合。
#   任一项差集非空 → 非零退出并打印缺失 / 多余明细。
#
# 为什么不自己解析 C++ 源码提取类型清单：
#   生成器经 WidgetRegistry 运行时反射 + 各 widget 的 serializeProps 提取，手写解析器
#   必然与生成器口径漂移（三方漂移）。复用构建产物是零漂移的唯一可靠做法。
#
# 前提：须先构建 gen_api_tools（与 check_gen_api_merge 同前提，CI 必须先 build 再跑该用例）。
#
# Usage:
#   python tools/check/check_api_schema_sync.py <build_dir> [repo_root]
#   (build_dir defaults to "build"; repo_root defaults to the repo root where the script lives)
# ============================================================================
import json
import os
import subprocess
import sys


def repo_root_of(path):
    """Walk up to find the repo root containing CMakeLists.txt (still resolves correctly when the
    script lives under tools/check/)."""
    d = os.path.dirname(os.path.abspath(path))
    while d and d != os.path.dirname(d):
        if os.path.isfile(os.path.join(d, "CMakeLists.txt")):
            return d
        d = os.path.dirname(d)
    return d


def prop_keys(widget):
    """Collect the property-key set of one widget entry (symmetric on both sides)."""
    keys = set()
    descriptors = widget.get("prop_descriptors")
    if isinstance(descriptors, list):
        keys |= {d.get("name") for d in descriptors if isinstance(d, dict)}
    props = widget.get("props")
    if isinstance(props, dict):
        keys |= set(props.keys())
    elif isinstance(props, list):
        keys |= {p for p in props if isinstance(p, str)}
    defaults = widget.get("default_props")
    if isinstance(defaults, dict):
        keys |= set(defaults.keys())
    keys.discard(None)
    return keys


def widget_map(api):
    """type -> property-key set."""
    out = {}
    for widget in api.get("widgets", []) or []:
        if not isinstance(widget, dict):
            continue
        name = widget.get("type")
        if name:
            out[name] = prop_keys(widget)
    return out


def enum_names(api):
    """Enum name set (compat with both dict entries and bare strings)."""
    out = set()
    for item in api.get("enums", []) or []:
        if isinstance(item, dict):
            name = item.get("name")
        else:
            name = item
        if name:
            out.add(name)
    return out


def main() -> int:
    repo = sys.argv[2] if len(sys.argv) > 2 else repo_root_of(__file__)
    build = sys.argv[1] if len(sys.argv) > 1 else "build"
    build_abs = build if os.path.isabs(build) else os.path.join(repo, build)

    # Cross-platform executable name: build artifacts carry a .exe suffix on Windows, but not on
    # other platforms (Linux/macOS).
    suffix = ".exe" if os.name == "nt" else ""
    gen_api = os.path.join(build_abs, "gen_api_tools" + suffix)

    if not os.path.isfile(gen_api):
        print(f"[FAIL] gen_api_tools not found: {gen_api}")
        print("       Build it first: cmake --build <build_dir> --target gen_api_tools")
        print("       (this guard shares the same prerequisite as check_gen_api_merge)")
        return 2

    # `-` => pure code view: no merge of existing error_codes / debug sections, JSON to stdout.
    proc = subprocess.run([gen_api, "-"], cwd=repo, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"[FAIL] gen_api_tools exited {proc.returncode}")
        if proc.stderr:
            print(proc.stderr.strip())
        return 2
    try:
        fresh = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        print(f"[FAIL] gen_api_tools output is not valid JSON: {exc}")
        return 2

    api_path = os.path.join(repo, "aurora_api.json")
    try:
        with open(api_path, encoding="utf-8") as handle:
            committed = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[FAIL] cannot read {api_path}: {exc}")
        return 2

    fresh_widgets, committed_widgets = widget_map(fresh), widget_map(committed)
    problems = []

    missing = sorted(set(fresh_widgets) - set(committed_widgets))
    extra = sorted(set(committed_widgets) - set(fresh_widgets))
    if missing:
        problems.append(
            "widgets in code but missing from aurora_api.json (run --target aurora_api_json):\n      "
            + ", ".join(missing)
        )
    if extra:
        problems.append(
            "widgets in aurora_api.json but no longer in code (stale entries):\n      "
            + ", ".join(extra)
        )

    for name in sorted(set(fresh_widgets) & set(committed_widgets)):
        lost = sorted(fresh_widgets[name] - committed_widgets[name])
        gained = sorted(committed_widgets[name] - fresh_widgets[name])
        if lost:
            problems.append(f"widget '{name}': property keys missing from json: {', '.join(lost)}")
        if gained:
            problems.append(f"widget '{name}': property keys stale in json: {', '.join(gained)}")

    fresh_enums, committed_enums = enum_names(fresh), enum_names(committed)
    enum_missing = sorted(fresh_enums - committed_enums)
    enum_extra = sorted(committed_enums - fresh_enums)
    if enum_missing:
        problems.append(f"enums missing from json: {', '.join(enum_missing)}")
    if enum_extra:
        problems.append(f"enums stale in json: {', '.join(enum_extra)}")

    if problems:
        print("[FAIL] aurora_api.json is out of sync with the current public API:")
        for item in problems:
            print(f"  - {item}")
        print(f"\n  widgets: code={len(fresh_widgets)} json={len(committed_widgets)}"
              f" | enums: code={len(fresh_enums)} json={len(committed_enums)}")
        print("  Fix: cmake --build <build_dir> --target aurora_api_json")
        return 1

    print(f"[OK] aurora_api.json in sync: {len(fresh_widgets)} widget types, "
          f"{len(fresh_enums)} enums.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

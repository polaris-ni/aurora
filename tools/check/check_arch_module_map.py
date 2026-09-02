#!/usr/bin/env python3
"""
check_arch_module_map.py — 校验 codespec ARCHITECTURE_RUNTIME §4 模块映射中的头/源文件引用。

规则：
- 仅扫描 `## 4. 模块映射` 到下一个 `## ` 之间的内容。
- 提取所有反引号包裹、以源代码扩展名结尾的 token（.h/.hpp/.hh/.cpp/.cc/.cxx/.inl）。
  - 类型名/符号（如 `Result<T>`、`Signal`、`Window`）无扩展名，自动排除。
  - 目录引用（如 `core/`）无扩展名，自动排除。
- 解析策略：
  - 含 `/` 的 token 视为相对 include/aurora/ (头) 或 src/aurora/ (实现) 的完整路径。
  - 裸文件名（无 `/`）结合「最近一行表格的『路径』单元格」的目录前缀推断；
    若无法锚定，则在对应根目录下回退全量搜索。
- 输出逐条 STATUS（OK / MISSING / AMBIGUOUS / FALLBACK）与汇总。
- 退出码：存在 MISSING 或 AMBIGUOUS → 1（可接入 CI 门禁）；否则 0。

用法：
  python3 tools/check/check_arch_module_map.py [--root <aurora_root>]
"""
import argparse
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


DOC_REL = "codespec/ARCHITECTURE.md"
HEADER_EXT = {".h", ".hpp", ".hh", ".inl"}
IMPL_EXT = {".cpp", ".cc", ".cxx"}
SRC_EXT = HEADER_EXT | IMPL_EXT

TOKEN_RE = re.compile(r"`([^`]+)`")
FILE_RE = re.compile(r"^[\w./\\-]+$")  # 反引号内须为合法路径字符


def iter_section4_lines(lines):
    """yield 仅 §4 区段的行（含 §4 标题，止于下一个 `## `）。"""
    in_sec = False
    for ln in lines:
        if ln.startswith("## 4."):
            in_sec = True
            yield ln
            continue
        if in_sec and ln.startswith("## ") and not ln.startswith("## 4."):
            return
        if in_sec:
            yield ln


def parse_table_row(line):
    """若为 3 列表格行，返回 (模块, 路径单元格, 整行文本)；否则 None。"""
    s = line.strip()
    if not s.startswith("|"):
        return None
    parts = [p.strip() for p in s.strip("|").split("|")]
    if len(parts) < 3:
        return None
    return parts[0], parts[1], line


def collect_refs(lines):
    """
    返回 refs: list of dict {token, base_dir, hint_dir, section_no, line_no, in_table}
    base_dir 解析优先级（见 resolve）：
      - 表格行内 token：用本行「路径」列目录（最精确）。
      - 自由文本/列表项 token：用正文提示词目录（如 `（include/aurora/storage/）`）优先，
        其次用最近表格行的模块目录（inherited，如 §4.3 列表项继承 window/）。
      - 顶层入口头（aurora.h 等）：直接核对 include/aurora/<tok>。
    """
    hint_re = re.compile(r"include/aurora/([A-Za-z_][\w/]*/)")  # 捕获形如 storage/ 的目录
    refs = []
    inherited_base = None  # 最近表格行的模块目录（供自由文本继承）
    last_hint_dir = None  # 最近正文提示词目录
    sec_no = None
    line_no = 0
    for raw in lines:
        line_no += 1
        msec = re.match(r"^###\s+(4\.\d+)\s", raw)
        if msec:
            sec_no = msec.group(1)
        mhint = hint_re.search(raw)
        if mhint:
            last_hint_dir = mhint.group(1)
        row = parse_table_row(raw)
        row_base = None
        if row is not None:
            _, path_cell, _ = row
            pcell_tokens = TOKEN_RE.findall(path_cell)
            if pcell_tokens:
                pt = pcell_tokens[0]
                if pt.endswith("/") and "/" not in pt[:-1]:
                    row_base = pt
                    inherited_base = pt
        # 自由文本/列表项：提示词目录优先，否则继承本模块目录
        eff_base = row_base if row is not None else (last_hint_dir or inherited_base)
        for tok in TOKEN_RE.findall(raw):
            if not FILE_RE.match(tok):
                continue
            ext = os.path.splitext(tok)[1].lower()
            if ext not in SRC_EXT:
                continue
            refs.append({
                "token": tok,
                "base_dir": row_base if row is not None else eff_base,
                "hint_dir": last_hint_dir,
                "sec": sec_no,
                "line": line_no,
                "in_table": row is not None,
            })
    return refs


def resolve(tok, base_dir, hint_dir, include_root, src_root):
    """返回 (status, resolved_path)。

    status 语义：
      OK        锚定目录/完整路径下存在
      MISPLACED 锚定目录（行内「路径」列）下不存在，但在别处找到 → 文档写错模块归属（硬失败）
      FALLBACK  无锚定目录、经回退搜索解析（建议文档显式化路径）
      MISSING   任何位置都不存在
      AMBIGUOUS 回退搜索命中多处
    """
    ext = os.path.splitext(tok)[1].lower()
    is_impl = ext in IMPL_EXT
    if "/" in tok:
        cand = os.path.join((src_root if is_impl else include_root), tok)
        if os.path.isfile(cand):
            return "OK", os.path.relpath(cand)
        alt = os.path.join((include_root if is_impl else src_root), tok)
        if os.path.isfile(alt):
            return "OK", os.path.relpath(alt)
        return "MISSING", tok
    # 裸文件名：优先用行内「路径」列锚定
    if base_dir:
        anchored = os.path.join((src_root if is_impl else include_root), base_dir, tok)
        if os.path.isfile(anchored):
            return "OK", os.path.relpath(anchored)
        # 锚定处不存在 → 全量搜索，若别处存在则为「错位」（文档模块归属写错）
        root = src_root if is_impl else include_root
        hits = [os.path.relpath(os.path.join(dp, tok))
                for dp, _, fn in os.walk(root) if tok in fn]
        if len(hits) == 1:
            return "MISPLACED", f"文档写于 `{base_dir}{tok}`，实际位于 `{hits[0]}`"
        if len(hits) > 1:
            return "MISPLACED", f"文档写于 `{base_dir}{tok}`，实际多处: " + " | ".join(hits)
        return "MISSING", f"{base_dir}{tok}"
    # 无行内锚定：用正文提示词目录（如 `（include/aurora/storage/）`）锚定
    if hint_dir:
        anchored = os.path.join((src_root if is_impl else include_root), hint_dir, tok)
        if os.path.isfile(anchored):
            return "OK", os.path.relpath(anchored)
    # 顶层入口头（aurora.h / aurora_fwd.h 等直接在 include/aurora/ 下）
    top = os.path.join(include_root, tok)
    if os.path.isfile(top):
        return "OK", os.path.relpath(top)
    # 回退：全量搜索
    root = src_root if is_impl else include_root
    hits = [os.path.relpath(os.path.join(dp, tok))
            for dp, _, fn in os.walk(root) if tok in fn]
    if len(hits) == 1:
        return "FALLBACK", hits[0]
    if len(hits) > 1:
        return "AMBIGUOUS", " | ".join(hits)
    return "MISSING", tok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None, help="Aurora 仓库根目录（默认脚本上级两级）")
    args = ap.parse_args()
    root = args.root or repo_root_of(__file__)
    doc_path = os.path.join(root, DOC_REL)
    include_root = os.path.join(root, "include", "aurora")
    src_root = os.path.join(root, "src", "aurora")

    if not os.path.isfile(doc_path):
        print(f"[ERR] 找不到文档: {doc_path}", file=sys.stderr)
        return 2
    if not os.path.isdir(include_root):
        print(f"[ERR] 找不到头目录: {include_root}", file=sys.stderr)
        return 2

    with open(doc_path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    sec_lines = list(iter_section4_lines(lines))
    refs = collect_refs(sec_lines)

    # 去重（同 token 同锚定只报一次）
    seen = set()
    rows = []
    for r in refs:
        status, resolved = resolve(r["token"], r["base_dir"], r["hint_dir"], include_root, src_root)
        key = (r["token"], resolved, status)
        if key in seen:
            continue
        seen.add(key)
        rows.append((r, status, resolved))

    # 打印报告
    print(f"模块映射一致性校验 — {DOC_REL}")
    print(f"扫描到文件引用 {len(rows)} 条（含回退/歧义去重后）\n")
    print(f"{'STATUS':<10} {'SEC':<6} {'TOKEN':<34} RESOLVED")
    print("-" * 90)
    n_ok = n_miss = n_amb = n_fb = n_mis = 0
    for r, status, resolved in rows:
        loc = f"[{r['sec'] or '?'} : L{r['line']}]"
        print(f"{status:<10} {loc:<16} {r['token']:<30} {resolved}")
        if status == "OK":
            n_ok += 1
        elif status == "MISSING":
            n_miss += 1
        elif status == "AMBIGUOUS":
            n_amb += 1
        elif status == "FALLBACK":
            n_fb += 1
        elif status == "MISPLACED":
            n_mis += 1

    print("-" * 90)
    print(f"OK={n_ok}  FALLBACK={n_fb}  MISPLACED={n_mis}  MISSING={n_miss}  AMBIGUOUS={n_amb}")
    if n_miss or n_amb or n_mis:
        print("\n[FAIL] 存在缺失/歧义/错位引用，请修正文档或代码。")
        return 1
    if n_fb:
        print("\n[WARN] 部分引用经回退搜索解析（文档未显式给出路径前缀），建议显式化。")
    print("\n[PASS] 无缺失/歧义/错位引用。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

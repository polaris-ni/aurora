#!/usr/bin/env python3
# ============================================================================
# gcov_aggregate.py — gcov 覆盖率聚合 + 自绘 HTML 报告
# ----------------------------------------------------------------------------
# 背景：本机 gcov (GCC 16) 无 --html 选项，而 LLVM 分支有 llvm-cov show -format=html。
# 为让 GCC / LLVM 两个覆盖率分支共享**同一形态**的 HTML 报告，且不强依赖 gcov 版本，
# 由本脚本自绘 HTML（仅依赖标准库 + 解析 gcov 中间格式）。
#
# 输入：gcov 中间格式（GCC >= 9 的 `gcov -i -t` JSON 文档流，或旧式 file:/lcount: 段落）。
#       多个 .gcda 的 `gcov -i -t` 输出可直接拼接传入（每段自描述）。
# 输出：
#   - 终端：总体行覆盖率 + 低于阈值文件清单（升序）+ 公共头↔测试 1:1 映射概览；
#   - --csv  <path>：完整表 CSV；
#   - --html <path>：自绘 HTML 报告（汇总表 + 每文件命中/未命中行）。
#
# 用法：
#   python3 gcov_aggregate.py <all.gcov> [--src-root <repo>] [--threshold 90]
#                               [--csv <coverage.csv>] [--html <coverage.html>]
# ============================================================================
import argparse
import csv
import json
import os
import sys


def parse_intermediate(text, src_root, counts):
    """解析 gcov 中间格式（JSON 流或旧式 file:/lcount: 段落）到 counts[path][line]=sum。"""

    def add(p, line, cnt):
        p = os.path.normpath(p)
        if p and not os.path.isabs(p):
            p = os.path.normpath(os.path.join(src_root, p))
        d = counts.setdefault(p, {})
        d[line] = d.get(line, 0) + cnt

    if text.lstrip().startswith("{"):
        dec = json.JSONDecoder()
        pos, n = 0, len(text)
        while pos < n:
            while pos < n and text[pos] in " \t\r\n":
                pos += 1
            if pos >= n:
                break
            doc, pos = dec.raw_decode(text, pos)
            for f in doc.get("files", []):
                path = f.get("file", "")
                for l in f.get("lines", []):
                    add(path, int(l["line_number"]), int(l.get("count", 0)))
    else:
        cur = None
        for ln in text.splitlines():
            if ln.startswith("file:"):
                cur = ln[5:].strip()
            elif ln.startswith("lcount:") and cur:
                parts = ln[7:].split(",")
                add(cur, int(parts[0]), int(parts[1]))


def in_scope(p, src_root):
    return p.startswith(src_root) and (
            p.startswith(src_root + "src/aurora/") or p.startswith(src_root + "include/aurora/")
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="+", help="gcov 中间格式文件（可多个）；目录则收集其下 *.gcov")
    ap.add_argument("--src-root", default=None, help="仓库根（默认脚本上级两级）")
    ap.add_argument("--threshold", type=float, default=90.0)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--html", default=None)
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    src_root = os.path.normpath(args.src_root or os.path.dirname(os.path.dirname(here))).rstrip("/") + "/"

    files = []
    for p in args.input:
        if os.path.isdir(p):
            files.extend(os.path.join(p, f) for f in os.listdir(p) if f.endswith(".gcov"))
        else:
            files.append(p)

    counts = {}
    for fp in files:
        with open(fp, encoding="utf-8", errors="replace") as fh:
            parse_intermediate(fh.read(), src_root, counts)

    rows = []
    for path, lines in counts.items():
        if not in_scope(path, src_root):
            continue
        total = len(lines)
        covered = sum(1 for c in lines.values() if c > 0)
        if total == 0:
            continue
        pct = 100.0 * covered / total
        rel = path[len(src_root):]
        rows.append((pct, rel, covered, total, lines))

    rows.sort()
    overall_c = sum(r[2] for r in rows)
    overall_t = sum(r[3] for r in rows)
    overall = 100.0 * overall_c / overall_t if overall_t else 0.0

    print("=" * 62)
    print(f"Aurora 行覆盖率（gcov 聚合, {len(rows)} 个源文件, 阈值 {args.threshold:.0f}%）")
    print("=" * 62)
    print(f"总体: {overall:.1f}%  ({overall_c}/{overall_t} 可执行行)")

    low = [r for r in rows if r[0] < args.threshold]
    print(f"\n低于阈值 {args.threshold:.0f}% 的文件 ({len(low)} 个，升序):")
    for pct, rel, c, t, _ in low:
        print(f"  {pct:6.1f}%  {rel}  ({c}/{t})")

    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["coverage_pct", "covered_lines", "executable_lines", "file"])
            for pct, rel, c, t, _ in rows:
                w.writerow([f"{pct:.1f}", c, t, rel])
        print(f"\n完整表已写入: {args.csv}")

    if args.html:
        write_html(args.html, rows, overall, overall_c, overall_t, args.threshold)
        print(f"HTML 报告已写入: {args.html}")

    # 公共头 ↔ 测试 1:1 映射概览（项目约定代理指标）
    inc = os.path.join(src_root, "include", "aurora")
    tests_dir = os.path.join(src_root, "tests")
    headers = []
    if os.path.isdir(inc):
        for root, dirs, fs in os.walk(inc):
            dirs[:] = [d for d in dirs if d not in ("detail", "internal")]
            for f in fs:
                b = os.path.basename(f)
                if f.endswith(".h") and "_pch" not in b and "_fwd" not in b and b != "aurora.h":
                    headers.append(os.path.join(root, f))
    have_direct = sum(
        1 for h in headers
        if os.path.exists(os.path.join(tests_dir, "test_" + os.path.basename(h)[:-2] + ".cpp"))
    )
    print(f"\n公共头 {len(headers)} 个；同名直测文件 {have_direct} 个"
          f"（其余经模块宿主测试或映射注释覆盖）")

    return 0


def write_html(path, rows, overall, overall_c, overall_t, threshold):
    parts = ["<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>", "<title>Aurora 行覆盖率报告</title>",
             "<style>body{font-family:monospace;margin:2rem}table{border-collapse:collapse;"
             "width:100%}td,th{border:1px solid #ccc;padding:4px 8px;text-align:right}"
             "td.file,th.file{text-align:left}.low{color:#b00}.ok{color:#070}"
             ".miss{background:#fdd}.hit{background:#dfd}pre{margin:0}</style></head><body>",
             f"<h1>Aurora 行覆盖率报告</h1>",
             f"<p>总体 <b>{overall:.1f}%</b> ({overall_c}/{overall_t} 可执行行)，阈值 {threshold:.0f}%</p>",
             "<table><tr><th class='file'>file</th><th>coverage %</th><th>covered</th>"
             "<th>executable</th></tr>"]
    for pct, rel, c, t, _ in rows:
        cls = "low" if pct < threshold else "ok"
        parts.append(f"<tr><td class='file'><a href='#{rel}'>{rel}</a></td>"
                     f"<td class='{cls}'>{pct:.1f}</td><td>{c}</td><td>{t}</td></tr>")
    parts.append("</table>")
    # 每文件命中/未命中行（仅列出行号，便于点开定位）
    for pct, rel, c, t, lines in rows:
        miss = sorted(ln for ln, cnt in lines.items() if cnt == 0)
        parts.append(f"<h3 id='{rel}'>{rel} — {pct:.1f}%</h3>")
        if miss:
            parts.append(f"<p class='miss'>未覆盖行 ({len(miss)}): {', '.join(map(str, miss))}</p>")
        else:
            parts.append("<p class='hit'>全部覆盖 ✅</p>")
    parts.append("</body></html>")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(parts))


if __name__ == "__main__":
    sys.exit(main())

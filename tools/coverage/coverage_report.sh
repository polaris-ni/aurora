#!/usr/bin/env bash
# coverage_report.sh - Aurora 行覆盖率终端摘要（Linux/macOS，GCC gcov；无 HTML）。
#
# 对应 Windows 版：tools/coverage/coverage_report.ps1。由 CMake `coverage` 目标在 GCC 分支调用，
# 也可独立运行。前提：构建目录已用 -DAURORA_ENABLE_COVERAGE=ON 配置、全量构建并运行过
# 测试（对象目录下存在 .gcda）。
#
# 用法：
#   tools/coverage/coverage_report.sh [build_dir] [threshold]
#     build_dir   默认 build-cov
#     threshold   逐文件达标线，默认 90
#
# 与 ps1 版的差异：本脚本用 `gcov -i -t` 中间格式聚合「所有目标目录」的行计数后再求和
# （同一源文件被库与多个测试 TU 共享时按并集统计），而非逐 .gcda 覆盖同名 .gcov。
#
# 输出三段（均由 tools/coverage/gcov_aggregate.py 产出，HTML 由该脚本自绘——
# 因本机 gcov 无 --html，GCC 与 LLVM 两个分支共用同一形态 HTML，见 gcov_aggregate.py）：
#   (A) 总体行覆盖率 + 低于阈值文件清单（升序）
#   (B) 全量表 CSV 写至 <build_dir>/coverage.csv + HTML 写至 <build_dir>/coverage.html
#   (C) 公共头 ↔ 测试文件 1:1 映射概览（项目约定代理指标）
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${1:-build-cov}
THRESHOLD=${2:-90}

command -v gcov >/dev/null 2>&1 || { echo "[coverage] 未找到 gcov" >&2; exit 2; }
if [ ! -d "$BUILD_DIR" ]; then
    echo "[coverage] 构建目录不存在: $BUILD_DIR（先以 -DAURORA_ENABLE_COVERAGE=ON 构建并跑测）" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---- 收集全部 .gcda，逐个以中间格式输出到同一流（file:/lcount: 段落自描述，可直接拼接）
find "$BUILD_DIR" -name '*.gcda' | sort > "$WORK/gcdas.txt"
count=$(wc -l < "$WORK/gcdas.txt")
if [ "$count" -eq 0 ]; then
    echo "[coverage] 无 .gcda——请先运行测试（ctest --test-dir $BUILD_DIR）" >&2
    exit 2
fi

while IFS= read -r gcda; do
    objdir=$(dirname "$gcda")
    # 失败不中断：个别对象可能缺配对 .gcno（外部工具链产物等），跳过即可
    gcov -i -t -o "$objdir" "$gcda" >> "$WORK/all.gcov" 2>/dev/null || true
done < "$WORK/gcdas.txt"

# ---- 聚合 + 报告（文本 / CSV / 自绘 HTML）统一交给 gcov_aggregate.py ----
AGG="$SCRIPT_DIR/gcov_aggregate.py"
command -v python3 >/dev/null 2>&1 || { echo "[coverage] 未找到 python3（聚合/HTML 需要）" >&2; exit 2; }
python3 "$AGG" "$WORK/all.gcov" --src-root "$SRC_ROOT" --threshold "$THRESHOLD" \
    --csv "$BUILD_DIR/coverage.csv" --html "$BUILD_DIR/coverage.html"

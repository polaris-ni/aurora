#!/usr/bin/env bash
# G23/G24 验收判据的「变异自证」脚本。
#
# 为什么要它：判据全绿不构成证据 —— 把被测逻辑整段删掉（退化为「不生效」）时，很多断言
# 依然全绿。本脚本逐个注入真实变异，确认对应判据**转红**，再还原。
#
# 两个已踩过的坑（务必保留）：
#   ① 还原必须覆盖头文件，否则改的是 .cpp、编的是旧 .obj；
#   ② 每次跑前先 `rm -f <exe>`，否则编译失败后照跑，用的是上一轮的旧 exe，「全绿」是假的。
#
# 变异写法必须能编译：类型不同不能互赋时，改用「存完即清空 = nullptr」这类等义写法。
# 编译失败的变异不算验证过 —— 脚本会显式打印该结论并计入失败。
set -uo pipefail

# 仓库根：本脚本位于 tools/verify/，故需上溯**两级**（只上一级会落到 tools/，
# 于是下面所有相对路径都错位 —— 表现为「源文件找不到」而非锚点缺失）。
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
# 自检：锚点文件必须在位，否则后续失败会被误读成「锚点没找到」而非「路径错了」。
if [ ! -f "src/aurora/render/font_engine.cpp" ]; then
    echo "FATAL: ROOT=$ROOT 不是仓库根（找不到 src/aurora/render/font_engine.cpp）"
    exit 2
fi

RUNNER="build/aurora_test_runner.exe"
FD_SRC="src/aurora/render/font_discovery.cpp"
FE_SRC="src/aurora/render/font_engine.cpp"
FE_HDR="include/aurora/render/font_engine.h"
FD_HDR="include/aurora/render/font_discovery.h"

PASS=0
FAIL=0
# 备份目录放在仓库内：Git Bash 的 `mktemp -d` 返回的是 Windows 风格路径（指向用户临时目录），
# `cp` 拿它当源会失败，且末尾 `rm -rf` 会触发路径安全拦截。
# 固定放 build/ 下（产物目录不入版控），并用相对路径供 cp 使用。
BACKUP_DIR="build/.g23_g24_mutation_backup"
rm -rf "$BACKUP_DIR"
mkdir -p "$BACKUP_DIR"

# 备份全部可能被改动的文件（还原时统一覆盖，含头文件）。
backup() {
    for f in "$FD_SRC" "$FE_SRC" "$FE_HDR" "$FD_HDR"; do
        cp "$f" "$BACKUP_DIR/$(basename "$f")"
    done
}

restore() {
    for f in "$FD_SRC" "$FE_SRC" "$FE_HDR" "$FD_HDR"; do
        cp "$BACKUP_DIR/$(basename "$f")" "$f"
    done
}

# mutate <file>：从 stdin 读一段 python，做锚点替换。**锚点失配即报错退出**。
#
# 为什么必须校验：python 里 `assert old in s` 失败时退出码非 0，但若调用方不检查，
# 源文件其实**没被改动** —— 随后的编译会成功、测试会全绿，脚本把它记成「判据空转」。
# 这两种失败的含义完全相反（一个是判据无效，一个是变异没注入），混为一谈会误导后续修复。
# 格式化（run_clang_format.py --fix）会重排锚点附近的代码，是这类失配的常见诱因。
mutate() {
    local f="$1"
    if ! python - "$f" >"$BACKUP_DIR/mutate.log" 2>&1; then
        echo "  [INJECT-FAIL] 锚点失配，变异未注入（不是判据问题）:"
        tail -3 "$BACKUP_DIR/mutate.log"
        FAIL=$((FAIL + 1))
        return 1
    fi
    return 0
}

# run_case <suite> <label>：重建并跑指定套件，判据须「有失败」才算变异生效。
run_case() {
    local suite="$1" label="$2" expect_red="${3:-yes}"
    rm -f "$RUNNER"   # 坑②：先删 exe，防止编译失败后跑旧产物
    if ! cmake --build build --target aurora_test_runner >"$BACKUP_DIR/build.log" 2>&1; then
        echo "  [COMPILE-FAIL] $label —— 变异写法不能编译，本次不算验证过"
        grep -m3 "error" "$BACKUP_DIR/build.log"
        FAIL=$((FAIL + 1))
        restore
        return
    fi
    local out
    out="$("./$RUNNER" --run="$suite" 2>&1)"
    if echo "$out" | grep -q "\[  FAILED  \]"; then
        if [ "$expect_red" = "no" ]; then
            echo "  [UNEXPECTED-RED] $label —— 覆盖度探测项却转红了，说明这条此前被漏判，需补判据"
            FAIL=$((FAIL + 1))
        else
            echo "  [OK-RED ] $label —— 判据如期转红：$(echo "$out" | grep -o '\[  FAILED  \] [a-z_]*' | head -2 | tr '\n' ' ')"
            PASS=$((PASS + 1))
        fi
    else
        if [ "$expect_red" = "no" ]; then
            echo "  [AS-EXPECTED-NO-RED] $label —— 变异已注入但不转红，即为该探测项的预期结论（不计失败）"
            PASS=$((PASS + 1))
        else
            echo "  [NO-RED ] $label —— 判据未转红（变异已注入 ⇒ 该批断言确实无法证明本条行为）"
            FAIL=$((FAIL + 1))
        fi
    fi
    restore
}

echo "=== 变异自证：G23 按族回退链 / G24 固定格推进 ==="
backup

# 变异 1：让 resolve_faces 完全忽略调用方给的回退链（退化为加字段前的行为）。
#   期望：「链生效 / 顺序不被重排」两条转红；「空链等价」「链上族不存在回落」仍应全绿
#   （它们钉的正是「无影响」这一侧，不该被本变异打红）。
mutate "$FD_SRC" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = "    const bool has_chain = !fallback_families.empty();"
new = "    const bool has_chain = false;  // MUTATION: 忽略调用方给的回退链\n" \
      "    (void)fallback_families;"
assert old in s, "anchor not found"
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
run_case utest_font_discovery "G23 忽略回退链（应转红：链生效 / 顺序）"

# 变异 2：把「分段保序」改回「全局 weight 重排」——即让框架重排调用方声明的链顺序。
#   期望：「顺序不被重排」转红。
mutate "$FD_SRC" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = "    g_resolve_cache[cache_key] = uniq;  // uniq 已按最终段序排好"
new = ("    std::ranges::stable_sort(uniq, [weight](const auto &a, const auto &b) {\n"
       "        return std::abs(a->weight - weight) < std::abs(b->weight - weight);\n"
       "    });\n"
       "    for (std::size_t i = 0; i < uniq.size(); ++i) {\n"
       "        seen[i] = uniq[i].get();\n"
       "    }\n"
       "    g_resolve_cache[cache_key] = uniq;  // MUTATION: 跨族重排")
assert old in s, "anchor not found"
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
run_case utest_font_discovery "G23 跨族重排链（应转红：顺序不被重排）"

# 变异 3：让固定格档位在绘制侧失效（回到各自 face 的 advance）。
#   期望：「推进与 face 无关」「命中与像素同源」转红；
#   「缺字回退仍生效」仍应全绿（它钉的是选面，与推进无关）。
mutate "$FE_SRC" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = "[[nodiscard]] inline auto glyph_advance_px(const ShapedGlyph &g, const TextLayoutOpts &opts) -> float {\n    return opts.fixed_cell_advance_px.has_value() ? *opts.fixed_cell_advance_px : g.x_adv;"
new = "[[nodiscard]] inline auto glyph_advance_px(const ShapedGlyph &g, const TextLayoutOpts &opts) -> float {\n    (void)opts;\n    return g.x_adv;  // MUTATION: 固定格档位不生效"
assert old in s, "anchor not found"
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
run_case utest_font_engine "G24 档位不生效（应转红：推进 / 命中）"

# 变异 4：只让**绘制侧**忽略档位、度量侧照用（模拟「改了绘制忘了改度量」这个真实漏法）。
#   期望：「推进与 face 无关」转红（绘制宽度不再等于 3 格）；
#   这条变异专门证明度量与像素是**两处**判据在管，而不是同一处自证。
mutate "$FE_SRC" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = "            const bool fixed_cell = opts.fixed_cell_advance_px.has_value();\n            if (fixed_cell) {"
new = "            const bool fixed_cell = false;  // MUTATION: 仅绘制侧忽略档位\n            (void)opts;\n            if (fixed_cell) {"
assert old in s, "anchor not found"
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
run_case utest_font_engine "G24 仅绘制侧忽略档位（应转红：度量与像素失配）"

# 变异 5：让回退链不进缓存键 —— 两条不同链会共用 shaping 缓存条目。
#   期望：可能不转红（本仓用例的链都指向不同面、faces_key 已不同）⇒ 属「覆盖不足」，
#   脚本会如实记为 NO-RED，这是需要人工判断的信号，不是脚本故障。
mutate "$FE_SRC" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
old = "        for (std::size_t i = 0; i < k.opts.font_fallback_chain_size; ++i) {\n            mix(std::hash<std::string>{}(k.opts.font_fallback_chain.at(i)));\n        }\n        mix(static_cast<std::uint64_t>(k.opts.font_fallback_chain_size) * 0x9E37ULL);"
new = "        // MUTATION: 链不进缓存键"
assert old in s, "anchor not found"
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
PY
# 第 5 项是**覆盖度探测**，预期 NO-RED（不计失败）。实测结论：缓存键里已有
# `faces_key`（face id 序列的 FNV），链解析出不同 faces 时 faces_key 本身就不同 ⇒ 缓存条目
# 天然隔离；而「两条不同链解析出同一组 faces」时共用条目也无害（faces 相同 ⇒ shaping 输出相同）。
# 即链的哈希混入是 belt-and-suspenders，不是正确性必需项。它观测不到，正是预期结论。
run_case utest_font_engine "G24 链不进 shaping 缓存键（覆盖度探测，预期 NO-RED）" no

restore
rm -rf "$BACKUP_DIR"

echo
echo "=== 变异自证结果：转红确认 $PASS 项，未转红/编译失败 $FAIL 项 ==="
[ "$FAIL" -eq 0 ] || exit 1
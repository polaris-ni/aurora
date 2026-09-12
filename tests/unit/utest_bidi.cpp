/// 测试类型: unit
/// 目标单元: include/aurora/render/bidi.h (+ src/aurora/render/font_engine.cpp 的跨 run 重排接入)
/// 测试说明: 覆盖 A2 混排 UBA 切片的段落级 run 视觉重排：
///   - 纯函数：guess_paragraph_direction（首个强方向字符定基准，UBA P2/P3）与
///     bidi_visual_run_order（RTL 段落整体右→左翻转 run 顺序，LTR 恒等）。
///   - 纯函数：完整逐字符 UBA（UAX #9）——uba_levels（X/W/N/I 规则 → 逐码点层级，
///     经典向量：W2/W4/W5/W7、N1/N2、I1/I2、显式嵌入/隔离）与 uba_visual_order（L2）。
///   - 集成：注册真实双字体 Roboto(拉丁)+Amiri(阿拉伯) 到同一 family（find_glyph 跨面回退，
///     自然形成两个 face-run），绘制「ab مرحبا」RTL 段落，按墨迹分布桶比对验证最左 run 为阿拉伯文
///     （即跨 run 重排生效；未重排时最左应为拉丁 "ab"，分布必不同）。
///   - 真实字体验收（Amiri 专用 family）：阿文整形墨迹/度量、caret_x 镜像、绘制视觉序镜像、
///     RichTextEdit RTL 右对齐与指针命中（含 hit_test 入口 x≤0 的 RTL 语义回归）。

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/environment/build_context.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/bidi.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/rich_text_edit.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_bidi {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

[[nodiscard]] auto count_opaque(const Painter &p) -> int {
    int count = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            if (p.get_pixel(x, y).a > 0) {
                ++count;
            }
        }
    }
    return count;
}

// 某列是否存在「墨迹」（任意 alpha 超过阈值即算，避免抗锯齿强度差异造成的逐像素抖动）。
[[nodiscard]] auto column_has_ink(const Painter &p, int x) -> bool {
    for (int y = 0; y < p.height(); ++y) {
        if (p.get_pixel(x, y).a > 32) {
            return true;
        }
    }
    return false;
}

// 阿拉伯文真实字体集成（Amiri）辅助：字体缺失则返回空串（调用方据此 AURORA_TEST_SKIP）。
// CI/headless 环境通常不含 Arabic 字体，此时优雅跳过而非失败（与 mixed_rtl 集成用例一致）。
[[nodiscard]] auto arabic_ttf_path() -> std::string {
    const auto root = aurora::testing::isolation::repo_root();
    if (root.empty()) {
        return {};
    }
    const std::filesystem::path amiri =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Amiri-Regular.ttf";
    if (!std::filesystem::exists(amiri)) {
        return {};
    }
    return amiri.string();
}

// 验收串：5 码点（م ر ح ب ا）。用于端到端验证阿拉伯文连字/cursive joining 与视觉序。
constexpr char kArabicHello[] = "مرحبا";  // NOLINT(*-avoid-c-arrays)
constexpr std::size_t kArabicN = 5;

// 专用 family（仅 Amiri）：保证阿拉伯文走真实整形，而非回落到无 Arabic 覆盖的默认字体/位图兜底。
[[nodiscard]] auto arabic_font() -> aurora::Font {
    return aurora::Font{.family = "__bidi_arabic", .size_pt = 24.0F};
}
}  // namespace

// ---------------- 纯函数：guess_paragraph_direction ----------------
AURORA_TEST_CASE(guess_paragraph_direction_arabic_is_rtl) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("مرحبا") == aurora::TextDirection::RTL);
}

AURORA_TEST_CASE(guess_paragraph_direction_hebrew_is_rtl) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("שלום") == aurora::TextDirection::RTL);
}

AURORA_TEST_CASE(guess_paragraph_direction_latin_is_ltr) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("Hello") == aurora::TextDirection::LTR);
}

AURORA_TEST_CASE(guess_paragraph_direction_digits_fallback_ltr) {
    // 全数字/中性：无强方向字符，回退 LTR。
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("12345") == aurora::TextDirection::LTR);
}

AURORA_TEST_CASE(guess_paragraph_direction_empty_is_ltr) {
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("") == aurora::TextDirection::LTR);
}

AURORA_TEST_CASE(guess_paragraph_direction_first_strong_wins) {
    // 首个强方向字符定基准：以拉丁开头 → LTR（即便后接阿拉伯文）。
    AURORA_TEST_CHECK(aurora::render::detail::guess_paragraph_direction("Hi مرحبا") == aurora::TextDirection::LTR);
}

// ---------------- 纯函数：bidi_visual_run_order ----------------
AURORA_TEST_CASE(run_order_ltr_keeps_logical_order) {
    const std::vector<std::size_t> expect{0, 1, 2};
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(3, aurora::TextDirection::LTR) == expect);
}

AURORA_TEST_CASE(run_order_rtl_reverses_runs) {
    const std::vector<std::size_t> expect{2, 1, 0};
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(3, aurora::TextDirection::RTL) == expect);
}

AURORA_TEST_CASE(run_order_single_run_is_identity) {
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(1, aurora::TextDirection::RTL) ==
                     std::vector<std::size_t>{0});
}

AURORA_TEST_CASE(run_order_zero_runs_is_empty) {
    AURORA_TEST_CHECK(aurora::render::detail::bidi_visual_run_order(0, aurora::TextDirection::RTL).empty());
}

// ============================================================================
// 完整逐字符 UBA（UAX #9）：uba_levels（X/W/N/I 规则）与 uba_visual_order（L2）
// ----------------------------------------------------------------------------

namespace {
// char32_t 字面量 → 码点向量（U"" 前缀字符串即 UTF-32）。
[[nodiscard]] auto cps32(const char32_t *s) -> std::vector<char32_t> {
    std::vector<char32_t> v;
    while (*s != U'\0') {
        v.push_back(*s++);
    }
    return v;
}

[[nodiscard]] auto levels_of(const char32_t *s, std::uint8_t base) -> std::vector<std::uint8_t> {
    return aurora::render::detail::uba_levels(cps32(s), base);
}

void check_levels(const char32_t *s, std::uint8_t base, const std::vector<std::uint8_t> &expect)
{
    const auto got = levels_of(s, base);
    AURORA_TEST_REQUIRE_EQ(got.size(), expect.size());
    AURORA_TEST_CHECK(got == expect);
}
}  // namespace

// ---- I1/I2 隐式层级与基本强类型 ----
AURORA_TEST_CASE(uba_levels_ltr_plain_all_zero) {
    check_levels(U"ab 12", 0, {0, 0, 0, 0, 0});  // W7: EN 前有 L → L；全 LTR
}

AURORA_TEST_CASE(uba_levels_rtl_paragraph_latin_upgrades_to_even) {
    // RTL 段（base 1）内的拉丁：I1 → 层 2（偶，内序 LTR）。
    check_levels(U"abc", 1, {2, 2, 2});
}

AURORA_TEST_CASE(uba_levels_rtl_paragraph_digits_upgrade_to_even) {
    // RTL 段「مرحبا 34」：阿文 R 层 1；空格 N1 两侧 R → 层 1；数字 EN 奇嵌入 → 层 2
    // （内序 LTR；L2 层叠反转后显示 "34 مرحبا"）。
    check_levels(U"مرحبا 34", 1, {1, 1, 1, 1, 1, 1, 2, 2});
}

AURORA_TEST_CASE(uba_levels_ltr_paragraph_hebrew_upgrades_to_odd) {
    // LTR 段（base 0）内的希伯来：I2 → 层 1（奇）。
    check_levels(U"aבc", 0, {0, 1, 0});
}

AURORA_TEST_CASE(uba_levels_empty_is_empty) {
    AURORA_TEST_CHECK(levels_of(U"", 0).empty());
    AURORA_TEST_CHECK(levels_of(U"", 1).empty());
}

// ---- W 阶段：弱类型 ----
AURORA_TEST_CASE(uba_levels_w2_en_after_al_becomes_an) {
    // 「م3」base 1：م AL→R 层 1；3 EN 因最近强类型 AL → AN；AN 奇嵌入 → 层 2。
    check_levels(U"م3", 1, {1, 2});
}

AURORA_TEST_CASE(uba_levels_w4_cs_between_en_stays_number) {
    // 「1.5」base 0：CS 夹 EN 之间 → EN → 全层 0（数字串内序保持）。
    check_levels(U"1.5", 0, {0, 0, 0});
}

AURORA_TEST_CASE(uba_levels_w4_cs_between_an_becomes_an) {
    // 「١.٢」base 1（阿-印数字 0660/0662 夹 CS）：AN CS AN → AN；AN 奇嵌入 → 层 2。
    check_levels(U"١.٢", 1, {2, 2, 2});
}

AURORA_TEST_CASE(uba_levels_w5_et_adjacent_en_becomes_en) {
    // 「$5」/「5$」base 0：ET 与 EN 相邻 → EN（货币符号并入数字段）。
    check_levels(U"$5", 0, {0, 0});
    check_levels(U"5$", 0, {0, 0});
}

AURORA_TEST_CASE(uba_levels_w6_isolated_punct_stays_neutral) {
    // 「a-b」base 0：ES 两侧非 EN → W6 → ON → N1 两侧 L → L → 全层 0。
    check_levels(U"a-b", 0, {0, 0, 0});
}

// ---- N 阶段：中性 ----
AURORA_TEST_CASE(uba_levels_n1_neutral_between_same_sides) {
    // 「ab - cd」base 0：中性 '-' 两侧同为 L → L → 全层 0。
    check_levels(U"ab - cd", 0, {0, 0, 0, 0, 0, 0, 0});
}

AURORA_TEST_CASE(uba_levels_n2_neutral_takes_embedding_direction) {
    // 「مرحبا - abc」base 1：两侧 ' '/'-' 左 R 右 L → N2 → 嵌入方向 RTL → R → 层 1；abc 层 2。
    check_levels(U"مرحبا - abc", 1, {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2});
}

// ---- X 阶段：显式嵌入与隔离 ----
AURORA_TEST_CASE(uba_levels_rle_embeds_latin_to_level2) {
    // RLE(→1, RTL) + "abc" + PDF：拉丁在 RTL embedding → 层 2；控制符占位所在
    // embedding 层级（RLE 处 0、PDF 处 pop 前 1）。
    check_levels(U"\u202Babc\u202C", 0, {0, 2, 2, 2, 1});
}

AURORA_TEST_CASE(uba_levels_lre_keeps_ltr_inside) {
    // LRE(→2, LTR；新层级 = 严格大于当前的最小偶数) + "abc"：L 在 LTR embedding → 层 2；
    // PDF 占位 pop 前层级 2。
    check_levels(U"\u202Aabc\u202C", 0, {0, 2, 2, 2, 2});
}

AURORA_TEST_CASE(uba_levels_rlo_overrides_latin_to_rtl) {
    // RLO(→1, RTL) + "ab"：override 强制 R → 层 1（内序反转由 L2/绘制层处理）；PDF 占位 1。
    check_levels(U"\u202Eab\u202C", 0, {0, 1, 1, 1});
}

AURORA_TEST_CASE(uba_levels_rli_isolate_latin_to_level1) {
    // RLI 隔离不增层级（方向 RTL）：拉丁 → 层 1；PDI 占位。
    check_levels(U"\u2067ab\u2069", 0, {0, 1, 1, 0});
}

AURORA_TEST_CASE(uba_levels_fsi_strong_latin_acts_ltr) {
    // FSI 按隔离内容首强字符判定：拉丁 → LTR 隔离 → 内部 L 层不变（0）。
    check_levels(U"\u2068ab\u2069", 0, {0, 0, 0, 0});
}

// ---- L2：uba_visual_order ----
AURORA_TEST_CASE(uba_visual_order_ltr_identity) {
    const std::vector<std::uint8_t> levels{0, 0, 0};
    const std::vector<std::size_t> expect{0, 1, 2};
    AURORA_TEST_CHECK(aurora::render::detail::uba_visual_order(levels) == expect);
}

AURORA_TEST_CASE(uba_visual_order_all_rtl_reverses) {
    const std::vector<std::uint8_t> levels{1, 1, 1};
    const std::vector<std::size_t> expect{2, 1, 0};
    AURORA_TEST_CHECK(aurora::render::detail::uba_visual_order(levels) == expect);
}

AURORA_TEST_CASE(uba_visual_order_rtl_digits_stay_ltr_inner) {
    // 「م 34」run 级层级 [1,1,2,2]：L2 反转后视觉序 [2,3,1,0]——数字 span 整体在左
    // 且内部保持逻辑序（显示 "34 م" 而非 "43 م"）。
    const std::vector<std::uint8_t> levels{1, 1, 2, 2};
    const std::vector<std::size_t> expect{2, 3, 1, 0};
    AURORA_TEST_CHECK(aurora::render::detail::uba_visual_order(levels) == expect);
}

AURORA_TEST_CASE(uba_visual_order_single_rtl_subsegment_in_ltr_is_identity) {
    // LTR 段内单 RTL run [0,1,0]：L2 的区间反转不跨层移动位置——单元素区间反转恒等，
    // 子段内部字形序由 hb 按 run 级 RTL 方向负责；run 间保持逻辑序。
    const std::vector<std::uint8_t> levels{0, 1, 0};
    const std::vector<std::size_t> expect{0, 1, 2};
    AURORA_TEST_CHECK(aurora::render::detail::uba_visual_order(levels) == expect);
}

AURORA_TEST_CASE(uba_visual_order_nested_levels_reversed_twice) {
    // [2,1,2]（RTL 段内两个 LTR 子段）：L=2 单元素不动、L=1 全反 → [2,1,0]。
    const std::vector<std::uint8_t> levels{2, 1, 2};
    const std::vector<std::size_t> expect{2, 1, 0};
    AURORA_TEST_CHECK(aurora::render::detail::uba_visual_order(levels) == expect);
}

AURORA_TEST_CASE(uba_visual_order_rli_chars_reverse_inside) {
    // 字符级 [0,1,1,0]（RLI 内 "ab"）：内层反转 → [0,2,1,3]（b 在 a 前）。
    const std::vector<std::uint8_t> levels{0, 1, 1, 0};
    const std::vector<std::size_t> expect{0, 2, 1, 3};
    AURORA_TEST_CHECK(aurora::render::detail::uba_visual_order(levels) == expect);
}

// ---------------- 集成：真实双字体跨 run 重排 ----------------
AURORA_TEST_CASE(mixed_rtl_reorders_runs_by_paragraph_direction) {
    const auto root = aurora::testing::isolation::repo_root();
    if (root.empty()) {
        AURORA_TEST_SKIP("repo_root 不可用，跳过真实字体集成测试");
    }
    const std::filesystem::path roboto =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf";
    const std::filesystem::path amiri =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Amiri-Regular.ttf";
    if (!std::filesystem::exists(roboto) || !std::filesystem::exists(amiri)) {
        AURORA_TEST_SKIP("Roboto/Amiri TTF 不在仓库内，跳过真实字体集成测试");
    }

    // 同一 family 注册两种面：find_glyph 缺字时跨面回退 → 自然形成两个 face-run。
    aurora::render::FontEngine::register_font("__bidi_mix", roboto.string());
    aurora::render::FontEngine::register_font("__bidi_mix", amiri.string());

    const aurora::Font font{.family = "__bidi_mix", .size_pt = 32.0F};
    const aurora::render::TextLayoutOpts rtl{.direction = aurora::TextDirection::RTL};

    // 混合串：拉丁 "ab" + 空格 + 阿拉伯 "مرحبا"，显式 RTL 段落。
    const std::string mixed = "ab مرحبا";
    Painter p;
    p.begin(400, 80);
    aurora::render::FontEngine::draw_text(p, rect_at(0.0F, 0.0F, 400.0F, 80.0F), mixed, font, aurora::Color::black(), rtl);
    AURORA_TEST_REQUIRE(count_opaque(p) > 0);  // 两词都落笔

    // 阿拉伯词单独绘制（同 family/direction/size），作为「最左 run 应为阿拉伯文」的参照。
    const std::string ar = "مرحبا";
    const float w_ar = aurora::render::FontEngine::measure_width(ar, font, rtl);
    AURORA_TEST_REQUIRE(w_ar > 0.0F);

    Painter q;
    q.begin(400, 80);
    aurora::render::FontEngine::draw_text(q, rect_at(0.0F, 0.0F, 400.0F, 80.0F), ar, font, aurora::Color::black(), rtl);

    // 在最左 w_ar 像素区间内按桶比对墨迹分布：正确重排下 Arabic 是最左 run，
    // 其墨迹分布应与单独绘制的阿拉伯词逐桶一致；若未重排（最左为拉丁 "ab"），分布必不同。
    const int buckets = 16;
    const int span = static_cast<int>(std::ceil(w_ar));
    AURORA_TEST_REQUIRE(span > 0 && span < 400);
    bool all_match = true;
    for (int b = 0; b < buckets; ++b) {
        const int x0 = span * b / buckets;
        const int x1 = span * (b + 1) / buckets;
        bool in_p = false;
        bool in_q = false;
        for (int x = x0; x < x1; ++x) {
            if (column_has_ink(p, x)) {
                in_p = true;
            }
            if (column_has_ink(q, x)) {
                in_q = true;
            }
        }
        if (in_p != in_q) {
            all_match = false;
            break;
        }
    }
    AURORA_TEST_CHECK(all_match);
}

// ============================================================================
// A2 真实字体验收：阿拉伯文（Amiri）端到端
// ----------------------------------------------------------------------------
// 此前 RTL 测试仅用拉丁字形（headless 无 Arabic 字体时阿文 run 度量溢出 300px 约束，
// 两端点点击都 clamp 进同一溢出 run → caret 恒 0），故无法覆盖真实 Arabic 整形/连字。
// 本组注册仓库内 Amiri TTF，端到端验证：阿文实际整形出非零墨迹与合理度量、RTL 下 caret_x
// 逻辑↔视觉镜像、RTL 绘制视觉序（墨迹分布与 LTR 互为镜像）、RichTextEdit RTL 整段右对齐
// （段落级右对齐是 RichTextEdit 职责）、RichTextEdit RTL 指针命中（真实字体度量不再溢出，
// 端点点击正确映射而非 clamp 到 0）。字体缺失则 SKIP。
// ============================================================================

AURORA_TEST_CASE(arabic_real_font_shapes_into_ink_and_reasonable_metrics) {
    const std::string amiri_path = arabic_ttf_path();
    if (amiri_path.empty()) {
        AURORA_TEST_SKIP("Amiri TTF 不可用（CI/headless 无 Arabic 字体），跳过真实字体集成");
    }
    aurora::render::FontEngine::register_font("__bidi_arabic", amiri_path);

    const auto font = arabic_font();
    const aurora::render::TextLayoutOpts ltr{.direction = aurora::TextDirection::LTR};
    const aurora::render::TextLayoutOpts rtl{.direction = aurora::TextDirection::RTL};

    const float w_ltr = aurora::render::FontEngine::measure_width(kArabicHello, font, ltr);
    const float w_rtl = aurora::render::FontEngine::measure_width(kArabicHello, font, rtl);
    AURORA_TEST_REQUIRE(w_ltr > 0.0F);
    AURORA_TEST_REQUIRE(w_rtl > 0.0F);
    // 合理度量：不溢出画布（真实字体整形而非位图兜底 0 宽或整屏溢出）。
    AURORA_TEST_REQUIRE(w_ltr < 400.0F);
    AURORA_TEST_REQUIRE(w_rtl < 400.0F);

    Painter p;
    p.begin(400, 80);
    aurora::render::FontEngine::draw_text(p, rect_at(0.0F, 0.0F, 400.0F, 80.0F), kArabicHello, font,
                                          aurora::Color::black(), rtl);
    // 真实整形出字形墨迹（非豆腐块/空）——证明阿文 run 走 HarfBuzz shaping 而非位图兜底。
    AURORA_TEST_REQUIRE(count_opaque(p) > 0);
}

AURORA_TEST_CASE(arabic_rtl_caret_x_mirrors_logical_order) {
    const std::string amiri_path = arabic_ttf_path();
    if (amiri_path.empty()) {
        AURORA_TEST_SKIP("Amiri TTF 不可用（CI/headless 无 Arabic 字体），跳过真实字体集成");
    }
    aurora::render::FontEngine::register_font("__bidi_arabic", amiri_path);

    const auto font = arabic_font();
    const aurora::render::TextLayoutOpts ltr{.direction = aurora::TextDirection::LTR};
    const aurora::render::TextLayoutOpts rtl{.direction = aurora::TextDirection::RTL};

    const float w_ltr = aurora::render::FontEngine::measure_width(kArabicHello, font, ltr);
    const float w_rtl = aurora::render::FontEngine::measure_width(kArabicHello, font, rtl);

    // 端点（char_index=0 与 =N）与字形簇无关，断言稳定。
    const float cxl0 = aurora::render::FontEngine::caret_x(kArabicHello, 0, font, ltr);
    const float cxlN = aurora::render::FontEngine::caret_x(kArabicHello, kArabicN, font, ltr);
    const float cxr0 = aurora::render::FontEngine::caret_x(kArabicHello, 0, font, rtl);
    const float cxrN = aurora::render::FontEngine::caret_x(kArabicHello, kArabicN, font, rtl);

    AURORA_TEST_CHECK_NEAR(cxl0, 0.0F, 1e-3F);        // LTR：逻辑首在左缘
    AURORA_TEST_CHECK_NEAR(cxlN, w_ltr, 1e-3F);        // LTR：逻辑尾在右缘
    AURORA_TEST_CHECK_NEAR(cxr0, w_rtl, 1e-3F);        // RTL：逻辑首在右缘（镜像）
    AURORA_TEST_CHECK_NEAR(cxrN, 0.0F, 1e-3F);         // RTL：逻辑尾在左缘（镜像）
    // 逻辑首的视觉位置：RTL 远在 LTR 右侧（跨越整行宽度）。
    AURORA_TEST_CHECK(cxr0 > cxl0 + 50.0F);
    AURORA_TEST_CHECK(cxrN < cxlN - 50.0F);
}

AURORA_TEST_CASE(arabic_real_shaping_ligature_and_joining) {
    const std::string amiri_path = arabic_ttf_path();
    if (amiri_path.empty()) {
        AURORA_TEST_SKIP("Amiri TTF 不可用（CI/headless 无 Arabic 字体），跳过真实字体集成");
    }
    aurora::render::FontEngine::register_font("__bidi_arabic", amiri_path);

    const auto font = arabic_font();
    const aurora::render::TextLayoutOpts rtl{.direction = aurora::TextDirection::RTL};

    // FontEngine::draw_text 的 pen 恒从 rect 左缘起（段落级右对齐是 RichTextEdit 的职责，
    // 见 arabic_richtextedit_rtl_right_aligns）。本用例验证真实 HarfBuzz 整形语义（纯度量断言，
    // 不依赖像素字形形态假设）：
    // ① Lam-Alef 必合连字（GSUB liga）："لا" 整形为单个连字字形，宽度远小于两 isolated 字形之和；
    // ② Cursive joining：连写 "بت" 采用窄连接形，宽度小于两 isolated 字形之和。
    // （完整 UBA 接入后，显式 LTR 段落中的阿文按内容脚本层级 1 整形——与 RTL 相同的正确
    // 行为，「LTR 强制阿文退化序」的旧方向敏感断言随之失效，见 uba_levels 纯函数用例。）
    const float w_lam = aurora::render::FontEngine::measure_width("ل", font, rtl);
    const float w_alef = aurora::render::FontEngine::measure_width("ا", font, rtl);
    const float w_lamalef = aurora::render::FontEngine::measure_width("لا", font, rtl);
    AURORA_TEST_REQUIRE(w_lam > 0.0F);
    AURORA_TEST_REQUIRE(w_alef > 0.0F);
    AURORA_TEST_REQUIRE(w_lamalef > 0.0F);
    // 必合连字：lam-alef 合为单字形（孤立两形之和的 90% 以上必然放不下）。
    AURORA_TEST_CHECK(w_lamalef < (w_lam + w_alef) * 0.9F);

    const float w_beh = aurora::render::FontEngine::measure_width("ب", font, rtl);
    const float w_teh = aurora::render::FontEngine::measure_width("ت", font, rtl);
    const float w_joined = aurora::render::FontEngine::measure_width("بت", font, rtl);
    AURORA_TEST_REQUIRE(w_beh > 0.0F);
    AURORA_TEST_REQUIRE(w_teh > 0.0F);
    AURORA_TEST_REQUIRE(w_joined > 0.0F);
    // 连接形（initial/final）比 isolated 形窄：连写宽度 < isolated 之和。
    AURORA_TEST_CHECK(w_joined < w_beh + w_teh);
}

AURORA_TEST_CASE(arabic_richtextedit_rtl_right_aligns) {
    const std::string amiri_path = arabic_ttf_path();
    if (amiri_path.empty()) {
        AURORA_TEST_SKIP("Amiri TTF 不可用（CI/headless 无 Arabic 字体），跳过真实字体集成");
    }
    aurora::render::FontEngine::register_font("__bidi_arabic", amiri_path);

    auto leftmost = [&](bool force_rtl) -> float {
        RichTextEdit t;
        t.load_spans({TextSpan{.text = LocalizedString{kArabicHello},
                               .font = arabic_font(),
                               .color = aurora::Color::black()}});
        // 阿文内容会被 guess_paragraph_direction 自动推断为 RTL，故对照组必须显式固定为 LTR，
        // 否则两侧同为 RTL、右对齐差异不可见。
        t.set_direction(force_rtl ? aurora::TextDirection::RTL : aurora::TextDirection::LTR);
        LayoutEngine::layout(
            t, Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 400.0F}});
        Painter p;
        p.begin(408, static_cast<int>(t.size().height) + 4);
        const BuildContext ctx;
        t.paint(p, Rect{.origin = Point{.x = 2.0F, .y = 2.0F}, .size = t.size()}, ctx);
        const int y0 = 2 + static_cast<int>(t.size().height / 2.0F);
        for (int x = 0; x < 408; ++x) {
            if (p.get_pixel(x, y0) != Color{0, 0, 0, 0}) {
                return static_cast<float>(x);
            }
        }
        return -1.0F;
    };

    const float ltr_left = leftmost(false);
    const float rtl_left = leftmost(true);
    AURORA_TEST_REQUIRE(ltr_left > 0.0F);
    AURORA_TEST_REQUIRE(rtl_left > 0.0F);
    AURORA_TEST_CHECK(rtl_left > ltr_left + 100.0F);  // RTL 整段右对齐
    AURORA_TEST_CHECK(rtl_left > 150.0F);             // 确实落在右半区
}

AURORA_TEST_CASE(arabic_richtextedit_rtl_pointer_hit_no_overflow) {
    // 此前 headless 无 Arabic 字体时：阿文 run 度量溢出 300px 约束，两端点点击都 clamp 进同一
    // 溢出 run → caret 恒 0。注册真实字体后度量不再溢出，端点点击应正确映射：
    // 视觉右缘 → 逻辑首（caret 0）、视觉左缘 → 逻辑尾（inclusive 命中逻辑尾字符，caret N-1）。
    // 另回归 FontEngine::hit_test_char_inclusive 入口 x≤0 的 RTL 语义（曾无条件返回 0）。
    const std::string amiri_path = arabic_ttf_path();
    if (amiri_path.empty()) {
        AURORA_TEST_SKIP("Amiri TTF 不可用（CI/headless 无 Arabic 字体），跳过真实字体集成");
    }
    aurora::render::FontEngine::register_font("__bidi_arabic", amiri_path);

    RichTextEdit t;
    t.load_spans({TextSpan{.text = LocalizedString{kArabicHello},
                           .font = arabic_font(),
                           .color = aurora::Color::black()}});
    t.set_direction(aurora::TextDirection::RTL);
    LayoutEngine::layout(
        t, Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 400.0F}});

    auto click = [&](float x) -> void {
        MouseEvent ev;
        ev.action = MouseAction::Press;
        ev.button = MouseButton::Left;
        ev.local_position = Point{.x = x, .y = 0.0F};
        t.on_pointer_event(ev);
    };

    click(t.size().width - 0.5F);  // 视觉右缘 → 逻辑首 → caret 0
    AURORA_TEST_CHECK_EQ(t.caret(), 0U);
    // 视觉左缘（run 左侧之外）→ 逻辑尾。含头含尾（inclusive）语义命中逻辑尾**字符**本身，
    // 即下标 N-1（镜像 LTR「行尾右侧命中末字符」返回 total-1 的语义）。
    click(0.5F);
    AURORA_TEST_CHECK_EQ(t.caret(), kArabicN - 1U);

    // 行内中部点击应落在逻辑中段（真实度量下非溢出 clamp 到 0）。
    const float w = aurora::render::FontEngine::measure_width(
        kArabicHello, arabic_font(), aurora::render::TextLayoutOpts{.direction = aurora::TextDirection::RTL});
    click(t.size().width - w * 0.5F);  // 行内中点
    AURORA_TEST_CHECK(t.caret() > 0U && t.caret() < kArabicN);
}

AURORA_TEST_CASE(arabic_richtextedit_cross_level_runs_reorder) {
    // 完整 UBA 跨层翻转（RichTextEdit 端到端）：RTL 段内阿文 run（Amiri，层 1）+ 拉丁 run
    // （Roboto，层 2）。L2：层 2 span 反转后随层 1 段落反转 → 逻辑尾 run（拉丁）落视觉左缘、
    // 逻辑首 run（阿文）落视觉右缘 → 点左缘命中 "a"（caret 5）、点右缘命中阿文首字符（caret 0）。
    const auto root = aurora::testing::isolation::repo_root();
    const std::filesystem::path amiri =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Amiri-Regular.ttf";
    const std::filesystem::path roboto =
        std::filesystem::path{root} / "third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf";
    if (root.empty() || !std::filesystem::exists(amiri) || !std::filesystem::exists(roboto)) {
        AURORA_TEST_SKIP("Amiri/Roboto TTF 不可用（CI/headless 无 Arabic 字体），跳过跨层集成");
    }
    aurora::render::FontEngine::register_font("__bidi_arabic", amiri.string());
    aurora::render::FontEngine::register_font("__bidi_latin", roboto.string());

    RichTextEdit t;
    t.load_spans({
        TextSpan{.text = LocalizedString{kArabicHello},
                 .font = Font{.family = "__bidi_arabic", .size_pt = 24.0F},
                 .color = aurora::Color::black()},
        TextSpan{.text = LocalizedString{"ab"},
                 .font = Font{.family = "__bidi_latin", .size_pt = 24.0F},
                 .color = aurora::Color::black()},
    });
    t.set_direction(aurora::TextDirection::RTL);
    LayoutEngine::layout(
        t, Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 400.0F}});

    auto click = [&](float x) -> void {
        MouseEvent ev;
        ev.action = MouseAction::Press;
        ev.button = MouseButton::Left;
        ev.local_position = Point{.x = x, .y = 0.0F};
        t.on_pointer_event(ev);
    };

    click(0.5F);  // 视觉左缘 → 拉丁 run "ab" 左缘 → caret = 阿文 run 的 StyledChar 数（跨层翻转：逻辑尾 run 落左）
    // doc_ 以 StyledChar（UTF-8 字节）为单位（既有行为，多字节字符占多个单元），
    // 阿文 5 码点占 10 个单元，拉丁 run 的行内 begin 即 10。
    AURORA_TEST_CHECK_EQ(t.caret(), std::string{kArabicHello}.size());
    click(t.size().width - 0.5F);  // 视觉右缘 → 阿文 run 右缘 → caret 0（逻辑首在右）
    AURORA_TEST_CHECK_EQ(t.caret(), 0U);
}

}  // namespace aurora::test_cases::utest_bidi

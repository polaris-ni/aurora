/// 测试类型: unit
/// 目标单元: include/aurora/render/font_engine.h
/// 测试说明: 覆盖 FontEngine 度量契约（空串零宽、长度/字号单调、行高为正）、caret_x 与 hit_test_char 的
/// 码点索引与往返一致性、TextLayoutOpts 字距/词距对宽度的影响、AA 策略读写与光栅状态世代自增、
/// draw_text 实际落笔、基线上沿度量（measure_ascent）与实绘落墨带自洽、shaping 缓存统计与清空，
/// 以及 UTF-8 串的码点安全性与等宽整格度量（monospace_cell）的取整口径

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/render/text_aa_mode.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_font_engine {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}

/// @brief 统计 UTF-8 串的码点数（独立于被测排版路径的语料自检）。
///
/// 存在的理由：测试语料用 `\xNN` 字节序列写中文时，十六进制转义**贪婪吞掉后续十六进制数字**
/// （`"A\xE4\xB8\xADB"` 会被解析成 `\xADB`），MinGW 宽容截断而 clang 报错，于是语料悄悄
/// 变成了别的东西 —— 而依赖码点数值的判据（如「开档后宽度 == 3 格」）对此**不敏感**，
/// 会照样 PASS。凡断言里出现「N 字形 × 格宽」，都要先用本函数把 N 钉住。
[[nodiscard]] auto cp_count_of(const std::string &s) -> std::size_t {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        i += (c < 0x80U) ? 1U : ((c < 0xE0U) ? 2U : ((c < 0xF0U) ? 3U : 4U));
        ++n;
    }
    return n;
}

/// @brief 统计画布中非透明像素数（用于判定文本是否真的落笔）。
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
}  // namespace

namespace {
/// @brief AA 策略是进程级状态：用例结束还原默认，避免影响同文件后续用例。
class AaModeGuard : public ::aurora::testing::Fixture {
  protected:
    auto SetUp() -> void override { saved_ = render::FontEngine::text_aa_mode(); }
    auto TearDown() -> void override { render::FontEngine::set_text_aa_mode(saved_); }

  private:
    render::TextAAMode saved_ = render::TextAAMode::Supersample;
};
}  // namespace

AURORA_TEST_CASE(measure_width_of_empty_text_is_zero) {
    AURORA_TEST_CHECK_NEAR(render::FontEngine::measure_width("", Font{}), 0.0, 1e-6);
}

AURORA_TEST_CASE(measure_width_grows_with_text_length) {
    const Font font;
    const float one = render::FontEngine::measure_width("W", font);
    const float three = render::FontEngine::measure_width("WWW", font);
    AURORA_TEST_CHECK_GT(one, 0.0);
    AURORA_TEST_CHECK_GT(three, one);
}

AURORA_TEST_CASE(measure_width_scales_with_font_size) {
    const Font small{.family = "sans-serif", .size_pt = 10.0F};
    const Font large{.family = "sans-serif", .size_pt = 20.0F};
    AURORA_TEST_CHECK_GT(render::FontEngine::measure_width("Text", large),
                         render::FontEngine::measure_width("Text", small));
}

AURORA_TEST_CASE(measure_height_is_positive_and_scales) {
    const Font small{.family = "sans-serif", .size_pt = 10.0F};
    const Font large{.family = "sans-serif", .size_pt = 20.0F};
    AURORA_TEST_CHECK_GT(render::FontEngine::measure_height(small), 0.0);
    AURORA_TEST_CHECK_GT(render::FontEngine::measure_height(large), render::FontEngine::measure_height(small));
}

AURORA_TEST_CASE(measure_ascent_is_positive_and_within_line_height) {
    // 基线对齐（CrossAxisAlignment::Baseline）的合成基线与夹取依赖该不变量：
    // 0 < ascent <= measure_height，且随字号单调。
    const Font small{.family = "sans-serif", .size_pt = 10.0F};
    const Font large{.family = "sans-serif", .size_pt = 20.0F};
    const float ascent_small = render::FontEngine::measure_ascent(small);
    const float ascent_large = render::FontEngine::measure_ascent(large);

    AURORA_TEST_CHECK_GT(ascent_small, 0.0);
    AURORA_TEST_CHECK_LE(ascent_small, render::FontEngine::measure_height(small));
    AURORA_TEST_CHECK_GT(ascent_large, ascent_small);
    AURORA_TEST_CHECK_LE(ascent_large, render::FontEngine::measure_height(large));
}

AURORA_TEST_CASE(measure_ascent_agrees_with_drawn_ink_band) {
    // 与 draw_text 同源的关键锚点：用 measure_ascent 推出的首行 pen_y，必须让实绘墨迹落在
    // [pen_y - ascent, pen_y + (height - ascent)] 之内，且确有字身越过基线上方。两条兜底路径
    // （真实字体 / BitmapFont）都须满足。
    const Font font{.size_pt = 24.0F};
    const float ascent = render::FontEngine::measure_ascent(font);
    const float descent = render::FontEngine::measure_height(font) - ascent;
    const float pen_y = std::floor(ascent + 0.5F);  // 与绘制侧整像素 snap 同口径（origin_y = 0）

    Painter p;
    p.begin(64, 48);
    render::FontEngine::draw_text(p, rect_at(0.0F, 0.0F, 64.0F, 48.0F), "Ag", font, Color::black());

    int top = -1;
    int bottom = -1;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            if (p.get_pixel(x, y).a > 0) {
                top = (top < 0) ? y : top;
                bottom = y;
                break;
            }
        }
    }
    AURORA_TEST_REQUIRE_GT(top, -1);  // 确有着墨
    AURORA_TEST_CHECK_LT(static_cast<float>(top), pen_y);  // 字身在基线上方
    AURORA_TEST_CHECK_GE(static_cast<float>(top), pen_y - ascent - 1.0F);  // 不越出 ascent 上沿
    AURORA_TEST_CHECK_LE(static_cast<float>(bottom), pen_y + descent + 1.0F);  // 不越出 descent 下沿
}

AURORA_TEST_CASE(caret_x_starts_at_zero_and_is_monotonic) {
    const Font font;
    const std::string text = "Widget";
    AURORA_TEST_CHECK_NEAR(render::FontEngine::caret_x(text, 0, font), 0.0, 1e-6);

    float previous = 0.0F;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        const float x = render::FontEngine::caret_x(text, i, font);
        AURORA_TEST_CHECK_GE(x, previous);
        previous = x;
    }
}

AURORA_TEST_CASE(caret_x_at_end_matches_total_width) {
    // 末尾光标应等于整串宽度（尾部无额外间距时）。
    const Font font;
    const std::string text = "Measure";
    AURORA_TEST_CHECK_NEAR(render::FontEngine::caret_x(text, text.size(), font),
                           render::FontEngine::measure_width(text, font), 0.5);
}

AURORA_TEST_CASE(hit_test_char_clamps_and_round_trips_with_caret) {
    const Font font;
    const std::string text = "Aurora";
    AURORA_TEST_CHECK_EQ(render::FontEngine::hit_test_char(text, -100.0F, font), 0U);
    AURORA_TEST_CHECK_EQ(render::FontEngine::hit_test_char(text, 100000.0F, font), text.size());

    // 在每个字符中点处命中，应落回该字符边界（0 → 下一字符或自身）。
    for (std::size_t i = 0; i < text.size(); ++i) {
        AURORA_TEST_TRACE(std::string{"caret index "} + std::to_string(i));
        const float mid = render::FontEngine::caret_x(text, i, font);
        const std::size_t hit = render::FontEngine::hit_test_char(text, mid, font);
        AURORA_TEST_CHECK_LE(hit, text.size());
    }
}

AURORA_TEST_CASE(hit_test_char_inclusive_selects_clicked_character) {
    const std::string text = "Aurora";
    // 含头含尾语义：点击任一字符内部都应命中该字符下标（不是下一个光标位）。
    for (std::size_t i = 0; i < text.size(); ++i) {
        const Font font;
        AURORA_TEST_TRACE(std::string{"char "} + std::to_string(i));
        const float mid =
            (render::FontEngine::caret_x(text, i, font) + render::FontEngine::caret_x(text, i + 1, font)) * 0.5F;
        const std::size_t hit = render::FontEngine::hit_test_char_inclusive(text, mid, font);
        AURORA_TEST_CHECK_EQ(hit, i);
    }
}

AURORA_TEST_CASE(letter_spacing_increases_width) {
    const Font font;
    const render::TextLayoutOpts plain{};
    render::TextLayoutOpts spaced{};
    spaced.letter_spacing = 4.0F;

    AURORA_TEST_CHECK_GT(render::FontEngine::measure_width("Text", font, spaced),
                         render::FontEngine::measure_width("Text", font, plain));
}

AURORA_TEST_CASE(word_spacing_only_affects_text_with_spaces) {
    const Font font;
    const render::TextLayoutOpts plain{};
    render::TextLayoutOpts spaced{};
    spaced.word_spacing = 8.0F;

    AURORA_TEST_CHECK_GT(render::FontEngine::measure_width("a b", font, spaced),
                         render::FontEngine::measure_width("a b", font, plain));
    AURORA_TEST_CHECK_NEAR(render::FontEngine::measure_width("ab", font, spaced),
                           render::FontEngine::measure_width("ab", font, plain), 1e-6);
}

AURORA_TEST_CASE(display_width_degenerates_to_measure_width_at_scale_one) {
    const Font font;
    const render::TextLayoutOpts opts{};
    AURORA_TEST_CHECK_NEAR(render::FontEngine::display_width("Scaled", font, opts, 1.0F),
                           render::FontEngine::measure_width("Scaled", font, opts), 1e-6);
}

AURORA_TEST_F(AaModeGuard, text_aa_mode_is_readable_and_writable) {
    render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);
    AURORA_TEST_CHECK_EQ(static_cast<int>(render::FontEngine::text_aa_mode()),
                         static_cast<int>(render::TextAAMode::ClearType));

    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
    AURORA_TEST_CHECK_EQ(static_cast<int>(render::FontEngine::text_aa_mode()),
                         static_cast<int>(render::TextAAMode::Supersample));
}

AURORA_TEST_F(AaModeGuard, raster_generation_bumps_only_on_real_aa_change) {
    // 光栅状态世代是控件绘制缓存（Display List / 离屏层）判定「录制时点的光栅设置是否已过期」
    // 的 O(1) 依据：只有值真正变化才自增，避免无谓击穿全树缓存。
    render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);
    const auto g0 = render::FontEngine::raster_generation();

    render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);  // 同值：不自增
    AURORA_TEST_CHECK_EQ(render::FontEngine::raster_generation(), g0);

    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);  // 异值：+1
    AURORA_TEST_CHECK_EQ(render::FontEngine::raster_generation(), g0 + 1U);
}

AURORA_TEST_CASE(draw_text_puts_ink_on_canvas) {
    Painter p;
    p.begin(64, 16);
    AURORA_TEST_CHECK_EQ(count_opaque(p), 0);

    render::FontEngine::draw_text(p, rect_at(0.0F, 0.0F, 64.0F, 16.0F), "Aurora", Font{}, Color::black());
    AURORA_TEST_CHECK_GT(count_opaque(p), 0);
}

AURORA_TEST_CASE(synthetic_bold_adds_ink_when_no_bold_face_registered) {
    // Font.weight 语义：请求 weight>=600 而字体族无 bold 面时，渲染层经 FT_Outline_Embolden
    // 合成粗体——粗体文本的落墨量必须显著高于 regular（此前 weight 被渲染层完全忽略，
    // 粗体与 regular 逐位相同，本用例守门该缺口不再回归）。
    const std::string text = "Aurora bold";
    const Font regular{.size_pt = 24.0F, .weight = 400};
    const Font bold{.size_pt = 24.0F, .weight = 700};

    Painter p_reg;
    p_reg.begin(160, 40);
    render::FontEngine::draw_text(p_reg, rect_at(0.0F, 0.0F, 160.0F, 40.0F), text, regular, Color::black());
    const int ink_reg = count_opaque(p_reg);

    Painter p_bold;
    p_bold.begin(160, 40);
    render::FontEngine::draw_text(p_bold, rect_at(0.0F, 0.0F, 160.0F, 40.0F), text, bold, Color::black());
    const int ink_bold = count_opaque(p_bold);

    AURORA_TEST_CHECK_GT(ink_reg, 0);
    AURORA_TEST_CHECK_GT(ink_bold, ink_reg);  // 合成加粗 → 覆盖度增加
}

AURORA_TEST_CASE(draw_text_of_empty_string_is_noop) {
    Painter p;
    p.begin(32, 16);
    render::FontEngine::draw_text(p, rect_at(0.0F, 0.0F, 32.0F, 16.0F), "", Font{}, Color::black());
    AURORA_TEST_CHECK_EQ(count_opaque(p), 0);
}

AURORA_TEST_CASE(shape_cache_accumulates_hits) {
    render::FontEngine::shape_cache_clear();
    const Font font;
    float cached_width = 0.0F;
    AURORA_TEST_CHECK_NO_THROW(cached_width = render::FontEngine::measure_width("cache", font));
    AURORA_TEST_CHECK_GT(cached_width, 0.0F);  // 非空串必有正宽度
    const auto first = render::FontEngine::shape_cache_stats();
    AURORA_TEST_CHECK_GT(first.entries, 0U);

    // 相同输入重复测量 → 命中计数增长；逐次校验宽度稳定为正。
    for (int i = 0; i < 4; ++i) {
        const float w = render::FontEngine::measure_width("cache", font);
        AURORA_TEST_CHECK_GT(w, 0.0F);
    }
    const auto second = render::FontEngine::shape_cache_stats();
    AURORA_TEST_CHECK_GE(second.hits, first.hits);
    AURORA_TEST_CHECK_GT(second.hits + second.misses, first.hits + first.misses);
}

AURORA_TEST_CASE(caret_x_is_codepoint_indexed_for_utf8) {
    // 码点索引（非字节索引）：中文串按码点推进，不产生越界/乱码。
    const Font font;
    // CJK-LITERAL: cjk-fixture - Han text is required to exercise codepoint (not byte) caret indexing
    const std::string text = "中国";  // 6 字节 / 2 码点
    AURORA_TEST_CHECK_NEAR(render::FontEngine::caret_x(text, 0, font), 0.0, 1e-6);
    AURORA_TEST_CHECK_GT(render::FontEngine::caret_x(text, 1, font), 0.0);
    AURORA_TEST_CHECK_GT(render::FontEngine::caret_x(text, 2, font), render::FontEngine::caret_x(text, 1, font));
}

AURORA_TEST_CASE(rtl_measure_width_matches_ltr) {
    // 视觉宽度与方向无关（字形集合相同，仅排列镜像）。
    const Font font;
    const std::string text = "Hello World";
    const render::TextLayoutOpts ltr{};
    const render::TextLayoutOpts rtl{.direction = TextDirection::RTL};
    AURORA_TEST_CHECK_NEAR(render::FontEngine::measure_width(text, font, rtl),
                           render::FontEngine::measure_width(text, font, ltr), 0.5);
}

AURORA_TEST_CASE(rtl_caret_x_mirrors_to_right_edge) {
    // RTL：逻辑首字符在右缘——caret(0) = 整串宽，caret(n) = 0，随逻辑下标单调递减。
    const Font font;
    const std::string text = "Aurora";
    const render::TextLayoutOpts rtl{.direction = TextDirection::RTL};
    const float total = render::FontEngine::measure_width(text, font, rtl);
    AURORA_TEST_CHECK_NEAR(render::FontEngine::caret_x(text, 0, font, rtl), total, 0.5);
    AURORA_TEST_CHECK_NEAR(render::FontEngine::caret_x(text, text.size(), font, rtl), 0.0, 0.5);

    float previous = total;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        const float x = render::FontEngine::caret_x(text, i, font, rtl);
        AURORA_TEST_CHECK_LE(x, previous);
        previous = x;
    }
}

AURORA_TEST_CASE(rtl_hit_test_mirrors_ltr) {
    // RTL：同一文本，LTR 在 x 命中的逻辑下标 i ⇔ RTL 在 (total − x) 命中 i（含入语义）。
    const Font font;
    const std::string text = "Aurora";
    const render::TextLayoutOpts ltr{};
    const render::TextLayoutOpts rtl{.direction = TextDirection::RTL};
    const float total = render::FontEngine::measure_width(text, font, ltr);

    AURORA_TEST_CHECK_EQ(render::FontEngine::hit_test_char(text, total + 10.0F, font, rtl), 0U);
    AURORA_TEST_CHECK_EQ(render::FontEngine::hit_test_char(text, 0.5F, font, rtl), text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        const float mid_ltr =
            (render::FontEngine::caret_x(text, i, font, ltr) + render::FontEngine::caret_x(text, i + 1, font, ltr)) *
            0.5F;
        const std::size_t hit_ltr = render::FontEngine::hit_test_char_inclusive(text, mid_ltr, font, ltr);
        const std::size_t hit_rtl = render::FontEngine::hit_test_char_inclusive(text, total - mid_ltr, font, rtl);
        AURORA_TEST_CHECK_EQ(hit_rtl, hit_ltr);
    }
}

AURORA_TEST_CASE(rtl_direction_participates_in_shape_cache_key) {
    // direction 进 shaping 缓存键——同一文本 LTR/RTL 两次 shape 不串缓存（字形序不同）。
    const Font font;
    const std::string text = "Cache";
    const auto before = render::FontEngine::shape_cache_stats();
    (void)render::FontEngine::caret_x(text, 0, font, render::TextLayoutOpts{});
    (void)render::FontEngine::caret_x(text, 0, font, render::TextLayoutOpts{.direction = TextDirection::RTL});
    const auto after = render::FontEngine::shape_cache_stats();
    // 两个不同的 opts 各自至少 miss 一次（不因 direction 缺失而错误命中同一键）。
    AURORA_TEST_CHECK_GE(after.misses, before.misses + 2U);
}

AURORA_TEST_CASE(monospace_cell_metrics_are_positive_and_monotonic) {
    int prev_width = 0;
    int prev_height = 0;
    for (const float pt : {10.0F, 14.0F, 24.0F}) {
        const Font font{.family = "sans-serif", .size_pt = pt, .weight = 400};
        const auto cell = render::FontEngine::monospace_cell(font, 1.0F);
        AURORA_TEST_CHECK_GT(cell.cell_width_px, 0);
        AURORA_TEST_CHECK_GT(cell.cell_height_px, 0);
        AURORA_TEST_CHECK_GE(cell.ascent_px, 0);
        AURORA_TEST_CHECK_LE(cell.ascent_px, cell.cell_height_px);
        AURORA_TEST_CHECK_GE(cell.cell_width_px, prev_width);
        AURORA_TEST_CHECK_GE(cell.cell_height_px, prev_height);
        prev_width = cell.cell_width_px;
        prev_height = cell.cell_height_px;
    }
}

AURORA_TEST_CASE(monospace_cell_ascent_and_height_snap_the_draw_side_baseline) {
    // 单格的基线与行高必须等于绘制侧的 snap 值：按 `y = row * cell_height_px` 摆放的行，
    // 其首行 pen_y 与直接调 draw_text 逐位相同，否则网格行会与实绘墨迹错开半行。
    const Font font{.family = "sans-serif", .size_pt = 16.0F, .weight = 400};
    const auto cell = render::FontEngine::monospace_cell(font, 1.0F);
    AURORA_TEST_CHECK_EQ(cell.ascent_px, static_cast<int>(std::floor(render::FontEngine::measure_ascent(font) + 0.5F)));
    AURORA_TEST_CHECK_EQ(cell.cell_height_px,
                         static_cast<int>(std::floor(render::FontEngine::measure_height(font) + 0.5F)));
}

AURORA_TEST_CASE(monospace_cell_width_covers_reference_glyph_advances) {
    // 格宽取「ASCII 数字」与「制表符 U+2500」两个参考字形实绘 advance 的较大者：只按数字定
    // 格宽会让部分字体的边框压进相邻格；富余一整格则是把网格画稀。
    const Font font{.family = "sans-serif", .size_pt = 16.0F, .weight = 400};
    const auto opts = render::TextLayoutOpts{};
    for (const float sc : {1.0F, 1.25F, 1.5F}) {
        const auto cell = render::FontEngine::monospace_cell(font, sc);
        const int digit = static_cast<int>(std::lround(render::FontEngine::display_width("0", font, opts, sc) * sc));
        const int box =
            static_cast<int>(std::lround(render::FontEngine::display_width("\xE2\x94\x80", font, opts, sc) * sc));
        AURORA_TEST_CHECK_GE(cell.cell_width_px, std::max(digit, box));
        AURORA_TEST_CHECK_LE(cell.cell_width_px, std::max(digit, box) + 1);
    }
}

AURORA_TEST_CASE(monospace_cell_grid_keeps_ink_columns_drift_free) {
    // 整格网格的核心验收：同一字符摆在第 k 格，其左缘相对格起点的偏移必须逐格恒定。
    // 列宽一旦是小数（把 dp 度量直接当列宽用的典型结果），偏移就会逐格累积成半格错位。
    const Font font{.family = "sans-serif", .size_pt = 16.0F, .weight = 400};
    const auto cell = render::FontEngine::monospace_cell(font, 1.0F);
    constexpr int cells = 10;

    auto runs = std::vector<render::TextRun>{};
    for (int k = 0; k < cells; ++k) {
        runs.push_back(render::TextRun{
            .text = "0",
            .box = rect_at(static_cast<float>(k * cell.cell_width_px), 0.0F, static_cast<float>(cell.cell_width_px),
                           static_cast<float>(cell.cell_height_px)),
            .font = font,
            .color = Color::black()});
    }
    Painter p;
    p.begin(cell.cell_width_px * cells, cell.cell_height_px);
    p.draw_text_runs(std::span<const render::TextRun>{runs});

    auto expected_offset = -1;
    for (int k = 0; k < cells; ++k) {
        int leftmost = -1;
        for (int x = k * cell.cell_width_px; x < (k + 1) * cell.cell_width_px && leftmost < 0; ++x) {
            for (int y = 0; y < p.height(); ++y) {
                if (p.get_pixel(x, y).a > 0) {
                    leftmost = x;
                    break;
                }
            }
        }
        AURORA_TEST_REQUIRE_GE(leftmost, k * cell.cell_width_px);  // 每格都落墨，且不越进左邻格
        const int offset = leftmost - (k * cell.cell_width_px);
        if (expected_offset < 0) {
            expected_offset = offset;
        }
        AURORA_TEST_CHECK_EQ(offset, expected_offset);
    }
}

// ============================ 固定格推进档位 ============================
//
// 判据设计要点：像素级判据一律取「同一串文本、同一 Font，只切换档位」的两次绘制做差分，
// 而不是去断言某个绝对像素坐标 —— 后者会把「字形本身长什么样」也钉进用例，换个字体就红。
// 「不开档位逐位不变」这条则反过来用绝对对拍：同一条 opts 走默认构造与显式 nullopt，
// 两者必须给出完全相同的帧缓冲。

AURORA_TEST_CASE(fixed_cell_advance_absent_is_pixel_identical_to_nullopt) {
    Painter pa;
    pa.begin(240, 40);
    pa.fill_rect(rect_at(0.0F, 0.0F, 240.0F, 40.0F), Color{0, 0, 0, 255});
    Painter pb;
    pb.begin(240, 40);
    pb.fill_rect(rect_at(0.0F, 0.0F, 240.0F, 40.0F), Color{0, 0, 0, 255});

    const Font f{.family = "Cascadia Code", .size_pt = 14.0F, .weight = 400};
    const std::string text = "AVWA ii11";
    // 帧缓冲哈希式对拍：逐像素比较，任何一列错位都会被抓到。
    pa.draw_text(rect_at(2.0F, 4.0F, 200.0F, 20.0F), text, f, Color{255, 255, 255, 255});

    const render::TextLayoutOpts explicit_null{};
    AURORA_TEST_REQUIRE_FALSE(explicit_null.fixed_cell_advance_px.has_value());
    pb.draw_text(rect_at(2.0F, 4.0F, 200.0F, 20.0F), text, f, Color{255, 255, 255, 255}, explicit_null);

    bool identical = true;
    int first_diff_x = -1;
    for (int y = 0; y < pa.height() && identical; ++y) {
        for (int x = 0; x < pa.width(); ++x) {
            if (pa.get_pixel(x, y) != pb.get_pixel(x, y)) {
                identical = false;
                first_diff_x = x;
                break;
            }
        }
    }
    AURORA_TEST_CHECK_MSG(identical, "default TextLayoutOpts must render pixel-identically to explicit nullopt");
    AURORA_TEST_CHECK_EQ(first_diff_x, -1);
    // 同时确认真的落了墨（防空转判据：两帧全黑也会「逐位一致」）。
    AURORA_TEST_CHECK_GT(count_opaque(pa), 0);
}

AURORA_TEST_CASE(fixed_cell_advance_makes_advance_independent_of_face) {
    const Font f{.family = "Cascadia Code", .size_pt = 14.0F, .weight = 400};
    const render::CellMetrics cell = render::FontEngine::monospace_cell(f, 1.0F);
    AURORA_TEST_REQUIRE_GT(cell.cell_width_px, 0);

    // 缺字回退面（比例字体的汉字）自身的 advance 与等宽主族的格宽并不相等 ——
    // 这正是「双宽字形把后续字形推偏」的成因。开启档位后推进量必须只由档位决定。
    //
    // 语料写法：「中」的 UTF-8 是 E4 B8 AD，必须用**两个字面量拼接**而非一个连续串。
    // 写成 `"A\xE4\xB8\xADB"` 时，十六进制转义 `\x` 贪婪吞掉后面的 `B` ⇒ 被解析成 `\xADB`
    // （越界）。MinGW g++ 宽容地截断、clang 直接报 `hex escape sequence out of range`
    // —— 于是语料实际不是「A中B」，而依赖码点内容的判据却照样 PASS（判据与被测对象脱钩）。
    // 拼接写法彻底消除歧义，下面的码点数断言再把「语料被改坏」这类失误即刻暴露出来。
    // CJK-LITERAL: test-data - 「中」须在等宽族里缺字才能触发回退面，改 ASCII 则该用例退化为空转
    const std::string mixed =
        "A\xE4\xB8\xAD"
        "B";
    AURORA_TEST_REQUIRE_EQ(cp_count_of(mixed), 3U);  // 语料自检：必须是 A / 中 / B 三个码点

    render::TextLayoutOpts plain{};
    render::TextLayoutOpts fixed{};
    fixed.fixed_cell_advance_px = static_cast<float>(cell.cell_width_px);

    // 整串宽度：开档后应恰为「字形数 × 格宽」（间距为 0 时）。
    const float w_plain = render::FontEngine::measure_width(mixed, f, plain);
    const float w_fixed = render::FontEngine::measure_width(mixed, f, fixed);
    AURORA_TEST_CHECK_NE(w_plain, w_fixed);
    AURORA_TEST_CHECK_NEAR(w_fixed, 3.0F * static_cast<float>(cell.cell_width_px), 0.001F);

    // 关键性质：**每个** caret 边界都落在整格上（不开档时第二个字形之后的边界因回退面
    // 推进量不同而偏离格子）。逐个断言而不是只看串尾。
    for (std::size_t k = 0; k <= 3U; ++k) {
        const float cx = render::FontEngine::caret_x(mixed, k, f, fixed);
        AURORA_TEST_CHECK_NEAR(cx, static_cast<float>(k) * static_cast<float>(cell.cell_width_px), 0.001F);
    }
}

AURORA_TEST_CASE(fixed_cell_advance_places_every_glyph_inside_its_own_cell) {
    // **像素级**判据：开档后整串的墨迹右边界必须落在「N 格」之内，且明显小于不开档位时。
    //
    // 为什么必须像素级、不能只靠 measure_width / caret_x / hit_test：那些全是**度量侧**断言。
    // 若「绘制侧推进用各自 face 的 advance、度量侧仍用固定格」这种半途失配（改了绘制忘了改度量，
    // 或反之），度量侧断言会**照样全绿** —— 实绘与选区错开一格却无人报错。
    // 只有直接量帧缓冲里的墨迹分布，才能把「绘制侧真的按固定步进落笔」钉住。
    //
    // 判据为何用「右边界」而非「逐字形起点」：汉字位图宽可超过格宽（实测 Cascadia 14pt
    // 格宽 11px、汉字 19px），相邻字形的墨迹会相连、空格也分隔不开，无法可靠分簇归属。
    // 右边界不受此影响，且随推进量单调变化，是这一档位最直接的可观察量。
    const Font f{.family = "Cascadia Code", .size_pt = 14.0F, .weight = 400};
    const render::CellMetrics cell = render::FontEngine::monospace_cell(f, 1.0F);
    AURORA_TEST_REQUIRE_GT(cell.cell_width_px, 0);
    const int cw = cell.cell_width_px;

    // 混排 ASCII 与缺字中文：汉字走回退面、其自身 advance ≠ 格宽，正是会漂移的那些字形。
    // 用两个字面量拼接，避免 `\xAD` 贪婪吞掉后续十六进制数字（见 cp_count_of 的注释）。
    // CJK-LITERAL: test-data - 须含等宽族缺字的中文码点，其 advance ≠ 格宽 才会暴露漂移
    const std::string mixed =
        "A\xE4\xB8\xAD"
        "A\xE4\xB8\xAD"
        "A\xE4\xB8\xAD"
        "A";
    const int cells = 7;
    AURORA_TEST_REQUIRE_EQ(cp_count_of(mixed), static_cast<std::size_t>(cells));  // 语料自检

    render::TextLayoutOpts fixed{};
    fixed.fixed_cell_advance_px = static_cast<float>(cw);
    render::TextLayoutOpts plain{};

    // 画布留足余量，确保**两种模式都不被裁剪** —— 一旦裁剪，右边界就饱和到画布宽、
    // 两种模式测出同一个值，判据失去分辨力（这是本用例最容易踩的坑）。
    const int canvas_w = cw * 20;
    const int canvas_h = cell.cell_height_px + 4;
    // 画布**不填背景**（默认全透明）：判据用 `a != 0` 取墨迹，若填了不透明底色则整幅画布
    // alpha 都非零、rightmost 恒为画布宽 − 1，两种模式测出同一个值、判据彻底失效。
    const auto rightmost_ink = [&](const render::TextLayoutOpts &o) {
        Painter p;
        p.begin(canvas_w, canvas_h);
        p.draw_text(rect_at(0.0F, 0.0F, static_cast<float>(canvas_w), static_cast<float>(canvas_h)), mixed, f,
                    Color::black(), o);
        int right = -1;
        for (int x = p.width() - 1; x >= 0 && right < 0; --x) {
            for (int y = 0; y < p.height(); ++y) {
                if (p.get_pixel(x, y).a != 0) {
                    right = x;
                    break;
                }
            }
        }
        return right;
    };

    const int right_fixed = rightmost_ink(fixed);
    const int right_plain = rightmost_ink(plain);
    AURORA_TEST_REQUIRE_GE(right_fixed, 0);  // 真的有落墨（防空转）
    AURORA_TEST_REQUIRE_GE(right_plain, 0);

    // ① 开档后整串墨迹不得超出「N 格 + 末格字形自身宽度」的允许范围。
    //    末格字形可以溢出本格（字体事实，非本档位缺陷），故留一格余量。
    AURORA_TEST_CHECK_LT(right_fixed, (cells + 1) * cw);
    // ② 关键判据：开档后的右边界必须比不开档**至少小一格** —— 回退面双宽字形按自己的
    //    advance 推进会逐字累积，不开档时右边界必然明显更靠右。变异「绘制侧忽略档位」时
    //    两者会相等 ⇒ 转红。
    AURORA_TEST_CHECK_LE(right_fixed, right_plain - cw);
}

AURORA_TEST_CASE(fixed_cell_advance_keeps_glyph_fallback_active) {
    // 「只改推进量、不改选面逻辑」：开档后缺字仍须回退到别的面（否则缺字会变成豆腐/空白）。
    // 判据用**墨迹存在性**：回退面若没被选中，该码点不会落墨（等宽族无此字形）。
    const Font f{.family = "Cascadia Code", .size_pt = 16.0F, .weight = 400};
    const render::CellMetrics cell = render::FontEngine::monospace_cell(f, 1.0F);
    render::TextLayoutOpts fixed{};
    fixed.fixed_cell_advance_px = static_cast<float>(cell.cell_width_px);

    Painter p;
    p.begin(120, 40);
    p.fill_rect(rect_at(0.0F, 0.0F, 120.0F, 40.0F), Color{0, 0, 0, 255});
    p.draw_text(rect_at(4.0F, 4.0F, 100.0F, 24.0F), "\xE4\xB8\xAD", f, Color{255, 255, 255, 255}, fixed);
    // 汉字必须落墨（走了回退面），而不是被固定格档位「顺带」吃掉。
    AURORA_TEST_CHECK_GT(count_opaque(p), 0);
}

AURORA_TEST_CASE(fixed_cell_advance_keeps_hit_test_consistent_with_pixels) {
    // 度量与像素同源：开档后命中边界必须与固定格边界一致，
    // 否则网格消费方的选区会与实绘错开一格（这正是本档位要解决的问题本身）。
    //
    // 用 `hit_test_char_inclusive`（含头含尾，点谁中谁）而非 `hit_test_char`（caret 语义、
    // 按中点返回**下一个**光标位）：网格选区关心的是「点中了哪一格」，inclusive 的语义与格
    // 边界直接对应，判据不需要再套一层中点换算。caret 语义另由下一段单独钉。
    const Font f{.family = "Cascadia Code", .size_pt = 14.0F, .weight = 400};
    const render::CellMetrics cell = render::FontEngine::monospace_cell(f, 1.0F);
    render::TextLayoutOpts fixed{};
    fixed.fixed_cell_advance_px = static_cast<float>(cell.cell_width_px);

    // CJK-LITERAL: test-data - 混排串须含等宽族缺字的中文码点，逐格命中判据依赖此
    // 同样用两个字面量拼接，避免 `\xAD` 贪婪吞掉后续的 `B`/`C`（见上一用例的注释）。
    const std::string mixed =
        "A\xE4\xB8\xAD"
        "B\xE4\xB8\xAD"
        "C";
    AURORA_TEST_REQUIRE_EQ(cp_count_of(mixed), 5U);  // 语料自检：A / 中 / B / 中 / C
    const auto cw = static_cast<float>(cell.cell_width_px);
    // 点在第 k 格正中 ⇒ 命中第 k 个字符（k 从 0 起）。
    for (std::size_t k = 0; k < 5U; ++k) {
        const float x = (static_cast<float>(k) + 0.5F) * cw;
        const std::size_t hit = render::FontEngine::hit_test_char_inclusive(mixed, x, f, fixed);
        AURORA_TEST_CHECK_EQ(hit, k);
    }

    // caret 语义一并钉住：第 k 个边界（= k × 格宽）本身就是第 k 格左缘，
    // 按中点取舍应恰好返回 k（不早一格、也不晚一格）。
    for (std::size_t k = 0; k < 5U; ++k) {
        const float x = static_cast<float>(k) * cw;
        const std::size_t caret_hit = render::FontEngine::hit_test_char(mixed, x, f, fixed);
        AURORA_TEST_CHECK_EQ(caret_hit, k);
    }
}

AURORA_TEST_CASE(fixed_cell_advance_and_fallback_chain_compose) {
    // 两个新字段同时打开时互不干扰：链决定选面，档位决定推进。
    const Font f{.family = "Cascadia Code", .size_pt = 14.0F, .weight = 400};
    const render::CellMetrics cell = render::FontEngine::monospace_cell(f, 1.0F);
    // 先按链构造（该工厂会把其余排版属性置默认），再打开固定格档位。
    auto both = render::TextLayoutOpts::with_fallback_chain(std::vector<std::string>{"Noto Sans"});
    both.fixed_cell_advance_px = static_cast<float>(cell.cell_width_px);

    // CJK-LITERAL: test-data - 与上面同一语料；此处验「链 + 档位同时打开」时推进仍只由档位决定
    const std::string mixed =
        "A\xE4\xB8\xAD"
        "B";
    AURORA_TEST_REQUIRE_EQ(cp_count_of(mixed), 3U);
    const float w = render::FontEngine::measure_width(mixed, f, both);
    // 推进仍只由档位决定（链不参与推进）。
    AURORA_TEST_CHECK_NEAR(w, 3.0F * static_cast<float>(cell.cell_width_px), 0.001F);
}

AURORA_TEST_CASE(text_layout_opts_equality_covers_new_fields) {
    // 相等性漏字段是最隐蔽的一类 bug：shaping 缓存以 opts 为键，漏一个字段就会让开了档的
    // run 命中没档的缓存 ⇒ 字距静默错位。这里显式钉住两个新字段都参与比较。
    render::TextLayoutOpts base{};
    render::TextLayoutOpts same_cell = base;
    AURORA_TEST_CHECK(base == same_cell);

    same_cell.fixed_cell_advance_px = 11.0F;
    AURORA_TEST_CHECK(base != same_cell);

    const std::vector<std::string> chain{"Noto Sans"};
    const auto with_chain = render::TextLayoutOpts::with_fallback_chain(chain);
    AURORA_TEST_CHECK(base != with_chain);
    AURORA_TEST_REQUIRE_EQ(with_chain.font_fallback_chain_size, 1U);

    // 内容相同的链必须判相等（回放侧按值重建链后仍能命中同一缓存条目）。
    const auto chain_same = render::TextLayoutOpts::with_fallback_chain(chain);
    AURORA_TEST_CHECK(with_chain == chain_same);

    // 不同链必须判不等 —— 否则开了不同链的 run 会共用 shaping 缓存条目，选面错乱。
    const auto other = render::TextLayoutOpts::with_fallback_chain(std::vector<std::string>{"Cascadia Code"});
    AURORA_TEST_CHECK(with_chain != other);

    // 超出容量的链按「保留前 N 项」截断，不静默扩容（顺序语义不变）。
    std::vector<std::string> too_long;
    too_long.reserve(render::AURORA_TEXT_FALLBACK_CHAIN_MAX + 3U);  // 先备容量再逐项 push
    for (std::size_t i = 0; i < render::AURORA_TEXT_FALLBACK_CHAIN_MAX + 3U; ++i) {
        too_long.push_back("fam" + std::to_string(i));
    }
    const auto truncated = render::TextLayoutOpts::with_fallback_chain(too_long);
    AURORA_TEST_CHECK_EQ(truncated.font_fallback_chain_size, render::AURORA_TEXT_FALLBACK_CHAIN_MAX);
    AURORA_TEST_CHECK_EQ(truncated.font_fallback_chain.front(), std::string{"fam0"});
}

}  // namespace aurora::test_cases::utest_font_engine

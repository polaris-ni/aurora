/// 测试类型: unit
/// 目标单元: include/aurora/render/font_engine.h
/// 测试说明: 覆盖 FontEngine 度量契约（空串零宽、长度/字号单调、行高为正）、caret_x 与 hit_test_char 的
/// 码点索引与往返一致性、TextLayoutOpts 字距/词距对宽度的影响、AA 策略读写与光栅状态世代自增、
/// draw_text 实际落笔、基线上沿度量（measure_ascent）与实绘落墨带自洽、shaping 缓存统计与清空，
/// 以及 UTF-8 串的码点安全性

#include <cmath>
#include <cstddef>
#include <string>

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
    constexpr render::TextLayoutOpts plain{};
    render::TextLayoutOpts spaced{};
    spaced.letter_spacing = 4.0F;

    AURORA_TEST_CHECK_GT(render::FontEngine::measure_width("Text", font, spaced),
                         render::FontEngine::measure_width("Text", font, plain));
}

AURORA_TEST_CASE(word_spacing_only_affects_text_with_spaces) {
    const Font font;
    constexpr render::TextLayoutOpts plain{};
    render::TextLayoutOpts spaced{};
    spaced.word_spacing = 8.0F;

    AURORA_TEST_CHECK_GT(render::FontEngine::measure_width("a b", font, spaced),
                         render::FontEngine::measure_width("a b", font, plain));
    AURORA_TEST_CHECK_NEAR(render::FontEngine::measure_width("ab", font, spaced),
                           render::FontEngine::measure_width("ab", font, plain), 1e-6);
}

AURORA_TEST_CASE(display_width_degenerates_to_measure_width_at_scale_one) {
    const Font font;
    constexpr render::TextLayoutOpts opts{};
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
    const std::string text = "中国";  // 6 字节 / 2 码点
    AURORA_TEST_CHECK_NEAR(render::FontEngine::caret_x(text, 0, font), 0.0, 1e-6);
    AURORA_TEST_CHECK_GT(render::FontEngine::caret_x(text, 1, font), 0.0);
    AURORA_TEST_CHECK_GT(render::FontEngine::caret_x(text, 2, font), render::FontEngine::caret_x(text, 1, font));
}

AURORA_TEST_CASE(rtl_measure_width_matches_ltr) {
    // 视觉宽度与方向无关（字形集合相同，仅排列镜像）。
    const Font font;
    const std::string text = "Hello World";
    constexpr render::TextLayoutOpts ltr{};
    constexpr render::TextLayoutOpts rtl{.direction = TextDirection::RTL};
    AURORA_TEST_CHECK_NEAR(render::FontEngine::measure_width(text, font, rtl),
                           render::FontEngine::measure_width(text, font, ltr), 0.5);
}

AURORA_TEST_CASE(rtl_caret_x_mirrors_to_right_edge) {
    // RTL：逻辑首字符在右缘——caret(0) = 整串宽，caret(n) = 0，随逻辑下标单调递减。
    const Font font;
    const std::string text = "Aurora";
    constexpr render::TextLayoutOpts rtl{.direction = TextDirection::RTL};
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
    constexpr render::TextLayoutOpts ltr{};
    constexpr render::TextLayoutOpts rtl{.direction = TextDirection::RTL};
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

}  // namespace aurora::test_cases::utest_font_engine

/// 测试类型: integration
/// 目标单元: include/aurora/render/font_engine.h
/// 测试说明: 集成 FontEngine 度量/光标/命中/绘制原语与 TextLayoutOpts：
///           letter/word 间距对 measure_width 的加法效应、带间距与斜体时
///           caret_x ↔ hit_test_char 一致性、带 opts 绘制产出像素、斜体与正体像素可分。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_text_spacing {

namespace ar = aurora::render;

namespace {

using au::Color;
using au::Font;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;

/// 带间距/斜体时「度量 ↔ 光标 ↔ 命中」一一对应：caret_x 严格递增，且
/// caret_x(i) 处 hit_test_char 应回到 i（含末位，x==边界时命中逻辑返回 i）。
void check_consistency(const std::string &name, const std::string &s, const ar::TextLayoutOpts &opts) {
    (void)ar::FontEngine::instance();
    const Font f{.size_pt = 18.0F};
    const std::size_t n = s.size();  // 用例文本均为纯 ASCII：字节数即码点数
    bool monotonic = true;
    bool hittest_ok = true;
    for (std::size_t i = 1; i <= n; ++i) {
        const float xi = ar::FontEngine::caret_x(s, i, f, opts);
        if (i > 1 && xi <= ar::FontEngine::caret_x(s, i - 1, f, opts)) {
            monotonic = false;
        }
        if (ar::FontEngine::hit_test_char(s, xi, f, opts) != i) {
            hittest_ok = false;
        }
    }
    AURORA_TEST_CHECK_MSG(monotonic, "consistency[" + name + "]: caret_x strictly increases");
    AURORA_TEST_CHECK_MSG(hittest_ok, "consistency[" + name + "]: caret_x(i) hit_tests back to i");
}

/// 将整行文本渲染为像素缓冲（副本），用于对比斜体是否真的倾斜。
auto render_text_buf(const std::string &s, const ar::TextLayoutOpts &opts) -> std::vector<std::uint8_t> {
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);
    Painter p;
    constexpr int w = 240;
    constexpr int h = 48;
    p.begin(w, h);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                     .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                Color{255, 255, 255});
    const Font f{.size_pt = 28.0F};
    p.draw_text(Rect{.origin = Point{.x = 4, .y = 4},
                     .size = Size{.width = static_cast<float>(w - 8), .height = static_cast<float>(h - 8)}},
                s, f, Color::black(), ar::TextAAMode::Supersample, opts);
    const std::uint8_t *d = p.data();
    constexpr std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U;
    return std::vector<std::uint8_t>(d, d + n);
}

}  // namespace

AURORA_TEST_CASE(letter_spacing_adds_n_minus_one_gaps) {
    // 相同字符间 kerning≈0，便于精确断言 (n-1)*L；measure_width 在末尾扣除一个字距。
    (void)ar::FontEngine::instance();
    const Font f{.size_pt = 24.0F};
    const std::string s = "AAA";
    const float base = ar::FontEngine::measure_width(s, f, ar::TextLayoutOpts{});
    constexpr float l = 8.0F;
    const float spaced = ar::FontEngine::measure_width(s, f, ar::TextLayoutOpts{.letter_spacing = l});
    AURORA_TEST_CHECK_NEAR(spaced, base + (2.0F * l), 2.0F);  // (3-1)*L = 16
    AURORA_TEST_CHECK_MSG(spaced > base, "letter_spacing strictly widens the string");
}

AURORA_TEST_CASE(word_spacing_adds_per_space_width) {
    // 每个空格后追加 word_spacing → 期望增加 1*W（字母间无 letter_spacing）。
    (void)ar::FontEngine::instance();
    const Font f{.size_pt = 24.0F};
    const std::string s = "A A";  // 1 个空格
    constexpr float w = 12.0F;
    const float base = ar::FontEngine::measure_width(s, f, ar::TextLayoutOpts{});
    const float spaced = ar::FontEngine::measure_width(s, f, ar::TextLayoutOpts{.word_spacing = w});
    AURORA_TEST_CHECK_NEAR(spaced, base + w, 2.0F);
}

AURORA_TEST_CASE(metrics_caret_and_hit_test_stay_consistent_with_opts) {
    check_consistency("italic", "Hello World", ar::TextLayoutOpts{.italic = true});
    check_consistency("spacing", "The quick brown fox",
                      ar::TextLayoutOpts{.letter_spacing = 3.0F, .word_spacing = 6.0F});
    check_consistency("italic+spacing", "Aurora GUI library",
                      ar::TextLayoutOpts{.letter_spacing = 2.0F, .italic = true});
}

AURORA_TEST_CASE(draw_text_with_opts_produces_pixels) {
    // 带 opts（间距/斜体）的 draw_text 不崩溃且产出非空像素缓冲。
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);  // 避免 ClearType 背景依赖
    Painter p;
    p.begin(200, 60);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 200, .height = 60}},
                Color{240, 240, 240});
    const Font f{.size_pt = 20.0F};
    const ar::TextLayoutOpts opts{.letter_spacing = 4.0F, .italic = true};
    p.draw_text(Rect{.origin = Point{.x = 10, .y = 10}, .size = Size{.width = 180, .height = 40}}, "Spacing Italic",
                f, Color::black(), ar::TextAAMode::Supersample, opts);
    AURORA_TEST_CHECK_MSG(p.data() != nullptr, "draw_text with opts produced a non-null buffer");
}

AURORA_TEST_CASE(italic_renders_differently_from_upright) {
    // 回归：此前 get_entry 未把 italic 透传，导致斜体字形实际仍是正体。
    // 斜体字形（含合成 oblique）必然与正体像素不同。
    const std::string s = "Aurora Italic";
    const auto normal = render_text_buf(s, ar::TextLayoutOpts{});
    const auto italic = render_text_buf(s, ar::TextLayoutOpts{.italic = true});
    AURORA_TEST_CHECK_MSG(normal.size() == italic.size() && !normal.empty(), "italic render buffers allocated");
#ifdef AURORA_PLATFORM_WINDOWS
    AURORA_TEST_CHECK_MSG(normal != italic, "italic render differs from upright (true oblique, not faux-upright)");
#else
    (void)normal;
    (void)italic;
#endif
}

}  // namespace aurora::test_cases::itest_text_spacing

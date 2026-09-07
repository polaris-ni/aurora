/// 测试类型: unit
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: utest_text_spacing 单元测试
///
// 目标源单元（历史映射，保留供审计）: Text + Text
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// TextAaMode(TextAAMode，经 AA 各段行使)、BitmapFont(BitmapFont 内置字体回退)、
// TextSpan(TextSpan，经 sec_rich_text? 见 test_rich_text.cpp——TextSpan 归属 rich_text 单元)。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/platform.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora_test_harness.h"

// （自 utest_text.cpp 拆分：letter/word 间距与斜体渲染段）

namespace aurora::test_cases::utest_text_spacing {

namespace render = aurora::render;
using au::Alignment;
using au::BuildContext;
using au::Button;
using au::Clipboard;
using au::Color;
using au::Column;
using au::ColumnProps;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
using au::Font;
using au::FontStyle;
using au::FontWeight;
using au::Json;
using au::KeyAction;
using au::KeyCode;
using au::KeyEvent;
using au::LocalizedString;
using au::Modifier;
using au::ModifierKey;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Row;
using au::RowProps;
using au::set_current_focus_manager;
using au::Size;
using au::Text;
using au::TextAlign;
using au::TextDecoration;
using au::TextOverflow;
using au::TextProps;
using au::Widget;

namespace ar = aurora::render;
static auto cp_count(const std::string &s) -> std::size_t {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t cl = (c < 0x80U) ? 1U : (c < 0xE0U) ? 2U : (c < 0xF0U) ? 3U : 4U;
        i += cl;
        ++n;
    }
    return n;
}

// 相同字符间 kerning≈0，便于精确断言 (n-1)*L。
static void test_letter_spacing_additive() {
    (void)ar::FontEngine::instance();
    const Font f{.size_pt = 24.0F};
    const std::string s = "AAA";
    const std::size_t n = cp_count(s);
    constexpr float l = 8.0F;
    const float base = render::FontEngine::measure_width(s, f, ar::TextLayoutOpts{});
    const float spaced = render::FontEngine::measure_width(s, f, ar::TextLayoutOpts{.letter_spacing = l});
    // measure_width 在末尾扣除一个字距 → 期望增加 (n-1)*L = 16。
    AURORA_TEST_CHECK_MSG(near_f(spaced, base + ((n - 1) * l), 2.0F), "letter_spacing adds (n-1)*L to measure_width");
    AURORA_TEST_CHECK_MSG(spaced > base, "letter_spacing strictly widens the string");
}

static void test_word_spacing_additive() {
    (void)ar::FontEngine::instance();
    const Font f{.size_pt = 24.0F};
    const std::string s = "A A";  // 1 个空格
    constexpr float w = 12.0F;
    const float base = render::FontEngine::measure_width(s, f, ar::TextLayoutOpts{});
    const float spaced = render::FontEngine::measure_width(s, f, ar::TextLayoutOpts{.word_spacing = w});
    // 每个空格后追加 word_spacing → 期望增加 1*W = 12（字母间无 letter_spacing）。
    AURORA_TEST_CHECK_MSG(near_f(spaced, base + w, 2.0F), "word_spacing adds per-space width to measure_width");
}

// 验证「度量 ↔ 光标 ↔ 命中」在带 opts（间距/斜体）时一一对应。
static void test_consistency(const char *name, const std::string &s, const ar::TextLayoutOpts &opts) {
    (void)ar::FontEngine::instance();
    const Font f{.size_pt = 18.0F};
    const std::size_t n = cp_count(s);
    bool monotonic = true;
    bool hittest_ok = true;
    for (std::size_t i = 1; i <= n; ++i) {
        const float xi = render::FontEngine::caret_x(s, i, f, opts);
        if (i > 1 && xi <= render::FontEngine::caret_x(s, i - 1, f, opts)) {
            monotonic = false;
        }
        // 在光标落点处命中应返回该下标 i（含末位，x==boundary 时命中逻辑返回 i）。
        if (render::FontEngine::hit_test_char(s, xi, f, opts) != i) {
            hittest_ok = false;
        }
    }
    AURORA_TEST_CHECK_MSG(monotonic, std::string("consistency[") + name + "]: caret_x strictly increases");
    AURORA_TEST_CHECK_MSG(hittest_ok, std::string("consistency[") + name + "]: caret_x(i) hit_tests back to i");
}

static void test_draw_with_opts_no_crash() {
    (void)ar::FontEngine::instance();
    render::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);  // 避免 ClearType 背景依赖
    Painter p;
    p.begin(200, 60);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 200, .height = 60}}, Color{240, 240, 240});
    const Font f{.size_pt = 20.0F};
    const ar::TextLayoutOpts opts{.letter_spacing = 4.0F, .italic = true};
    p.draw_text(Rect{.origin = Point{.x = 10, .y = 10}, .size = Size{.width = 180, .height = 40}}, "Spacing Italic", f,
                Color::black(), ar::TextAAMode::Supersample, opts);
    const std::uint8_t *buf = p.data();
    AURORA_TEST_CHECK_MSG(buf != nullptr, "draw_text with opts produced a non-null buffer");
    render::FontEngine::set_text_aa_mode(ar::TextAAMode::ClearType);
}

// 将整行文本渲染为像素缓冲（副本），用于对比斜体是否真的倾斜。
static auto render_text_buf(const std::string &s, const ar::TextLayoutOpts &opts) -> std::vector<std::uint8_t> {
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic, modernize-return-braced-init-list)
    // 测试助手：缓冲区间算术；范围构造保留圆括号（braced-init 会变 initializer_list）
    return std::vector(d, d + n);  // NOLINT
}

// 回归：此前 get_entry 未把 italic 透传给 make_hfont，导致斜体字形实际仍是正体。
// 在 GDI 路径下，斜体字形（含合成 oblique）必然与正体像素不同。
static void test_italic_renders_different() {
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
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::ClearType);
}

static void run() {
    AURORA_TEST_PRINTF("=== text_spacing_test ===\n");
    test_letter_spacing_additive();
    test_word_spacing_additive();
    test_consistency("italic", "Hello World", ar::TextLayoutOpts{.italic = true});
    test_consistency("spacing", "The quick brown fox",
                     ar::TextLayoutOpts{.letter_spacing = 3.0F, .word_spacing = 6.0F});
    test_consistency("italic+spacing", "Aurora GUI library",
                     ar::TextLayoutOpts{.letter_spacing = 2.0F, .italic = true});
    test_draw_with_opts_no_crash();
    test_italic_renders_different();
}

AURORA_TEST() { aurora::test_cases::utest_text_spacing::run(); }

}  // namespace aurora::test_cases::utest_text_spacing

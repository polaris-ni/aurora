// 目标源单元：widget/text.h + src/aurora/widget/text.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_text.cpp
//   - test_text_aa_cleartype_fringe.cpp
//   - test_text_aa_override.cpp
//   - test_text_focus_clear.cpp
//   - test_text_justify.cpp
//   - test_text_no_bg.cpp
//   - test_text_ptr_capture.cpp
//   - test_text_selection.cpp
//   - test_text_spacing.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// render/text_aa_mode.h(TextAAMode，经 AA 各段行使)、render/bitmap_font.h(BitmapFont 内置字体回退)、
// widget/text_span.h(TextSpan，经 sec_rich_text? 见 test_rich_text.cpp——TextSpan 归属 rich_text 单元)。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
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
#include "test_harness.h"

namespace render = aurora::render;
using aurora::Alignment;
using aurora::BuildContext;
using aurora::Button;
using aurora::Clipboard;
using aurora::Color;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::FocusManager;
using aurora::Font;
using aurora::FontStyle;
using aurora::FontWeight;
using aurora::Json;
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyEvent;
using aurora::LocalizedString;
using aurora::Modifier;
using aurora::ModifierKey;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Row;
using aurora::RowProps;
using aurora::set_current_focus_manager;
using aurora::Size;
using aurora::Text;
using aurora::TextAlign;
using aurora::TextDecoration;
using aurora::TextOverflow;
using aurora::TextProps;
using aurora::Widget;

namespace sec_text {
static void test_chained_setters() {
    Text t{"Hello"};
    t.font_size(18)
        .color(Color::red())
        .set_align(TextAlign::Center)
        .set_max_lines(2)
        .set_overflow(TextOverflow::Ellipsis)
        .set_soft_wrap(false)
        .set_line_height(1.5F)
        .font_weight(FontWeight::Bold)
        .set_font_style(FontStyle::Italic)
        .set_decoration(TextDecoration::Underline | TextDecoration::LineThrough)
        .set_decoration_color(Color::blue())
        .set_background_color(Color{255, 255, 0, 40});

    AURORA_TEST_CHECK_MSG(t.text_align == TextAlign::Center, "text_align set");
    AURORA_TEST_CHECK_MSG(t.max_lines == 2, "max_lines set");
    AURORA_TEST_CHECK_MSG(t.overflow == TextOverflow::Ellipsis, "overflow set");
    AURORA_TEST_CHECK_MSG(t.soft_wrap == false, "soft_wrap set");
    AURORA_TEST_CHECK_MSG(near_f(t.line_height, 1.5F), "line_height set");
    AURORA_TEST_CHECK_MSG(t.font.weight == 700, "font_weight -> font.weight=700");
    AURORA_TEST_CHECK_MSG(t.font_style == FontStyle::Italic, "font_style set");
    AURORA_TEST_CHECK_MSG(decoration_has(t.decoration, TextDecoration::Underline), "decoration has Underline");
    AURORA_TEST_CHECK_MSG(decoration_has(t.decoration, TextDecoration::LineThrough), "decoration has LineThrough");
    AURORA_TEST_CHECK_MSG(!decoration_has(t.decoration, TextDecoration::Overline), "decoration not Overline");
    AURORA_TEST_CHECK_MSG(t.decoration_color == Color::blue(), "decoration_color set");
    AURORA_TEST_CHECK_MSG(t.background_color == Color{255, 255, 0, 40}, "background_color set");
}

static void test_serialize_roundtrip() {
    Text a{"Multi\nline"};
    a.font_size(16)
        .set_align(TextAlign::Right)
        .set_max_lines(3)
        .set_overflow(TextOverflow::Ellipsis)
        .set_soft_wrap(true)
        .set_line_height(1.2F)
        .font_weight(FontWeight::SemiBold)
        .set_decoration(TextDecoration::Underline)
        .set_decoration_color(Color::green())
        .set_background_color(Color{10, 20, 30, 40});

    Json j;
    a.serialize_props(j);

    // 直接校验关键 JSON 键
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["text_align"].get<std::string>() == "Right", "json text_align=Right");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["max_lines"].get<int>() == 3, "json max_lines=3");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["overflow"].get<std::string>() == "Ellipsis", "json overflow=Ellipsis");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(j["line_height"].get<float>(), 1.2F), "json line_height=1.2");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["soft_wrap"].get<bool>() == true, "json soft_wrap=true");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["font_weight"].get<std::string>() == "600", "json font_weight=600");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["decoration"].is_array() && j["decoration"].size() == 1, "json decoration=[Underline]");

    // 反序列化到新实例
    Text b;
    b.deserialize_props(j);

    AURORA_TEST_CHECK_MSG(b.text_align == TextAlign::Right, "rt text_align");
    AURORA_TEST_CHECK_MSG(b.max_lines == 3, "rt max_lines");
    AURORA_TEST_CHECK_MSG(b.overflow == TextOverflow::Ellipsis, "rt overflow");
    AURORA_TEST_CHECK_MSG(b.soft_wrap == true, "rt soft_wrap");
    AURORA_TEST_CHECK_MSG(near_f(b.line_height, 1.2F), "rt line_height");
    AURORA_TEST_CHECK_MSG(b.font.weight == 600, "rt font.weight=600");
    AURORA_TEST_CHECK_MSG(b.decoration == TextDecoration::Underline, "rt decoration");
    AURORA_TEST_CHECK_MSG(b.decoration_color == Color::green(), "rt decoration_color");
    AURORA_TEST_CHECK_MSG(b.background_color == Color{10, 20, 30, 40}, "rt background_color");
    AURORA_TEST_CHECK_MSG(b.font.size_pt == 16.0F, "rt font_size");
}

static void test_defaults() {
    const Text t = {};
    AURORA_TEST_CHECK_MSG(t.text_align == TextAlign::Left, "default text_align Left");
    AURORA_TEST_CHECK_MSG(t.max_lines == 0, "default max_lines 0");
    AURORA_TEST_CHECK_MSG(t.overflow == TextOverflow::Clip, "default overflow Clip");
    AURORA_TEST_CHECK_MSG(t.soft_wrap == true, "default soft_wrap true");
    AURORA_TEST_CHECK_MSG(near_f(t.line_height, 1.0F), "default line_height 1.0");
    AURORA_TEST_CHECK_MSG(t.decoration == TextDecoration::None, "default decoration None");
    AURORA_TEST_CHECK_MSG(t.background_color.m_a == 0, "default background alpha 0");
}

/// @brief 回归：多行文本命中测试须按点击的「行」定位，而非把整段当成单行。
///        曾在折行后点击第 N 行的小 x 会被错误落回第 1 行，导致选区跨越多行。
static void test_multiline_selection_hit_test() {
    const std::string src = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
    const auto t = std::make_shared<Text>(src);
    t->font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);

    const aurora::BuildContext ctx;
    t->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 25.0F, .height = 600.0F};  // 窄到每个词独占一行
    t->layout(c, ctx);

    AURORA_TEST_CHECK_MSG(t->display_text() == src, "display_text preserved (no newline injected)");

    // 点击第 1 行左上角：应当落在整段起点（码点下标 0）。
    MouseEvent top;
    top.action = MouseAction::Press;
    top.local_position = Point{.x = 0.0F, .y = 0.0F};
    t->on_pointer_event(top);
    AURORA_TEST_CHECK_MSG(t->selection().first == 0, "press on first visual line -> caret at line0 start");

    // 点击最后一行（y 给一个远超所有行的极大值，被钳制到最后一行）左上角：
    // 修复后应当落在最后一行（文本末尾附近的某一块）的起点，而非整段起点。
    // 修复前会把整段当作单行，y 被忽略，x=0 恒落到第 1 个字符（下标 0）。
    MouseEvent bottom;
    bottom.action = MouseAction::Press;
    bottom.local_position = Point{.x = 0.0F, .y = 100000.0F};
    t->on_pointer_event(bottom);
    const size_t total = t->display_text().size();  // 全 ASCII：字节数 == 码点数
    AURORA_TEST_CHECK_MSG(t->selection().first > total / 2,
                          "press on last visual line -> caret near text end (not line0)");

    // 从首行拖到末行：选区应覆盖整段（首行起点 → 末行终点），且端点方向正确。
    top.action = MouseAction::Press;
    top.local_position = Point{.x = 0.0F, .y = 0.0F};
    t->on_pointer_event(top);
    MouseEvent drag;
    drag.action = MouseAction::Move;
    drag.local_position = Point{.x = 100000.0F, .y = 100000.0F};  // 末行末字符
    t->on_pointer_event(drag);
    AURORA_TEST_CHECK_MSG(t->has_selection(), "drag selects something");
    AURORA_TEST_CHECK_MSG(t->selection().first == 0, "drag selection starts at line0 start");
    AURORA_TEST_CHECK_MSG(t->selection().second == total, "drag selection ends at last line end");
}

/// @brief 回归：对多字节文本做「部分拖选」后 Ctrl+C，剪贴板必须是选中片段，
///        而非整段。曾在无选区时回退复制整段 m_display_text，导致「选中后复制」得到全文。
static auto cp_prefix(const std::string &s, size_t n) -> std::string {
    std::string out;
    size_t i = 0;
    size_t got = 0;
    while (i < s.size() && got < n) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0x80U) {
            if ((c >> 5U) == 0x6U) {
                len = 2;
            } else if ((c >> 4U) == 0x0EU) {
                len = 3;
            } else if ((c >> 3U) == 0x1EU) {
                len = 4;
            }
        }
        out.append(s, i, len);
        i += len;
        ++got;
    }
    return out;
}

static void focus(Widget *w) {
    FocusManager fm;  // 栈对象：触发 on_focus_change 把焦点写到 widget 上后即弃用
    set_current_focus_manager(&fm);
    fm.request_focus(w);
    set_current_focus_manager(nullptr);  // 焦点已落到 widget.m_is_focused，避免 fm 析构后全局悬空
}

/// @brief 探测系统剪贴板是否可用（无头/被其它进程占用时不可用）。
static auto clipboard_available() -> bool {
    Clipboard::set_text("__PROBE__");
    return Clipboard::get_text() == "__PROBE__";
}

static void test_selection_copy_clipboard() {
    const std::string src = "héllo-世界";
    const auto t = std::make_shared<Text>(src);
    t->font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);

    const BuildContext ctx;
    t->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 1000.0F, .height = 600.0F};  // 宽约束 -> 单行，便于用 x 选前若干码点
    t->layout(c, ctx);

    const Font f = t->font;  // font_size 已设 -> effective_font 返回原样
    const render::TextLayoutOpts opts{.letter_spacing = t->letter_spacing,
                                      .word_spacing = t->word_spacing,
                                      .italic = (t->font_style == FontStyle::Italic)};

    // 拖选前 3 个码点 "hél"（h, é, l）。
    const float x_sel = render::FontEngine::measure_width("hél", f, opts);

    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 0.0F, .y = 0.0F};
    t->on_pointer_event(press);
    MouseEvent move;
    move.action = MouseAction::Move;
    move.local_position = Point{.x = x_sel, .y = 0.0F};
    t->on_pointer_event(move);

    const size_t sel_end = t->selection().second;
    AURORA_TEST_CHECK_MSG(sel_end >= 2 && sel_end <= 4, "copy-test: partial selection has 2..4 code points");
    AURORA_TEST_CHECK_MSG(t->selection().first == 0, "copy-test: selection starts at 0");

    focus(t.get());
    // 必须先探测剪贴板可用性再发 Ctrl+C：探测会写入 "__PROBE__"，
    // 若放在复制之后会把刚复制的选区内容覆盖掉，导致断言恒失败。
    if (!clipboard_available()) {
        AURORA_TEST_PRINTF("[SKIP] copy-test: system clipboard unavailable in this env\n");
        return;
    }
    KeyEvent ke;
    ke.key_ = static_cast<int>(KeyCode::C);
    ke.action_ = KeyAction::Down;
    ke.modifiers_ = ModifierKey::Control;
    t->on_key_event(ke);

    const std::string clip = Clipboard::get_text();
    const std::string expected = cp_prefix(src, sel_end);
    AURORA_TEST_CHECK_MSG(clip == expected, "copy-test: clipboard == selected fragment (not whole text)");
    AURORA_TEST_CHECK_MSG(clip != src, "copy-test: did NOT copy whole text when a subset is selected");
}

/// @brief 回归：无选区（仅落点光标、未拖选）时 Ctrl+C 不应回退复制整段文本。
static void test_no_selection_copy_clipboard() {
    const std::string src = "héllo-世界";
    const auto t = std::make_shared<Text>(src);
    t->font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);

    const BuildContext ctx;
    t->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 1000.0F, .height = 600.0F};
    t->layout(c, ctx);

    // 仅按下不拖拽 -> 空选区
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 5.0F, .y = 0.0F};
    t->on_pointer_event(press);
    AURORA_TEST_CHECK_MSG(!t->has_selection(), "no-selection-test: single click leaves no selection");

    focus(t.get());
    if (!clipboard_available()) {
        AURORA_TEST_PRINTF("[SKIP] no-selection-test: system clipboard unavailable in this env\n");
        return;
    }
    // 先把剪贴板置成一个已知非 src 的内容，验证 Ctrl+C 不会改写它。
    Clipboard::set_text("__KEEP__");
    KeyEvent ke;
    ke.key_ = static_cast<int>(KeyCode::C);
    ke.action_ = KeyAction::Down;
    ke.modifiers_ = ModifierKey::Control;
    t->on_key_event(ke);

    const std::string clip = Clipboard::get_text();
    AURORA_TEST_CHECK_MSG(clip == "__KEEP__",
                          "no-selection-test: Ctrl+C with empty selection does NOT copy whole text");
}

// 斜体剪切方向回归：正常斜体（oblique）是「越高的点越向右」（FT_Matrix.xy 分量）——
// 竖笔字母 'l' 的墨迹上半带质心必须明显右于下半带质心。
// 若斜量误设在 yx 分量（竖向歪斜：字形逆时针翻转、基线在字内爬坡），
// 则上/下半带质心的水平偏移近于 0，本断言失败。
static void test_italic_shear_direction() {
    const Font f{.size_pt = 40.0F};  // 大字号放大剪切量，质心偏移远大于 AA 噪声
    constexpr render::TextLayoutOpts italic{.italic = true};
    Painter p;
    p.begin(120, 80);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 120, .height = 80}}, Color::white());
    // 用 Supersample 避开 ClearType 彩色羽化，墨迹检测只看近黑像素。
    render::FontEngine::draw_text(
        p, Rect{.origin = Point{.x = 20.0F, .y = 10.0F}, .size = Size{.width = 80, .height = 60}}, "l", f,
        Color::black(), render::TextAAMode::Supersample, italic);
    // 逐行扫墨迹，求上半带与下半带的 x 质心。
    int y_min = p.height();
    int y_max = -1;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.m_r < 100 && c.m_g < 100 && c.m_b < 100) {
                y_min = std::min(y_min, y);
                y_max = std::max(y_max, y);
            }
        }
    }
    AURORA_TEST_CHECK_MSG(y_max > y_min + 10, "italic-test: glyph 'l' has enough vertical ink span");
    const int y_mid = (y_min + y_max) / 2;
    double top_sum = 0.0;
    double bot_sum = 0.0;
    int top_n = 0;
    int bot_n = 0;
    for (int y = y_min; y <= y_max; ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.m_r < 100 && c.m_g < 100 && c.m_b < 100) {
                if (y <= y_mid) {
                    top_sum += x;
                    ++top_n;
                } else {
                    bot_sum += x;
                    ++bot_n;
                }
            }
        }
    }
    AURORA_TEST_CHECK_MSG(top_n > 0 && bot_n > 0, "italic-test: ink in both halves");
    const double dx = (top_sum / std::max(1, top_n)) - (bot_sum / std::max(1, bot_n));
    // 0.22 斜量×半字高（≈20px）预期偏移 ≈4px；竖向歪斜（yx 误设）时 dx≈0。
    if (!(dx > 1.5)) {
        AURORA_TEST_PRINTF("  italic-test: top-bottom centroid dx = %.2f (expect > 1.5)\n", dx);
    }
    AURORA_TEST_CHECK_MSG(dx > 1.5, "italic-test: upper half leans right of lower half (xy shear)");
}

void run() {
    AURORA_TEST_PRINTF("=== text_test ===\n");
    test_chained_setters();
    test_serialize_roundtrip();
    test_defaults();
    test_multiline_selection_hit_test();
    test_selection_copy_clipboard();
    test_no_selection_copy_clipboard();
    test_italic_shear_direction();
}
}  // namespace sec_text

namespace sec_text_aa_cleartype_fringe {
namespace ar = aurora::render;

void run() {
    using aurora::Alignment;
    using aurora::BuildContext;
    using aurora::Color;
    using aurora::Column;
    using aurora::Constraints;
    using aurora::Modifier;
    using aurora::Node;
    using aurora::Painter;
    using aurora::Point;
    using aurora::Rect;
    using aurora::Size;
    using aurora::Text;

    constexpr int w = 360;
    constexpr int h = 80;
    constexpr auto winbg = Color{245, 245, 247};

    auto render = [&](bool supersample) -> Painter {
        const auto t = std::make_shared<Text>(LocalizedString{"curve@0.5 = 0.500000"});
        if (supersample) {
            t->text_aa_mode = ar::TextAAMode::Supersample;
        } else {
            // 显式设为 ClearType 以触发子像素 RGB 着色路径
            // （默认已是 Supersample，不显式切换则两边都是灰度 AA，无法测到彩色镶边）
            t->text_aa_mode = ar::TextAAMode::ClearType;
        }
        auto const col = std::make_shared<Column>(std::initializer_list{Node{t}});
        const BuildContext ctx;
        col->mount(ctx);
        Constraints c;
        c.min = Size{.width = 0, .height = 0};
        c.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
        col->layout(c, ctx);
        Painter p;
        p.begin(w, h);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                         .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                    winbg);
        col->paint(p,
                   Rect{.origin = Point{.x = 0, .y = 0},
                        .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                   ctx);
        return p;
    };

    Painter p_ct = render(false);  // 默认 ClearType
    Painter p_ss = render(true);  // Supersample

    const std::uint8_t *buf_ct = p_ct.data();
    const std::uint8_t *buf_ss = p_ss.data();
    auto px = [&](const std::uint8_t *b, int x, int y) -> std::array<int, 3> {
        const std::size_t i =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4U;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return {b[i], b[i + 1], b[i + 2]};
    };
    // 浅灰底上的彩色镶边判定：非背景、非纯黑、且三通道强失衡（R/G/B 差异大）。
    auto is_colored_fringe = [&](const std::array<int, 3> &c) -> bool {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool is_bg = std::abs(c[0] - 245) <= 12 && std::abs(c[1] - 245) <= 12 && std::abs(c[2] - 247) <= 12;
        if (is_bg) {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (c[0] < 60 && c[1] < 60 && c[2] < 60) {
            return false;  // 字形核心（黑）
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const int mx = std::max({c[0], c[1], c[2]});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const int mn = std::min({c[0], c[1], c[2]});
        return (mx - mn) > 40;  // 强通道失衡 = ClearType 红/蓝子像素镶边
    };

    int ct_fringe = 0;
    int ss_fringe = 0;
    int total = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto a = px(buf_ct, x, y);
            const auto b = px(buf_ss, x, y);
            ++total;
            if (is_colored_fringe(a)) {
                ++ct_fringe;
            }
            if (is_colored_fringe(b)) {
                ++ss_fringe;
            }
        }
    }
    AURORA_LOG_INFO("test", "ClearType colored-fringe pixels = ", ct_fringe, "/", total, " (",
                    100.0 * ct_fringe / total, "%)");
    AURORA_LOG_INFO("test", "Supersample colored-fringe pixels = ", ss_fringe, "/", total, " (",
                    100.0 * ss_fringe / total, "%)");

    // 逐帧闪烁验证：ClearType 渲染两帧（每帧都先清成 245,245,247），应完全一致。
    Painter p_c_t2 = render(false);
    const std::uint8_t *buf_c_t2 = p_c_t2.data();
    int diff = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto a = px(buf_ct, x, y);
            const auto b = px(buf_c_t2, x, y);
            if (a != b) {
                ++diff;
            }
        }
    }
    AURORA_LOG_INFO("test", "ClearType two-frame (cleared per frame) diff pixels = ", diff, "/", total,
                    " (=0 means no inter-frame flicker)");

    AURORA_TEST_CHECK(ct_fringe > ss_fringe && diff == 0);
}
}  // namespace sec_text_aa_cleartype_fringe

namespace sec_text_aa_override {
namespace ar = aurora::render;

void run() {
    using aurora::Alignment;
    using aurora::BuildContext;
    using aurora::Color;
    using aurora::Constraints;
    using aurora::Font;
    using aurora::Modifier;
    using aurora::Node;
    using aurora::Painter;
    using aurora::Point;
    using aurora::Rect;
    using aurora::Size;
    using aurora::Stack;
    using aurora::Text;

    constexpr int w = 240;
    constexpr int h = 240;
    constexpr float k_stage = 120.0F;
    constexpr float k_base_box = 80.0F;
    constexpr auto breathe = Color{236, 72, 153};  // 粉相（较亮），最易暴露白边
    constexpr auto winbg = Color{245, 245, 247};

    const auto box = std::make_shared<Text>(LocalizedString{"color pulse"});
    box->text_color = Color{255, 255, 255};  // 呼吸盒上白字
    box->text_aa_mode = ar::TextAAMode::Supersample;  // 修复：彩色背景走 Supersample，避免 ClearType 白边
    box->modifier.set(Modifier{}.size(k_stage, k_stage).background(breathe).align(Alignment::Center));

    // 两档缩放，分别看模糊
    auto mk_scale = [&](float s) -> std::shared_ptr<Text> {
        auto t = std::make_shared<Text>(LocalizedString{"scale"});
        t->modifier.set(Modifier{}.size(k_base_box, k_base_box).align(Alignment::Center).scale(s));
        return t;
    };
    const auto scale_inner = mk_scale(1.4F);

    AURORA_LOG_INFO("test", "box->text_aa_mode has_value=", box->text_aa_mode.has_value());

    auto const stage = std::make_shared<Stack>(std::vector{Node{box}, Node{scale_inner}}, Alignment::Center);
    stage->modifier.set(Modifier{}.size(k_stage, k_stage).clip());

    const BuildContext ctx;
    stage->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0, .height = 0};
    c.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    stage->layout(c, ctx);

    Painter p;
    p.begin(w, h);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                     .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                winbg);
    stage->paint(p,
                 Rect{.origin = Point{.x = (w - k_stage) / 2.0F, .y = (h - k_stage) / 2.0F},
                      .size = Size{.width = k_stage, .height = k_stage}},
                 ctx);

    const std::uint8_t *buf = p.data();
    auto px = [&](int x, int y) -> std::array<int, 3> {
        const std::size_t i =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4U;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return {buf[i], buf[i + 1], buf[i + 2]};
    };
    auto classify = [&](int x, int y) -> char {
        const auto pc = px(x, y);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (pc[0] > 200 && pc[1] > 200 && pc[2] > 200) {
            return 'W';  // 纯白字
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (std::abs(pc[0] - breathe.m_r) <= 10 && std::abs(pc[1] - breathe.m_g) <= 10 &&
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            std::abs(pc[2] - breathe.m_b) <= 10) {
            return '.';  // 呼吸色
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (pc[0] < 60 && pc[1] < 60 && pc[2] < 60) {
            return '#';  // 黑字
        }
        // ClearType 真·彩色尖刺：某一通道≈255 而另两通道仍贴近底色低值（红/蓝镶边）。
        // 注意呼吸底色本身 mx-mn 就很大（236-72=164），故不能用「整体方差」判定。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool brighter = (pc[0] > breathe.m_r + 12 || pc[1] > breathe.m_g + 12 || pc[2] > breathe.m_b + 12);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool near_g = std::abs(pc[1] - breathe.m_g) <= 30;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool near_b = std::abs(pc[2] - breathe.m_b) <= 30;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool near_r = std::abs(pc[0] - breathe.m_r) <= 30;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool red_spike = (pc[0] > 240 && near_g && near_b);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const bool blue_spike = (pc[2] > 240 && near_r && near_g);
        if (brighter && (red_spike || blue_spike)) {
            return 'S';  // ClearType 子像素红/蓝镶边
        }
        if (brighter) {
            return 'L';  // 亮于背景但柔和（正常 AA 边）
        }
        return '?';  // 其它（深色 AA 边）
    };

    AURORA_LOG_INFO("test", "=== stage region (120x120) downsampled to 60x60 ===");
    constexpr int x0 = static_cast<int>((w - k_stage) / 2);
    constexpr int y0 = static_cast<int>((h - k_stage) / 2);
    for (int y = y0; y < y0 + static_cast<int>(k_stage); y += 2) {
        for (int x = x0; x < x0 + static_cast<int>(k_stage); x += 2) {
            AURORA_LOG_INFO("test", classify(x, y));
        }
        AURORA_LOG_INFO("test");
    }

    // 统计 'S'（ClearType 彩色尖刺白边）与 'L'（柔和亮边）
    int spike = 0;
    int light = 0;
    int other = 0;
    int total = 0;
    for (int y = y0; y < y0 + static_cast<int>(k_stage); ++y) {
        for (int x = x0; x < x0 + static_cast<int>(k_stage); ++x) {
            ++total;
            const char k = classify(x, y);
            if (k == 'S') {
                ++spike;
            }
            if (k == 'L') {
                ++light;
            }
            if (k == '?') {
                ++other;
            }
        }
    }
    AURORA_LOG_INFO("test", "ClearType colored-spike white edge (S) = ", spike, "/", total, " (", 100.0 * spike / total,
                    "%)");
    AURORA_LOG_INFO("test", "soft bright edge (L) = ", light, "  dark AA edge (?) = ", other);
    AURORA_LOG_INFO(
        "test", "conclusion: S near 0 means white text on colored bg has no ClearType fringe (Supersample effective)");
    // 断言：白字在彩色/动画背景上显式 Supersample 后，不得出现 ClearType 子像素红/蓝镶边。
    // （缩放文字走离屏双线性合成，路径也需稳定无崩溃。）
    AURORA_TEST_CHECK(spike == 0);
}
}  // namespace sec_text_aa_override

namespace sec_text_focus_clear {

void run() {
    // 选区高亮为半透明蓝色矩形；ClearType 字形边缘的蓝/红彩色羽化会干扰蓝色检测。
    // 改用与背景无关的超采样抗锯齿，使「失焦后高亮应消失」的判定只反映选区本身。
    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);

    auto txt = std::make_shared<Text>("点击按钮改变计数（运行日志可见）");
    auto btn = std::make_shared<Button>();
    Column col{ColumnProps{.children = {Node{txt}, Node{btn}}}};
    col.set_focusable(false);  // 容器不抢占焦点，焦点应落在叶控件上

    BuildContext ctx;
    col.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = 640.0F, .height = 480.0F};
    col.layout(cc, ctx);

    Painter p;
    p.begin(640, 480);
    col.paint(p, Rect{.origin = Point{}, .size = Size{.width = 640.0F, .height = 480.0F}},
              ctx);  // 填充各叶控件的显示文本

    FocusManager fm;
    fm.set_root(&col);

    // 1) 点击 Text 建立选区（Press + Move）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const Rect tb = col.child_nodes()[0].bounds();
    const Point tc{.x = tb.origin.x + 2.0F, .y = tb.origin.y + (tb.size.height / 2.0F)};

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.position = tc;
    EventDispatcher::dispatch(col, press, &fm);
    AURORA_TEST_CHECK(fm.focused() == txt.get());  // 点击 Text 使其获焦

    MouseEvent move;
    move.action = MouseAction::Move;
    move.button = MouseButton::Left;
    move.position = Point{.x = tb.origin.x + tb.size.width - 2.0F, .y = tc.y};
    EventDispatcher::dispatch(col, move, &fm);

    AURORA_TEST_CHECK(txt->has_selection());  // 选区已建立
    AURORA_LOG_INFO("test", "[1] text selection established via dispatch OK");

    // 2) 点击按钮 → 焦点转移到按钮 → Text 失焦、选区清除。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const Rect bb = col.child_nodes()[1].bounds();
    const Point bc{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
    MouseEvent click_btn;
    click_btn.action = MouseAction::Press;
    click_btn.button = MouseButton::Left;
    click_btn.position = bc;
    EventDispatcher::dispatch(col, click_btn, &fm);

    AURORA_TEST_CHECK(!txt->has_selection());  // 选区被清除
    AURORA_TEST_CHECK(fm.focused() == btn.get());  // 焦点转移到按钮
    AURORA_LOG_INFO("test", "[2] clicking button blurs text and clears selection OK");

    // 3) 清背景重绘，确认文本选区高亮像素已消失。
    // 仅扫描文本自身包围盒——按钮默认背景为蓝色（Color::blue()），扫全画布会误命中按钮背景。
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 640, .height = 480}}, Color::white());
    col.paint(p, Rect{.origin = Point{}, .size = Size{.width = 640.0F, .height = 480.0F}}, ctx);
    int hl = 0;
    int x0 = static_cast<int>(std::floor(tb.origin.x));
    int y0 = static_cast<int>(std::floor(tb.origin.y));
    int x1 = static_cast<int>(std::ceil(tb.origin.x + tb.size.width));
    int y1 = static_cast<int>(std::ceil(tb.origin.y + tb.size.height));
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, p.width());
    y1 = std::min(y1, p.height());
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Color c = p.get_pixel(x, y);
            if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                ++hl;
            }
        }
    }
    AURORA_TEST_CHECK(hl == 0);  // 文本选区高亮已清除
    AURORA_LOG_INFO("test", "[3] highlight cleared after blur OK");

    // 4) 点击不可获焦容器（col.focusable=false，命中链无可获焦控件）→ 清焦点、选区消失。
    {
        // 先重新建立选区与焦点。
        MouseEvent p2 = press;
        EventDispatcher::dispatch(col, p2, &fm);
        MouseEvent m2 = move;
        EventDispatcher::dispatch(col, m2, &fm);
        MouseEvent r2 = move;
        r2.action = MouseAction::Release;
        EventDispatcher::dispatch(col, r2, &fm);
        AURORA_TEST_CHECK(txt->has_selection());
        AURORA_TEST_CHECK(fm.focused() == txt.get());
        // 点在容器内、但不在 Text/按钮上（两控件之间/下方的空白带）。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Rect bb2 = col.child_nodes()[1].bounds();
        MouseEvent blank;
        blank.action = MouseAction::Press;
        blank.button = MouseButton::Left;
        blank.position =
            Point{.x = bb2.origin.x + (bb2.size.width / 2.0F), .y = bb2.origin.y + bb2.size.height + 40.0F};
        EventDispatcher::dispatch(col, blank, &fm);
        AURORA_TEST_CHECK(fm.focused() == nullptr);  // 整条命中链不可获焦 → 清焦点
        AURORA_TEST_CHECK(!txt->has_selection());  // 旧选区随失焦清除
        AURORA_LOG_INFO("test", "[4] clicking non-focusable container area blurs text OK");
    }

    // 5) 点击根外空白（命中链为空）→ 同样清焦点、选区消失。
    {
        MouseEvent p3 = press;
        EventDispatcher::dispatch(col, p3, &fm);
        MouseEvent m3 = move;
        EventDispatcher::dispatch(col, m3, &fm);
        MouseEvent r3 = move;
        r3.action = MouseAction::Release;
        EventDispatcher::dispatch(col, r3, &fm);
        AURORA_TEST_CHECK(txt->has_selection());
        MouseEvent outside;
        outside.action = MouseAction::Press;
        outside.button = MouseButton::Left;
        outside.position = Point{.x = col.size().width + 100.0F, .y = col.size().height + 100.0F};
        EventDispatcher::dispatch(col, outside, &fm);
        AURORA_TEST_CHECK(fm.focused() == nullptr);  // 点击空白 → blur
        AURORA_TEST_CHECK(!txt->has_selection());
        AURORA_LOG_INFO("test", "[5] clicking empty space (no hit) blurs text OK");
    }

    AURORA_LOG_INFO("test", "ALL TEXT FOCUS CLEAR TESTS PASSED");
}
}  // namespace sec_text_focus_clear

namespace sec_text_justify {
namespace ar = aurora::render;

// 测试用只读常量长文本，仅极端分配失败才可能抛异常，测试进程中直接终止即可接受
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
static const std::string AURORA_K_PARA =
    "The quick brown fox jumps over the lazy dog while a silent river flows "
    "beyond the quiet hills and the pale moon rises above the sleeping town "
    "where soft lights glow and the long night slowly drifts into morning";

// 统计最右 band 内的墨迹像素（非近白像素）数量。
static auto right_band_ink(const std::uint8_t *buf, const int w, const int h, const int band) -> int {
    int cnt = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = w - band; x < w; ++x) {
            const std::size_t i =
                ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4U;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int r = buf[i];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int g = buf[i + 1];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int b = buf[i + 2];
            if (r <= 235 || g <= 235 || b <= 235) {
                ++cnt;  // 近白背景判为无墨
            }
        }
    }
    return cnt;
}

static auto render_right_band_ink(TextAlign align) -> int {
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);
    const auto t = std::make_shared<Text>(LocalizedString{AURORA_K_PARA});
    t->text_align = align;
    t->soft_wrap = true;
    t->font_size(16);
    t->text_color = Color{20, 20, 20};
    t->text_aa_mode = ar::TextAAMode::Supersample;

    constexpr int w = 320;
    constexpr int h = 400;
    const BuildContext ctx;
    t->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0, .height = 0};
    c.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    const Size sz = t->layout(c, ctx);
    AURORA_TEST_CHECK_MSG(sz.width > 0.0F && sz.height > 0.0F, "justify: layout produced a non-zero size");

    Painter p;
    p.begin(w, h);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                     .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                Color{255, 255, 255});
    t->paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = sz}, ctx);
    const std::uint8_t *buf = p.data();
    AURORA_TEST_CHECK_MSG(buf != nullptr, "justify: paint produced a non-null buffer");
    return right_band_ink(buf, w, h, 24);
}

void run() {
    AURORA_TEST_PRINTF("=== text_justify_test ===\n");
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::Supersample);
    const int left_ink = render_right_band_ink(TextAlign::Left);
    const int just_ink = render_right_band_ink(TextAlign::Justify);
    AURORA_TEST_PRINTF("right-band ink: Left=%d  Justify=%d\n", left_ink, just_ink);
    // Justify 强制非末行铺满右边界 → 最右 band 墨迹应明显多于左对齐的参差右缘。
    AURORA_TEST_CHECK_MSG(just_ink > left_ink, "justify fills more of the right edge than Left");
    ar::FontEngine::set_text_aa_mode(ar::TextAAMode::ClearType);
}
}  // namespace sec_text_justify

namespace sec_text_no_bg {
void run() {
    constexpr int w = 200;
    constexpr int h = 60;
    Painter p;
    p.begin(w, h);
    // 主帧背景 = 模拟窗口清屏色 (245,245,247) — 与 demo App::background 一致。
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0},
                     .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                Color{245, 245, 247});

    const auto txt = std::make_shared<Text>(LocalizedString{"Hello"});
    txt->modifier.set(Modifier{}.size(120.0F, 24.0F).align(Alignment::Center));
    const BuildContext ctx;
    txt->mount(ctx);
    txt->layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
                ctx);
    txt->paint(p, Rect{.origin = Point{.x = 40.0F, .y = 18.0F}, .size = Size{.width = 120.0F, .height = 24.0F}}, ctx);

    const std::uint8_t *buf = p.data();
    auto at = [&](int x, int y) -> std::array<int, 4> {
        const std::size_t i =
            ((static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)) * 4U;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        return std::array<int, 4>{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};
    };
    auto near = [](const std::array<int, 4> &c, int r, int g, int b, int tol = 4) -> bool {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        return std::abs(c[0] - r) <= tol && std::abs(c[1] - g) <= tol && std::abs(c[2] - b) <= tol;
    };
    int bg = 0;
    int glyph = 0;
    int other = 0;
    int total = 0;
    // 文字包围盒：(40,18) - (160,42)。避开边缘小细节像素，集中中部采样。
    for (int y = 20; y < 40; ++y) {
        for (int x = 42; x < 158; ++x) {
            const auto c = at(x, y);
            ++total;
            if (near(c, 245, 245, 247)) {
                ++bg;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            } else if (c[0] < 80 && c[1] < 80 && c[2] < 80) {
                ++glyph;
            } else {
                ++other;
            }
        }
    }
    AURORA_LOG_INFO("test", "in-box: total=", total, " bg(245,245,247)=", bg, " glyph(black)=", glyph,
                    " other=", other);
    const double bg_ratio = total > 0 ? static_cast<double>(bg) / total : 0.0;
    AURORA_LOG_INFO("test", "bg_ratio=", bg_ratio, " (expect > 0.50; after fix the bbox should show through bg)");
    AURORA_TEST_CHECK(bg_ratio > 0.50);
}
}  // namespace sec_text_no_bg

namespace sec_text_ptr_capture {

namespace {
auto layout_root(Widget &root, const float w, const float h) -> void {
    Constraints c;
    c.min = Size{.width = 0, .height = 0};
    c.max = Size{.width = w, .height = h};
    const BuildContext ctx;
    root.layout(c, ctx);
}
auto paint_root(Widget &root, const float w, const float h) -> void {
    Painter p;
    p.begin(static_cast<int>(w), static_cast<int>(h));
    const BuildContext ctx;
    root.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = w, .height = h}}, ctx);
}
// 扫描各 Text（按显示文本）的可命中盒（哨兵初始化，避免默认 Rect 误判）。
auto scan_texts(Widget &root) -> std::map<std::string, Rect> {
    std::map<std::string, Rect> out;
    for (int y = 0; y < 800; ++y) {
        for (int x = 0; x < 520; ++x) {
            Widget *h = EventDispatcher::hit_test(root, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
            auto const *t = dynamic_cast<Text *>(h);
            if (t == nullptr) {
                continue;
            }
            const std::string key = t->display_text();
            const auto ins = out.emplace(
                key, Rect{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}});
            Rect &r = ins.first->second;
            r.origin.x = std::min(r.origin.x, static_cast<float>(x));
            r.origin.y = std::min(r.origin.y, static_cast<float>(y));
            r.size.width = std::max(r.size.width, static_cast<float>(x) - r.origin.x);
            r.size.height = std::max(r.size.height, static_cast<float>(y) - r.origin.y);
        }
    }
    return out;
}
}  // namespace

void run() {
    int fails = 0;
    auto ck = [&](bool c, const char *m) -> void {
        if (!c) {
            AURORA_TEST_PRINTF("  [FAIL] %s\n", m);
            ++fails;
        } else {
            AURORA_TEST_PRINTF("  [PASS] %s\n", m);
        }
    };

    // 1) RTL 拖选最左字：从右端按下向左拖，须包含索引 0（首字 '默'）。
    {
        AURORA_TEST_PRINTF("[1] RTL drag-select leftmost char:\n");
        auto a = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
        Column col{ColumnProps{.children = {Node{a}}}};
        layout_root(col, 520, 800);
        paint_root(col, 520, 800);
        auto boxes = scan_texts(col);
        auto it = boxes.find("默认14pt文本");
        ck(it != boxes.end(), "text hit");
        if (it != boxes.end()) {
            const Rect &r = it->second;
            EventDispatcher ed;
            FocusManager fm;
            fm.set_root(&col);
            auto press = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Press;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                ed.dispatch_mouse(col, e, &fm);
            };
            auto move = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Move;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                ed.dispatch_mouse(col, e, &fm);
            };
            auto release = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Release;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                ed.dispatch_mouse(col, e, &fm);
            };
            const float yc = r.origin.y + (r.size.height * 0.5F);
            press(r.origin.x + r.size.width - 2.0F, yc);
            move(r.origin.x + 1.0F, yc);
            move(r.origin.x - 5.0F, yc);  // 越过左边界
            const auto sel = a->selection();
            const std::size_t lo = std::min(sel.first, sel.second);
            const std::size_t hi = std::max(sel.first, sel.second);
            ck(hi - lo == 8, "RTL drag selection covers all 8 codepoints");
            ck(lo == 0, "RTL drag selection starts at index 0 (incl. leftmost '默')");
            release(r.origin.x - 5.0F, yc);
        }
    }

    // 2) 窗口外释放：拖选时光标移出根/窗口，释放事件仍须送达并按捕获路径结束选择。
    {
        AURORA_TEST_PRINTF("[2] release outside window ends selection:\n");
        auto a = std::make_shared<Text>(TextProps{.content = LocalizedString{"默认14pt文本"}, .soft_wrap = true});
        Column col{ColumnProps{.children = {Node{a}}}};
        layout_root(col, 520, 800);
        paint_root(col, 520, 800);
        auto boxes = scan_texts(col);
        Rect r{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 30}};
        if (auto it = boxes.find("默认14pt文本"); it != boxes.end()) {
            r = it->second;
        }
        EventDispatcher ed;
        FocusManager fm;
        fm.set_root(&col);
        auto press = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Press;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            ed.dispatch_mouse(col, e, &fm);
        };
        auto move = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Move;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            ed.dispatch_mouse(col, e, &fm);
        };
        auto release = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Release;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            ed.dispatch_mouse(col, e, &fm);
        };
        const float yc = r.origin.y + (r.size.height * 0.5F);
        press(r.origin.x + r.size.width - 2.0F, yc);
        move(r.origin.x + 30.0F, yc);
        ck(a->has_selection(), "selection exists after drag");
        release(9999.0F, yc);  // 窗口外释放
        const auto s1 = a->selection();
        ck(a->has_selection(), "selection retained after release outside window (not lost)");
        move(r.origin.x + 5.0F, yc);  // 释放后再次 move
        const auto s2 = a->selection();
        ck(s2 == s1, "re-move after release does not change selection (selection ended, m_selecting=false)");
    }

    // 3) 相邻两 soft_wrap 文本不应重叠：默认 soft_wrap=true 时 Text 仅当确需换行才填满，
    //    短文本按内容宽度上报，兄弟控件可并排且各自可选中。
    {
        AURORA_TEST_PRINTF("[3] adjacent soft_wrap texts do not overlap:\n");
        auto a = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
        auto b = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"Text控件"}, .text_align = TextAlign::Left, .soft_wrap = true});
        Row row{RowProps{.children = {au::Node{a}, Node{b}}}};
        layout_root(row, 520, 800);
        paint_root(row, 520, 800);
        auto boxes = scan_texts(row);
        auto it_a = boxes.find("默认14pt文本");
        auto it_b = boxes.find("Text控件");
        ck(it_a != boxes.end() && it_b != boxes.end(), "both Text widgets are hittable");
        if (it_a != boxes.end() && it_b != boxes.end()) {
            const Rect &ra = it_a->second;
            const Rect &rb = it_b->second;
            const bool overlap = rb.origin.x < ra.origin.x + ra.size.width && ra.origin.x < rb.origin.x + rb.size.width;
            ck(!overlap, "two Text hit boxes do not overlap (second is selectable)");
        }
    }

    // 4) 复现「demo 经静态 EventDispatcher::dispatch（每个事件新建实例→无捕获）导致 RTL 拖选越过左边界后丢选区」：
    //    修复后静态 dispatch 内部委托持久实例，越过左边界仍延伸到索引 0（含最左'默'）。此路径与 run_demo 一致。
    {
        AURORA_TEST_PRINTF("[4] static dispatch path RTL drag-select leftmost char:\n");
        auto a = std::make_shared<Text>(
            TextProps{.content = LocalizedString{"默认14pt文本"}, .text_align = TextAlign::Left, .soft_wrap = true});
        Column col{ColumnProps{.children = {Node{a}}}};
        layout_root(col, 520, 800);
        paint_root(col, 520, 800);
        auto boxes = scan_texts(col);
        auto it = boxes.find("默认14pt文本");
        ck(it != boxes.end(), "text hit");
        if (it != boxes.end()) {
            const Rect &r = it->second;
            FocusManager fm;
            fm.set_root(&col);
            auto press = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Press;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                EventDispatcher::dispatch(col, e, &fm);
            };
            auto move = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Move;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                EventDispatcher::dispatch(col, e, &fm);
            };
            auto release = [&](float x, float y) -> void {
                MouseEvent e;
                e.action = MouseAction::Release;
                e.button = MouseButton::Left;
                e.position = Point{.x = x, .y = y};
                EventDispatcher::dispatch(col, e, &fm);
            };
            const float yc = r.origin.y + (r.size.height * 0.5F);
            press(r.origin.x + r.size.width - 2.0F, yc);
            move(r.origin.x + 1.0F, yc);
            move(r.origin.x - 5.0F, yc);  // 越过左边界
            const auto sel = a->selection();
            const std::size_t lo = std::min(sel.first, sel.second);
            const std::size_t hi = std::max(sel.first, sel.second);
            ck(hi - lo == 8, "static dispatch: RTL drag selection covers all 8 codepoints");
            ck(lo == 0, "static dispatch: RTL drag selection starts at index 0 (incl. leftmost '默')");
            release(r.origin.x - 5.0F, yc);
        }
    }

    AURORA_TEST_PRINTF(fails == 0 ? "text_ptr_capture: ALL PASS\n" : "text_ptr_capture: %d FAIL\n", fails);
    AURORA_TEST_CHECK_EQ(fails, 0);
}
}  // namespace sec_text_ptr_capture

namespace sec_text_selection {

void run() {
    // 选区高亮为半透明蓝色矩形；ClearType 会在字形边缘产生红/蓝彩色羽化，
    // 干扰「按 b-r>30 检测蓝色」的判定。改用与背景无关的超采样抗锯齿，使检测只反映选区高亮。
    render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);

    // 1) 绘制无选区的 Text，记录是否有蓝色高亮像素（应当没有）。
    {
        Text txt("Hello Selection");
        BuildContext ctx;  // env=nullptr 即可（用默认 Locale）
        Painter p;
        p.begin(400, 60);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
        constexpr Rect bounds{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}};
        txt.paint(p, bounds, ctx);  // 填充 m_display_text

        bool saw_highlight = false;
        for (int y = 0; y < p.height() && !saw_highlight; ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                    saw_highlight = true;  // 蓝色染色 = 高亮
                    break;
                }
            }
        }
        AURORA_TEST_CHECK(!saw_highlight);  // 无选区时不应有高亮
        AURORA_LOG_INFO("test", "[1] no highlight without selection OK");
    }

    // 1b) 只读 Text 点击获焦后不应绘制编辑光标（caret）：
    // 文本字形本身为黑（text_color），caret 亦为黑；故以「获焦 vs 未获焦的墨迹像素数差」
    // 做差分断言——旧实现下获焦会多出 caret 竖条像素，修复后应相等。
    {
        Text txt("readonly text no caret");
        BuildContext ctx;
        Painter base;
        base.begin(400, 60);
        base.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
        txt.paint(base, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                  ctx);  // 未获焦基线

        auto count_ink = [](Painter const &pp) -> int {
            int n = 0;
            for (int y = 0; y < pp.height(); ++y) {
                for (int x = 0; x < pp.width(); ++x) {
                    const Color c = pp.get_pixel(x, y);
                    if (c.m_r < 40 && c.m_g < 40 && c.m_b < 40) {
                        ++n;  // 近黑 = 字形墨迹
                    }
                }
            }
            return n;
        };
        const int ink_unfocused = count_ink(base);

        Painter focused;
        focused.begin(400, 60);
        focused.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                          Color::white());
        txt.paint(focused, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, ctx);
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 50.0F, .y = 0.0F};
        txt.on_pointer_event(press);  // 点击获焦
        focused.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                          Color::white());
        txt.paint(focused, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                  ctx);  // 获焦后重绘
        const int ink_focused = count_ink(focused);

        AURORA_TEST_CHECK(ink_focused == ink_unfocused);  // 获焦不应新增 caret 墨迹
        AURORA_LOG_INFO("test", "[1b] no caret drawn on focus (unfocused=", ink_unfocused, " focused=", ink_focused,
                        ") OK");
    }

    // 2) 制造选区（Press@0 + Move@300）后重绘，选区区域应出现蓝色高亮像素。
    {
        Text txt("Hello Selection");
        BuildContext ctx;
        Painter p;
        p.begin(400, 60);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
        constexpr Rect bounds{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}};
        txt.paint(p, bounds, ctx);  // 先填充 m_display_text

        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 0.0F, .y = 0.0F};
        txt.on_pointer_event(press);

        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.local_position = Point{.x = 300.0F, .y = 0.0F};
        txt.on_pointer_event(move);

        AURORA_TEST_CHECK(txt.has_selection());  // 选区已建立
        AURORA_LOG_INFO("test", "[2] selection established OK");

        // 重绘（高亮应在文本之后绘制，不会被文本包围盒填充覆盖）
        txt.paint(p, bounds, ctx);

        int hl_count = 0;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                    ++hl_count;
                }
            }
        }
        AURORA_TEST_CHECK(hl_count > 50);  // 高亮应覆盖相当区域（非单点噪点）
        AURORA_LOG_INFO("test", "[3] selection highlight visible (pixels=", hl_count, ") OK");

        // 4) 失焦应取消选区高亮（点击别处）
        txt.on_focus_change(false);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}},
                    Color::white());  // 清掉旧高亮
        txt.paint(p, bounds, ctx);
        int hl_after_blur = 0;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                    ++hl_after_blur;
                }
            }
        }
        AURORA_TEST_CHECK(!txt.has_selection());
        AURORA_TEST_CHECK(hl_after_blur == 0);  // 高亮已清除
        AURORA_LOG_INFO("test", "[4] highlight cleared on blur OK");
    }

    // 5) 选中某一行 Text，其选区高亮不得渗入下方相邻 Text 的绘制区域
    //    （回归：相邻控件仅余极小间隙时，半透明高亮矩形曾渗入邻行，造成假选中）。
    {
        auto line_a = std::make_shared<Text>("AAAA line one selected fully here");
        auto line_b = std::make_shared<Text>("BBBB line two must stay unselected");
        Column col{au::Node{line_a}, Node{line_b}};
        BuildContext lctx;
        Constraints lc;
        lc.min = Size{.width = 0, .height = 0};
        lc.max = Size{.width = 400, .height = 200};
        col.layout(lc, lctx);

        // 先绘制一次，填充两个 Text 的 m_display_text
        Painter warm;
        warm.begin(400, 200);
        warm.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}},
                       Color::white());
        col.paint(warm, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, lctx);

        // 定位 lineB 的像素盒
        Rect r_b{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}};
        for (int y = 0; y < 200; ++y) {
            for (int x = 0; x < 400; ++x) {
                Widget *h =
                    EventDispatcher::hit_test(col, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
                auto *t = dynamic_cast<Text *>(h);
                if (t != nullptr && t->display_text().find("BBBB") != std::string::npos) {
                    r_b.origin.x = std::min(r_b.origin.x, static_cast<float>(x));
                    r_b.origin.y = std::min(r_b.origin.y, static_cast<float>(y));
                    r_b.size.width = std::max(r_b.size.width, static_cast<float>(x) - r_b.origin.x);
                    r_b.size.height = std::max(r_b.size.height, static_cast<float>(y) - r_b.origin.y);
                }
            }
        }
        AURORA_TEST_CHECK(r_b.size.width > 0 && r_b.size.height > 0);  // lineB 确实被布局出来

        FocusManager fm;
        fm.set_root(&col);
        auto press = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Press;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto move = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Move;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto release = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Release;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };

        // 选满 lineA：右端按下 -> 拖到左端
        const float y_a = r_b.origin.y - 10.0F;  // lineA 在 lineB 上方
        press(360.0F, y_a);
        move(4.0F, y_a);
        release(4.0F, y_a);

        Painter p2;
        p2.begin(400, 200);
        p2.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, Color::white());
        col.paint(p2, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, lctx);

        int blue_in_b = 0;
        for (int yy = static_cast<int>(r_b.origin.y); yy < static_cast<int>(r_b.origin.y + r_b.size.height); ++yy) {
            for (int xx = static_cast<int>(r_b.origin.x); xx < static_cast<int>(r_b.origin.x + r_b.size.width); ++xx) {
                const Color c = p2.get_pixel(xx, yy);
                if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                    ++blue_in_b;
                }
            }
        }
        AURORA_TEST_CHECK(blue_in_b == 0);  // lineA 的高亮不得渗入 lineB
        AURORA_TEST_CHECK(!line_b->has_selection());  // lineB 自身也未被选中
        AURORA_LOG_INFO("test", "[5] selection highlight does not bleed into neighbor (blue_in_B=", blue_in_b, ") OK");
    }

    // 6) 默认 ClearType 渲染模式下（与真实 Win32 app 一致），拖选上一行不得让相邻行
    //    既被「真实选中」也不得被「视觉染色」。ClearType 文本边缘自带彩色羽化，故以
    //    「选中前后邻行蓝像素数不变」作为染色判据，而非简单断言为 0。
    {
        // 显式使用默认 ClearType，避免受其他用例改过的 AA 模式影响，贴合真实 app。
        render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType);

        auto line1 = std::make_shared<Text>("curve@0.5 = 0.500000");
        auto line2 = std::make_shared<Text>("spring value = 1.000017");
        auto line3 = std::make_shared<Text>("keyframe@0.5 = rgb(236,72,153)");
        Column col{au::Node{line1}, au::Node{line2}, Node{line3}};
        BuildContext lctx;
        Constraints lc;
        lc.min = Size{.width = 0, .height = 0};
        lc.max = Size{.width = 520, .height = 520};
        col.layout(lc, lctx);

        auto paint_all = [&](Painter &p) -> void {
            p.begin(520, 520);
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 520}},
                        Color::white());
            col.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 520}}, lctx);
        };
        Painter warm;
        paint_all(warm);  // 填充 m_display_text

        auto find_box = [&](const std::string &needle) -> Rect {
            Rect r{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}};
            for (int y = 0; y < 520; ++y) {
                for (int x = 0; x < 520; ++x) {
                    Widget *h =
                        EventDispatcher::hit_test(col, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
                    auto const *t = dynamic_cast<Text *>(h);
                    if (t && t->display_text().find(needle) != std::string::npos) {
                        r.origin.x = std::min(r.origin.x, static_cast<float>(x));
                        r.origin.y = std::min(r.origin.y, static_cast<float>(y));
                        r.size.width = std::max(r.size.width, static_cast<float>(x) - r.origin.x);
                        r.size.height = std::max(r.size.height, static_cast<float>(y) - r.origin.y);
                    }
                }
            }
            return r;
        };
        auto count_blue = [&](const Painter &p, const Rect &r) -> int {
            int n = 0;
            for (int y = static_cast<int>(r.origin.y); y < static_cast<int>(r.origin.y + r.size.height); ++y) {
                for (int x = static_cast<int>(r.origin.x); x < static_cast<int>(r.origin.x + r.size.width); ++x) {
                    const Color c = p.get_pixel(x, y);
                    if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                        ++n;
                    }
                }
            }
            return n;
        };

        Rect r1 = find_box("curve@0.5");
        Rect r2 = find_box("spring value");
        Rect r3 = find_box("keyframe@0.5");
        AURORA_TEST_CHECK(r1.size.height > 0 && r2.size.height > 0 && r3.size.height > 0);

        // 邻行蓝像素 baseline（未选中 line1 时）
        Painter base_p;
        paint_all(base_p);
        const int blue2_base = count_blue(base_p, r2);
        const int blue3_base = count_blue(base_p, r3);

        FocusManager fm;
        fm.set_root(&col);
        auto press = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Press;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto move = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Move;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };
        auto release = [&](float x, float y) -> void {
            MouseEvent e;
            e.action = MouseAction::Release;
            e.button = MouseButton::Left;
            e.position = Point{.x = x, .y = y};
            EventDispatcher::dispatch(col, e, &fm);
        };

        // 拖选 line1：右端按下 -> 拖到左端 'c' -> 继续向左下漂移进入 line2 区域
        const float y1c = r1.origin.y + (r1.size.height * 0.5F);
        press(r1.origin.x + r1.size.width - 3.0F, y1c);
        move(r1.origin.x + 2.0F, y1c);
        move(r1.origin.x + 2.0F, r2.origin.y + (r2.size.height * 0.5F));
        release(r1.origin.x + 2.0F, r2.origin.y + (r2.size.height * 0.5F));

        AURORA_TEST_CHECK(line1->has_selection());  // line1 应被选中（sanity）
        AURORA_TEST_CHECK(!line2->has_selection());  // line2 不得被真实选中
        AURORA_TEST_CHECK(!line3->has_selection());  // line3 不得被真实选中

        Painter post_p;
        paint_all(post_p);
        const int blue2_post = count_blue(post_p, r2);
        const int blue3_post = count_blue(post_p, r3);
        // 选中 line1 后，邻行蓝像素数不应增加（高亮不得渗入邻行）
        AURORA_TEST_CHECK(blue2_post == blue2_base);
        AURORA_TEST_CHECK(blue3_post == blue3_base);
        AURORA_LOG_INFO("test", "[6] neighbor unselected & uncolored under ClearType (blue2 base=", blue2_base,
                        " post=", blue2_post, ", blue3 base=", blue3_base, " post=", blue3_post, ") OK");
    }

    // 7) 回归：拖选时，按下与松开所在的字符都应计入选区（含头含尾）。
    //    此前若按下落在首字符右半、或松开落在末字符左半，端点字符会被漏选，
    //    表现为「行首/行尾存在若干字符没有被高亮选中」。
    {
        Text txt("Hello World");
        txt.font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);
        BuildContext ctx;
        txt.mount(ctx);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = 1000, .height = 100};
        txt.layout(cc, ctx);
        const Font f = txt.font;
        render::TextLayoutOpts o{};
        const std::string s = "Hello World";
        const size_t total = s.size();  // 全 ASCII：字节数 == 码点数
        auto right_half = [&](size_t idx) -> float {
            const float l = render::FontEngine::caret_x(s, idx, f, o);
            const float r = render::FontEngine::caret_x(s, idx + 1, f, o);
            return l + (0.75F * (r - l));
        };
        auto left_half = [&](size_t idx) -> float {
            const float l = render::FontEngine::caret_x(s, idx, f, o);
            const float r = render::FontEngine::caret_x(s, idx + 1, f, o);
            return l + (0.25F * (r - l));
        };

        // 按下首字符 'H'(idx0) 右半，拖到 'o'(idx4) 右半：首字符必须被选中。
        MouseEvent p;
        p.action = MouseAction::Press;
        p.local_position = Point{.x = right_half(0), .y = 5.0F};
        txt.on_pointer_event(p);
        MouseEvent mv;
        mv.action = MouseAction::Move;
        mv.local_position = Point{.x = right_half(4), .y = 5.0F};
        txt.on_pointer_event(mv);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 0);  // 首字符 'H' 被选中
        AURORA_TEST_CHECK(txt.selection().second == 5);  // 含 idx0..4 共 5 码点
        AURORA_LOG_INFO("test", "[7a] first-char inclusive on right-half press OK");

        // 按下首字符左半，拖到末字符 'd'(idx10) 左半：末字符必须被选中。
        txt.on_focus_change(false);
        MouseEvent p2;
        p2.action = MouseAction::Press;
        p2.local_position = Point{.x = left_half(0), .y = 5.0F};
        txt.on_pointer_event(p2);
        MouseEvent mv2;
        mv2.action = MouseAction::Move;
        mv2.local_position = Point{.x = left_half(10), .y = 5.0F};
        txt.on_pointer_event(mv2);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 0);
        AURORA_TEST_CHECK(txt.selection().second == total);  // 整段（末字符含入）
        AURORA_LOG_INFO("test", "[7b] last-char inclusive on left-half release OK");
    }

    // 8) 回归：多行（换行）选区中，行尾与行首的端点字符都应计入选区。
    {
        Text txt("Hello World");
        txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        render::TextLayoutOpts o{};
        const float w_hello = render::FontEngine::measure_width("Hello", f, o);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = w_hello + 2.0F, .height = 100};
        txt.layout(cc, ctx);
        // line0="Hello"(cp0-4)，line1="World"(cp6-10)。选 line0 的 'l'(idx3) 到 line1 的 'r'(idx8)。
        // 在字符内部（右半/左半）点击，端点含入无歧义。
        auto rh = [&](const std::string &line, size_t idx) -> float {
            const float l = render::FontEngine::caret_x(line, idx, f, o);
            const float r = render::FontEngine::caret_x(line, idx + 1, f, o);
            return l + (0.75F * (r - l));
        };
        auto lh = [&](const std::string &line, size_t idx) -> float {
            const float l = render::FontEngine::caret_x(line, idx, f, o);
            const float r = render::FontEngine::caret_x(line, idx + 1, f, o);
            return l + (0.25F * (r - l));
        };
        const float x0 = rh("Hello", 3);  // line0 内 idx3 右半
        const float x1 = lh("World", 8 - 6);  // line1 内 idx8 的相对位置(=2) 左半
        MouseEvent p;
        p.action = MouseAction::Press;
        p.local_position = Point{.x = x0, .y = 5.0F};
        txt.on_pointer_event(p);
        MouseEvent mv;
        mv.action = MouseAction::Move;
        mv.local_position = Point{.x = x1, .y = 50.0F};
        txt.on_pointer_event(mv);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 3);  // line0 端点 'l'(idx3) 选中
        AURORA_TEST_CHECK(txt.selection().second == 9);  // line1 端点 'r'(idx8) 选中 -> [3,9)
        AURORA_LOG_INFO("test", "[8] line-end/line-start endpoints inclusive in multi-line OK");
    }

    // 9) 严格回归：选中整段后，逐行检查高亮蓝色像素的最左/最右 x 边界，
    //    必须等于 caret_x(行, 0) 与 caret_x(行, 行码点数) —— 即每行行首与行尾字符
    //    都必须被高亮。此前若端点字符未高亮，本块会暴露（端点 x 比期望短）。
    {
        auto is_blue = [](const Color &c) -> bool { return static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30; };
        constexpr render::TextLayoutOpts o{};

        // --- 9A：单行整选，验证行尾字符被高亮 ---
        {
            const std::string s = "Hello World";
            Text txt(s);
            txt.font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);
            BuildContext ctx;
            txt.mount(ctx);
            const Font f = txt.font;
            const float full_w = render::FontEngine::measure_width(s, f, o);
            [[maybe_unused]] const float line_h = render::FontEngine::measure_height(f);
            Constraints cc;
            cc.min = Size{.width = 0, .height = 0};
            cc.max = Size{.width = 400, .height = 100};
            const Size sz = txt.layout(cc, ctx);

            MouseEvent press;
            press.action = MouseAction::Press;
            press.button = MouseButton::Left;
            press.local_position = Point{.x = 0.0F, .y = 5.0F};
            txt.on_pointer_event(press);
            MouseEvent move;
            move.action = MouseAction::Move;
            move.button = MouseButton::Left;
            move.local_position = Point{.x = 399.0F, .y = 5.0F};  // 拖到远超文本右侧
            txt.on_pointer_event(move);
            MouseEvent rel;
            rel.action = MouseAction::Release;
            rel.button = MouseButton::Left;
            rel.local_position = Point{.x = 399.0F, .y = 5.0F};
            txt.on_pointer_event(rel);
            AURORA_TEST_CHECK(txt.has_selection());

            Painter p;
            p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                        Color::white());
            txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                      ctx);

            int minx = 1e9;
            int maxx = -1e9;
            for (int y = 0; y < static_cast<int>(sz.height); ++y) {
                for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                    if (is_blue(p.get_pixel(x, y))) {
                        minx = std::min(minx, x);
                        maxx = std::max(maxx, x);
                    }
                }
            }
            const bool left_ok = (minx <= 2);
            const bool right_ok = (maxx >= static_cast<int>(full_w) - 2);
            if (!left_ok || !right_ok) {
                AURORA_LOG_INFO("test", "  [9A] minx=", minx, " maxx=", maxx, " exp_right=", full_w,
                                " left_ok=", left_ok, " right_ok=", right_ok);
            }
            AURORA_TEST_CHECK(left_ok && right_ok);
            AURORA_LOG_INFO("test", "[9A] single-line full-selection endpoint highlight OK (full_w=", full_w, ")");
        }

        // --- 9B：干净词折行（无 char-split），逐行验证端点高亮 ---
        {
            const std::string s = "Hello World";
            Text txt(s);
            txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);
            BuildContext ctx;
            txt.mount(ctx);
            const Font f = txt.font;
            const float w_hello = render::FontEngine::measure_width("Hello", f, o);
            const float w_world = render::FontEngine::measure_width("World", f, o);
            const float line_h = render::FontEngine::measure_height(f);
            // 宽度需 >= max("Hello","World")，确保按词折行、不触发 char-split。
            const float w = std::max(w_hello, w_world) + 3.0F;
            Constraints cc;
            cc.min = Size{.width = 0, .height = 0};
            cc.max = Size{.width = w, .height = 100};
            const Size sz = txt.layout(cc, ctx);
            AURORA_TEST_CHECK(line_h > 0);

            MouseEvent press;
            press.action = MouseAction::Press;
            press.button = MouseButton::Left;
            press.local_position = Point{.x = 0.0F, .y = 5.0F};
            txt.on_pointer_event(press);
            MouseEvent move;
            move.action = MouseAction::Move;
            move.button = MouseButton::Left;
            move.local_position = Point{.x = w - 1.0F, .y = line_h + 5.0F};
            txt.on_pointer_event(move);
            MouseEvent rel;
            rel.action = MouseAction::Release;
            rel.button = MouseButton::Left;
            rel.local_position = Point{.x = w - 1.0F, .y = line_h + 5.0F};
            txt.on_pointer_event(rel);
            AURORA_TEST_CHECK(txt.has_selection());

            Painter p;
            p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                        Color::white());
            txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                      ctx);

            auto line_blue_extent = [&](int li) -> std::pair<int, int> {
                int mn = 1e9;
                int mx = -1e9;
                const int y0 = static_cast<int>(static_cast<float>(li) * line_h);
                const int y1 = static_cast<int>(static_cast<float>(li + 1) * line_h);
                for (int y = y0; y < y1; ++y) {
                    for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                        if (is_blue(p.get_pixel(x, y))) {
                            mn = std::min(mn, x);
                            mx = std::max(mx, x);
                        }
                    }
                }
                return {mn, mx};
            };
            bool ep_ok = true;
            {
                auto [mn, mx] = line_blue_extent(0);
                const bool left_ok = (mn <= 2);
                const bool right_ok = (mx >= static_cast<int>(w_hello) - 2);
                if (!left_ok || !right_ok) {
                    ep_ok = false;
                    AURORA_LOG_INFO("test", "  [9B] line0 minx=", mn, " maxx=", mx, " exp=", w_hello);
                }
            }
            {
                auto [mn, mx] = line_blue_extent(1);
                const bool left_ok = (mn <= 2);
                const bool right_ok = (mx >= static_cast<int>(w_world) - 2);
                if (!left_ok || !right_ok) {
                    ep_ok = false;
                    AURORA_LOG_INFO("test", "  [9B] line1 minx=", mn, " maxx=", mx, " exp=", w_world);
                }
            }
            AURORA_TEST_CHECK(ep_ok);
            AURORA_LOG_INFO("test", "[9B] clean word-wrap per-line endpoint highlight OK (wHello=", w_hello,
                            " wWorld=", w_world, ")");
        }
    }

    // 10) 回归：Justify 两端对齐段落 —— 非末行按逐词均分拉伸铺满整行，选中该行后高亮
    //     必须延伸到行右缘（此前高亮按自然宽度计算，多行选中时行尾未被高亮）；
    //     且命中测试与拉伸后的词位一致（词间拉伸间隙归属其空格字符）。
    {
        render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
        auto is_blue = [](const Color &c) -> bool { return static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30; };
        constexpr render::TextLayoutOpts o{};

        // 构造确定性两行：line0="aa bb"（Justify 拉伸行），line1="cccccccc"（末行不拉伸）。
        Text txt("aa bb cccccccc");
        txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Justify);
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        const float w_aa = render::FontEngine::measure_width("aa", f, o);
        const float w_bb = render::FontEngine::measure_width("bb", f, o);
        const float w_c = render::FontEngine::measure_width("cccccccc", f, o);
        const float line_h = render::FontEngine::measure_height(f);
        const float w = w_c + 20.0F;  // "aa bb" 后挤不下 "cccccccc" → 折两行；"cccccccc" 单独成行
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = w, .height = 200};
        const Size sz = txt.layout(cc, ctx);
        AURORA_TEST_CHECK(sz.height > 1.5F * line_h);  // 确已折成两行

        // 10a：跨行全选后，line0（拉伸行）高亮必须达到行右缘。
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 1.0F, .y = 2.0F};
        txt.on_pointer_event(press);
        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.local_position = Point{.x = w - 1.0F, .y = line_h * 1.5F};
        txt.on_pointer_event(move);
        MouseEvent rel;
        rel.action = MouseAction::Release;
        rel.button = MouseButton::Left;
        rel.local_position = Point{.x = w - 1.0F, .y = line_h * 1.5F};
        txt.on_pointer_event(rel);
        AURORA_TEST_CHECK(txt.has_selection());

        Painter p;
        p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                    Color::white());
        txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

        int minx0 = static_cast<int>(1e9);
        int maxx0 = -1;
        for (int y = 0; y < static_cast<int>(line_h); ++y) {
            for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                if (is_blue(p.get_pixel(x, y))) {
                    minx0 = std::min(minx0, x);
                    maxx0 = std::max(maxx0, x);
                }
            }
        }
        const bool left_ok = (minx0 <= 2);
        const bool right_ok = (maxx0 >= static_cast<int>(sz.width) - 3);  // 行尾必须高亮到右缘
        if (!left_ok || !right_ok) {
            AURORA_LOG_INFO("test", "  [10a] line0 minx=", minx0, " maxx=", maxx0, " W=", sz.width);
        }
        AURORA_TEST_CHECK(left_ok && right_ok);
        AURORA_LOG_INFO("test", "[10a] justify line highlight reaches line right edge OK (W=", sz.width, ")");

        // 10b：点击拉伸间隙中点应命中词间空格（cp=2），而非按自然宽度误判为行尾字符。
        txt.on_focus_change(false);  // 清选区
        const float gap_mid = (w_aa + (w - w_bb)) * 0.5F;  // 间隙 = [wAA, W-wBB]
        MouseEvent p2;
        p2.action = MouseAction::Press;
        p2.button = MouseButton::Left;
        p2.local_position = Point{.x = gap_mid, .y = 2.0F};
        txt.on_pointer_event(p2);
        MouseEvent m2;
        m2.action = MouseAction::Move;
        m2.button = MouseButton::Left;
        m2.local_position = Point{.x = gap_mid, .y = 2.0F};
        txt.on_pointer_event(m2);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 2);  // 命中的是空格（"aa bb" 的 cp2）
        AURORA_TEST_CHECK(txt.selection().second == 3);
        AURORA_LOG_INFO("test", "[10b] justify gap hit-test maps to space char OK");
    }

    // 11) 回归：贴合 run_demo 的真实派发路径 —— 鼠标派发必须携带 FocusManager，
    //     点击拖选获焦后 Ctrl+C 才能复制选中文本（此前 demo 鼠标派发不带 fm，
    //     request_focus 静默 no-op，键盘事件到不了 Text，Ctrl+C 无效）。
    {
        const std::string src = "copy me via dispatcher";
        auto t = std::make_shared<Text>(src);
        Column col{Node{t}};
        BuildContext lctx;
        Constraints lc;
        lc.min = Size{.width = 0, .height = 0};
        lc.max = Size{.width = 400, .height = 100};
        col.layout(lc, lctx);

        FocusManager fm;
        fm.set_root(&col);
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.position = Point{.x = 2.0F, .y = 5.0F};
        EventDispatcher::dispatch(col, press, &fm);
        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.position = Point{.x = 399.0F, .y = 5.0F};
        EventDispatcher::dispatch(col, move, &fm);
        MouseEvent rel;
        rel.action = MouseAction::Release;
        rel.button = MouseButton::Left;
        rel.position = Point{.x = 399.0F, .y = 5.0F};
        EventDispatcher::dispatch(col, rel, &fm);

        AURORA_TEST_CHECK(t->has_selection());
        AURORA_TEST_CHECK(t->is_focused());  // 鼠标派发带 fm → 点击获焦
        AURORA_TEST_CHECK(fm.focused() == t.get());

        Clipboard::set_text("__PROBE__");
        if (Clipboard::get_text() != "__PROBE__") {
            AURORA_LOG_INFO("test", "[11][SKIP] system clipboard unavailable in this env");
        } else {
            KeyEvent ke;
            ke.action_ = KeyAction::Down;
            ke.key_ = static_cast<int>(KeyCode::C);
            ke.modifiers_ = ModifierKey::Control;
            EventDispatcher::dispatch(col, ke, fm);
            AURORA_TEST_CHECK(ke.is_handled_);
            AURORA_TEST_CHECK(Clipboard::get_text() == src);  // 整段拖选 → 复制全文
            AURORA_LOG_INFO("test", "[11] click-focus + Ctrl+C copy via dispatcher pipeline OK");
        }
    }

    // 12) 回归：缩放屏（如 150%，Painter::scale=1.5）下多行全选后，末行（整串按物理 DPI 绘制）
    //     行尾墨迹必须被高亮覆盖 —— GDI hinting 在 96dp 测量与物理 DPI 实绘间取整不成比例，
    //     误差在行尾累计，此前全选后末行尾部欠出「半个字符 + 标点」宽度的高亮。
    //     （高亮/命中按实显 caret（物理 DPI 前缀 extent）计算，与实绘像素对齐。）
    {
        render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
        constexpr float k_scale = 1.5F;  // 150% 缩放：字形按 144 DPI 光栅
        // 长段落软折两行：末行需足够长（≈半段），hinting 取整误差才能在行尾累计出可见宽度。
        const std::string k_para =
            "The pale moon rises above the sleeping town, and the silver light spills gently across the rooftops.";
        Text txt(k_para);
        txt.font_size(15).set_soft_wrap(true).set_align(TextAlign::Justify);
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        constexpr render::TextLayoutOpts o{};
        const float full = render::FontEngine::measure_width(k_para, f, o);
        const float line_h = render::FontEngine::measure_height(f);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = full * 0.52F, .height = 300};  // 折成两行：末行≈半段长度，以句号结尾
        const Size sz = txt.layout(cc, ctx);
        AURORA_TEST_CHECK(sz.height > 1.5F * line_h);
        // 此处 +0.5 四舍五入为既有断言口径（行数为小正数、无负值/半数值边界），改 lround 可能移动取整边界，故保留
        // NOLINTNEXTLINE(bugprone-incorrect-roundings)
        const auto n_lines = static_cast<size_t>(((sz.height - 2.0F) / line_h) + 0.5F);

        auto paint_once = [&](Painter &p) -> void {
            p.set_scale(k_scale);
            p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
            p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                        Color::white());
            txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                      ctx);
        };
        // 末行像素带（物理坐标）：[(n-1)*line_h, n*line_h) * scale
        const int y0 = static_cast<int>(static_cast<float>(n_lines - 1) * line_h * k_scale);
        const int y1 = static_cast<int>(static_cast<float>(n_lines) * line_h * k_scale);

        // 基线：无选区绘制，找末行墨迹（近黑）最右 x。
        Painter base;
        paint_once(base);
        int ink_max = -1;
        for (int y = y0; y < std::min(y1, base.height()); ++y) {
            for (int x = 0; x < base.width(); ++x) {
                const Color c = base.get_pixel(x, y);
                if (c.m_r < 100 && c.m_g < 100 && c.m_b < 100) {
                    ink_max = std::max(ink_max, x);
                }
            }
        }
        AURORA_TEST_CHECK(ink_max > 0);  // 末行确有墨迹

        // 拖选全文后重绘，末行蓝色高亮最右 x 必须覆盖墨迹最右 x。
        MouseEvent press;
        press.action = MouseAction::Press;
        press.button = MouseButton::Left;
        press.local_position = Point{.x = 0.5F, .y = 2.0F};
        txt.on_pointer_event(press);
        MouseEvent move;
        move.action = MouseAction::Move;
        move.button = MouseButton::Left;
        move.local_position = Point{.x = sz.width * 2.0F, .y = sz.height * 2.0F};  // 远超末行末字符
        txt.on_pointer_event(move);
        MouseEvent rel;
        rel.action = MouseAction::Release;
        rel.button = MouseButton::Left;
        rel.local_position = move.local_position;
        txt.on_pointer_event(rel);
        AURORA_TEST_CHECK(txt.has_selection());
        AURORA_TEST_CHECK(txt.selection().first == 0);
        AURORA_TEST_CHECK(txt.selection().second == k_para.size());  // 纯 ASCII：字节数即码点数，含末尾句号

        Painter sel;
        paint_once(sel);
        int blue_max = -1;
        for (int y = y0; y < std::min(y1, sel.height()); ++y) {
            for (int x = 0; x < sel.width(); ++x) {
                const Color c = sel.get_pixel(x, y);
                if (static_cast<int>(c.m_b) - static_cast<int>(c.m_r) > 30) {
                    blue_max = std::max(blue_max, x);
                }
            }
        }
        if (blue_max < ink_max - 1) {
            AURORA_LOG_INFO("test", "  [12] last-line ink_max=", ink_max, " blue_max=", blue_max, " (scale=", k_scale,
                            ")");
        }
        AURORA_TEST_CHECK(blue_max >= ink_max - 1);  // 高亮必须覆盖到末行墨迹右缘（含末尾标点）
        AURORA_LOG_INFO("test", "[12] scaled-display last-line tail fully highlighted OK (ink=", ink_max,
                        " blue=", blue_max, ")");
    }

    // 13) 回归：缩放屏（scale=1.5）下逐字符命中往返 —— 在每个字符的「实显中心」按下拖选，
    //     选中的必须正是该字符。此前命中按整行实显/自然宽度比线性换算，但 hinting 取整
    //     偏差在行内非线性，相邻窄字符边界处会跨界，造成「从第一个字符开始选择、
    //     实际选中的却是第二个」的 off-by-one（现改用 display_caret_x 逐字符精确边界）。
    {
        render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample);
        constexpr float k_scale = 1.5F;
        // 含大量窄字符（i/l/t）：窄字符处半字宽仅 1–2dp，线性近似残差最易跨边界。
        const std::string k_line = "The pale illimitable moonlit hills still fill the silent little mill.";
        Text txt(k_line);
        txt.font_size(15).set_soft_wrap(false);  // 单行，默认左对齐（line_off=0）
        BuildContext ctx;
        txt.mount(ctx);
        const Font f = txt.font;
        constexpr render::TextLayoutOpts o{};
        const float full = render::FontEngine::measure_width(k_line, f, o);
        Constraints cc;
        cc.min = Size{.width = 0, .height = 0};
        cc.max = Size{.width = full + 20.0F, .height = 100};
        const Size sz = txt.layout(cc, ctx);

        // 绘制一次以记录 m_paint_scale=1.5（实显命中与绘制同源）。
        Painter p;
        p.set_scale(k_scale);
        p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                    Color::white());
        txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

        auto click_sel = [&](float cx) -> void {
            txt.on_focus_change(false);  // 清上一轮选区
            MouseEvent pr;
            pr.action = MouseAction::Press;
            pr.button = MouseButton::Left;
            pr.local_position = Point{.x = cx, .y = 2.0F};
            txt.on_pointer_event(pr);
            MouseEvent mv;
            mv.action = MouseAction::Move;
            mv.button = MouseButton::Left;
            mv.local_position = Point{.x = cx, .y = 2.0F};
            txt.on_pointer_event(mv);
            MouseEvent rl;
            rl.action = MouseAction::Release;
            rl.button = MouseButton::Left;
            rl.local_position = Point{.x = cx, .y = 2.0F};
            txt.on_pointer_event(rl);
        };

        size_t mismatches = 0;
        const size_t n = k_line.size();  // 纯 ASCII：字节数即码点数
        for (size_t i = 0; i < n; ++i) {
            // 用户肉眼对准的是实绘字形 → 在该字符实显宽度内左/中/右三点采样（dp）：
            // 「从字符起始处按下」对应左采样点，选中的必须正是该字符。
            const float x0 = render::FontEngine::display_caret_x(k_line, i, f, o, k_scale);
            const float x1 = render::FontEngine::display_caret_x(k_line, i + 1, f, o, k_scale);
            if (x1 - x0 <= 1.0F) {
                continue;  // 零宽/极窄字形不采边缘
            }
            const float probes[3] = {x0 + 0.4F, (x0 + x1) * 0.5F, x1 - 0.4F};
            for (const float cx : probes) {
                click_sel(cx);
                if (!txt.has_selection() || txt.selection().first != i || txt.selection().second != i + 1) {
                    ++mismatches;
                    if (mismatches <= 5) {
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                        AURORA_LOG_INFO("test", "  [13] cp=", i, " ('", k_line[i], "') x=", cx, " got [",
                                        txt.selection().first, ", ", txt.selection().second, ")");
                    }
                }
            }
        }
        AURORA_TEST_CHECK(mismatches == 0);
        AURORA_LOG_INFO("test", "[13] per-char display-space hit round-trip OK (n=", n, ")");
    }

    // 14) 回归：实显度量必须按物理像素尺寸真算，不得退化为自然度量的伪转发 ——
    //     FT hinting 把 advance 取整到整像素，px=20 与 px=30 的逐字形 advance 不成 1.5 比，
    //     长串累计后实显宽度与自然宽度必然分叉；若有人把 display_* 改回转发别名，
    //     本用例的分叉断言与墨迹对齐断言都会失败（对应症状：缩放屏按 'a' 选中 'b'）。
    {
        constexpr float k_scale = 1.5F;
        const std::string k_line = "The pale illimitable moonlit hills still fill the silent little mill.";
        auto f = Font{.size_pt = 15.0F};
        constexpr render::TextLayoutOpts o{};
        const std::size_t n_cp = k_line.size();  // 纯 ASCII：字节数即码点数

        // 14a) 分叉：实显行宽与自然行宽在 1.5x 下必须不同（伪转发时两者恒等）。
        const float natural_w = render::FontEngine::caret_x(k_line, n_cp, f, o);
        const float display_w = render::FontEngine::display_caret_x(k_line, n_cp, f, o, k_scale);
        if (std::abs(display_w - natural_w) <= 0.1F) {
            AURORA_LOG_INFO("test", "  [14a] natural=", natural_w, " display=", display_w);
        }
        AURORA_TEST_CHECK(std::abs(display_w - natural_w) > 0.1F);
        // scale=1 退化：与自然度量逐位相等（Headless/golden 路径不受影响）。
        AURORA_TEST_CHECK(render::FontEngine::instance().display_caret_x(k_line, n_cp, f, o, 1.0F) == natural_w);

        // 14b) 墨迹对齐：1.5x 实绘整行墨迹右缘（物理 px）必须落在 display_w*scale 附近，
        //     且不得更贴近 natural_w*scale（否则说明实显度量没有与实绘同源）。
        const float phys_w = display_w * k_scale;
        Painter p;
        p.set_scale(k_scale);
        const int w = static_cast<int>(phys_w / k_scale) + 40;
        p.begin(w, 40);
        p.fill_rect(
            Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = static_cast<float>(w), .height = 40.0F}},
            Color::white());
        p.draw_text(
            Rect{.origin = Point{.x = 0.0F, .y = 2.0F}, .size = Size{.width = static_cast<float>(w), .height = 30.0F}},
            k_line, f, Color::black());
        int ink_max = -1;
        for (int y = 0; y < p.height(); ++y) {
            for (int x = 0; x < p.width(); ++x) {
                const Color c = p.get_pixel(x, y);
                if (c.m_r < 100 && c.m_g < 100 && c.m_b < 100) {
                    ink_max = std::max(ink_max, x);
                }
            }
        }
        AURORA_TEST_CHECK(ink_max > 0);
        // 末字符 '.' 右侧承距小；容差留足字形右边距与 AA 扩散（实测典型偏差 < 4px）。
        const float err_display = std::abs(static_cast<float>(ink_max) - phys_w);
        const float err_natural = std::abs(static_cast<float>(ink_max) - (natural_w * k_scale));
        if (err_display >= err_natural || err_display > 8.0F) {
            AURORA_LOG_INFO("test", "  [14b] ink_max=", ink_max, " display*s=", phys_w,
                            " natural*s=", natural_w * k_scale);
        }
        AURORA_TEST_CHECK(err_display < err_natural);  // 实显度量必须比自然度量更贴近实绘像素
        AURORA_TEST_CHECK(err_display <= 8.0F);  // 且绝对误差在字形右边距量级内
        AURORA_LOG_INFO("test", "[14] display metrics diverge from natural & align with drawn ink OK (ink=", ink_max,
                        " display*s=", phys_w, " natural*s=", natural_w * k_scale, ")");
    }

    AURORA_LOG_INFO("test", "ALL TEXT SELECTION TESTS PASSED");
}
}  // namespace sec_text_selection

namespace sec_text_spacing {
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
    return std::vector(d, d + n);
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

void run() {
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
}  // namespace sec_text_spacing

AURORA_TEST() {
    sec_text::run();
    sec_text_aa_cleartype_fringe::run();
    sec_text_aa_override::run();
    sec_text_focus_clear::run();
    sec_text_justify::run();
    sec_text_no_bg::run();
    sec_text_ptr_capture::run();
    sec_text_selection::run();
    sec_text_spacing::run();
}
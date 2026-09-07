/// 测试类型: unit
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: utest_text 单元测试
///
// 目标源单元（历史映射，保留供审计）: Text + Text
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// TextAaMode(TextAAMode，经 AA 各段行使)、BitmapFont(BitmapFont 内置字体回退)、
// TextSpan(TextSpan，经 sec_rich_text? 见 test_rich_text.cpp——TextSpan 归属 rich_text 单元)。

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
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_text {

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
    AURORA_TEST_CHECK_MSG(t.background_color == (Color{255, 255, 0, 40}), "background_color set");
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
    AURORA_TEST_CHECK_MSG(b.background_color == (Color{10, 20, 30, 40}), "rt background_color");
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
    AURORA_TEST_CHECK_MSG(t.background_color.a == 0, "default background alpha 0");
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
///        而非整段。曾在无选区时回退复制整段 display_text_，导致「选中后复制」得到全文。
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
    set_current_focus_manager(nullptr);  // 焦点已落到 widget.is_focused_，避免 fm 析构后全局悬空
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
    ke.key = static_cast<int>(KeyCode::C);
    ke.action = KeyAction::Down;
    ke.modifiers = ModifierKey::Control;
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
    ke.key = static_cast<int>(KeyCode::C);
    ke.action = KeyAction::Down;
    ke.modifiers = ModifierKey::Control;
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
            if (c.r < 100 && c.g < 100 && c.b < 100) {
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
            if (c.r < 100 && c.g < 100 && c.b < 100) {
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
            const int r = buf[i];  // NOLINT
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int g = buf[i + 1];  // NOLINT
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int b = buf[i + 2];  // NOLINT
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
        return std::array<int, 4>{buf[i], buf[i + 1], buf[i + 2], buf[i + 3]};  // NOLINT
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

AURORA_TEST() {
    sec_text::run();
    sec_text_justify::run();
    sec_text_no_bg::run();
}

}  // namespace aurora::test_cases::utest_text

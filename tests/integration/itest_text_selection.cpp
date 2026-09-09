/// 测试类型: integration
/// 目标单元: include/aurora/widget/text.h
/// 测试说明: 集成 Text 选区全链路（Text + EventDispatcher + FocusManager + FontEngine + Painter）：
///           无选区无高亮、只读不画 caret、拖选高亮与失焦清除、邻行不渗色（Supersample/ClearType）、
///           端点字符含入（单行/折行/Justify）、点击获焦 + Ctrl+C 复制、缩放屏实显度量与命中往返。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "aurora/app/clipboard.h"
#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_text_selection {

namespace render = aurora::render;

namespace {

using au::BuildContext;
using au::Clipboard;
using au::Color;
using au::Column;
using au::Constraints;
using au::EventDispatcher;
using au::FocusManager;
using au::Font;
using au::KeyAction;
using au::KeyCode;
using au::ModifierKey;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Text;
using au::TextAlign;
using au::Widget;

auto use_supersample_aa() -> void { render::FontEngine::set_text_aa_mode(render::TextAAMode::Supersample); }
auto use_cleartype_aa() -> void { render::FontEngine::set_text_aa_mode(render::TextAAMode::ClearType); }

/// 蓝色染色 = 选区高亮（高亮为半透明蓝色矩形）。
auto is_blue(const Color& c) -> bool { return static_cast<int>(c.b) - static_cast<int>(c.r) > 30; }

/// 统计 [r] 盒内蓝色染色像素数。
auto count_blue(const Painter& p, const Rect& r) -> int {
    const int x0 = std::max(static_cast<int>(r.origin.x), 0);
    const int y0 = std::max(static_cast<int>(r.origin.y), 0);
    const int x1 = std::min(static_cast<int>(r.origin.x + r.size.width), p.width());
    const int y1 = std::min(static_cast<int>(r.origin.y + r.size.height), p.height());
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (is_blue(p.get_pixel(x, y))) {
                ++n;
            }
        }
    }
    return n;
}

/// 近黑墨迹像素数（字形本体，阈值 40）。
auto count_ink(const Painter& p) -> int {
    int n = 0;
    for (int y = 0; y < p.height(); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.r < 40 && c.g < 40 && c.b < 40) {
                ++n;
            }
        }
    }
    return n;
}

/// [y0,y1) 物理行带内最深墨迹的 x（-1 = 无墨迹）。
auto max_ink_x(const Painter& p, int y0, int y1) -> int {
    int ink_max = -1;
    for (int y = std::max(y0, 0); y < std::min(y1, p.height()); ++y) {
        for (int x = 0; x < p.width(); ++x) {
            const Color c = p.get_pixel(x, y);
            if (c.r < 100 && c.g < 100 && c.b < 100) {
                ink_max = std::max(ink_max, x);
            }
        }
    }
    return ink_max;
}

/// 全画布逐点命中扫描：定位 display_text 含 needle 的 Text 的可命中盒。
auto find_text_box(Widget& root, int canvas_w, int canvas_h, const std::string& needle) -> Rect {
    Rect r{.origin = Point{.x = 1e9F, .y = 1e9F}, .size = Size{.width = -1e9F, .height = -1e9F}};
    for (int y = 0; y < canvas_h; ++y) {
        for (int x = 0; x < canvas_w; ++x) {
            Widget* h = EventDispatcher::hit_test(root, Point{.x = static_cast<float>(x), .y = static_cast<float>(y)});
            const auto* t = dynamic_cast<Text*>(h);
            if (t != nullptr && t->display_text().find(needle) != std::string::npos) {
                r.origin.x = std::min(r.origin.x, static_cast<float>(x));
                r.origin.y = std::min(r.origin.y, static_cast<float>(y));
                r.size.width = std::max(r.size.width, static_cast<float>(x) - r.origin.x);
                r.size.height = std::max(r.size.height, static_cast<float>(y) - r.origin.y);
            }
        }
    }
    return r;
}

/// 鼠标序列发射器（经 EventDispatcher 派发）。
using Sink = std::function<void(MouseAction, float, float)>;

/// 含头含尾采样点：字符 idx 的右半 / 左半 x（据 caret_x 边界）。
auto right_half(const std::string& s, std::size_t idx, const Font& f, const render::TextLayoutOpts& o) -> float {
    const float l = render::FontEngine::caret_x(s, idx, f, o);
    const float r = render::FontEngine::caret_x(s, idx + 1, f, o);
    return l + (0.75F * (r - l));
}
auto left_half(const std::string& s, std::size_t idx, const Font& f, const render::TextLayoutOpts& o) -> float {
    const float l = render::FontEngine::caret_x(s, idx, f, o);
    const float r = render::FontEngine::caret_x(s, idx + 1, f, o);
    return l + (0.25F * (r - l));
}

/// 构造有界约束。
auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(no_highlight_without_selection) {
    use_supersample_aa();
    Text txt("Hello Selection");
    BuildContext ctx;
    Painter p;
    p.begin(400, 60);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, Color::white());
    txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}, ctx);

    AURORA_TEST_CHECK_FALSE(txt.has_selection());
    AURORA_TEST_CHECK_EQ(count_blue(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}}),
                         0);
}

AURORA_TEST_CASE(readonly_text_draws_no_caret_on_focus) {
    // 只读 Text 点击获焦后不应绘制编辑光标（caret 亦为黑）：
    // 以「获焦 vs 未获焦的墨迹像素数差」做差分断言——旧实现下获焦会多出 caret 竖条像素。
    use_supersample_aa();
    Text txt("readonly text no caret");
    BuildContext ctx;
    const Rect full{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}};

    Painter base;
    base.begin(400, 60);
    base.fill_rect(full, Color::white());
    txt.paint(base, full, ctx);
    const int ink_unfocused = count_ink(base);
    AURORA_TEST_CHECK_MSG(ink_unfocused > 0, "baseline glyphs produced ink");

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 50.0F, .y = 0.0F};
    txt.on_pointer_event(press);  // 点击获焦

    Painter focused;
    focused.begin(400, 60);
    focused.fill_rect(full, Color::white());
    txt.paint(focused, full, ctx);
    const int ink_focused = count_ink(focused);

    AURORA_TEST_CHECK_EQ(ink_focused, ink_unfocused);
}

AURORA_TEST_CASE(selection_highlight_visible_and_cleared_on_blur) {
    use_supersample_aa();
    Text txt("Hello Selection");
    BuildContext ctx;
    const Rect full{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 60}};
    Painter p;
    p.begin(400, 60);
    p.fill_rect(full, Color::white());
    txt.paint(p, full, ctx);  // 先填充 display_text_

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 0.0F, .y = 0.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = 300.0F, .y = 0.0F};
    txt.on_pointer_event(mv);
    AURORA_TEST_CHECK_MSG(txt.has_selection(), "selection established");

    // 重绘（高亮在文本之后绘制，不会被文本包围盒填充覆盖）。
    txt.paint(p, full, ctx);
    const int hl_count = count_blue(p, full);
    AURORA_TEST_CHECK_MSG(hl_count > 50, "selection highlight covers a substantial area");

    // 失焦（点击别处）应取消选区高亮。
    txt.on_focus_change(false);
    p.fill_rect(full, Color::white());
    txt.paint(p, full, ctx);
    AURORA_TEST_CHECK_FALSE(txt.has_selection());
    AURORA_TEST_CHECK_EQ(count_blue(p, full), 0);
}

AURORA_TEST_CASE(highlight_does_not_bleed_into_neighbor_row) {
    // 回归：相邻控件仅余极小间隙时，半透明高亮矩形不得渗入邻行造成假选中。
    use_supersample_aa();
    auto line_a = std::make_shared<Text>("AAAA line one selected fully here");
    auto line_b = std::make_shared<Text>("BBBB line two must stay unselected");
    Column col{Node{line_a}, Node{line_b}};
    BuildContext lctx;
    col.layout(bounded(400.0F, 200.0F), lctx);

    // 定位两行的可命中盒。
    const Rect r_a = find_text_box(col, 400, 200, "AAAA");
    const Rect r_b = find_text_box(col, 400, 200, "BBBB");
    AURORA_TEST_REQUIRE_MSG(r_a.size.width > 0.0F && r_a.size.height > 0.0F, "line A laid out and hittable");
    AURORA_TEST_REQUIRE_MSG(r_b.size.width > 0.0F && r_b.size.height > 0.0F, "line B laid out and hittable");

    FocusManager fm;
    fm.set_root(&col);
    Sink dispatch = [&](MouseAction action, float x, float y) -> void {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        (void)EventDispatcher::dispatch(col, e, &fm);
    };

    // 选满 lineA：右端按下 -> 拖到左端 -> 抬起。
    const float y_a = r_a.origin.y + (r_a.size.height * 0.5F);
    dispatch(MouseAction::Press, r_a.origin.x + r_a.size.width - 3.0F, y_a);
    dispatch(MouseAction::Move, r_a.origin.x + 4.0F, y_a);
    dispatch(MouseAction::Release, r_a.origin.x + 4.0F, y_a);
    AURORA_TEST_REQUIRE_MSG(line_a->has_selection(), "line A fully selected");

    Painter p2;
    p2.begin(400, 200);
    p2.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, Color::white());
    col.paint(p2, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 400, .height = 200}}, lctx);

    AURORA_TEST_CHECK_EQ(count_blue(p2, r_b), 0);  // lineA 的高亮不得渗入 lineB
    AURORA_TEST_CHECK_FALSE(line_b->has_selection());
}

AURORA_TEST_CASE(neighbor_rows_unselected_and_uncolored_under_cleartype) {
    // 回归：默认 ClearType 渲染下（与真实 app 一致），拖选上一行不得让相邻行被真实选中、
    // 也不得被视觉染色。ClearType 文本边缘自带彩色羽化，故以「选中前后邻行蓝像素数不变」
    // 作为染色判据，而非简单断言为 0。
    use_cleartype_aa();
    auto line1 = std::make_shared<Text>("curve@0.5 = 0.500000");
    auto line2 = std::make_shared<Text>("spring value = 1.000017");
    auto line3 = std::make_shared<Text>("keyframe@0.5 = rgb(236,72,153)");
    Column col{Node{line1}, Node{line2}, Node{line3}};
    BuildContext lctx;
    col.layout(bounded(520.0F, 520.0F), lctx);

    auto paint_all = [&](Painter& p) -> void {
        p.begin(520, 520);
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 520}}, Color::white());
        col.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 520, .height = 520}}, lctx);
    };
    Painter warm;
    paint_all(warm);  // 填充 display_text_

    const Rect r1 = find_text_box(col, 520, 520, "curve@0.5");
    const Rect r2 = find_text_box(col, 520, 520, "spring value");
    const Rect r3 = find_text_box(col, 520, 520, "keyframe@0.5");
    AURORA_TEST_REQUIRE_MSG(r1.size.height > 0.0F && r2.size.height > 0.0F && r3.size.height > 0.0F,
                            "all three lines laid out and hittable");

    // 邻行蓝像素 baseline（未选中 line1 时）。
    Painter base_p;
    paint_all(base_p);
    const int blue2_base = count_blue(base_p, r2);
    const int blue3_base = count_blue(base_p, r3);

    FocusManager fm;
    fm.set_root(&col);
    Sink dispatch = [&](MouseAction action, float x, float y) -> void {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        (void)EventDispatcher::dispatch(col, e, &fm);
    };

    // 拖选 line1：右端按下 -> 拖到左端 -> 继续向左下漂移进入 line2 区域后释放。
    const float y1c = r1.origin.y + (r1.size.height * 0.5F);
    dispatch(MouseAction::Press, r1.origin.x + r1.size.width - 3.0F, y1c);
    dispatch(MouseAction::Move, r1.origin.x + 2.0F, y1c);
    dispatch(MouseAction::Move, r1.origin.x + 2.0F, r2.origin.y + (r2.size.height * 0.5F));
    dispatch(MouseAction::Release, r1.origin.x + 2.0F, r2.origin.y + (r2.size.height * 0.5F));

    AURORA_TEST_CHECK_MSG(line1->has_selection(), "line 1 selected (sanity)");
    AURORA_TEST_CHECK_FALSE(line2->has_selection());
    AURORA_TEST_CHECK_FALSE(line3->has_selection());

    Painter post_p;
    paint_all(post_p);
    const int blue2_post = count_blue(post_p, r2);
    const int blue3_post = count_blue(post_p, r3);
    AURORA_TEST_CHECK_EQ(blue2_post, blue2_base);  // 高亮不得渗入邻行
    AURORA_TEST_CHECK_EQ(blue3_post, blue3_base);
}

AURORA_TEST_CASE(endpoint_chars_inclusive_on_drag_select) {
    // 回归：拖选时，按下与松开所在的字符都应计入选区（含头含尾）。
    use_supersample_aa();
    Text txt("Hello World");
    txt.font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);
    BuildContext ctx;
    txt.mount(ctx);
    txt.layout(bounded(1000.0F, 100.0F), ctx);
    const Font f = txt.font;
    constexpr render::TextLayoutOpts o{};
    const std::string s = "Hello World";
    const std::size_t total = s.size();  // 全 ASCII：字节数 == 码点数

    // 按下首字符 'H'(idx0) 右半，拖到 'o'(idx4) 右半：首字符必须被选中。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = right_half(s, 0, f, o), .y = 5.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = right_half(s, 4, f, o), .y = 5.0F};
    txt.on_pointer_event(mv);
    AURORA_TEST_CHECK_MSG(txt.has_selection(), "selection established");
    AURORA_TEST_CHECK_EQ(txt.selection().first, std::size_t{0});  // 首字符 'H' 被选中
    AURORA_TEST_CHECK_EQ(txt.selection().second, std::size_t{5});  // 含 idx0..4 共 5 码点

    // 按下首字符左半，拖到末字符 'd'(idx10) 左半：末字符必须被选中。
    txt.on_focus_change(false);
    MouseEvent press2;
    press2.action = MouseAction::Press;
    press2.button = MouseButton::Left;
    press2.local_position = Point{.x = left_half(s, 0, f, o), .y = 5.0F};
    txt.on_pointer_event(press2);
    MouseEvent mv2;
    mv2.action = MouseAction::Move;
    mv2.button = MouseButton::Left;
    mv2.local_position = Point{.x = left_half(s, 10, f, o), .y = 5.0F};
    txt.on_pointer_event(mv2);
    AURORA_TEST_CHECK_MSG(txt.has_selection(), "selection re-established");
    AURORA_TEST_CHECK_EQ(txt.selection().first, std::size_t{0});
    AURORA_TEST_CHECK_EQ(txt.selection().second, total);  // 整段（末字符含入）
}

AURORA_TEST_CASE(multi_line_endpoints_inclusive) {
    // 回归：多行（换行）选区中，行尾与行首的端点字符都应计入选区。
    use_supersample_aa();
    Text txt("Hello World");
    txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);
    BuildContext ctx;
    txt.mount(ctx);
    const Font f = txt.font;
    constexpr render::TextLayoutOpts o{};
    const float w_hello = render::FontEngine::measure_width("Hello", f, o);
    txt.layout(bounded(w_hello + 2.0F, 100.0F), ctx);
    // line0="Hello"(cp0-4)，line1="World"(cp6-10)。选 line0 的 'l'(idx3) 到 line1 的 'r'(idx8)。
    // 在字符内部（右半/左半）点击，端点含入无歧义。
    const float x0 = right_half("Hello", 3, f, o);  // line0 内 idx3 右半
    const float x1 = left_half("World", 8 - 6, f, o);  // line1 内 idx8 的相对位置(=2) 左半
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = x0, .y = 5.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = x1, .y = 50.0F};
    txt.on_pointer_event(mv);
    AURORA_TEST_CHECK_MSG(txt.has_selection(), "cross-line selection established");
    AURORA_TEST_CHECK_EQ(txt.selection().first, std::size_t{3});  // line0 端点 'l'(idx3) 选中
    AURORA_TEST_CHECK_EQ(txt.selection().second, std::size_t{9});  // line1 端点 'r'(idx8) 选中 -> [3,9)
}

AURORA_TEST_CASE(single_line_full_selection_highlights_line_endpoints) {
    // 严格回归：整段选中后，高亮蓝色像素的最左/最右 x 必须达到行首与行尾字符
    // （此前若端点字符未高亮，本用例会暴露）。
    use_supersample_aa();
    const std::string s = "Hello World";
    Text txt(s);
    txt.font_size(24).set_soft_wrap(false).set_align(TextAlign::Left);
    BuildContext ctx;
    txt.mount(ctx);
    const Font f = txt.font;
    constexpr render::TextLayoutOpts o{};
    const float full_w = render::FontEngine::measure_width(s, f, o);
    const Size sz = txt.layout(bounded(400.0F, 100.0F), ctx);

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 0.0F, .y = 5.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = 399.0F, .y = 5.0F};  // 拖到远超文本右侧
    txt.on_pointer_event(mv);
    MouseEvent rel;
    rel.action = MouseAction::Release;
    rel.button = MouseButton::Left;
    rel.local_position = Point{.x = 399.0F, .y = 5.0F};
    txt.on_pointer_event(rel);
    AURORA_TEST_REQUIRE_MSG(txt.has_selection(), "full selection established");

    Painter p;
    p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                Color::white());
    txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

    const int minx_blue = [&]() -> int {
        int mn = 1 << 30;
        for (int y = 0; y < static_cast<int>(sz.height); ++y) {
            for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                if (is_blue(p.get_pixel(x, y))) {
                    mn = std::min(mn, x);
                }
            }
        }
        return mn;
    }();
    const int maxx_blue = [&]() -> int {
        int mx = -1;
        for (int y = 0; y < static_cast<int>(sz.height); ++y) {
            for (int x = 0; x < static_cast<int>(sz.width); ++x) {
                if (is_blue(p.get_pixel(x, y))) {
                    mx = std::max(mx, x);
                }
            }
        }
        return mx;
    }();
    AURORA_TEST_CHECK_MSG(minx_blue <= 2, "line-start char highlighted");
    AURORA_TEST_CHECK_MSG(maxx_blue >= static_cast<int>(full_w) - 2, "line-end char highlighted");
}

AURORA_TEST_CASE(word_wrap_per_line_endpoint_highlight) {
    // 干净词折行（无 char-split），逐行验证端点高亮。
    use_supersample_aa();
    const std::string s = "Hello World";
    Text txt(s);
    txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Left);
    BuildContext ctx;
    txt.mount(ctx);
    const Font f = txt.font;
    constexpr render::TextLayoutOpts o{};
    const float w_hello = render::FontEngine::measure_width("Hello", f, o);
    const float w_world = render::FontEngine::measure_width("World", f, o);
    const float line_h = render::FontEngine::measure_height(f);
    // 宽度需 >= max("Hello","World")，确保按词折行、不触发 char-split。
    const float w = std::max(w_hello, w_world) + 3.0F;
    const Size sz = txt.layout(bounded(w, 100.0F), ctx);
    AURORA_TEST_CHECK_MSG(line_h > 0.0F, "line height measured");

    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 0.0F, .y = 5.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = w - 1.0F, .y = line_h + 5.0F};
    txt.on_pointer_event(mv);
    MouseEvent rel;
    rel.action = MouseAction::Release;
    rel.button = MouseButton::Left;
    rel.local_position = Point{.x = w - 1.0F, .y = line_h + 5.0F};
    txt.on_pointer_event(rel);
    AURORA_TEST_REQUIRE_MSG(txt.has_selection(), "cross-line selection established");

    Painter p;
    p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                Color::white());
    txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

    auto line_blue_extent = [&](int li) -> std::pair<int, int> {
        int mn = 1 << 30;
        int mx = -1;
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
    const auto [mn0, mx0] = line_blue_extent(0);
    AURORA_TEST_CHECK_MSG(mn0 <= 2 && mx0 >= static_cast<int>(w_hello) - 2, "line0 endpoints highlighted");
    const auto [mn1, mx1] = line_blue_extent(1);
    AURORA_TEST_CHECK_MSG(mn1 <= 2 && mx1 >= static_cast<int>(w_world) - 2, "line1 endpoints highlighted");
}

AURORA_TEST_CASE(justify_line_highlight_reaches_right_edge_and_gap_hits_space) {
    // 回归：Justify 两端对齐段落 —— 非末行按逐词均分拉伸铺满整行，选中该行后高亮必须延伸到
    // 行右缘；且命中测试与拉伸后的词位一致（词间拉伸间隙归属其空格字符）。
    use_supersample_aa();
    Text txt("aa bb cccccccc");
    txt.font_size(24).set_soft_wrap(true).set_align(TextAlign::Justify);
    BuildContext ctx;
    txt.mount(ctx);
    const Font f = txt.font;
    constexpr render::TextLayoutOpts o{};
    const float w_aa = render::FontEngine::measure_width("aa", f, o);
    const float w_bb = render::FontEngine::measure_width("bb", f, o);
    const float w_c = render::FontEngine::measure_width("cccccccc", f, o);
    const float line_h = render::FontEngine::measure_height(f);
    const float w = w_c + 20.0F;  // "aa bb" 后挤不下 "cccccccc" → 折两行；"cccccccc" 单独成行
    const Size sz = txt.layout(bounded(w, 200.0F), ctx);
    AURORA_TEST_REQUIRE_MSG(sz.height > 1.5F * line_h, "wrapped into two lines");

    // 跨行全选后，line0（拉伸行）高亮必须达到行右缘。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 1.0F, .y = 2.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = w - 1.0F, .y = line_h * 1.5F};
    txt.on_pointer_event(mv);
    MouseEvent rel;
    rel.action = MouseAction::Release;
    rel.button = MouseButton::Left;
    rel.local_position = Point{.x = w - 1.0F, .y = line_h * 1.5F};
    txt.on_pointer_event(rel);
    AURORA_TEST_REQUIRE_MSG(txt.has_selection(), "full selection established");

    Painter p;
    p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                Color::white());
    txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);

    int minx0 = 1 << 30;
    int maxx0 = -1;
    for (int y = 0; y < static_cast<int>(line_h); ++y) {
        for (int x = 0; x < static_cast<int>(sz.width); ++x) {
            if (is_blue(p.get_pixel(x, y))) {
                minx0 = std::min(minx0, x);
                maxx0 = std::max(maxx0, x);
            }
        }
    }
    AURORA_TEST_CHECK_MSG(minx0 <= 2, "justify line highlight starts at line left edge");
    AURORA_TEST_CHECK_MSG(maxx0 >= static_cast<int>(sz.width) - 3, "justify line highlight reaches line right edge");

    // 点击拉伸间隙中点应命中词间空格（cp=2），而非按自然宽度误判为行尾字符。
    txt.on_focus_change(false);  // 清选区
    const float gap_mid = (w_aa + (w - w_bb)) * 0.5F;  // 间隙 = [wAA, W-wBB]
    MouseEvent press2;
    press2.action = MouseAction::Press;
    press2.button = MouseButton::Left;
    press2.local_position = Point{.x = gap_mid, .y = 2.0F};
    txt.on_pointer_event(press2);
    MouseEvent mv2;
    mv2.action = MouseAction::Move;
    mv2.button = MouseButton::Left;
    mv2.local_position = Point{.x = gap_mid, .y = 2.0F};
    txt.on_pointer_event(mv2);
    AURORA_TEST_CHECK_MSG(txt.has_selection(), "gap press establishes selection");
    AURORA_TEST_CHECK_EQ(txt.selection().first, std::size_t{2});  // 命中的是空格（"aa bb" 的 cp2）
    AURORA_TEST_CHECK_EQ(txt.selection().second, std::size_t{3});
}

AURORA_TEST_CASE(dispatcher_click_focus_enables_ctrl_c_copy) {
    // 回归：贴合 run_demo 的真实派发路径 —— 鼠标派发必须携带 FocusManager，
    // 点击拖选获焦后 Ctrl+C 才能复制选中文本。
    use_supersample_aa();
    const std::string src = "copy me via dispatcher";
    auto t = std::make_shared<Text>(src);
    Column col{Node{t}};
    BuildContext lctx;
    col.layout(bounded(400.0F, 100.0F), lctx);

    FocusManager fm;
    fm.set_root(&col);
    Sink dispatch = [&](MouseAction action, float x, float y) -> void {
        MouseEvent e;
        e.action = action;
        e.button = MouseButton::Left;
        e.position = Point{.x = x, .y = y};
        (void)EventDispatcher::dispatch(col, e, &fm);
    };
    dispatch(MouseAction::Press, 2.0F, 5.0F);
    dispatch(MouseAction::Move, 399.0F, 5.0F);
    dispatch(MouseAction::Release, 399.0F, 5.0F);

    AURORA_TEST_CHECK_MSG(t->has_selection(), "selection established via dispatcher");
    AURORA_TEST_CHECK_MSG(t->is_focused(), "mouse dispatch with fm focuses the text");
    AURORA_TEST_CHECK_MSG(fm.focused() == t.get(), "focus manager tracks the text");

    Clipboard::set_text("__PROBE__");
    if (Clipboard::get_text() != "__PROBE__") {
        AURORA_TEST_TRACE("system clipboard unavailable in this env; skip Ctrl+C copy assertion");
        return;
    }
    KeyEvent ke;
    ke.action = KeyAction::Down;
    ke.key = static_cast<int>(KeyCode::C);
    ke.modifiers = ModifierKey::Control;
    (void)EventDispatcher::dispatch(col, ke, fm);
    AURORA_TEST_CHECK_MSG(ke.is_handled, "Ctrl+C consumed by focused text");
    AURORA_TEST_CHECK_MSG(Clipboard::get_text() == src, "full drag-selection copied to clipboard");
}

AURORA_TEST_CASE(scaled_display_last_line_tail_fully_highlighted) {
    // 回归：缩放屏（150%）下多行全选后，末行行尾墨迹必须被高亮覆盖 ——
    // GDI/FT hinting 在 96dp 测量与物理 DPI 实绘间取整不成比例，误差在行尾累计。
    use_supersample_aa();
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
    const Size sz = txt.layout(bounded(full * 0.52F, 300.0F), ctx);  // 折成两行：末行≈半段长度
    AURORA_TEST_REQUIRE_MSG(sz.height > 1.5F * line_h, "wrapped into two lines");
    const auto n_lines =
        static_cast<std::size_t>(std::lround((sz.height - 2.0F) / line_h));  // 行数为小正数，四舍五入口径沿用旧断言

    auto paint_once = [&](Painter& p) -> void {
        p.set_scale(k_scale);
        p.begin(static_cast<int>(sz.width), static_cast<int>(sz.height));
        p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}},
                    Color::white());
        txt.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = sz.width, .height = sz.height}}, ctx);
    };
    // 末行像素带（物理坐标）：[(n-1)*line_h, n*line_h) * scale。
    const int y0 = static_cast<int>(static_cast<float>(n_lines - 1) * line_h * k_scale);
    const int y1 = static_cast<int>(static_cast<float>(n_lines) * line_h * k_scale);

    // 基线：无选区绘制，找末行墨迹（近黑）最右 x。
    Painter base;
    paint_once(base);
    const int ink_max = max_ink_x(base, y0, y1);
    AURORA_TEST_CHECK_MSG(ink_max > 0, "last line has ink");

    // 拖选全文后重绘，末行蓝色高亮最右 x 必须覆盖墨迹最右 x。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    press.local_position = Point{.x = 0.5F, .y = 2.0F};
    txt.on_pointer_event(press);
    MouseEvent mv;
    mv.action = MouseAction::Move;
    mv.button = MouseButton::Left;
    mv.local_position = Point{.x = sz.width * 2.0F, .y = sz.height * 2.0F};  // 远超末行末字符
    txt.on_pointer_event(mv);
    MouseEvent rel;
    rel.action = MouseAction::Release;
    rel.button = MouseButton::Left;
    rel.local_position = mv.local_position;
    txt.on_pointer_event(rel);
    AURORA_TEST_REQUIRE_MSG(txt.has_selection(), "full selection established");
    AURORA_TEST_CHECK_EQ(txt.selection().first, std::size_t{0});
    AURORA_TEST_CHECK_EQ(txt.selection().second, k_para.size());  // 纯 ASCII：含末尾句号

    Painter sel;
    paint_once(sel);
    int blue_max = -1;
    for (int y = y0; y < std::min(y1, sel.height()); ++y) {
        for (int x = 0; x < sel.width(); ++x) {
            if (is_blue(sel.get_pixel(x, y))) {
                blue_max = std::max(blue_max, x);
            }
        }
    }
    AURORA_TEST_CHECK_MSG(blue_max >= ink_max - 1, "highlight covers last-line ink tail (incl. trailing punctuation)");
}

AURORA_TEST_CASE(per_char_display_space_hit_round_trip) {
    // 回归：缩放屏（scale=1.5）下逐字符命中往返 —— 在每个字符的「实显中心」按下拖选，
    // 选中的必须正是该字符（display_caret_x 逐字符精确边界，而非整行线性换算）。
    use_supersample_aa();
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
    const Size sz = txt.layout(bounded(full + 20.0F, 100.0F), ctx);

    // 绘制一次以记录 paint_scale_=1.5（实显命中与绘制同源）。
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

    std::size_t mismatches = 0;
    const std::size_t n = k_line.size();  // 纯 ASCII：字节数即码点数
    for (std::size_t i = 0; i < n; ++i) {
        // 用户肉眼对准的是实绘字形 → 在该字符实显宽度内左/中/右三点采样（dp）。
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
                    AURORA_TEST_TRACE("cp " + std::to_string(i) + " probe " + std::to_string(cx) + " got [" +
                                      std::to_string(txt.selection().first) + ", " +
                                      std::to_string(txt.selection().second) + ")");
                }
            }
        }
    }
    AURORA_TEST_CHECK_EQ(mismatches, std::size_t{0});
}

AURORA_TEST_CASE(display_metrics_diverge_from_natural_and_align_with_ink) {
    // 回归：实显度量必须按物理像素尺寸真算，不得退化为自然度量的伪转发 ——
    // FT hinting 把 advance 取整到整像素，px=20 与 px=30 的逐字形 advance 不成 1.5 比；
    // 若有人把 display_* 改回转发别名，本用例的分叉断言与墨迹对齐断言都会失败。
    use_supersample_aa();
    constexpr float k_scale = 1.5F;
    const std::string k_line = "The pale illimitable moonlit hills still fill the silent little mill.";
    const auto f = Font{.size_pt = 15.0F};
    constexpr render::TextLayoutOpts o{};
    const std::size_t n_cp = k_line.size();  // 纯 ASCII：字节数即码点数

    // 分叉：实显行宽与自然行宽在 1.5x 下必须不同（伪转发时两者恒等）。
    const float natural_w = render::FontEngine::caret_x(k_line, n_cp, f, o);
    const float display_w = render::FontEngine::display_caret_x(k_line, n_cp, f, o, k_scale);
    AURORA_TEST_CHECK_MSG(std::abs(display_w - natural_w) > 0.1F, "display width diverges from natural at 1.5x");
    // scale=1 退化：与自然度量逐位相等（Headless/golden 路径不受影响）。
    AURORA_TEST_CHECK_EQ(render::FontEngine::instance().display_caret_x(k_line, n_cp, f, o, 1.0F), natural_w);

    // 墨迹对齐：1.5x 实绘整行墨迹右缘（物理 px）必须更贴近 display_w*scale 而非 natural_w*scale。
    const float phys_w = display_w * k_scale;
    Painter p;
    p.set_scale(k_scale);
    const int w = static_cast<int>(phys_w / k_scale) + 40;
    p.begin(w, 40);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = static_cast<float>(w), .height = 40.0F}},
                Color::white());
    p.draw_text(
        Rect{.origin = Point{.x = 0.0F, .y = 2.0F}, .size = Size{.width = static_cast<float>(w), .height = 30.0F}},
        k_line, f, Color::black());
    const int ink_max = max_ink_x(p, 0, p.height());
    AURORA_TEST_CHECK_MSG(ink_max > 0, "drawn line produced ink");
    // 末字符 '.' 右侧承距小；容差留足字形右边距与 AA 扩散（实测典型偏差 < 4px）。
    const float err_display = std::abs(static_cast<float>(ink_max) - phys_w);
    const float err_natural = std::abs(static_cast<float>(ink_max) - (natural_w * k_scale));
    AURORA_TEST_CHECK_MSG(err_display < err_natural, "display metrics align closer to drawn ink than natural");
    AURORA_TEST_CHECK_MSG(err_display <= 8.0F, "absolute error within glyph right-bearing magnitude");
}

}  // namespace aurora::test_cases::itest_text_selection

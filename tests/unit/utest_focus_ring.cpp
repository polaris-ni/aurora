/// 测试类型: unit
/// 目标单元: src/aurora/widget/widget.cpp（Widget::paint_content 的统一焦点环）
/// 测试说明: 持有焦点的控件由基类在自身盒外画出主题色焦点环（可观测停点，规格 §4.3）；
///           未获焦无环；已自带聚焦态外观的控件（TextInput）经 wants_focus_ring() 关闭，不出环。
///           判据为「盒外 2–4 dp 环带内的蓝色染色像素数」，与控件自身外观无关。

#include <algorithm>
#include <cmath>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_focus_ring {

namespace {

using au::BuildContext;
using au::Button;
using au::Color;
using au::Constraints;
using au::FocusManager;
using au::Modifier;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::TextInput;
using au::Widget;

constexpr float AURORA_CANVAS_W = 400.0F;
constexpr float AURORA_CANVAS_H = 300.0F;
constexpr float AURORA_BOX_X = 60.0F;  ///< 被测盒原点：离画布边缘足够远，环带完整可见
constexpr float AURORA_BOX_Y = 80.0F;
constexpr float AURORA_RING_GAP = 2.0F;  ///< 与 widget.cpp 的环几何常量一致
constexpr float AURORA_RING_THICKNESS = 2.0F;

/// 主题 primary 为蓝色系；选区/环等蓝色染色的共同特征是蓝通道显著占优。
auto is_blue(const Color &c) -> bool { return static_cast<int>(c.b) - static_cast<int>(c.r) > 30; }

/// 把控件挂载/布局后画到白底画布，返回其绝对盒。
template <typename W>
auto paint_on_white(W &w, Painter &p, bool focused) -> Rect {
    BuildContext ctx;
    w.modifier.set(Modifier{}.size(160.0F, 40.0F));
    w.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F},
                         .max = Size{.width = AURORA_CANVAS_W, .height = AURORA_CANVAS_H}};
    w.layout(cc, ctx);
    if (focused) {
        FocusManager fm;
        fm.set_root(&w);
        fm.set_focus(&w);
    }
    p.begin(static_cast<int>(AURORA_CANVAS_W), static_cast<int>(AURORA_CANVAS_H));
    p.fill_rect(
        Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = AURORA_CANVAS_W, .height = AURORA_CANVAS_H}},
        Color::white());
    const Rect box{.origin = Point{.x = AURORA_BOX_X, .y = AURORA_BOX_Y}, .size = w.size()};
    w.paint(p, box, ctx);
    return box;
}

/// 统计盒外上边环带（[top-gap-thickness, top-gap) 两行）内的蓝色像素数。
/// 该带不含控件自身任何像素，命中即只可能是焦点环。
auto ring_pixels_above(const Painter &p, const Rect &box) -> int {
    const int y0 = static_cast<int>(std::floor(box.origin.y - AURORA_RING_GAP - AURORA_RING_THICKNESS));
    const int y1 = static_cast<int>(std::floor(box.origin.y - AURORA_RING_GAP));
    const int x0 = static_cast<int>(std::floor(box.origin.x + 20.0F));
    const int x1 = static_cast<int>(std::ceil(box.origin.x + box.size.width - 20.0F));
    int n = 0;
    for (int y = std::max(y0, 0); y < std::min(y1, p.height()); ++y) {
        for (int x = std::max(x0, 0); x < std::min(x1, p.width()); ++x) {
            if (is_blue(p.get_pixel(x, y))) {
                ++n;
            }
        }
    }
    return n;
}

}  // namespace

AURORA_TEST_CASE(focused_button_paints_ring_outside_its_box) {
    Button btn{"Go"};
    Painter unfocused;
    const Rect box = paint_on_white(btn, unfocused, false);
    AURORA_TEST_CHECK_FALSE(btn.is_focused());
    AURORA_TEST_REQUIRE_EQ(ring_pixels_above(unfocused, box), 0);  // 对照：未获焦无环

    Painter focused_p;
    Button btn2{"Go"};
    const Rect box2 = paint_on_white(btn2, focused_p, true);
    AURORA_TEST_REQUIRE_TRUE(btn2.is_focused());
    AURORA_TEST_CHECK_MSG(ring_pixels_above(focused_p, box2) > 0,
                          "focused control must paint the base focus ring in the band outside its box");
}

AURORA_TEST_CASE(text_input_opts_out_of_the_base_ring) {
    // TextInput 已自带 Fluent 式主题色聚焦边框：基类环须关闭，否则出现双环。
    TextInput in;
    Painter p_unfocused;
    const Rect box = paint_on_white(in, p_unfocused, false);
    AURORA_TEST_REQUIRE_EQ(ring_pixels_above(p_unfocused, box), 0);

    Painter p_focused;
    TextInput in2;
    const Rect box2 = paint_on_white(in2, p_focused, true);
    AURORA_TEST_REQUIRE_TRUE(in2.is_focused());
    AURORA_TEST_CHECK_EQ(ring_pixels_above(p_focused, box2), 0);
}

}  // namespace aurora::test_cases::utest_focus_ring

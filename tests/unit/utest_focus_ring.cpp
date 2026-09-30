/// 测试类型: unit
/// 目标单元: src/aurora/widget/widget.cpp（Widget::paint_content 的统一焦点环）
/// 测试说明: 持有焦点的控件由基类在自身盒外画出焦点环（可观测停点，规格 §4.4）；
///           未获焦无环；已自带聚焦态外观的控件（TextInput）经 wants_focus_ring() 关闭，不出环。
///           判据为「盒外 2–4 dp 环带内偏离画布白底的像素数」，与控件自身外观无关，且不锁定色相
///           （环色由主题命名令牌 focus.ring 决定，见 Theme::light()/dark()）。

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
using au::Theme;
using au::Widget;

constexpr float AURORA_CANVAS_W = 400.0F;
constexpr float AURORA_CANVAS_H = 300.0F;
constexpr float AURORA_BOX_X = 60.0F;  ///< 被测盒原点：离画布边缘足够远，环带完整可见
constexpr float AURORA_BOX_Y = 80.0F;
constexpr float AURORA_RING_GAP = 2.0F;  ///< 与 widget.cpp 的环几何常量一致
constexpr float AURORA_RING_THICKNESS = 2.0F;

/// 画布铺白且计数区在控件盒之外：偏离白底的像素即基类画出的装饰（焦点环）。
/// 不写成「蓝主导」——环色自 1.0.0-alpha.9 起取命名令牌 focus.ring（浅色主题黑/深色主题白），
/// 锁色相会让本用例随主题配色漂移而静默空转。
auto is_decoration(const Color &c) -> bool {
    return std::abs(static_cast<int>(c.r) - 255) > 30 || std::abs(static_cast<int>(c.g) - 255) > 30 ||
           std::abs(static_cast<int>(c.b) - 255) > 30;
}

/// 统计盒外上边环带（[top-gap-thickness, top-gap) 两行）内的装饰像素数。
/// 该带不含控件自身任何像素，命中即只可能是焦点环。
auto ring_pixels_above(const Painter &p, const Rect &box) -> int {
    const int y0 = static_cast<int>(std::floor(box.origin.y - AURORA_RING_GAP - AURORA_RING_THICKNESS));
    const int y1 = static_cast<int>(std::floor(box.origin.y - AURORA_RING_GAP));
    const int x0 = static_cast<int>(std::floor(box.origin.x + 20.0F));
    const int x1 = static_cast<int>(std::ceil(box.origin.x + box.size.width - 20.0F));
    int n = 0;
    for (int y = std::max(y0, 0); y < std::min(y1, p.height()); ++y) {
        for (int x = std::max(x0, 0); x < std::min(x1, p.width()); ++x) {
            if (is_decoration(p.get_pixel(x, y))) {
                ++n;
            }
        }
    }
    return n;
}

/// 取环带中点的实绘像素色（供断言环色确取自主题令牌）。
auto ring_sample(const Painter &p, const Rect &box) -> Color {
    const int y = static_cast<int>(std::floor(box.origin.y - AURORA_RING_GAP - AURORA_RING_THICKNESS)) + 1;
    return p.get_pixel(static_cast<int>(std::floor(box.origin.x + (box.size.width * 0.5F))), y);
}

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

    // 环色取自主题命名令牌，而非直接取 primary：与控件自身底色同色时，环会被读成「控件自带
    // 的一圈边框」（即用户报的「按钮点击后变小、外侧多一圈边」的成因之一）。
    const Theme light = Theme::light();
    AURORA_TEST_CHECK_EQ(ring_sample(focused_p, box2), light.token_or<Color>("focus.ring", light.primary));
    AURORA_TEST_CHECK(Color{ring_sample(focused_p, box2)} != light.primary);
}

AURORA_TEST_CASE(ring_color_falls_back_to_primary_without_token) {
    // 未登记 focus.ring 的自定义主题必须保持改动前行为（环色 == primary），否则本次变更
    // 对所有既有自定义主题构成未经请求的视觉改动。
    Theme custom;
    custom.primary = Color{10, 200, 30, 255};
    AURORA_TEST_CHECK_FALSE(custom.tokens.contains("focus.ring"));
    AURORA_TEST_CHECK_EQ(custom.token_or<Color>("focus.ring", custom.primary), custom.primary);
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

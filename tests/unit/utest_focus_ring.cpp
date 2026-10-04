/// 测试类型: unit
/// 目标单元: src/aurora/widget/widget.cpp（Widget::paint_content 的统一焦点环）
/// 测试说明: 持有焦点的控件由基类在自身盒外画出焦点环（可观测停点，规格 §4.4）；未获焦无环；
///           已自带聚焦态外观的控件（TextInput）经 wants_focus_ring() 关闭，不出环。
///           环的显隐还取决于焦点**到达方式**（FocusArrival）：指针按下不出环、键盘停点必出环、
///           程序化聚焦保守出环。
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
using au::FocusArrival;
using au::FocusDirection;
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

/// 挂载 + 布局成 160×40 的盒，供用例自行驱动焦点管理器后再绘制。
template <typename W>
auto prep(W &w, BuildContext &ctx) -> void {
    w.modifier.set(Modifier{}.size(160.0F, 40.0F));
    w.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F},
                         .max = Size{.width = AURORA_CANVAS_W, .height = AURORA_CANVAS_H}};
    w.layout(cc, ctx);
}

/// 铺白底后把控件画到固定盒上，返回该盒（环带判据相对它取）。
template <typename W>
auto paint_frame(W &w, Painter &p, BuildContext &ctx) -> Rect {
    p.begin(static_cast<int>(AURORA_CANVAS_W), static_cast<int>(AURORA_CANVAS_H));
    p.fill_rect(
        Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = AURORA_CANVAS_W, .height = AURORA_CANVAS_H}},
        Color::white());
    const Rect box{.origin = Point{.x = AURORA_BOX_X, .y = AURORA_BOX_Y}, .size = w.size()};
    w.paint(p, box, ctx);
    return box;
}

/// 一步到位：挂载/布局 → 按需聚焦（arrival 指定到达方式）→ 画到白底画布。
template <typename W>
auto paint_on_white(W &w, Painter &p, bool focused, FocusArrival arrival = FocusArrival::Programmatic) -> Rect {
    BuildContext ctx;
    prep(w, ctx);
    if (focused) {
        FocusManager fm;
        fm.set_root(&w);
        fm.set_focus(&w, FocusDirection::Forward, arrival);
    }
    return paint_frame(w, p, ctx);
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

AURORA_TEST_CASE(pointer_arrival_paints_no_ring) {
    // 指针按下把焦点交给控件：焦点照给，但基类环不画——此刻控件已有 pressed/hover 反馈，再补一圈
    // 会被知觉归组成「控件自带的一圈边框」（规格 §4.4 与浏览器 :focus-visible 同口径）。
    Button btn{"Go"};
    Painter p;
    const Rect box = paint_on_white(btn, p, true, FocusArrival::Pointer);
    AURORA_TEST_REQUIRE_TRUE(btn.is_focused());  // 焦点归属不因环而变
    AURORA_TEST_CHECK_FALSE(btn.focus_ring_shown());
    AURORA_TEST_CHECK_MSG(ring_pixels_above(p, box) == 0,
                          "focus arriving by pointer must not paint the base ring (the reported 'extra border after "
                          "click' is exactly this band)");
}

AURORA_TEST_CASE(keyboard_arrival_paints_ring) {
    // 键盘停点是无障碍可达性的唯一可见线索，必须出环；色仍取 focus.ring 令牌。
    Button btn{"Go"};
    Painter p;
    const Rect box = paint_on_white(btn, p, true, FocusArrival::Keyboard);
    AURORA_TEST_REQUIRE_TRUE(btn.focus_ring_shown());
    AURORA_TEST_CHECK_MSG(ring_pixels_above(p, box) > 0, "keyboard stop must paint the base focus ring");
    const Theme light = Theme::light();
    AURORA_TEST_CHECK_EQ(ring_sample(p, box), light.token_or<Color>("focus.ring", light.primary));
}

AURORA_TEST_CASE(tab_after_pointer_click_restores_the_ring) {
    // 模态切换回归：指针点击后环隐去，紧接着按 Tab 移焦必须**立刻**重新出环，否则键盘用户
    // 从这一次点击起就再也看不见停点。
    Button btn{"Go"};
    BuildContext ctx;
    prep(btn, ctx);
    FocusManager fm;
    fm.set_root(&btn);
    fm.set_focus(&btn, FocusDirection::Forward, FocusArrival::Pointer);
    AURORA_TEST_REQUIRE_FALSE(btn.focus_ring_shown());

    AURORA_TEST_REQUIRE_TRUE(fm.move_focus(FocusDirection::Forward));  // 单停点：焦点回到同一控件
    AURORA_TEST_CHECK(btn.is_focused());
    AURORA_TEST_CHECK_MSG(btn.focus_arrival() == FocusArrival::Keyboard,
                          "move_focus is keyboard-only, so it must re-establish the visible modality");
    AURORA_TEST_CHECK(btn.focus_ring_shown());
    Painter p;
    const Rect box = paint_frame(btn, p, ctx);
    AURORA_TEST_CHECK(ring_pixels_above(p, box) > 0);
}

AURORA_TEST_CASE(pointer_click_on_a_keyboard_focused_widget_hides_the_ring) {
    // 同一控件再次获焦：焦点没换、不重发 on_focus_change，但到达方式须跟着最后一次手势更新。
    // 漏掉这一步就留下「按 Tab 出了环、又用鼠标点它，环却不消」的残影。
    Button btn{"Go"};
    BuildContext ctx;
    prep(btn, ctx);
    FocusManager fm;
    fm.set_root(&btn);
    fm.set_focus(&btn, FocusDirection::Forward, FocusArrival::Keyboard);
    AURORA_TEST_REQUIRE(btn.focus_ring_shown());

    fm.set_focus(&btn, FocusDirection::Forward, FocusArrival::Pointer);
    AURORA_TEST_CHECK(btn.is_focused());
    AURORA_TEST_CHECK(btn.focus_arrival() == FocusArrival::Pointer);
    AURORA_TEST_CHECK_FALSE(btn.focus_ring_shown());
    Painter p;
    const Rect box = paint_frame(btn, p, ctx);
    AURORA_TEST_CHECK_EQ(ring_pixels_above(p, box), 0);
}

AURORA_TEST_CASE(request_focus_keeps_the_pointer_arrival) {
    // 控件在自有 on_pointer_event 里自主要焦点（Spin 值区、Text 选区等）时，派发器已把 Pointer
    // 记在同一控件上；request_focus 若恒按 Programmatic 覆盖，同一次点击会因「谁先改焦点」而
    // 给出不同观感（点按钮无环、点 Spin 值区有环）。
    Button btn{"Go"};
    BuildContext ctx;
    prep(btn, ctx);
    FocusManager fm;
    fm.set_root(&btn);
    fm.set_focus(&btn, FocusDirection::Forward, FocusArrival::Pointer);
    AURORA_TEST_REQUIRE(btn.focus_arrival() == FocusArrival::Pointer);

    au::set_current_focus_manager(&fm);  // request_focus() 经线程局部槽位解析焦点管理器
    btn.request_focus();
    au::set_current_focus_manager(nullptr);
    AURORA_TEST_CHECK(btn.is_focused());
    AURORA_TEST_CHECK(btn.focus_arrival() == FocusArrival::Pointer);
    AURORA_TEST_CHECK_FALSE(btn.focus_ring_shown());
}

}  // namespace aurora::test_cases::utest_focus_ring

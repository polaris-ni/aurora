/// 测试类型: integration
/// 目标单元: include/aurora/render/offscreen.h
/// 测试说明: 验证渐变绘制与 Modifier.gradient——Painter 线性/径向渐变的像素级插值（双色、竖向、
///           多色标）、GradientBackground 节点字段、带渐变修饰控件的无头渲染快照与退化输入降级

#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "framework/aurora_test.h"

using au::Color;
using au::GradientBackground;
using au::Json;
using au::Modifier;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Text;

namespace aurora::test_cases::itest_gradient {

AURORA_TEST_CASE(linear_gradient_basic_left_to_right) {
    Painter p;
    p.begin(100, 100);

    // 从左(红)到右(蓝)的线性渐变
    const std::vector colors = {Color(255, 0, 0, 255), Color(0, 0, 255, 255)};
    const std::vector stops = {0.0F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 100}},
                           Point{.x = 0, .y = 50}, Point{.x = 100, .y = 50}, colors, stops);

    // 左侧像素应偏红
    const Color left = p.get_pixel(5, 50);
    AURORA_TEST_CHECK_GT(left.r, 200);
    AURORA_TEST_CHECK_LT(left.b, 50);

    // 右侧像素应偏蓝
    const Color right = p.get_pixel(95, 50);
    AURORA_TEST_CHECK_LT(right.r, 50);
    AURORA_TEST_CHECK_GT(right.b, 200);

    // 中间像素应为紫色混合
    const Color mid = p.get_pixel(50, 50);
    AURORA_TEST_CHECK_GT(mid.r, 80);
    AURORA_TEST_CHECK_LT(mid.r, 180);
    AURORA_TEST_CHECK_GT(mid.b, 80);
    AURORA_TEST_CHECK_LT(mid.b, 180);
}

AURORA_TEST_CASE(linear_gradient_vertical_top_to_bottom) {
    Painter p;
    p.begin(50, 100);

    // 从上(白)到下(黑)
    const std::vector colors = {Color(255, 255, 255, 255), Color(0, 0, 0, 255)};
    const std::vector stops = {0.0F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 50, .height = 100}},
                           Point{.x = 25, .y = 0}, Point{.x = 25, .y = 100}, colors, stops);

    // 顶部应亮
    AURORA_TEST_CHECK_GT(p.get_pixel(25, 5).r, 230);
    // 底部应暗
    AURORA_TEST_CHECK_LT(p.get_pixel(25, 95).r, 25);
}

AURORA_TEST_CASE(linear_gradient_multi_stop_middle_is_green) {
    Painter p;
    p.begin(90, 10);

    // 红→绿→蓝 三色标
    const std::vector colors = {Color(255, 0, 0, 255), Color(0, 255, 0, 255), Color(0, 0, 255, 255)};
    const std::vector stops = {0.0F, 0.5F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 90, .height = 10}},
                           Point{.x = 0, .y = 5}, Point{.x = 90, .y = 5}, colors, stops);

    // 中间应偏绿
    const Color mid = p.get_pixel(45, 5);
    AURORA_TEST_CHECK_GT(mid.g, 200);
    AURORA_TEST_CHECK_LT(mid.r, 50);
    AURORA_TEST_CHECK_LT(mid.b, 50);
}

AURORA_TEST_CASE(radial_gradient_center_bright_edge_dark) {
    Painter p;
    p.begin(100, 100);

    // 中心白→边缘黑
    const std::vector colors = {Color(255, 255, 255, 255), Color(0, 0, 0, 255)};
    const std::vector stops = {0.0F, 1.0F};
    p.draw_radial_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 100}},
                           Point{.x = 50, .y = 50}, 50.0F, colors, stops);

    // 中心应亮
    AURORA_TEST_CHECK_GT(p.get_pixel(50, 50).r, 230);
    // 边缘应暗
    AURORA_TEST_CHECK_LT(p.get_pixel(5, 50).r, 50);
}

AURORA_TEST_CASE(modifier_gradient_linear_builds_linear_node) {
    const auto mod = Modifier{}.gradient_linear(Color(255, 0, 0, 255), Color(0, 0, 255, 255), 0.0F);
    AURORA_TEST_CHECK_FALSE(mod.nodes().empty());

    // 验证节点类型
    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *g = dynamic_cast<const GradientBackground *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK_TRUE(g->type() == GradientBackground::Type::Linear);
            AURORA_TEST_CHECK_EQ(g->colors().size(), 2U);
            AURORA_TEST_CHECK_EQ(g->stops().size(), 2U);
            AURORA_TEST_CHECK_EQ(g->angle(), 0.0F);
        }
    }
    AURORA_TEST_CHECK_TRUE(found);
}

AURORA_TEST_CASE(modifier_gradient_radial_builds_radial_node) {
    const auto mod = Modifier{}.gradient_radial(Color(255, 255, 255, 255), Color(0, 0, 0, 255));
    AURORA_TEST_CHECK_FALSE(mod.nodes().empty());

    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *g = dynamic_cast<const GradientBackground *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK_TRUE(g->type() == GradientBackground::Type::Radial);
            AURORA_TEST_CHECK_EQ(g->colors().size(), 2U);
        }
    }
    AURORA_TEST_CHECK_TRUE(found);
}

AURORA_TEST_CASE(gradient_modifier_widget_offscreen_snapshot) {
    // 构建一个带渐变背景的 Text 控件并离屏渲染（逻辑快照）
    auto txt = std::make_shared<Text>("Hello");
    txt->modifier.set(Modifier{}.gradient_linear(Color(255, 0, 0, 255), Color(0, 0, 255, 255)));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 200, 50);
    AURORA_TEST_CHECK_TRUE(snap.contains("type"));
    AURORA_TEST_CHECK_EQ(snap["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_GT(snap["box"]["w"].get<float>(), 0.0F);
}

AURORA_TEST_CASE(gradient_degenerate_inputs_degrade_gracefully) {
    Painter p;
    p.begin(10, 10);

    // 空色标不崩溃
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 10, .height = 10}},
                           Point{.x = 0, .y = 0}, Point{.x = 10, .y = 10}, {}, {});
    AURORA_TEST_CHECK_TRUE(true);  // 不崩溃即通过

    // 单色标
    const std::vector single = {Color(128, 128, 128, 255)};
    const std::vector stops = {0.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 10, .height = 10}},
                           Point{.x = 0, .y = 0}, Point{.x = 10, .y = 10}, single, stops);
    AURORA_TEST_CHECK_EQ(p.get_pixel(5, 5).r, 128);

    // 零方向向量（start == end）不崩溃
    const std::vector two = {Color(255, 0, 0, 255), Color(0, 0, 255, 255)};
    const std::vector two_stops = {0.0F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 10, .height = 10}},
                           Point{.x = 5, .y = 5}, Point{.x = 5, .y = 5}, two, two_stops);
    AURORA_TEST_CHECK_TRUE(true);  // 不崩溃即通过
}

}  // namespace aurora::test_cases::itest_gradient

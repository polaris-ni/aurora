// test_gradient.cpp — 渐变绘制与 Modifier.gradient() 测试。
#include <cmath>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "test_harness.h"

using aurora::Color;
using aurora::GradientBackground;
using aurora::Json;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::Text;

// ---------- Painter 线性渐变 ----------

static void test_linear_gradient_basic() {
    Painter p;
    p.begin(100, 100);

    // 从左(红)到右(蓝)的线性渐变
    const std::vector colors = {Color(255, 0, 0, 255), Color(0, 0, 255, 255)};
    const std::vector stops = {0.0F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 100}},
                           Point{.x = 0, .y = 50}, Point{.x = 100, .y = 50}, colors, stops);

    // 左侧像素应偏红
    const Color left = p.get_pixel(5, 50);
    AURORA_TEST_CHECK(left.m_r > 200);
    AURORA_TEST_CHECK(left.m_b < 50);

    // 右侧像素应偏蓝
    const Color right = p.get_pixel(95, 50);
    AURORA_TEST_CHECK(right.m_r < 50);
    AURORA_TEST_CHECK(right.m_b > 200);

    // 中间像素应为紫色混合
    const Color mid = p.get_pixel(50, 50);
    AURORA_TEST_CHECK(mid.m_r > 80 && mid.m_r < 180);
    AURORA_TEST_CHECK(mid.m_b > 80 && mid.m_b < 180);
}

static void test_linear_gradient_vertical() {
    Painter p;
    p.begin(50, 100);

    // 从上(白)到下(黑)
    const std::vector colors = {Color(255, 255, 255, 255), Color(0, 0, 0, 255)};
    const std::vector stops = {0.0F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 50, .height = 100}},
                           Point{.x = 25, .y = 0}, Point{.x = 25, .y = 100}, colors, stops);

    // 顶部应亮
    const Color top = p.get_pixel(25, 5);
    AURORA_TEST_CHECK(top.m_r > 230);

    // 底部应暗
    const Color bottom = p.get_pixel(25, 95);
    AURORA_TEST_CHECK(bottom.m_r < 25);
}

static void test_linear_gradient_multi_stop() {
    Painter p;
    p.begin(90, 10);

    // 红→绿→蓝 三色标
    const std::vector colors = {Color(255, 0, 0, 255), Color(0, 255, 0, 255), Color(0, 0, 255, 255)};
    const std::vector stops = {0.0F, 0.5F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 90, .height = 10}},
                           Point{.x = 0, .y = 5}, Point{.x = 90, .y = 5}, colors, stops);

    // 中间应偏绿
    const Color mid = p.get_pixel(45, 5);
    AURORA_TEST_CHECK(mid.m_g > 200);
    AURORA_TEST_CHECK(mid.m_r < 50);
    AURORA_TEST_CHECK(mid.m_b < 50);
}

// ---------- Painter 径向渐变 ----------

static void test_radial_gradient_basic() {
    Painter p;
    p.begin(100, 100);

    // 中心白→边缘黑
    const std::vector colors = {Color(255, 255, 255, 255), Color(0, 0, 0, 255)};
    const std::vector stops = {0.0F, 1.0F};
    p.draw_radial_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 100}},
                           Point{.x = 50, .y = 50}, 50.0F, colors, stops);

    // 中心应亮
    const Color center = p.get_pixel(50, 50);
    AURORA_TEST_CHECK(center.m_r > 230);

    // 边缘应暗
    const Color edge = p.get_pixel(5, 50);
    AURORA_TEST_CHECK(edge.m_r < 50);
}

// ---------- Modifier.gradient_linear ----------

static void test_modifier_gradient_linear() {
    const auto mod = Modifier{}.gradient_linear(Color(255, 0, 0, 255), Color(0, 0, 255, 255), 0.0F);
    AURORA_TEST_CHECK(!mod.nodes().empty());

    // 验证节点类型
    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *g = dynamic_cast<const GradientBackground *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK(g->type() == GradientBackground::Type::Linear);
            AURORA_TEST_CHECK(g->colors().size() == 2);
            AURORA_TEST_CHECK(g->stops().size() == 2);
            AURORA_TEST_CHECK(g->angle() == 0.0F);
        }
    }
    AURORA_TEST_CHECK(found);
}

static void test_modifier_gradient_radial() {
    const auto mod = Modifier{}.gradient_radial(Color(255, 255, 255, 255), Color(0, 0, 0, 255));
    AURORA_TEST_CHECK(!mod.nodes().empty());

    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *g = dynamic_cast<const GradientBackground *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK(g->type() == GradientBackground::Type::Radial);
            AURORA_TEST_CHECK(g->colors().size() == 2);
        }
    }
    AURORA_TEST_CHECK(found);
}

// ---------- 渐变 + Widget 集成（离屏渲染） ----------

static void test_gradient_widget_render() {
    // 构建一个带渐变背景的 Text 控件并离屏渲染
    auto txt = Text("Hello");
    txt.modifier = Modifier{}.gradient_linear(Color(255, 0, 0, 255), Color(0, 0, 255, 255));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 200, 50);
    AURORA_TEST_CHECK(snap.contains("type"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["type"] == "Text");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(snap["box"]["w"].get<float>() > 0);
}

// ---------- 退化情况 ----------

static void test_gradient_degenerate() {
    Painter p;
    p.begin(10, 10);

    // 空色标不崩溃
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 10, .height = 10}},
                           Point{.x = 0, .y = 0}, Point{.x = 10, .y = 10}, {}, {});
    AURORA_TEST_CHECK(true);  // 不崩溃即通过

    // 单色标
    const std::vector single = {Color(128, 128, 128, 255)};
    const std::vector stops = {0.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 10, .height = 10}},
                           Point{.x = 0, .y = 0}, Point{.x = 10, .y = 10}, single, stops);
    const Color c = p.get_pixel(5, 5);
    AURORA_TEST_CHECK(c.m_r == 128);

    // 零方向向量（start == end）
    const std::vector two = {Color(255, 0, 0, 255), Color(0, 0, 255, 255)};
    const std::vector two_stops = {0.0F, 1.0F};
    p.draw_linear_gradient(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 10, .height = 10}},
                           Point{.x = 5, .y = 5}, Point{.x = 5, .y = 5}, two, two_stops);
    AURORA_TEST_CHECK(true);  // 不崩溃即通过
}

AURORA_TEST() {
    test_linear_gradient_basic();
    test_linear_gradient_vertical();
    test_linear_gradient_multi_stop();
    test_radial_gradient_basic();
    test_modifier_gradient_linear();
    test_modifier_gradient_radial();
    test_gradient_widget_render();
    test_gradient_degenerate();
}
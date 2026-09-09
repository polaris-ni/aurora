/// 测试类型: integration
/// 目标单元: include/aurora/render/offscreen.h
/// 测试说明: 验证阴影绘制与 Modifier::shadow——Painter::draw_shadow 硬边/模糊阴影的像素级衰减、
///           ShadowNode 字段与默认参数、带阴影修饰的控件无头渲染快照集成

#include <memory>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "framework/aurora_test.h"

using au::Color;
using au::Json;
using au::Modifier;
using au::Node;
using au::Painter;
using au::Point;
using au::Rect;
using au::ShadowNode;
using au::Size;
using au::Text;

namespace aurora::test_cases::itest_shadow {

AURORA_TEST_CASE(draw_shadow_hard_edge_darkens_offset_region) {
    Painter p;
    p.begin(100, 100);

    // 先填白色背景
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 100}},
                Color(255, 255, 255, 255));

    // 硬边阴影（blur=0），偏移 (5,5)，黑色半透明
    p.draw_shadow(Rect{.origin = Point{.x = 20, .y = 20}, .size = Size{.width = 40, .height = 40}}, 5.0F, 5.0F, 0.0F,
                  Color(0, 0, 0, 128));

    // 阴影在 shape 偏移 (25,25)-(65,65) 区域：取阴影区内但不在原始 shape 内的采样点
    const Color shadow_area = p.get_pixel(62, 62);
    AURORA_TEST_CHECK_LT(shadow_area.r, 200);  // 被阴影染暗

    // 远离阴影的区域应保持白色
    const Color far = p.get_pixel(90, 10);
    AURORA_TEST_CHECK_GT(far.r, 250);
}

AURORA_TEST_CASE(draw_shadow_blurred_decays_from_edge) {
    Painter p;
    p.begin(100, 100);
    p.fill_rect(Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = 100, .height = 100}},
                Color(255, 255, 255, 255));

    // 模糊阴影
    p.draw_shadow(Rect{.origin = Point{.x = 30, .y = 30}, .size = Size{.width = 40, .height = 40}}, 0.0F, 4.0F, 6.0F,
                  Color(0, 0, 0, 100));

    // 阴影正下方应较暗
    const Color below = p.get_pixel(50, 75);
    AURORA_TEST_CHECK_LT(below.r, 240);

    // 远离阴影处应保持白色
    const Color far = p.get_pixel(5, 5);
    AURORA_TEST_CHECK_GT(far.r, 250);

    // 模糊边缘应有中间值（渐变）：阴影矩形底部在 y=74，blur=6 距边缘 2px 衰减
    const Color edge = p.get_pixel(50, 76);
    AURORA_TEST_CHECK_LT(edge.r, 255);  // 有些暗
    AURORA_TEST_CHECK_GT(edge.r, 150);  // 但不是很暗（衰减了）
}

AURORA_TEST_CASE(modifier_shadow_appends_shadow_node_fields) {
    const auto mod = Modifier{}.shadow(0.0F, 3.0F, 5.0F, Color(0, 0, 0, 80));
    AURORA_TEST_CHECK_FALSE(mod.nodes().empty());

    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *s = dynamic_cast<const ShadowNode *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK_EQ(s->offset_x(), 0.0F);
            AURORA_TEST_CHECK_EQ(s->offset_y(), 3.0F);
            AURORA_TEST_CHECK_EQ(s->blur(), 5.0F);
            AURORA_TEST_CHECK_EQ(s->color().a, 80);
        }
    }
    AURORA_TEST_CHECK_TRUE(found);
}

AURORA_TEST_CASE(modifier_shadow_default_parameters) {
    const auto mod = Modifier{}.shadow();
    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *s = dynamic_cast<const ShadowNode *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK_EQ(s->offset_x(), 0.0F);
            AURORA_TEST_CHECK_EQ(s->offset_y(), 2.0F);
            AURORA_TEST_CHECK_EQ(s->blur(), 4.0F);
            AURORA_TEST_CHECK_EQ(s->color().a, 64);
        }
    }
    AURORA_TEST_CHECK_TRUE(found);
}

AURORA_TEST_CASE(shadow_modifier_widget_offscreen_snapshot) {
    auto txt = std::make_shared<Text>("Shadow");
    txt->modifier.set(Modifier{}.shadow(0, 3, 5).background(Color(255, 255, 255, 255)));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 200, 80);
    AURORA_TEST_CHECK_TRUE(snap.contains("type"));
    AURORA_TEST_CHECK_EQ(snap["type"].get<std::string>(), std::string{"Text"});
    AURORA_TEST_CHECK_GT(snap["box"]["w"].get<float>(), 0.0F);
}

}  // namespace aurora::test_cases::itest_shadow

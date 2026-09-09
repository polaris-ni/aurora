/// 测试类型: integration
/// 目标单元: include/aurora/modifier/modifier.h
/// 测试说明: 验证模糊修饰——BlurNode 构造与负值降级、Modifier::blur/backdrop_filter 工厂链、
///           Painter::blur_region 像素级模糊（锐利边界平滑、区域限定、非法参数无操作），
///           以及带 blur/backdrop_filter 修饰的控件无头 layout+paint 集成不崩溃

#include <memory>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

using au::BlurNode;
using au::BuildContext;
using au::Color;
using au::Constraints;
using au::LocalizedString;
using au::Modifier;
using au::Painter;
using au::Point;
using au::Rect;
using au::Size;
using au::Text;

namespace aurora::test_cases::itest_blur {

AURORA_TEST_CASE(blur_node_construction_degrades_negative_radius) {
    const BlurNode b1{4.0F};
    AURORA_TEST_CHECK_EQ(b1.radius(), 4.0F);
    AURORA_TEST_CHECK_FALSE(b1.is_backdrop());

    const BlurNode b2{8.0F, true};
    AURORA_TEST_CHECK_TRUE(b2.is_backdrop());

    const BlurNode b3{-5.0F};
    AURORA_TEST_CHECK_EQ(b3.radius(), 0.0F);  // 负值降级
}

AURORA_TEST_CASE(modifier_blur_backdrop_factories_append_in_order) {
    const auto mod = Modifier{}.blur(3.0F).backdrop_filter(6.0F);
    AURORA_TEST_CHECK_EQ(mod.nodes().size(), 2U);
    const auto *n0 = dynamic_cast<const BlurNode *>(mod.nodes()[0].get());
    const auto *n1 = dynamic_cast<const BlurNode *>(mod.nodes()[1].get());
    AURORA_TEST_CHECK_NOT_NULL(n0);
    AURORA_TEST_CHECK_FALSE(n0->is_backdrop());
    AURORA_TEST_CHECK_NOT_NULL(n1);
    AURORA_TEST_CHECK_TRUE(n1->is_backdrop());
}

AURORA_TEST_CASE(blur_region_smooths_hard_edge) {
    Painter p;
    p.begin(100, 100);
    // 左半黑右半白（x=50 处硬边界）
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 100.0F}},
                Color(0, 0, 0, 255));
    p.fill_rect(Rect{.origin = Point{.x = 50.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 100.0F}},
                Color(255, 255, 255, 255));

    // 模糊前：边界两侧对比强烈
    AURORA_TEST_CHECK_EQ(p.get_pixel(48, 50).r, 0);
    AURORA_TEST_CHECK_EQ(p.get_pixel(52, 50).r, 255);

    p.blur_region(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 100.0F}}, 5.0F);

    // 模糊后：边界处出现中间灰度
    const unsigned char after_edge = p.get_pixel(50, 50).r;
    AURORA_TEST_CHECK_GT(after_edge, 30);
    AURORA_TEST_CHECK_LT(after_edge, 225);
    // 远离边界处基本不变
    AURORA_TEST_CHECK_LT(p.get_pixel(5, 50).r, 30);
    AURORA_TEST_CHECK_GT(p.get_pixel(95, 50).r, 225);
}

AURORA_TEST_CASE(blur_region_only_affects_specified_region) {
    Painter p;
    p.begin(100, 100);
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 100.0F}},
                Color(0, 0, 0, 255));
    p.fill_rect(Rect{.origin = Point{.x = 50.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 100.0F}},
                Color(255, 255, 255, 255));

    // 只模糊上半 30 行
    p.blur_region(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 30.0F}}, 5.0F);

    // 上半边界模糊
    const unsigned char blurred = p.get_pixel(50, 15).r;
    AURORA_TEST_CHECK_GT(blurred, 30);
    AURORA_TEST_CHECK_LT(blurred, 225);
    // 下半边界仍锐利
    AURORA_TEST_CHECK_EQ(p.get_pixel(48, 80).r, 0);
    AURORA_TEST_CHECK_EQ(p.get_pixel(52, 80).r, 255);
}

AURORA_TEST_CASE(blur_region_ignores_invalid_params) {
    Painter p;
    p.begin(50, 50);
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 50.0F}},
                Color(100, 100, 100, 255));

    p.blur_region(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 50.0F}},
                  0.0F);  // 零半径
    p.blur_region(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 50.0F}},
                  -3.0F);  // 负半径
    p.blur_region(Rect{.origin = Point{.x = 200.0F, .y = 200.0F}, .size = Size{.width = 10.0F, .height = 10.0F}},
                  5.0F);  // 区域出界
    AURORA_TEST_CHECK_EQ(p.get_pixel(25, 25).r, 100);  // 全部无操作
}

AURORA_TEST_CASE(widget_with_blur_modifier_paints) {
    auto t = std::make_shared<Text>();
    t->content = LocalizedString{"blurred text"};
    t->modifier.set(Modifier{}.background(Color(255, 0, 0, 255)).blur(2.0F));

    BuildContext ctx;
    t->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 200.0F, .height = 100.0F};
    t->layout(c, ctx);

    Painter p;
    p.begin(200, 100);
    t->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 100.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p.width(), 200);
}

AURORA_TEST_CASE(widget_with_backdrop_filter_paints) {
    auto t = std::make_shared<Text>();
    t->content = LocalizedString{"frosted"};
    t->modifier.set(Modifier{}.backdrop_filter(4.0F).background(Color(255, 255, 255, 120)));

    BuildContext ctx;
    t->mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = 200.0F, .height = 100.0F};
    t->layout(c, ctx);

    Painter p;
    p.begin(200, 100);
    // 背景先画点内容供模糊
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 100.0F}},
                Color(0, 0, 255, 255));
    t->paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 200.0F, .height = 100.0F}}, ctx);
    AURORA_TEST_CHECK_EQ(p.width(), 200);
}

}  // namespace aurora::test_cases::itest_blur

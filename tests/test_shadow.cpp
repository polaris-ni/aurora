// test_shadow.cpp — 阴影绘制与 Modifier.shadow() 测试。
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"

#include "test_harness.h"

using aurora::Color;
using aurora::Json;
using aurora::Modifier;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::ShadowNode;
using aurora::Size;
using aurora::Text;

// ---------- Painter 硬边阴影 ----------

static void test_shadow_hard() {
    Painter p;
    p.begin(100, 100);

    // 先填白色背景
    p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 100, .height = 100 } },
                Color(255, 255, 255, 255));

    // 硬边阴影（blur=0），偏移 (5,5)，黑色半透明
    p.draw_shadow(Rect{ .origin = Point{ .x = 20, .y = 20 }, .size = Size{ .width = 40, .height = 40 } }, 5.0f, 5.0f,
                  0.0f, Color(0, 0, 0, 128));

    // 阴影区域（偏移后）应有暗色
    [[maybe_unused]] Color in_shadow = p.get_pixel(30, 30); // 在 shape 内但有阴影偏移覆盖
    // 阴影在 shape 偏移 (25,25)-(65,65) 区域
    const Color shadow_area = p.get_pixel(62, 62); // 在阴影区但不在原始 shape 内
    AURORA_TEST_CHECK(shadow_area.m_r < 200);      // 被阴影染暗

    // 远离阴影的区域应保持白色
    const Color far = p.get_pixel(90, 10);
    AURORA_TEST_CHECK(far.m_r > 250);
}

// ---------- Painter 模糊阴影 ----------

static void test_shadow_blur() {
    Painter p;
    p.begin(100, 100);
    p.fill_rect(Rect{ .origin = Point{ .x = 0, .y = 0 }, .size = Size{ .width = 100, .height = 100 } },
                Color(255, 255, 255, 255));

    // 模糊阴影
    p.draw_shadow(Rect{ .origin = Point{ .x = 30, .y = 30 }, .size = Size{ .width = 40, .height = 40 } }, 0.0f, 4.0f,
                  6.0f, Color(0, 0, 0, 100));

    // 阴影正下方应较暗
    const Color below = p.get_pixel(50, 75);
    AURORA_TEST_CHECK(below.m_r < 240);

    // 远离阴影处应保持白色
    const Color far = p.get_pixel(5, 5);
    AURORA_TEST_CHECK(far.m_r > 250);

    // 模糊边缘应有中间值（渐变）
    // 阴影矩形底部在 y=74，blur=6 意味着距离边缘 6px 内衰减
    const Color edge = p.get_pixel(50, 76); // 距边缘 2px，alpha_factor ≈ 0.67
    AURORA_TEST_CHECK(edge.m_r < 255);      // 有些暗
    AURORA_TEST_CHECK(edge.m_r > 150);      // 但不是很暗（衰减了）
}

// ---------- Modifier.shadow() ----------

static void test_modifier_shadow() {
    const auto mod = Modifier{}.shadow(0.0f, 3.0f, 5.0f, Color(0, 0, 0, 80));
    AURORA_TEST_CHECK(!mod.nodes().empty());

    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *s = dynamic_cast<const ShadowNode *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK(s->offset_x() == 0.0f);
            AURORA_TEST_CHECK(s->offset_y() == 3.0f);
            AURORA_TEST_CHECK(s->blur() == 5.0f);
            AURORA_TEST_CHECK(s->color().m_a == 80);
        }
    }
    AURORA_TEST_CHECK(found);
}

static void test_modifier_shadow_defaults() {
    // 默认参数
    const auto mod = Modifier{}.shadow();
    bool found = false;
    for (const auto &n : mod.nodes()) {
        if (const auto *s = dynamic_cast<const ShadowNode *>(n.get())) {
            found = true;
            AURORA_TEST_CHECK(s->offset_x() == 0.0f);
            AURORA_TEST_CHECK(s->offset_y() == 2.0f);
            AURORA_TEST_CHECK(s->blur() == 4.0f);
            AURORA_TEST_CHECK(s->color().m_a == 64);
        }
    }
    AURORA_TEST_CHECK(found);
}

// ---------- Widget 集成 ----------

static void test_shadow_widget_render() {
    auto txt = Text("Shadow");
    txt.modifier = Modifier{}.shadow(0, 3, 5).background(Color(255, 255, 255, 255));

    Node root(std::move(txt));
    Json snap = render_to_logical_snapshot(root, 200, 80);
    AURORA_TEST_CHECK(snap.contains("type"));
    AURORA_TEST_CHECK(snap["type"] == "Text");
}

AURORA_TEST() {
    test_shadow_hard();
    test_shadow_blur();
    test_modifier_shadow();
    test_modifier_shadow_defaults();
    test_shadow_widget_render();
}

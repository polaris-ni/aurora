// button_test.cpp — 覆盖 Button 富属性：链式 setter、字段读写、序列化往返。
#include <string>

#include "aurora/aurora.h"
#include "aurora/widget/button.h"

#include "test_harness.h"

using aurora::Button;
using aurora::Color;
using aurora::EdgeInsets;
using aurora::Json;

static void test_chained_setters() {
    Button b;
    b.set_label("OK")
        .set_corner_radius(8.0f)
        .set_padding(EdgeInsets{ .left = 2.0f, .top = 3.0f, .right = 4.0f, .bottom = 5.0f })
        .set_enabled(false);

    AURORA_TEST_CHECK_MSG(near_f(b.corner_radius, 8.0f), "corner_radius set");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.left, 2.0f), "padding.left set");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.top, 3.0f), "padding.top set");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.right, 4.0f), "padding.right set");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.bottom, 5.0f), "padding.bottom set");
    AURORA_TEST_CHECK_MSG(b.enabled == false, "enabled set");
}

static void test_serialize_roundtrip() {
    Button a;
    a.set_label("Hi")
        .set_corner_radius(10.0f)
        .set_padding(EdgeInsets{ .left = 1.0f, .top = 2.0f, .right = 3.0f, .bottom = 4.0f })
        .set_enabled(false);

    Json j;
    a.serialize_props(j);

    AURORA_TEST_CHECK_MSG(near_f(j["corner_radius"].get<float>(), 10.0f), "json corner_radius=10");
    AURORA_TEST_CHECK_MSG(j["enabled"].get<bool>() == false, "json enabled=false");
    AURORA_TEST_CHECK_MSG(j["padding"].is_object(), "json padding is object");
    AURORA_TEST_CHECK_MSG(near_f(j["padding"]["left"].get<float>(), 1.0f), "json padding.left=1");

    Button b;
    b.deserialize_props(j);

    AURORA_TEST_CHECK_MSG(near_f(b.corner_radius, 10.0f), "rt corner_radius");
    AURORA_TEST_CHECK_MSG(b.enabled == false, "rt enabled");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.left, 1.0f), "rt padding.left");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.bottom, 4.0f), "rt padding.bottom");
    AURORA_TEST_CHECK_MSG(b.label.get().text == "Hi", "rt label");
}

static void test_defaults() {
    const Button b = {};
    AURORA_TEST_CHECK_MSG(near_f(b.corner_radius, 6.0f), "default corner_radius 6 (modernized)");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.left, 12.0f), "default padding.left 12");
    AURORA_TEST_CHECK_MSG(near_f(b.padding.top, 6.0f), "default padding.top 6");
    AURORA_TEST_CHECK_MSG(b.enabled == true, "default enabled true");
    AURORA_TEST_CHECK_MSG(!b.hover_color.has_value(), "default hover_color auto");
    AURORA_TEST_CHECK_MSG(!b.pressed_color.has_value(), "default pressed_color auto");
    AURORA_TEST_CHECK_MSG(!b.border_color.has_value() && near_f(b.border_width, 0.0f), "default no border");
    AURORA_TEST_CHECK_MSG(near_f(b.min_width, 0.0f) && near_f(b.min_height, 0.0f), "default min size 0");

    // Button::defaults()：运行时可查询默认值（规格 #5；静态与经实例调用等价）
    const auto d = Button::defaults();
    AURORA_TEST_CHECK_MSG(near_f(d.corner_radius, 6.0f), "defaults(): corner_radius 6");
    AURORA_TEST_CHECK_MSG(near_f(d.padding.left, 12.0f) && near_f(d.padding.top, 6.0f), "defaults(): padding 12/6");
    AURORA_TEST_CHECK_MSG(d.enabled == true, "defaults(): enabled true");
    AURORA_TEST_CHECK_MSG(near_f(b.defaults().corner_radius, 6.0f), "defaults() callable via instance");
}

static void test_style_props() {
    Button a;
    a.set_hover_color(Color{ 10, 20, 30, 255 })
        .set_pressed_color(Color{ 40, 50, 60, 255 })
        .set_border(Color::red(), 2.0f)
        .set_disabled_colors(Color{ 1, 2, 3, 255 }, Color{ 4, 5, 6, 255 })
        .set_min_size(120.0f, 40.0f);

    Json j;
    a.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["hover_color"][2].get<int>() == 30, "json hover_color.b=30");
    AURORA_TEST_CHECK_MSG(j["pressed_color"][0].get<int>() == 40, "json pressed_color.r=40");
    AURORA_TEST_CHECK_MSG(j["border_color"][0].get<int>() == 255, "json border_color=red");
    AURORA_TEST_CHECK_MSG(near_f(j["border_width"].get<float>(), 2.0f), "json border_width=2");
    AURORA_TEST_CHECK_MSG(near_f(j["min_width"].get<float>(), 120.0f), "json min_width=120");

    Button b;
    b.deserialize_props(j);
    AURORA_TEST_CHECK_MSG(b.hover_color.has_value() && b.hover_color->m_b == 30, "rt hover_color");
    AURORA_TEST_CHECK_MSG(b.pressed_color.has_value() && b.pressed_color->m_r == 40, "rt pressed_color");
    AURORA_TEST_CHECK_MSG(b.border_color.has_value() && near_f(b.border_width, 2.0f), "rt border");
    AURORA_TEST_CHECK_MSG(b.disabled_color.has_value() && b.disabled_color->m_g == 2, "rt disabled_color");
    AURORA_TEST_CHECK_MSG(near_f(b.min_height, 40.0f), "rt min_height");

    // 未显式设置的 optional 颜色不应序列化（保留自动派生语义）
    const Button c;
    Json k;
    c.serialize_props(k);
    AURORA_TEST_CHECK_MSG(!k.contains("hover_color") && !k.contains("pressed_color") && !k.contains("border_color"),
                          "unset optional colors not serialized");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== button_test ===\n");
    test_chained_setters();
    test_serialize_roundtrip();
    test_defaults();
    test_style_props();
}

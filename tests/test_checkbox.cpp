// test_checkbox.cpp — Checkbox 控件 1:1 测试：属性往返 / 点击切换 / Binding 透写 / 注册与 to_json 往返。

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/serialization.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::Binding;
using aurora::Checkbox;
using aurora::Color;
using aurora::describe_component;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Reactive;
using aurora::State;
using aurora::Widget;

using Json = nlohmann::json;
static auto make_press(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.position = Point{ .x = x, .y = y };
    return e;
}
static auto make_release(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Release;
    e.position = Point{ .x = x, .y = y };
    return e;
}
static void fire(Widget &w, MouseEvent e) {
    e.local_position = e.position;
    w.on_pointer_event(e);
}

template<typename W> static auto roundtrip(const Json &props, const std::string &type) -> std::shared_ptr<W> {
    auto back = serialization::from_json(props);
    AURORA_TEST_CHECK_MSG(back.ok(), type + ": from_json succeeded");
    if (!back.ok()) {
        return nullptr;
    }
    auto w = std::static_pointer_cast<W>(back.value());
    AURORA_TEST_CHECK_MSG(w->type_name() == type, type + ": type_name matches");
    return w;
}

// A) 属性 setter + serialize_props / deserialize_props 往返
static void test_props() {
    Checkbox c;
    c.set_active_color(Color::red()).set_border_color(Color::green()).set_size(24.0f).set_value(true);
    Json j;
    c.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["checked"].get<bool>() == true, "checkbox checked=true");
    AURORA_TEST_CHECK_MSG(j["active_color"][0].get<int>() == 255 && j["active_color"][1].get<int>() == 0,
                          "checkbox active=red");
    AURORA_TEST_CHECK_MSG(near_f(j["size"].get<float>(), 24.0f), "checkbox size=24");

    Checkbox d;
    d.deserialize_props(j);
    Json k;
    d.serialize_props(k);
    AURORA_TEST_CHECK_MSG(k["checked"].get<bool>() == true, "checkbox rt checked");
    AURORA_TEST_CHECK_MSG(near_f(k["size"].get<float>(), 24.0f), "checkbox rt size");
}

// A2) 新增样式属性：check_color / corner_radius / border_width / enabled 往返；
//     active_color 未设置时不序列化（保留跟随主题 primary 语义）。
static void test_style_props() {
    Checkbox c;
    c.set_check_color(Color{ 10, 20, 30, 255 }).set_corner_radius(6.0f).set_border_width(2.0f).set_enabled(false);
    Json j;
    c.serialize_props(j);
    AURORA_TEST_CHECK_MSG(!j.contains("active_color"), "active_color not explicitly set: not emitted (follows theme)");
    AURORA_TEST_CHECK_MSG(j["check_color"][0].get<int>() == 10, "check_color serialization");
    AURORA_TEST_CHECK_MSG(near_f(j["corner_radius"].get<float>(), 6.0f), "corner_radius serialization");
    AURORA_TEST_CHECK_MSG(near_f(j["border_width"].get<float>(), 2.0f), "border_width serialization");
    AURORA_TEST_CHECK_MSG(j["enabled"].get<bool>() == false, "enabled serialization");

    Checkbox d;
    d.deserialize_props(j);
    Json k;
    d.serialize_props(k);
    AURORA_TEST_CHECK_MSG(k["check_color"][2].get<int>() == 30, "check_color roundtrip");
    AURORA_TEST_CHECK_MSG(near_f(k["corner_radius"].get<float>(), 6.0f), "corner_radius roundtrip");
    AURORA_TEST_CHECK_MSG(near_f(k["border_width"].get<float>(), 2.0f), "border_width roundtrip");
    AURORA_TEST_CHECK_MSG(k["enabled"].get<bool>() == false, "enabled roundtrip");
    AURORA_TEST_CHECK_MSG(d.enabled() == false, "enabled getter");
}

// A3) 禁用态：点击不切换、不触发 on_changed。
static void test_disabled() {
    bool fired = false;
    Checkbox cb{ Reactive{ false }, [&](bool) -> void { fired = true; } };
    cb.set_enabled(false);
    fire(cb, make_press(5, 5));
    fire(cb, make_release(5, 5));
    AURORA_TEST_CHECK_MSG(cb.value() == false, "disabled: click does not toggle");
    AURORA_TEST_CHECK_MSG(!fired, "disabled: on_changed not fired");
    cb.set_enabled(true);
    fire(cb, make_press(5, 5));
    fire(cb, make_release(5, 5));
    AURORA_TEST_CHECK_MSG(cb.value() == true, "re-enabled: click restores toggle");
}

// B) 点击切换 / Binding 透写
static void test_interaction() {
    Checkbox cb{ Reactive{ false } };
    AURORA_TEST_CHECK_MSG(cb.value() == false, "Checkbox: initial false");
    cb.set_value(true);
    AURORA_TEST_CHECK_MSG(cb.value() == true, "Checkbox: set_value true");

    Checkbox cb2{ Reactive{ false } };
    fire(cb2, make_press(5, 5));
    fire(cb2, make_release(5, 5));
    AURORA_TEST_CHECK_MSG(cb2.value() == true, "Checkbox: press+release toggles to true");

    bool changed = false;
    Checkbox cb3{ Reactive{ false }, [&](bool v) -> void { changed = v; } };
    fire(cb3, make_press(2, 2));
    fire(cb3, make_release(2, 2));
    AURORA_TEST_CHECK_MSG(changed == true, "Checkbox: onChanged fired with true");

    State s{ false };
    Checkbox cb4{ Binding{ s } };
    cb4.set_value(true);
    AURORA_TEST_CHECK_MSG(s.get() == true, "Checkbox: set_value writes through Binding");
    fire(cb4, make_press(1, 1));
    fire(cb4, make_release(1, 1));
    AURORA_TEST_CHECK_MSG(s.get() == false, "Checkbox: toggle writes through Binding");
}

// C) 注册可见性 + to_json / from_json 往返
static void test_roundtrip() {
    const auto schema = describe_component("Checkbox");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(Checkbox) non-empty");

    const auto w = std::make_shared<Checkbox>();
    w->set_value(true);
    Json j = serialization::to_json(*w);
    AURORA_TEST_CHECK_MSG(j["props"].contains("checked") && j["props"]["checked"].get<bool>() == true,
                          "Checkbox serialization checked");
    const auto back = roundtrip<Checkbox>(j, "Checkbox");
    AURORA_TEST_CHECK_MSG(back && back->value() == true, "Checkbox roundtrip preserves checked");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_checkbox ===\n");
    test_props();
    test_style_props();
    test_disabled();
    test_interaction();
    test_roundtrip();
}

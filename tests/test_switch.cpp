#include <cstdio>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/switch.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::Binding;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::describe_component;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Reactive;
using aurora::Size;
using aurora::State;
using aurora::Switch;
using aurora::Widget;

using Json = nlohmann::json;

static auto const make_press(float x, const float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.position = Point{ .x = x, .y = y };
    return e;
}
static auto const make_release(float x, const float y) -> MouseEvent {
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

static void test_props() {
    Switch s;
    s.set_active_color(Color::red()).set_inactive_color(Color::blue()).set_thumb_color(Color::green());
    Json j;
    s.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["active_color"][0].get<int>() == 255, "switch active=red");
    AURORA_TEST_CHECK_MSG(j["inactive_color"][2].get<int>() == 255, "switch inactive=blue");
    AURORA_TEST_CHECK_MSG(j["thumb_color"][1].get<int>() == Color::green().m_g, "switch thumb=green");

    Switch t;
    t.deserialize_props(j);
    Json k;
    t.serialize_props(k);
    AURORA_TEST_CHECK_MSG(k["inactive_color"][2].get<int>() == 255, "switch rt inactive");
}

static void test_interaction() {
    Switch sw{ Reactive{ false } };
    AURORA_TEST_CHECK_MSG(sw.value() == false, "Switch: initial false");
    sw.set_value(true);
    AURORA_TEST_CHECK_MSG(sw.value() == true, "Switch: set_value true");

    Switch sw2{ Reactive{ false } };
    fire(sw2, make_press(20, 12));
    fire(sw2, make_release(20, 12));
    AURORA_TEST_CHECK_MSG(sw2.value() == true, "Switch: press+release toggles");

    bool changed = false;
    Switch sw3{ Reactive{ true }, [&](bool v) -> void { changed = v; } };
    fire(sw3, make_press(20, 12));
    fire(sw3, make_release(20, 12));
    AURORA_TEST_CHECK_MSG(changed == false, "Switch: onChanged fired with false");

    State s{ true };
    Switch sw4{ Binding{ s } };
    sw4.set_value(false);
    AURORA_TEST_CHECK_MSG(s.get() == false, "Switch: Binding write-through");
}

static void test_roundtrip() {
    const auto schema = describe_component("Switch");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(Switch) non-empty");

    const auto w = std::make_shared<Switch>();
    w->set_value(true);
    Json j = serialization::to_json(*w);
    AURORA_TEST_CHECK_MSG(j["props"].contains("checked") && j["props"]["checked"].get<bool>() == true,
                          "Switch serialization checked");
    const auto back = roundtrip<Switch>(j, "Switch");
    AURORA_TEST_CHECK_MSG(back && back->value() == true, "Switch roundtrip preserves checked");
}

static void test_modern_props() {
    // 禁用态：点击不切换
    Switch sw{ Reactive{ false } };
    sw.set_enabled(false);
    fire(sw, make_press(20, 12));
    fire(sw, make_release(20, 12));
    AURORA_TEST_CHECK_MSG(sw.value() == false, "Switch: disabled ignores click");
    sw.set_enabled(true);
    fire(sw, make_press(20, 12));
    fire(sw, make_release(20, 12));
    AURORA_TEST_CHECK_MSG(sw.value() == true, "Switch: re-enabled restores toggle");

    // active_color 未设置不序列化（跟随主题 primary）
    const Switch s0;
    Json j0;
    s0.serialize_props(j0);
    AURORA_TEST_CHECK_MSG(!j0.contains("active_color"), "Switch: unset active_color not serialized (follows theme)");

    // 新属性往返：轨道尺寸/滑块边距/描边/禁用
    Switch s1;
    s1.set_track_size(52.0f, 30.0f).set_thumb_inset(3.0f).set_border(Color::red(), 2.0f).set_enabled(false);
    Json j;
    s1.serialize_props(j);
    AURORA_TEST_CHECK_MSG(near_d(j["track_width"].get<double>(), 52.0), "Switch: track_width serialization");
    AURORA_TEST_CHECK_MSG(near_d(j["track_height"].get<double>(), 30.0), "Switch: track_height serialization");
    AURORA_TEST_CHECK_MSG(near_d(j["thumb_inset"].get<double>(), 3.0), "Switch: thumb_inset serialization");
    AURORA_TEST_CHECK_MSG(j["border_color"][0].get<int>() == 255 && near_d(j["border_width"].get<double>(), 2.0),
                          "Switch: border serialization");
    AURORA_TEST_CHECK_MSG(j["enabled"].get<bool>() == false, "Switch: enabled serialization");

    Switch s2;
    s2.deserialize_props(j);
    Json k;
    s2.serialize_props(k);
    AURORA_TEST_CHECK_MSG(near_d(k["track_width"].get<double>(), 52.0), "Switch: track_width roundtrip");
    AURORA_TEST_CHECK_MSG(s2.enabled() == false, "Switch: enabled roundtrip");

    // 布局尺寸随 track_size 变化
    const BuildContext ctx;
    Switch s3;
    s3.set_track_size(60.0f, 32.0f);
    const Size sz = s3.layout(
        Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 500, .height = 500 } }, ctx);
    AURORA_TEST_CHECK_MSG(near_d(sz.width, 60.0) && near_d(sz.height, 32.0), "Switch: layout follows track_size");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_switch ===\n");
    test_props();
    test_interaction();
    test_roundtrip();
    test_modern_props();
}

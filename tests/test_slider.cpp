#include <cstdio>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/slider.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::Binding;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::describe_component;
using aurora::Json;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Point;
using aurora::Reactive;
using aurora::Size;
using aurora::Slider;
using aurora::State;
using aurora::Widget;

static auto make_press(float x, const float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
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
    Slider sl;
    sl.set_active_color(Color::red()).set_inactive_color(Color::blue()).set_range(-1.0, 1.0);
    Json j;
    sl.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["active_color"][0].get<int>() == 255, "slider active=red");
    AURORA_TEST_CHECK_MSG(j["inactive_color"][2].get<int>() == 255, "slider inactive=blue");
    AURORA_TEST_CHECK_MSG(near_d(j["min"].get<double>(), -1.0), "slider min=-1");
    AURORA_TEST_CHECK_MSG(near_d(j["max"].get<double>(), 1.0), "slider max=1");
}

static void test_interaction() {
    Slider sl{ Reactive{ 0.5 } };
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 0.5), "Slider: initial 0.5");
    sl.set_value(0.8);
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 0.8), "Slider: set_value 0.8");
    sl.set_range(0.0, 10.0);
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 0.8), "Slider: value unchanged after set_range");
    sl.set_value(20.0);
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 10.0), "Slider: clamps to max");
    sl.set_value(-5.0);
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 0.0), "Slider: clamps to min");

    Slider sl2{ Reactive{ 0.0 } };
    constexpr BuildContext ctx;
    sl2.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 200, .height = 200 } }, ctx);

    fire(sl2, make_press(100.0f, 12.0f));
    AURORA_TEST_CHECK_MSG(near_d(sl2.value(), 0.5, 1e-2), "Slider: press at midpoint sets ~0.5");

    State st{ 0.3 };
    Slider sl3{ Binding{ st } };
    sl3.set_value(0.9);
    AURORA_TEST_CHECK_MSG(near_d(st.get(), 0.9), "Slider: Binding write-through");
}

static void test_roundtrip() {
    const auto schema = describe_component("Slider");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(Slider) non-empty");

    const auto w = std::make_shared<Slider>();
    w->set_range(0.0, 10.0);
    w->set_value(3.5);
    Json j = serialization::to_json(*w);
    AURORA_TEST_CHECK_MSG(j["props"].contains("value") && j["props"]["value"].get<double>() == 3.5,
                          "Slider serialization value");
    const auto back = roundtrip<Slider>(j, "Slider");
    AURORA_TEST_CHECK_MSG(back && back->value() == 3.5, "Slider roundtrip preserves value");
}

static void test_modern_props() {
    // 步进吸附（对标 Qt singleStep / Flutter divisions）
    Slider sl;
    sl.set_range(0.0, 10.0).set_step(2.0);
    sl.set_value(3.4);
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 4.0), "Slider: step=2 snaps to 4");
    sl.set_value(9.9);
    AURORA_TEST_CHECK_MSG(near_d(sl.value(), 10.0), "Slider: step snapping still clamps to max");

    // 禁用态：忽略拖拽
    Slider sl2{ Reactive{ 0.2 } };
    constexpr BuildContext ctx;
    sl2.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 200, .height = 200 } }, ctx);
    sl2.set_enabled(false);
    fire(sl2, make_press(100.0f, 12.0f));
    AURORA_TEST_CHECK_MSG(near_d(sl2.value(), 0.2), "Slider: disabled ignores click, value unchanged");

    // 新属性序列化往返；active_color 未设置不输出（跟随主题）
    const Slider sl3;
    Json j0;
    sl3.serialize_props(j0);
    AURORA_TEST_CHECK_MSG(!j0.contains("active_color"), "Slider: unset active_color not serialized (follows theme)");

    Slider sl4;
    sl4.set_thumb_color(Color::red()).set_track_height(8.0f).set_thumb_size(20.0f).set_step(0.5).set_enabled(false);
    Json j;
    sl4.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["thumb_color"][0].get<int>() == 255, "Slider: thumb_color serialization");
    AURORA_TEST_CHECK_MSG(near_d(j["track_height"].get<double>(), 8.0), "Slider: track_height serialization");
    AURORA_TEST_CHECK_MSG(near_d(j["thumb_size"].get<double>(), 20.0), "Slider: thumb_size serialization");
    AURORA_TEST_CHECK_MSG(near_d(j["step"].get<double>(), 0.5), "Slider: step serialization");
    AURORA_TEST_CHECK_MSG(j["enabled"].get<bool>() == false, "Slider: enabled serialization");

    Slider sl5;
    sl5.deserialize_props(j);
    AURORA_TEST_CHECK_MSG(near_d(sl5.step(), 0.5), "Slider: step roundtrip");
    AURORA_TEST_CHECK_MSG(sl5.enabled() == false, "Slider: enabled roundtrip");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_slider ===\n");
    test_props();
    test_interaction();
    test_roundtrip();
    test_modern_props();
}

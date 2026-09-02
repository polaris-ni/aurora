#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/serialization.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::Binding;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::describe_component;
using aurora::Json;
using aurora::ProgressIndicator;
using aurora::Reactive;
using aurora::Size;
using aurora::State;

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
    ProgressIndicator p;
    p.set_color(Color::red()).set_track_color(Color::blue());
    Json j;
    p.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["color"][0].get<int>() == 255, "progress color=red");
    AURORA_TEST_CHECK_MSG(j["track_color"][2].get<int>() == 255, "progress track=blue");

    ProgressIndicator q;
    q.deserialize_props(j);
    Json k;
    q.serialize_props(k);
    AURORA_TEST_CHECK_MSG(k["color"][0].get<int>() == 255, "progress rt color");
}

static void test_interaction() {
    ProgressIndicator pi{ Reactive{ 0.3 } };
    AURORA_TEST_CHECK_MSG(near_d(pi.value(), 0.3), "Progress: initial 0.3");
    pi.set_value(0.7);
    AURORA_TEST_CHECK_MSG(near_d(pi.value(), 0.7), "Progress: set_value 0.7");
    pi.set_value(2.0);
    AURORA_TEST_CHECK_MSG(near_d(pi.value(), 1.0), "Progress: clamps to 1.0");
    pi.set_value(-1.0);
    AURORA_TEST_CHECK_MSG(near_d(pi.value(), 0.0), "Progress: clamps to 0.0");

    State st{ 0.2 };
    ProgressIndicator pi2{ Binding{ st } };
    pi2.set_value(0.5);
    AURORA_TEST_CHECK_MSG(near_d(st.get(), 0.5), "Progress: Binding write-through");
}

static void test_roundtrip() {
    const auto schema = describe_component("ProgressIndicator");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(ProgressIndicator) non-empty");

    const auto w = std::make_shared<ProgressIndicator>();
    w->set_value(0.3);
    Json j = serialization::to_json(*w);
    AURORA_TEST_CHECK_MSG(j["props"].contains("value") && j["props"]["value"].get<double>() == 0.3,
                          "ProgressIndicator serialization value");
    const auto back = roundtrip<ProgressIndicator>(j, "ProgressIndicator");
    AURORA_TEST_CHECK_MSG(back && back->value() == 0.3, "ProgressIndicator roundtrip preserves value");
}

static void test_modern_props() {
    // color 未设置不序列化（跟随主题 primary）
    const ProgressIndicator p0;
    Json j0;
    p0.serialize_props(j0);
    AURORA_TEST_CHECK_MSG(!j0.contains("color"), "Progress: unset color not serialized (follows theme)");

    // 厚度/圆角往返；布局高度随 thickness
    ProgressIndicator p1;
    p1.set_thickness(10.0f).set_corner_radius(3.0f);
    Json j;
    p1.serialize_props(j);
    AURORA_TEST_CHECK_MSG(near_d(j["thickness"].get<double>(), 10.0), "Progress: thickness serialization");
    AURORA_TEST_CHECK_MSG(near_d(j["corner_radius"].get<double>(), 3.0), "Progress: corner_radius serialization");

    ProgressIndicator p2;
    p2.deserialize_props(j);
    const BuildContext ctx;
    const Size sz = p2.layout(
        Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 300, .height = 300 } }, ctx);
    AURORA_TEST_CHECK_MSG(near_d(sz.height, 10.0), "Progress: layout height follows thickness");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_progress ===\n");
    test_props();
    test_interaction();
    test_roundtrip();
    test_modern_props();
}

#include <cstdio>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "aurora/widget/divider.h"
#include "aurora/widget/serialization.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::describe_component;
using aurora::Divider;
using aurora::DividerProps;
using aurora::Json;
using aurora::Orientation;
using aurora::Size;

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
    Divider d;
    d.orientation = Orientation::Vertical;
    d.set_indent(4.0f).set_end_indent(6.0f);
    d.color = Color::red();
    d.thickness = 2.0f;
    Json j;
    d.serialize_props(j);
    AURORA_TEST_CHECK_MSG(j["orientation"].get<std::string>() == "vertical", "divider vertical");
    AURORA_TEST_CHECK_MSG(near_f(j["indent"].get<float>(), 4.0f), "divider indent=4");
    AURORA_TEST_CHECK_MSG(near_f(j["end_indent"].get<float>(), 6.0f), "divider end_indent=6");
    AURORA_TEST_CHECK_MSG(j["color"][0].get<int>() == 255, "divider color=red");

    Divider e;
    e.deserialize_props(j);
    Json k;
    e.serialize_props(k);
    AURORA_TEST_CHECK_MSG(k["orientation"].get<std::string>() == "vertical", "divider rt vertical");
    AURORA_TEST_CHECK_MSG(near_f(k["end_indent"].get<float>(), 6.0f), "divider rt end_indent");
}

static void test_layout() {
    Divider d{};
    AURORA_TEST_CHECK_MSG(d.orientation == Orientation::Horizontal, "Divider: default horizontal");
    AURORA_TEST_CHECK_MSG(near_f(d.thickness, 1.0f), "Divider: default thickness 1");
    Divider dv{ DividerProps{ .orientation = Orientation::Vertical, .thickness = 2.0f } };
    AURORA_TEST_CHECK_MSG(dv.orientation == Orientation::Vertical, "Divider: config vertical");
    AURORA_TEST_CHECK_MSG(near_f(dv.thickness, 2.0f), "Divider: config thickness 2");

    constexpr BuildContext ctx;
    d.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 100 } }, ctx);
    AURORA_TEST_CHECK_MSG(near_f(d.size().height, 1.0f), "Divider: horizontal height = thickness");
    AURORA_TEST_CHECK_MSG(near_f(d.size().width, 100.0f), "Divider: horizontal fills width");

    dv.layout(Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 100 } }, ctx);
    AURORA_TEST_CHECK_MSG(near_f(dv.size().width, 2.0f), "Divider: vertical width = thickness");
    AURORA_TEST_CHECK_MSG(near_f(dv.size().height, 100.0f), "Divider: vertical fills height");
}

static void test_roundtrip() {
    const auto schema = describe_component("Divider");
    AURORA_TEST_CHECK_MSG(!schema.empty(), "describe_component(Divider) non-empty");

    const auto w = std::make_shared<Divider>();
    w->orientation = Orientation::Vertical;
    w->thickness = 2.0f;
    Json j = serialization::to_json(*w);
    const Json &p = j["props"];
    AURORA_TEST_CHECK_MSG(p.contains("orientation"), "Divider serialization orientation");
    AURORA_TEST_CHECK_MSG(p.contains("thickness") && p["thickness"].get<float>() == 2.0f,
                          "Divider serialization thickness");
    const auto back = roundtrip<Divider>(j, "Divider");
    AURORA_TEST_CHECK_MSG(back && back->thickness == 2.0f, "Divider roundtrip preserves thickness");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_divider ===\n");
    test_props();
    test_layout();
    test_roundtrip();
}

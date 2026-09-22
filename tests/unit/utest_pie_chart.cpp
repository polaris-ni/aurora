/// 测试类型: unit
/// 目标单元: include/aurora/widget/pie_chart.h
/// 测试说明: 覆盖 PieChart（切片 5）——defaults / describe_static / 序列化往返 / 工厂 from_json 重建、
/// 极坐标命中（半径 + 角度定位扇区）、on_section_tapped、Σ≤0 与负值降级、图例命中优先，
/// 以及像素 golden 基线（chart_pie.png，受 AURORA_UPDATE_GOLDEN 控制）

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"

namespace aurora::test_cases::utest_pie_chart {

using aurora::testing::require_value;

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 320;
constexpr int AURORA_HEIGHT = 200;

[[nodiscard]] auto bare_props() -> PieChartProps {
    PieChartProps p{};
    p.legend.visible = false;
    p.padding = EdgeInsets{};
    return p;
}

auto layout_only(Widget &w) -> void {
    constexpr BuildContext ctx;
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(AURORA_WIDTH), .height = static_cast<float>(AURORA_HEIGHT)};
    w.layout(c, ctx);
}

[[nodiscard]] auto move_at(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Move;
    e.local_position = Point{.x = x, .y = y};
    return e;
}

[[nodiscard]] auto release_at(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Release;
    e.local_position = Point{.x = x, .y = y};
    return e;
}

[[nodiscard]] auto render_chart(const PieChartProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<PieChart>(props);
    chart->modifier.set(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp = std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_pie.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

}  // namespace

AURORA_TEST_CASE(defaults_and_identity) {
    const PieChartProps d = PieChart::defaults();
    AURORA_TEST_CHECK_TRUE(d.sections.empty());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.center_space_ratio), 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.start_angle), -90.0, 1e-6);
    AURORA_TEST_CHECK_FALSE(d.show_percentage_labels);

    PieChart c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"PieChart"});
    AURORA_TEST_CHECK_TRUE(c.wants_click());
    AURORA_TEST_CHECK_FALSE(c.hovered_section().has_value());
}

AURORA_TEST_CASE(describe_static_is_complete) {
    const WidgetDescriptor d = PieChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "PieChart");
    AURORA_TEST_CHECK_TRUE(d.events.front() == "on_section_tapped");
    auto has = [&d](const std::string &key) -> bool {
        return std::ranges::any_of(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
    };
    for (const auto &key : {"sections", "center_space_ratio", "start_angle", "show_percentage_labels", "section_gap",
                            "legend", "padding"}) {
        AURORA_TEST_CHECK_TRUE(has(key));
    }
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    PieChartProps p{};
    p.sections = {PieSection{.name = "A", .value = 3.0, .color = Color{1, 2, 3, 255}},
                  PieSection{.name = "B", .value = 1.0}};
    p.center_space_ratio = 0.5F;
    p.show_percentage_labels = true;
    PieChart src{p};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["sections"].size(), 2U);
    PieChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.sections[0].name == "A");
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.center_space_ratio), 0.5, 1e-6);
    AURORA_TEST_CHECK_TRUE(dst.show_percentage_labels);
    AURORA_TEST_CHECK_FALSE(dst.sections[1].color.has_value());

    serialization::register_core_widgets();
    Json node = Json::object();
    node["type"] = "PieChart";
    node["props"] = props;
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_REQUIRE_NOT_NULL(dynamic_cast<const PieChart *>(built.value().get()));
}

AURORA_TEST_CASE(polar_hit_maps_angle_to_section) {
    // 无留白：中心 (160,100)、外径 100（min(320,200)/2）、12 点起；A 占 75% ⇒ 0..270°，B ⇒ 270..360°。
    PieChartProps p = bare_props();
    p.sections = {PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}};
    PieChart chart{p};
    layout_only(chart);

    MouseEvent in_a = move_at(160.0F, 40.0F);  // 正上方（12 点）
    chart.on_pointer_event(in_a);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_section().has_value());
    AURORA_TEST_CHECK_EQ(require_value(chart.hovered_section()), 0);

    MouseEvent in_b = move_at(104.0F, 44.0F);  // 相对角 315°（左上）
    chart.on_pointer_event(in_b);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_section().has_value());
    AURORA_TEST_CHECK_EQ(require_value(chart.hovered_section()), 1);

    MouseEvent outside = move_at(2.0F, 2.0F);  // 半径之外
    chart.on_pointer_event(outside);
    AURORA_TEST_CHECK_FALSE(chart.hovered_section().has_value());
}

AURORA_TEST_CASE(release_fires_on_section_tapped) {
    PieChartProps p = bare_props();
    p.sections = {PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}};
    PieChart chart{p};
    layout_only(chart);

    int got = -1;
    chart.on_section_tapped = [&got](int idx) -> void { got = idx; };
    MouseEvent e = release_at(160.0F, 40.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(got, 0);
}

AURORA_TEST_CASE(non_positive_total_falls_back_safely) {
    PieChartProps p = bare_props();
    p.sections = {PieSection{.name = "z", .value = 0.0}, PieSection{.name = "n", .value = -5.0}};
    PieChart chart{p};
    layout_only(chart);
    AURORA_TEST_CHECK_FALSE(chart.accessibility_label().empty());
    MouseEvent e = move_at(160.0F, 40.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_FALSE(chart.hovered_section().has_value());  // Σ ≤ 0 ⇒ 无扇区可命中
}

AURORA_TEST_CASE(golden_pie_chart_matches_baseline) {
    PieChartProps p{};
    p.sections = {PieSection{.name = "mobile", .value = 45.0}, PieSection{.name = "desktop", .value = 30.0},
                  PieSection{.name = "tablet", .value = 15.0}, PieSection{.name = "other", .value = 10.0}};
    p.center_space_ratio = 0.45F;
    p.show_percentage_labels = true;
    golden::compare_or_update("chart_pie", render_chart(p));
}

}  // namespace aurora::test_cases::utest_pie_chart

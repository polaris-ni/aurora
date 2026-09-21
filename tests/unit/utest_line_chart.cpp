/// 测试类型: unit
/// 目标单元: include/aurora/widget/line_chart.h
/// 测试说明: 覆盖 LineChart（切片 4）——defaults / describe_static / 序列化往返 / 工厂 from_json 重建、
/// hover 最近点命中（等距 x = 索引，域与渲染同源）、on_point_tapped 触发、空数据与 NaN 降级、
/// 以及像素 golden 基线（chart_line.png，受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/png.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"

namespace aurora::test_cases::utest_line_chart {

using aurora::testing::require_value;

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 320;
constexpr int AURORA_HEIGHT = 200;

/// @brief 关掉轴与图例、清零留白：绘图区 = 整个控件，命中几何可精确预期。
[[nodiscard]] auto bare_props() -> LineChartProps {
    LineChartProps p{};
    p.axis_x.visible = false;
    p.axis_y.visible = false;
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

/// @brief 无头渲染一帧到临时 PNG（无 Application ⇒ grow-in 降级为终态，输出确定）。
[[nodiscard]] auto render_chart(const LineChartProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<LineChart>(props);
    chart->modifier.set(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp = std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_line.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

}  // namespace

AURORA_TEST_CASE(defaults_and_identity) {
    const LineChartProps d = LineChart::defaults();
    AURORA_TEST_CHECK_TRUE(d.series.empty());
    AURORA_TEST_CHECK_TRUE(d.show_dots);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.line_width), 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.dot_radius), 3.0, 1e-6);

    LineChart c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"LineChart"});
    AURORA_TEST_CHECK_TRUE(c.wants_click());
    AURORA_TEST_CHECK_FALSE(c.hovered_point().has_value());
}

AURORA_TEST_CASE(describe_static_is_complete) {
    const WidgetDescriptor d = LineChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "LineChart");
    auto has = [&d](const std::string &key) -> bool {
        return std::ranges::any_of(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
    };
    for (const auto &key : {"series", "categories", "show_dots", "line_width", "dot_radius", "show_crosshair", "axis_x",
                            "axis_y", "legend", "padding"}) {
        AURORA_TEST_CHECK_TRUE(has(key));
    }
    AURORA_TEST_CHECK_FALSE(d.events.empty());
    AURORA_TEST_CHECK_TRUE(d.events.front() == "on_point_tapped");
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    LineChartProps p{};
    p.series = {ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}, .color = Color{9, 8, 7, 255}}};
    p.categories = {"a", "b", "c"};
    p.show_dots = false;
    p.line_width = 3.5F;
    p.show_crosshair = false;
    LineChart src{p};
    Json props = Json::object();
    src.serialize_props(props);
    LineChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.series[0].name == "A");
    AURORA_TEST_CHECK_TRUE(dst.series[0].values == std::vector<double>{1.0, 2.0, 3.0});
    AURORA_TEST_CHECK_FALSE(dst.show_dots);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.line_width), 3.5, 1e-6);
    AURORA_TEST_CHECK_FALSE(dst.show_crosshair);

    serialization::register_core_widgets();
    Json node = Json::object();
    node["type"] = "LineChart";
    node["props"] = props;
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    const auto *chart = dynamic_cast<const LineChart *>(built.value().get());
    AURORA_TEST_REQUIRE_NOT_NULL(chart);
    AURORA_TEST_CHECK_EQ(chart->series.size(), 1U);
}

AURORA_TEST_CASE(hover_snaps_to_nearest_point_index) {
    // 显式 y 域 [0,4] 使几何确定：3 点等距 x = 0 / 160 / 320，值 {0,2,4} 映射 y = 200 / 100 / 0。
    // x 由索引线性映射、y 由显式域线性映射，与渲染同源（见 nearest_point）。
    LineChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}};
    p.axis_y = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    LineChart chart{p};
    layout_only(chart);

    MouseEvent at_mid = move_at(160.0F, 100.0F);  // 点 index 1 (v=2 → y=100)
    chart.on_pointer_event(at_mid);
    const auto mid = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(mid.second, 1);

    MouseEvent at_first = move_at(2.0F, 198.0F);  // 点 index 0 (v=0 → y=200)
    chart.on_pointer_event(at_first);
    const auto first_pt = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(first_pt.second, 0);

    MouseEvent at_last = move_at(318.0F, 2.0F);  // 点 index 2 (v=4 → y=0)
    chart.on_pointer_event(at_last);
    const auto last_pt = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(last_pt.second, 2);

    MouseEvent outside = move_at(-30.0F, -30.0F);
    chart.on_pointer_event(outside);
    AURORA_TEST_CHECK_FALSE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(legend_hover_takes_priority_over_data) {
    LineChartProps p{};
    p.series = {ChartSeries{.name = "A", .values = {1.0, 2.0}}, ChartSeries{.name = "B", .values = {2.0, 1.0}}};
    p.padding = EdgeInsets{};
    p.axis_x.visible = false;
    p.axis_y.visible = false;
    LineChart chart{p};
    layout_only(chart);

    // 图例在顶部（padding=0 ⇒ 图例带紧贴 y=0），命中图例项即联动，不再解析数据点
    MouseEvent on_legend = move_at(2.0F, 2.0F);
    chart.on_pointer_event(on_legend);
    AURORA_TEST_CHECK_TRUE(chart.hovered_legend().has_value());
    AURORA_TEST_CHECK_FALSE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(release_fires_on_point_tapped) {
    LineChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}};
    p.axis_y = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    LineChart chart{p};
    layout_only(chart);

    int got_series = -1;
    int got_point = -1;
    chart.on_point_tapped = [&got_series, &got_point](int si, int pi) -> void {
        got_series = si;
        got_point = pi;
    };
    MouseEvent e = release_at(160.0F, 100.0F);  // 点 index 1 (v=2 → y=100)
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(got_series, 0);
    AURORA_TEST_CHECK_EQ(got_point, 1);
}

AURORA_TEST_CASE(degenerate_data_is_safe) {
    LineChart empty{};
    layout_only(empty);
    AURORA_TEST_CHECK_FALSE(empty.accessibility_label().empty());
    AURORA_TEST_CHECK_TRUE(empty.accessibility_value().empty());

    LineChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {NAN, INFINITY, 1.0}}};
    LineChart chart{p};
    layout_only(chart);
    AURORA_TEST_CHECK_FALSE(chart.accessibility_label().empty());
}

AURORA_TEST_CASE(golden_line_chart_matches_baseline) {
    LineChartProps p{};
    p.series = {ChartSeries{.name = "cpu", .values = {12.0, 18.0, 9.0, 24.0, 15.0, 20.0}},
                ChartSeries{.name = "mem", .values = {8.0, 10.0, 14.0, 12.0, 18.0, 16.0}}};
    p.categories = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    p.axis_y = ChartAxisSpec{.label = "%", .tick_count = 5};
    golden::compare_or_update("chart_line", render_chart(p));
}

}  // namespace aurora::test_cases::utest_line_chart

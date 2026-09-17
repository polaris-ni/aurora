/// 测试类型: unit
/// 目标单元: include/aurora/widget/bar_chart.h
/// 测试说明: 覆盖 BarChart 切片 3 全链路——构造不变量与 defaults、describe_static 属性完备、
/// 序列化往返（含 series 对象数组 / axis / legend / padding）、工厂 from_json 重建、
/// hover 命中几何（纯计算，同源 BandScale 反查）、on_point_tapped 触发、空数据与畸形输入降级，
/// 以及像素 golden 基线（chart_bar.png，受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/png.h"
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"

namespace aurora::test_cases::utest_bar_chart {

namespace golden = aurora::testing::golden;

namespace {

constexpr float WIDTH = 320.0F;
constexpr float HEIGHT = 200.0F;

/// @brief 关掉轴与图例、清零留白：绘图区 = 整个控件，命中几何可精确预期。
[[nodiscard]] auto bare_props() -> BarChartProps {
    BarChartProps p{};
    p.axis_x.visible = false;
    p.axis_y.visible = false;
    p.legend.visible = false;
    p.padding = EdgeInsets{};
    return p;
}

/// @brief 布局一帧（无头，无 Application）。
auto layout_only(Widget &w) -> void {
    constexpr BuildContext ctx;
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = WIDTH, .height = HEIGHT};
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


}  // namespace

AURORA_TEST_CASE(defaults_match_declared_property_defaults) {
    const BarChartProps d = BarChart::defaults();
    AURORA_TEST_CHECK_TRUE(d.series.empty());
    AURORA_TEST_CHECK_TRUE(d.categories.empty());
    AURORA_TEST_CHECK_FALSE(d.stacked);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.bar_width_ratio), 0.7, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.bar_corner_radius), 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding.left), 8.0, 1e-6);

    BarChart c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"BarChart"});
    AURORA_TEST_CHECK_TRUE(c.hovered_point() == std::nullopt);
    AURORA_TEST_CHECK_TRUE(c.wants_click());
}

AURORA_TEST_CASE(describe_static_lists_all_props_and_events) {
    const WidgetDescriptor d = BarChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "BarChart");
    AURORA_TEST_CHECK_TRUE(d.children_policy == "none");
    AURORA_TEST_CHECK_FALSE(d.properties.empty());

    auto has = [&d](const std::string &key) -> bool {
        for (const PropDescriptor &p : d.properties) {
            if (p.name == key) {
                return true;
            }
        }
        return false;
    };
    for (const auto &key : {"series", "categories", "stacked", "bar_width_ratio", "bar_corner_radius", "axis_x",
                            "axis_y", "legend", "padding", "width", "height", "show"}) {
        AURORA_TEST_CHECK_TRUE(has(key));
    }
    bool has_event = false;
    for (const std::string &e : d.events) {
        if (e == "on_point_tapped") {
            has_event = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_event);
}

AURORA_TEST_CASE(props_roundtrip_including_series_array) {
    BarChartProps p{};
    p.series = {ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}, .color = Color{10, 20, 30, 255}},
                ChartSeries{.name = "B", .values = {4.0, 5.0}}};
    p.categories = {"Mon", "Tue", "Wed"};
    p.stacked = true;
    p.bar_width_ratio = 0.5F;
    p.bar_corner_radius = 4.0F;
    p.axis_y = ChartAxisSpec{.label = "k", .tick_count = 4, .min = 0.0, .show_grid_lines = false};
    p.legend = ChartLegendSpec{.visible = true, .position = LegendPosition::Right};
    p.padding = EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F};

    BarChart src{p};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_REQUIRE_TRUE(props.contains("series"));
    AURORA_TEST_REQUIRE_TRUE(props["series"].is_array());
    AURORA_TEST_CHECK_EQ(props["series"].size(), 2U);

    BarChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.series.size(), 2U);
    AURORA_TEST_CHECK_TRUE(dst.series[0].name == "A");
    AURORA_TEST_CHECK_TRUE(dst.series[0].values == std::vector<double>{1.0, 2.0, 3.0});
    AURORA_TEST_REQUIRE_TRUE(dst.series[0].color.has_value());
    AURORA_TEST_CHECK_TRUE(*dst.series[0].color == Color{10, 20, 30, 255});
    AURORA_TEST_CHECK_FALSE(dst.series[1].color.has_value());
    AURORA_TEST_CHECK_TRUE(dst.categories == std::vector<std::string>{"Mon", "Tue", "Wed"});
    AURORA_TEST_CHECK_TRUE(dst.stacked);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.bar_width_ratio), 0.5, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.bar_corner_radius), 4.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(dst.axis_y.label == "k");
    AURORA_TEST_CHECK_EQ(dst.axis_y.tick_count, 4);
    AURORA_TEST_CHECK_FALSE(dst.axis_y.show_grid_lines);
    AURORA_TEST_CHECK_TRUE(dst.legend.position == LegendPosition::Right);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.padding.bottom), 5.0, 1e-6);
}

AURORA_TEST_CASE(factory_rebuilds_from_json) {
    serialization::register_core_widgets();
    Json props = Json::object();
    props["series"] = chart_series_vector_to_json({ChartSeries{.name = "S", .values = {2.0, 4.0, 6.0}}});
    Json node = Json::object();
    node["type"] = "BarChart";
    node["props"] = props;

    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_CHECK_TRUE(built.value()->type_name() == std::string{"BarChart"});
    const auto *chart = dynamic_cast<const BarChart *>(built.value().get());
    AURORA_TEST_REQUIRE_NOT_NULL(chart);
    AURORA_TEST_CHECK_EQ(chart->series.size(), 1U);
    AURORA_TEST_CHECK_TRUE(chart->series[0].values == std::vector<double>{2.0, 4.0, 6.0});
}

AURORA_TEST_CASE(hover_maps_local_position_to_category) {
    // 4 个类目、无留白 → 绘图区 = 320×200，带中心分别在 40 / 120 / 200 / 280。
    BarChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0, 4.0}}};
    BarChart chart{p};
    layout_only(chart);

    MouseEvent e1 = move_at(120.0F, 100.0F);
    chart.on_pointer_event(e1);
    const auto hit = chart.hovered_point();
    AURORA_TEST_REQUIRE_TRUE(hit.has_value());
    AURORA_TEST_CHECK_EQ(hit->second, 1);
    AURORA_TEST_CHECK_EQ(hit->first, 0);

    MouseEvent e2 = move_at(280.0F, 100.0F);
    chart.on_pointer_event(e2);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_point().has_value());
    AURORA_TEST_CHECK_EQ(chart.hovered_point()->second, 3);

    // 移出绘图区（负坐标）→ 高亮清除
    MouseEvent e3 = move_at(-10.0F, 100.0F);
    chart.on_pointer_event(e3);
    AURORA_TEST_CHECK_TRUE(chart.hovered_point() == std::nullopt);
}

AURORA_TEST_CASE(hover_splits_grouped_series_by_bar_width) {
    // 两系列分组：band=80、group=56、bar=28 → 带 1 的组内 [92,120) 属系列 0，[120,148) 属系列 1。
    BarChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0, 4.0}},
                ChartSeries{.name = "B", .values = {4.0, 3.0, 2.0, 1.0}}};
    BarChart chart{p};
    layout_only(chart);

    MouseEvent in_first = move_at(100.0F, 100.0F);
    chart.on_pointer_event(in_first);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_point().has_value());
    AURORA_TEST_CHECK_EQ(chart.hovered_point()->first, 0);

    MouseEvent in_second = move_at(130.0F, 100.0F);
    chart.on_pointer_event(in_second);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_point().has_value());
    AURORA_TEST_CHECK_EQ(chart.hovered_point()->first, 1);
}

AURORA_TEST_CASE(release_fires_on_point_tapped) {
    BarChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0, 4.0}}};
    BarChart chart{p};
    layout_only(chart);

    int got_series = -1;
    int got_point = -1;
    chart.on_point_tapped = [&got_series, &got_point](int si, int pi) -> void {
        got_series = si;
        got_point = pi;
    };

    MouseEvent e = release_at(200.0F, 100.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(got_series, 0);
    AURORA_TEST_CHECK_EQ(got_point, 2);
}

AURORA_TEST_CASE(empty_and_degenerate_data_are_safe) {
    BarChart empty{};
    layout_only(empty);
    AURORA_TEST_CHECK_TRUE(empty.hovered_point() == std::nullopt);
    AURORA_TEST_CHECK_FALSE(empty.accessibility_label().empty());
    AURORA_TEST_CHECK_TRUE(empty.accessibility_value().empty());

    // 全 NaN：不得产生除零 / NaN 几何
    BarChartProps p = bare_props();
    p.series = {ChartSeries{.name = "A", .values = {NAN, INFINITY}}};
    BarChart chart{p};
    layout_only(chart);
    MouseEvent e = move_at(40.0F, 100.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(golden_bar_chart_matches_baseline) {
    BarChartProps p{};
    p.series = {ChartSeries{.name = "2024", .values = {12.0, 18.0, 9.0, 24.0, 15.0}},
                ChartSeries{.name = "2025", .values = {16.0, 14.0, 20.0, 18.0, 22.0}}};
    p.categories = {"Mon", "Tue", "Wed", "Thu", "Fri"};
    p.axis_y = ChartAxisSpec{.label = "sales", .tick_count = 5};
    auto chart = std::make_shared<BarChart>(p);
    chart->modifier.set(Modifier{}.width(320.0F).height(200.0F));

    Node root{Column{Node{chart}}};
    const std::filesystem::path current_path =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_bar.png";

    AURORA_TEST_REQUIRE_TRUE(
        render_to_png(root, static_cast<int>(WIDTH), static_cast<int>(HEIGHT), current_path.string().c_str()).ok());

    // 传入 root 以启用归因：失败时报告会说清差异落在哪个控件的盒子里。
    golden::compare_or_update("chart_bar", current_path, &root);
}

}  // namespace aurora::test_cases::utest_bar_chart

/// 测试类型: unit
/// 目标单元: include/aurora/widget/scatter_chart.h
/// 测试说明: 覆盖 ScatterChart（切片 6）——defaults / describe_static / 序列化往返（points 为 [[x,y]] 数组）/
/// 工厂 from_json 重建、最近点欧氏距离命中（阈值 = dot_radius + 4dp）、on_point_tapped、
/// 空数据与 NaN 点跳过，以及像素 golden 基线（chart_scatter.png，受 AURORA_UPDATE_GOLDEN 控制）

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
#include "aurora/render/snapshot_diff.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scatter_chart {

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 200;

[[nodiscard]] auto bare_props() -> ScatterChartProps {
    ScatterChartProps p{};
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
    c.max = Size{.width = static_cast<float>(WIDTH), .height = static_cast<float>(HEIGHT)};
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

[[nodiscard]] auto env_value(const char *name) -> std::string_view {
    const char *raw = std::getenv(name);
    if (raw == nullptr) {
        return {};
    }
    return {raw};
}

[[nodiscard]] auto env_flag(const char *name) -> bool { return !env_value(name).empty(); }

[[nodiscard]] auto env_int(const char *name, int fallback) -> int {
    const std::string_view raw = env_value(name);
    if (raw.empty()) {
        return fallback;
    }
    int parsed = fallback;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *last = raw.data() + raw.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto [end, ec] = std::from_chars(raw.data(), last, parsed);
    return (ec == std::errc{} && end == last) ? parsed : fallback;
}

auto compare_or_update_golden(const std::filesystem::path &current_path, const std::string &base_name) -> void {
    const std::filesystem::path dir = std::filesystem::path(testing::isolation::repo_root()) / "tests" / "golden";
    const std::filesystem::path golden_path = dir / (base_name + ".png");
    const auto current = Image::load(current_path.string());
    AURORA_TEST_REQUIRE_TRUE(current.ok());
    if (env_flag("AURORA_UPDATE_GOLDEN")) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(current_path, golden_path, std::filesystem::copy_options::overwrite_existing, ec);
        AURORA_TEST_CHECK_FALSE(static_cast<bool>(ec));
        return;
    }
    const auto golden = Image::load(golden_path.string());
    AURORA_TEST_REQUIRE_MSG(golden.ok(), base_name + ".png missing (run with AURORA_UPDATE_GOLDEN=1)");
    const SnapshotDiff diff =
        compare_snapshots(golden.value(), current.value(), env_int("AURORA_GOLDEN_MAX_DIFF", 0));
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison)
    const bool within_budget =
        diff.pixel_diff_count <= static_cast<std::size_t>(std::max(0, env_int("AURORA_GOLDEN_MAX_PIXELS", 0)));
    AURORA_TEST_CHECK_MSG(within_budget,
                          "pixel drift vs golden " + base_name + ": " + std::to_string(diff.pixel_diff_count) + " px");
}

[[nodiscard]] auto render_chart(const ScatterChartProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<ScatterChart>(props);
    chart->modifier.set(Modifier{}.width(static_cast<float>(WIDTH)).height(static_cast<float>(HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_scatter.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, WIDTH, HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

}  // namespace

AURORA_TEST_CASE(defaults_and_identity) {
    const ScatterChartProps d = ScatterChart::defaults();
    AURORA_TEST_CHECK_TRUE(d.series.empty());
    AURORA_TEST_CHECK_TRUE(d.show_crosshair);

    ScatterChart c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"ScatterChart"});
    AURORA_TEST_CHECK_TRUE(c.wants_click());
    AURORA_TEST_CHECK_FALSE(c.hovered_point().has_value());
}

AURORA_TEST_CASE(describe_static_is_complete) {
    const WidgetDescriptor d = ScatterChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "ScatterChart");
    AURORA_TEST_CHECK_TRUE(d.events.front() == "on_point_tapped");
    auto has = [&d](const std::string &key) -> bool {
        for (const PropDescriptor &p : d.properties) {
            if (p.name == key) {
                return true;
            }
        }
        return false;
    };
    for (const auto &key : {"series", "show_crosshair", "axis_x", "axis_y", "legend", "padding"}) {
        AURORA_TEST_CHECK_TRUE(has(key));
    }
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    ScatterSeries s{};
    s.name = "P";
    s.points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 3.0, .y = 4.0}};
    s.dot_radius = 5.0F;
    ScatterChartProps p{};
    p.series = {s};
    ScatterChart src{p};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_REQUIRE_TRUE(props["series"].is_array());
    AURORA_TEST_CHECK_EQ(props["series"][0]["points"].size(), 2U);

    ScatterChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_REQUIRE_EQ(dst.series.size(), 1U);
    AURORA_TEST_CHECK_NEAR(dst.series[0].points[1].y, 4.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.series[0].dot_radius), 5.0, 1e-6);

    serialization::register_core_widgets();
    Json node = Json::object();
    node["type"] = "ScatterChart";
    node["props"] = props;
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_REQUIRE_NOT_NULL(dynamic_cast<const ScatterChart *>(built.value().get()));
}

AURORA_TEST_CASE(nearest_point_hit_by_euclidean_distance) {
    // 显式双轴域 [0,4] 使几何确定：点 (2,2) → (160,100)，hover 命中 index 0。
    ScatterChartProps p = bare_props();
    p.axis_x = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    p.axis_y = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    ScatterSeries s{};
    s.name = "P";
    s.points = {ChartPoint{.x = 2.0, .y = 2.0}};
    p.series = {s};
    ScatterChart chart{p};
    layout_only(chart);

    MouseEvent at_center = move_at(160.0F, 100.0F);
    chart.on_pointer_event(at_center);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_point().has_value());
    AURORA_TEST_CHECK_EQ(chart.hovered_point()->second, 0);

    MouseEvent far = move_at(10.0F, 10.0F);  // 超出命中半径
    chart.on_pointer_event(far);
    AURORA_TEST_CHECK_FALSE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(release_fires_on_point_tapped) {
    ScatterChartProps p = bare_props();
    p.axis_x = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    p.axis_y = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    ScatterSeries s{};
    s.name = "P";
    s.points = {ChartPoint{.x = 2.0, .y = 2.0}};
    p.series = {s};
    ScatterChart chart{p};
    layout_only(chart);

    int got_point = -1;
    chart.on_point_tapped = [&got_point](int /*si*/, int pi) -> void { got_point = pi; };
    MouseEvent e = release_at(160.0F, 100.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(got_point, 0);
}

AURORA_TEST_CASE(degenerate_data_is_safe) {
    ScatterChart empty{};
    layout_only(empty);
    AURORA_TEST_CHECK_FALSE(empty.accessibility_label().empty());

    ScatterChartProps p = bare_props();
    p.axis_x = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    p.axis_y = ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
    ScatterSeries s{};
    s.name = "P";
    s.points = {ChartPoint{.x = NAN, .y = 1.0}, ChartPoint{.x = 2.0, .y = 2.0}};
    p.series = {s};
    ScatterChart chart{p};
    layout_only(chart);
    MouseEvent e = move_at(160.0F, 100.0F);  // 点 (2,2) 在 [0,4] 域下 → (160,100)
    chart.on_pointer_event(e);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_point().has_value());
    AURORA_TEST_CHECK_EQ(chart.hovered_point()->second, 1);  // NaN 点被跳过
}

AURORA_TEST_CASE(golden_scatter_chart_matches_baseline) {
    ScatterSeries a{};
    a.name = "A";
    a.points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 2.5, .y = 4.0}, ChartPoint{.x = 4.0, .y = 3.0},
                ChartPoint{.x = 5.5, .y = 6.0}};
    ScatterSeries b{};
    b.name = "B";
    b.points = {ChartPoint{.x = 1.5, .y = 1.0}, ChartPoint{.x = 3.0, .y = 2.5}, ChartPoint{.x = 5.0, .y = 4.5}};
    ScatterChartProps p{};
    p.series = {a, b};
    p.axis_x = ChartAxisSpec{.label = "x", .tick_count = 5};
    p.axis_y = ChartAxisSpec{.label = "y", .tick_count = 5};
    compare_or_update_golden(render_chart(p), "chart_scatter");
}

}  // namespace aurora::test_cases::utest_scatter_chart

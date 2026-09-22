/// 测试类型: unit
/// 目标单元: include/aurora/widget/chart_common.h
/// 测试说明: 覆盖图表公共纯值数据层——LinearScale（D3 nice 步长、to_px/invert 往返、退化域兜底）、
/// BandScale（带中心/带宽/命中夹取/空带）、内置色板取模与 resolve_series_color 三级优先
/// （显式 color > Theme 令牌 chart.palette.N > 内置色板）、以及进序列化面的数组编解码
/// （vector<double> / ChartSeries / ScatterSeries / PieSection / 轴与图例规格）往返与畸形输入兜底

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_chart_common {

using aurora::testing::require_value;

namespace {

[[nodiscard]] auto is_near(double a, double b, double eps = 1e-9) -> bool { return std::abs(a - b) <= eps; }

}  // namespace

AURORA_TEST_CASE(linear_scale_nice_domain_and_step) {
    const auto s = LinearScale::from_domain(0.0, 100.0, 5);
    const auto [d0, d1] = s.domain();
    AURORA_TEST_CHECK_TRUE(is_near(d0, 0.0));
    AURORA_TEST_CHECK_TRUE(is_near(d1, 100.0));
    AURORA_TEST_CHECK_TRUE(is_near(s.step(), 20.0));  // D3 tickIncrement：0..100 分 5 档 → 20
}

AURORA_TEST_CASE(linear_scale_ticks_cover_domain) {
    const auto s = LinearScale::from_domain(0.0, 100.0, 5);
    const std::vector<double> t = s.ticks();
    AURORA_TEST_REQUIRE_EQ(t.size(), 6U);
    AURORA_TEST_CHECK_TRUE(is_near(t.front(), 0.0));
    AURORA_TEST_CHECK_TRUE(is_near(t.back(), 100.0));
    AURORA_TEST_CHECK_TRUE(is_near(t[1], 20.0));
}

AURORA_TEST_CASE(linear_scale_to_px_and_invert_roundtrip) {
    const auto s = LinearScale::from_domain(0.0, 100.0, 5);
    // x 轴（左 → 右）
    AURORA_TEST_CHECK_TRUE(is_near(s.to_px(50.0, 0.0F, 200.0F), 100.0, 1e-3));
    AURORA_TEST_CHECK_TRUE(is_near(s.invert(100.0F, 0.0F, 200.0F), 50.0, 1e-6));
    // y 轴（下 → 上，px1 < px0）：映射与反查仍须同源互逆
    AURORA_TEST_CHECK_TRUE(is_near(s.to_px(50.0, 200.0F, 0.0F), 100.0, 1e-3));
    AURORA_TEST_CHECK_TRUE(is_near(s.invert(100.0F, 200.0F, 0.0F), 50.0, 1e-6));
}

AURORA_TEST_CASE(linear_scale_degenerate_domain_falls_back) {
    // D15：range == 0 不得产生除零 / NaN；全 0 退化到 [0,1]，非零退化到 [v, v+1]。
    const auto zero = LinearScale::from_domain(0.0, 0.0, 5);
    AURORA_TEST_CHECK_TRUE(is_near(zero.domain().first, 0.0));
    AURORA_TEST_CHECK_TRUE(is_near(zero.domain().second, 1.0));

    const auto flat = LinearScale::from_domain(5.0, 5.0, 5);
    AURORA_TEST_CHECK_TRUE(is_near(flat.domain().first, 5.0));
    AURORA_TEST_CHECK_TRUE(is_near(flat.domain().second, 6.0));
    AURORA_TEST_CHECK_TRUE(std::isfinite(flat.to_px(5.0, 0.0F, 100.0F)));

    const auto nan_domain = LinearScale::from_domain(NAN, NAN, 5);
    AURORA_TEST_CHECK_TRUE(std::isfinite(nan_domain.step()));
}

AURORA_TEST_CASE(linear_scale_explicit_bounds_keep_domain) {
    const auto s = LinearScale::from_explicit(-10.0, 10.0, 5);
    AURORA_TEST_CHECK_TRUE(is_near(s.domain().first, -10.0));
    AURORA_TEST_CHECK_TRUE(is_near(s.domain().second, 10.0));
    // 非法显式域（上界 ≤ 下界）回退到 from_domain 的 nice 化路径
    const auto bad = LinearScale::from_explicit(10.0, 10.0, 5);
    AURORA_TEST_CHECK_TRUE(std::isfinite(bad.step()));
}

AURORA_TEST_CASE(band_scale_geometry_and_hit_clamp) {
    const BandScale b{4};
    AURORA_TEST_CHECK_EQ(b.count(), 4U);
    AURORA_TEST_CHECK_TRUE(is_near(b.band_width(0.0F, 400.0F), 100.0, 1e-3));
    AURORA_TEST_CHECK_TRUE(is_near(b.band_center_px(0U, 0.0F, 400.0F), 50.0, 1e-3));
    AURORA_TEST_CHECK_TRUE(is_near(b.band_center_px(3U, 0.0F, 400.0F), 350.0, 1e-3));

    AURORA_TEST_CHECK_EQ(b.index_at(50.0F, 0.0F, 400.0F), 0U);
    AURORA_TEST_CHECK_EQ(b.index_at(399.0F, 0.0F, 400.0F), 3U);
    // 越界夹取（不得返回 n 造成越界下标）
    AURORA_TEST_CHECK_EQ(b.index_at(-100.0F, 0.0F, 400.0F), 0U);
    AURORA_TEST_CHECK_EQ(b.index_at(9999.0F, 0.0F, 400.0F), 3U);
    AURORA_TEST_CHECK_EQ(b.band_center_px(99U, 0.0F, 400.0F), 350.0F);
}

AURORA_TEST_CASE(band_scale_empty_is_safe) {
    const BandScale b{0};
    AURORA_TEST_CHECK_EQ(b.count(), 0U);
    AURORA_TEST_CHECK_TRUE(is_near(b.band_width(0.0F, 400.0F), 0.0, 1e-6));
    AURORA_TEST_CHECK_TRUE(is_near(b.band_center_px(0U, 0.0F, 400.0F), 0.0, 1e-6));
    AURORA_TEST_CHECK_EQ(b.index_at(123.0F, 0.0F, 400.0F), 0U);
}

AURORA_TEST_CASE(palette_wraps_and_resolve_prefers_explicit) {
    AURORA_TEST_CHECK_TRUE(!(chart_palette(0) == chart_palette(1)));
    AURORA_TEST_CHECK_TRUE(chart_palette(8) == chart_palette(0));  // 超界取模

    const Color custom{1, 2, 3, 255};
    const Theme theme{};
    AURORA_TEST_CHECK_TRUE(resolve_series_color(2, custom, theme) == custom);
    AURORA_TEST_CHECK_TRUE(resolve_series_color(2, std::nullopt, theme) == chart_palette(2));

    Theme themed{};
    themed.set_token("chart.palette.2", TokenValue{Color{9, 9, 9, 255}});
    AURORA_TEST_CHECK_TRUE(resolve_series_color(2, std::nullopt, themed) == Color{9, 9, 9, 255});
    AURORA_TEST_CHECK_TRUE(resolve_series_color(2, custom, themed) == custom);  // 显式仍优先
}

AURORA_TEST_CASE(json_roundtrip_double_vector) {
    const std::vector<double> src{1.0, -2.5, 0.0, 42.0};
    const Json j = double_vector_to_json(src);
    AURORA_TEST_REQUIRE_TRUE(j.is_array());
    AURORA_TEST_CHECK_EQ(j.size(), 4U);
    AURORA_TEST_CHECK_TRUE(json_to_double_vector(j) == src);
    // NaN/inf 落盘为 0（JSON 无该字面量，且避免下游除零）
    AURORA_TEST_CHECK_TRUE(is_near(double_vector_to_json({NAN})[0].get<double>(), 0.0));
}

AURORA_TEST_CASE(json_roundtrip_series_array) {
    std::vector<ChartSeries> src{
        ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}, .color = Color{10, 20, 30, 255}},
        ChartSeries{.name = "B", .values = {4.0}, .color = std::nullopt},
    };
    const Json j = chart_series_vector_to_json(src);
    AURORA_TEST_REQUIRE_TRUE(j.is_array());
    AURORA_TEST_CHECK_EQ(j.size(), 2U);
    // 未设色的系列不输出 color 键（保留「按索引取色板」语义）
    AURORA_TEST_CHECK_FALSE(j[1].contains("color"));

    const std::vector<ChartSeries> back = json_to_chart_series_vector(j);
    AURORA_TEST_REQUIRE_EQ(back.size(), 2U);
    AURORA_TEST_CHECK_TRUE(back[0].name == "A");
    AURORA_TEST_CHECK_TRUE(back[0].values == src[0].values);
    AURORA_TEST_REQUIRE_TRUE(back[0].color.has_value());
    AURORA_TEST_CHECK_TRUE(require_value(back[0].color) == Color{10, 20, 30, 255});
    AURORA_TEST_CHECK_FALSE(back[1].color.has_value());
}

AURORA_TEST_CASE(json_roundtrip_scatter_and_pie) {
    ScatterSeries ss{};
    ss.name = "P";
    ss.points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 3.0, .y = -4.0}};
    ss.color = Color{5, 6, 7, 255};
    const ScatterSeries ss_back = json_to_scatter_series(scatter_series_to_json(ss));
    AURORA_TEST_CHECK_TRUE(ss_back.name == "P");
    AURORA_TEST_REQUIRE_EQ(ss_back.points.size(), 2U);
    AURORA_TEST_CHECK_TRUE(is_near(ss_back.points[1].y, -4.0));

    std::vector<PieSection> pie{PieSection{.name = "x", .value = 3.0}, PieSection{.name = "y", .value = 1.0}};
    const std::vector<PieSection> pie_back = json_to_pie_section_vector(pie_section_vector_to_json(pie));
    AURORA_TEST_REQUIRE_EQ(pie_back.size(), 2U);
    AURORA_TEST_CHECK_TRUE(is_near(pie_back[0].value, 3.0));

    const std::vector<double> ratios = pie_section_ratios(pie);
    AURORA_TEST_REQUIRE_EQ(ratios.size(), 2U);
    AURORA_TEST_CHECK_TRUE(is_near(ratios[0], 0.75));
    AURORA_TEST_CHECK_TRUE(is_near(ratios[1], 0.25));
    // Σ ≤ 0：全 0（由调用方按 D15 降级，不产生 NaN）
    const std::vector<double> empty = pie_section_ratios({PieSection{.name = "z", .value = 0.0}});
    AURORA_TEST_REQUIRE_EQ(empty.size(), 1U);
    AURORA_TEST_CHECK_TRUE(is_near(empty[0], 0.0));
}

AURORA_TEST_CASE(json_roundtrip_axis_and_legend) {
    ChartAxisSpec axis{};
    axis.label = "Y";
    axis.tick_count = 4;
    axis.min = -1.0;
    axis.show_grid_lines = false;
    const ChartAxisSpec axis_back = json_to_chart_axis_spec(chart_axis_spec_to_json(axis));
    AURORA_TEST_CHECK_TRUE(axis_back.label == "Y");
    AURORA_TEST_CHECK_EQ(axis_back.tick_count, 4);
    AURORA_TEST_REQUIRE_TRUE(axis_back.min.has_value());
    AURORA_TEST_CHECK_TRUE(is_near(require_value(axis_back.min), -1.0));
    AURORA_TEST_CHECK_FALSE(axis_back.max.has_value());
    AURORA_TEST_CHECK_FALSE(axis_back.show_grid_lines);

    ChartLegendSpec legend{};
    legend.position = LegendPosition::Right;
    const ChartLegendSpec legend_back = json_to_chart_legend_spec(chart_legend_spec_to_json(legend));
    AURORA_TEST_CHECK_TRUE(legend_back.position == LegendPosition::Right);
    AURORA_TEST_CHECK_TRUE(legend_position_to_json(LegendPosition::Bottom).get<std::string>() == "Bottom");
}

AURORA_TEST_CASE(json_malformed_input_degrades_without_throwing) {
    // Inspector PUT / JSON 文件加载是不可信通道：畸形元素一律跳过，绝不抛异常。
    AURORA_TEST_CHECK_TRUE(json_to_double_vector(Json::object()).empty());
    AURORA_TEST_CHECK_TRUE(json_to_double_vector(Json{1.0, std::string{"x"}, 3.0}).size() == 2U);
    AURORA_TEST_CHECK_TRUE(json_to_chart_series_vector(Json{1, 2, 3}).empty());
    AURORA_TEST_CHECK_TRUE(json_to_chart_series(Json{Json::array()}).values.empty());
    AURORA_TEST_CHECK_TRUE(json_to_scatter_series(Json::object()).points.empty());
    // 对象元素保留、非对象元素跳过（不是整包丢弃）
    AURORA_TEST_CHECK_EQ(json_to_pie_section_vector(Json{Json::object(), 5}).size(), 1U);
    AURORA_TEST_CHECK_TRUE(json_to_chart_axis_spec(Json{Json::array()}).tick_count == 5);
    AURORA_TEST_CHECK_TRUE(json_to_chart_legend_spec(Json{Json::array()}).visible);
    AURORA_TEST_CHECK_TRUE(json_to_legend_position(Json{"nope"}) == LegendPosition::Top);
    AURORA_TEST_CHECK_TRUE(json_to_string_vector(Json::object()).empty());
}

}  // namespace aurora::test_cases::utest_chart_common

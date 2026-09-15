/// 测试类型: unit
/// 目标单元: src/aurora/widget/serialization.cpp
/// 测试说明: WidgetRegistry 工厂注册——Skeleton 属性往返（从静态 JSON 完整重建）、
///           GridView 已知类型注册（运行时 ItemBuilder 占位重建 + 标量属性语义说明）、
///           to_json/from_json 基本往返与未知类型拒绝

#include <memory>
#include <string>
#include <vector>

#include "aurora/widget/bar_chart.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/serialization.h"
#include "aurora/widget/skeleton.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_serialization {

using serialization::from_json;
using serialization::to_json;

AURORA_TEST_CASE(skeleton_factory_rebuilds_props_from_json) {
    // Skeleton 接入序列化工厂——自定义属性经 to_json → from_json 完整往返。
    auto src = std::make_shared<Skeleton>();
    src->set_size(Size{.width = 120.0F, .height = 24.0F})
        .set_color(Color{10, 20, 30, 255})
        .set_highlight(Color{250, 250, 250, 200})
        .set_duration(2.5);

    const Json j = to_json(*src);
    const auto rebuilt = from_json(j);
    AURORA_TEST_REQUIRE_MSG(rebuilt.ok(), "Skeleton from_json succeeds");
    AURORA_TEST_CHECK_EQ(rebuilt.value()->type_name(), std::string{"Skeleton"});

    const auto *sk = dynamic_cast<const Skeleton *>(rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(sk != nullptr, "rebuilt widget is a Skeleton");
    AURORA_TEST_CHECK_NEAR(sk->size_hint().width, 120.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sk->size_hint().height, 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(sk->phase(), 0.0, 1e-9);  // 新实例相位从零开始（运行时态不序列化）
}

AURORA_TEST_CASE(gridview_registered_as_known_type_with_placeholder_rebuild) {
    // GridView 注册为已知类型——条目持运行时 ItemBuilder 不可从静态 JSON 重建，
    // 工厂重建出「空数据占位实例」（同 LazyList 先例），标量属性由宿主回填。
    Json j;
    j["type"] = "GridView";
    j["props"] = Json::object();
    const auto w = from_json(j);
    AURORA_TEST_REQUIRE_MSG(w.ok(), "GridView from_json succeeds (known type)");
    AURORA_TEST_CHECK_EQ(w.value()->type_name(), std::string{"GridView"});
    const auto *gv = dynamic_cast<const GridView *>(w.value().get());
    AURORA_TEST_REQUIRE_MSG(gv != nullptr, "rebuilt widget is a GridView");
    AURORA_TEST_CHECK_EQ(gv->count(), 0);  // 默认占位：无数据、无条目
    AURORA_TEST_CHECK_EQ(gv->live_item_count(), static_cast<std::size_t>(0));
}

AURORA_TEST_CASE(unknown_type_still_rejected) {
    Json j;
    j["type"] = "NoSuchWidget";
    j["props"] = Json::object();
    const auto w = from_json(j);
    AURORA_TEST_REQUIRE_MSG(!w.ok(), "unknown type rejected");
    AURORA_TEST_CHECK_EQ(w.error().code_enum, ErrorCode::WidgetUnknownType);
}

AURORA_TEST_CASE(barchart_rebuilds_nested_series_array) {
    // 图表数据进序列化面（D5）：series 是对象数组（首个「数组属性」先例），
    // 嵌套的 name / values / color 必须逐字段往返，且未设色的系列不输出 color 键。
    auto src = std::make_shared<BarChart>(BarChartProps{
        .series = {ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}, .color = Color{1, 2, 3, 255}},
                   ChartSeries{.name = "B", .values = {4.0}}},
        .categories = {"Mon", "Tue", "Wed"},
        .stacked = true,
        .legend = ChartLegendSpec{.visible = true, .position = LegendPosition::Right},
    });

    const Json j = to_json(*src);
    AURORA_TEST_REQUIRE_TRUE(j.contains("props"));
    AURORA_TEST_REQUIRE_TRUE(j["props"].contains("series"));
    AURORA_TEST_CHECK_EQ(j["props"]["series"].size(), 2U);
    AURORA_TEST_CHECK_FALSE(j["props"]["series"][1].contains("color"));

    const auto rebuilt = from_json(j);
    AURORA_TEST_REQUIRE_MSG(rebuilt.ok(), "BarChart from_json succeeds");
    const auto *chart = dynamic_cast<const BarChart *>(rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(chart != nullptr, "rebuilt widget is a BarChart");
    AURORA_TEST_CHECK_EQ(chart->series.size(), 2U);
    AURORA_TEST_CHECK_TRUE(chart->series[1].name == "B");
    AURORA_TEST_CHECK_TRUE(chart->series[0].values == std::vector<double>{1.0, 2.0, 3.0});
    AURORA_TEST_CHECK_TRUE(chart->categories == std::vector<std::string>{"Mon", "Tue", "Wed"});
    AURORA_TEST_CHECK_TRUE(chart->stacked);
    AURORA_TEST_CHECK_TRUE(chart->legend.position == LegendPosition::Right);
}

}  // namespace aurora::test_cases::utest_serialization

/// 测试类型: unit
/// 目标单元: src/aurora/widget/serialization.cpp
/// 测试说明: WidgetRegistry 工厂注册——Skeleton 属性往返（从静态 JSON 完整重建）、
///           虚拟化/回调控件（GridView / LazyList / BottomNavBar）登记即回填标量属性、
///           重建后经 set_item_builder 挂条目即出内容、
///           to_json/from_json 基本往返与未知类型拒绝

#include <memory>
#include <string>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/bar_chart.h"
#include "aurora/widget/bottom_nav_bar.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/lazy_list.h"
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
    // GridView 注册为已知类型——条目持运行时 ItemBuilder 不可从静态 JSON 重建，故空 props 的
    // 节点重建出「默认几何、无条目」的占位实例（标量属性齐备时由工厂回填，见下一用例）。
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

AURORA_TEST_CASE(virtualized_widgets_rebuild_with_their_props) {
    // 工厂登记的虚拟化 / 运行时回调控件不再是「登记类型但丢属性」：标量属性必须随 from_json 回来，
    // 条目则由宿主经 set_item_builder 挂上（builder 是回调，不可序列化）。
    auto grid = std::make_shared<GridView>(30, 3, nullptr, 96.0F);
    grid->set_scroll_offset(96.0F);
    const auto grid_rebuilt = from_json(to_json(*grid));
    AURORA_TEST_REQUIRE_MSG(grid_rebuilt.ok(), "GridView from_json succeeds");
    auto *gv = dynamic_cast<GridView *>(grid_rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(gv != nullptr, "rebuilt widget is a GridView");
    AURORA_TEST_CHECK_EQ(gv->count(), 30);
    AURORA_TEST_CHECK_EQ(gv->columns(), 3);
    AURORA_TEST_CHECK_NEAR(gv->cell_extent(), 96.0F, 1e-4F);
    // 偏移须等首次可滚动布局才落位（内容/视口已知才能夹取）——同 `Scroll` / `LazyRow` 的 pending 语义。
    AURORA_TEST_CHECK_NEAR(gv->scroll_offset(), 0.0F, 1e-4F);
    LayoutEngine::layout(
        *gv, Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 300.0F, .height = 300.0F}});
    AURORA_TEST_CHECK_NEAR(gv->scroll_offset(), 96.0F, 1e-3F);

    auto list = std::make_shared<LazyList>(7, nullptr, 24.0F);
    list->set_cache_extent(0.0F);
    const auto list_rebuilt = from_json(to_json(*list));
    AURORA_TEST_REQUIRE_MSG(list_rebuilt.ok(), "LazyList from_json succeeds");
    const auto *ll = dynamic_cast<const LazyList *>(list_rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(ll != nullptr, "rebuilt widget is a LazyList");
    AURORA_TEST_CHECK_EQ(ll->count(), 7);
    AURORA_TEST_CHECK_NEAR(ll->content_height(), 168.0F, 1e-3F);  // 7 × 24：行高随之还原

    BottomNavBar bar;
    bar.selected_index = 2;
    bar.bar_height = 80.0F;
    const auto bar_rebuilt = from_json(to_json(bar));
    AURORA_TEST_REQUIRE_MSG(bar_rebuilt.ok(), "BottomNavBar from_json succeeds");
    const auto *bnb = dynamic_cast<const BottomNavBar *>(bar_rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(bnb != nullptr, "rebuilt widget is a BottomNavBar");
    AURORA_TEST_CHECK_EQ(bnb->selected_index, 2);
    AURORA_TEST_CHECK_NEAR(bnb->bar_height, 80.0F, 1e-4F);
}

AURORA_TEST_CASE(rebuilt_grid_renders_after_host_attaches_builder) {
    // 「属性齐备、条目待挂」不是一句空话：set_item_builder 即那句「由宿主回填」的落点。
    Json j;
    j["type"] = "GridView";
    j["props"] = Json{{"count", 12}, {"columns", 4}, {"cell_extent", 40.0F}, {"cache_extent", 0.0F}};
    const auto rebuilt = from_json(j);
    AURORA_TEST_REQUIRE_MSG(rebuilt.ok(), "GridView from_json succeeds");
    auto *gv = dynamic_cast<GridView *>(rebuilt.value().get());
    AURORA_TEST_REQUIRE_MSG(gv != nullptr, "rebuilt widget is a GridView");
    AURORA_TEST_CHECK_EQ(gv->live_item_count(), static_cast<std::size_t>(0));  // 挂前无条目

    gv->set_item_builder(
        [](int /*index*/) -> Node { return Node{std::make_shared<Skeleton>(Size{.width = 30.0F, .height = 30.0F})}; });
    Constraints viewport{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 40.0F}};
    LayoutEngine::layout(*gv, viewport);
    AURORA_TEST_CHECK_EQ(gv->live_item_count(), static_cast<std::size_t>(4));  // 首行 4 格
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

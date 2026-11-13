/// 测试类型: unit
/// 目标单元: include/aurora/widget/bar_chart.h
/// 测试说明: 覆盖 BarChart 全链路——构造不变量与 defaults、describe_static 属性完备、
/// 序列化往返（含 series 对象数组 / axis / legend / padding）、工厂 from_json 重建、
/// hover 命中几何（纯计算，同源 BandScale 反查）、on_point_tapped 触发、空数据与畸形输入降级，
/// 以及像素 golden 基线（chart_bar.png，受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）。
/// 属性单一声明迁移后另覆盖权威表十行的读消费面：逐行撤表态该键退出序列化、显式空序列与未表态
/// 同值不同意、结构体行按整值表态而非字段合并、十行皆常量档故两档主题下与库内解析全同、
/// 全量键 JSON 逐行复原生效值并落同一帧像素；本型与折线族的口径分叉在两处——两枚数值行登记了
/// 上下界且 `float` 校验随描述符约束生效，故 JSON 越界值**不写入并撤表态**（迁移前是夹取写入），
/// 而结构化行仍走 `chart_common.h` 登记的宽容通路（逐项回退默认、照常表态、不产生降级上报）

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/diagnostics.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_bar_chart {

using aurora::testing::require_child;
using aurora::testing::require_field;
using aurora::testing::require_value;

namespace golden = aurora::testing::golden;

namespace {

constexpr float AURORA_WIDTH = 320.0F;
constexpr float AURORA_HEIGHT = 200.0F;

constexpr int AURORA_PIX_W = 160;
constexpr int AURORA_PIX_H = 100;

/// 权威表十行的键名（声明顺序 = schema 顺序）。
constexpr const char *AURORA_ALL_KEYS[10] = {
    "series",         "categories", "stacked", "bar_width_ratio", "bar_corner_radius",
    "show_crosshair", "axis_x",     "axis_y",  "legend",          "padding"};

/// @brief 关掉轴与图例、清零留白：绘图区 = 整个控件，命中几何可精确预期。
[[nodiscard]] auto bare_props() -> BarChartProps {
    return BarChartProps()
        .set_axis_x(ChartAxisSpec{.visible = false})
        .set_axis_y(ChartAxisSpec{.visible = false})
        .set_legend(ChartLegendSpec{.visible = false})
        .set_padding(EdgeInsets{});
}

/// @brief 布局一帧（无头，无 Application）。
auto layout_only(Widget &w) -> void {
    constexpr BuildContext ctx;
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = AURORA_WIDTH, .height = AURORA_HEIGHT};
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

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
[[nodiscard]] auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class BarChartProbe final : public BarChart {
  public:
    using BarChart::present;
    using BarChart::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct BarChartDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(BarChart &target) -> void {
        paints = 0;
        layouts = 0;
        target.on_dirty = [this](bool layout) {
            if (layout) {
                ++layouts;
            } else {
                ++paints;
            }
        };
    }
};

[[nodiscard]] auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 直绘一遍（非录制）并把整块画布读回，供逐像素比对。
[[nodiscard]] auto render_to_pixels(Widget &w, const Rect &box, const BuildContext &ctx, int cw, int ch)
    -> std::vector<Color> {
    Painter p;
    p.begin(cw, ch);
    w.paint(p, box, ctx);
    std::vector<Color> px;
    px.reserve(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch));
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            px.push_back(p.get_pixel(x, y));
        }
    }
    return px;
}

}  // namespace

AURORA_TEST_CASE(defaults_match_declared_property_defaults) {
    const BarChartProps d = BarChart::defaults();
    // 十行皆未表态 ⇒ 生效值逐行等于该行回落基线（与迁移前字段声明处默认值逐值相同）。
    AURORA_TEST_CHECK_TRUE(d.series().empty());
    AURORA_TEST_CHECK_TRUE(d.categories().empty());
    AURORA_TEST_CHECK_FALSE(d.stacked());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.bar_width_ratio()), 0.7, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.bar_corner_radius()), 2.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());
    AURORA_TEST_CHECK_TRUE(d.axis_x().visible);
    AURORA_TEST_CHECK_TRUE(d.axis_y().include_zero);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding().left), 8.0, 1e-6);

    BarChart const c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"BarChart"});
    AURORA_TEST_CHECK_TRUE(c.hovered_point() == std::nullopt);
    AURORA_TEST_CHECK_TRUE(c.wants_click());

    // 未表态 ⇒ 一律不写键（回落语义不外泄）；控件级 width/height/show 由基类写。
    Json props = Json::object();
    c.serialize_props(props);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(props.contains(key));
    }
}

AURORA_TEST_CASE(describe_static_lists_all_props_and_events) {
    const WidgetDescriptor d = BarChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "BarChart");
    AURORA_TEST_CHECK_TRUE(d.children_policy == "none");
    AURORA_TEST_CHECK_EQ(d.properties.size(), 10U);

    auto find = [&d](const std::string &key) -> const PropDescriptor * {
        const auto it = std::ranges::find_if(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
        return it == d.properties.end() ? nullptr : &*it;
    };
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(find(key) != nullptr);
    }
    // 结构化行的类型名与 JSON 类型由 `chart_common.h` 的编解码登记给出（迁移前那三行手写成 "Json"）。
    const PropDescriptor *series = find("series");
    AURORA_TEST_REQUIRE_NOT_NULL(series);
    AURORA_TEST_CHECK_TRUE(series->type == "vector<ChartSeries>");
    AURORA_TEST_CHECK_TRUE(series->json_type == "array");
    const PropDescriptor *axis_y = find("axis_y");
    AURORA_TEST_REQUIRE_NOT_NULL(axis_y);
    AURORA_TEST_CHECK_TRUE(axis_y->type == "ChartAxisSpec");
    AURORA_TEST_CHECK_TRUE(axis_y->json_type == "object");
    const PropDescriptor *leg = find("legend");
    AURORA_TEST_REQUIRE_NOT_NULL(leg);
    AURORA_TEST_CHECK_TRUE(leg->type == "ChartLegendSpec");
    // 两枚数值行的约束随表行走：柱宽占比是「占比」故上下界齐登，圆角只登记下界。
    const PropDescriptor *ratio = find("bar_width_ratio");
    AURORA_TEST_REQUIRE_NOT_NULL(ratio);
    AURORA_TEST_CHECK_TRUE(ratio->min_value == "0.05");
    AURORA_TEST_CHECK_TRUE(ratio->max_value == "1");
    const PropDescriptor *corner = find("bar_corner_radius");
    AURORA_TEST_REQUIRE_NOT_NULL(corner);
    AURORA_TEST_CHECK_TRUE(corner->min_value == "0");
    AURORA_TEST_CHECK_TRUE(corner->max_value.empty());

    bool has_event = false;
    for (const std::string &e : d.events) {
        if (e == "on_point_tapped") {
            has_event = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_event);
}

AURORA_TEST_CASE(props_roundtrip_including_series_array) {
    BarChart const src{
        BarChartProps()
            .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}, .color = Color{10, 20, 30, 255}},
                         ChartSeries{.name = "B", .values = {4.0, 5.0}}})
            .set_categories({"Mon", "Tue", "Wed"})
            .set_stacked(true)
            .set_bar_width_ratio(0.5F)
            .set_bar_corner_radius(4.0F)
            .set_axis_y(ChartAxisSpec{.label = "k", .tick_count = 4, .min = 0.0, .show_grid_lines = false})
            .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})
            .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F})};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_REQUIRE_TRUE(props.contains("series"));
    const auto *const series = require_child(props, "series");
    AURORA_TEST_REQUIRE_TRUE(series->is_array());
    AURORA_TEST_CHECK_EQ(series->size(), 2U);

    BarChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.series().size(), 2U);
    AURORA_TEST_CHECK_TRUE(dst.series()[0].name == "A");
    AURORA_TEST_CHECK_TRUE(dst.series()[0].values == std::vector<double>{1.0, 2.0, 3.0});
    AURORA_TEST_REQUIRE_TRUE(dst.series()[0].color.has_value());
    AURORA_TEST_CHECK_TRUE(require_value(dst.series()[0].color) == Color{10, 20, 30, 255});
    AURORA_TEST_CHECK_FALSE(dst.series()[1].color.has_value());
    AURORA_TEST_CHECK_TRUE(dst.categories() == std::vector<std::string>{"Mon", "Tue", "Wed"});
    AURORA_TEST_CHECK_TRUE(dst.stacked());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.bar_width_ratio()), 0.5, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.bar_corner_radius()), 4.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(dst.axis_y().label == "k");
    AURORA_TEST_CHECK_EQ(dst.axis_y().tick_count, 4);
    AURORA_TEST_CHECK_FALSE(dst.axis_y().show_grid_lines);
    AURORA_TEST_CHECK_TRUE(dst.legend().position == LegendPosition::Right);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.padding().bottom), 5.0, 1e-6);
}

AURORA_TEST_CASE(factory_rebuilds_from_json) {
    serialization::register_core_widgets();
    Json props = Json::object();
    props.set("series", chart_series_vector_to_json({ChartSeries{.name = "S", .values = {2.0, 4.0, 6.0}}}));
    Json node = Json::object();
    node.set("type", "BarChart");
    node.set("props", props);

    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_CHECK_TRUE(built.value()->type_name() == std::string{"BarChart"});
    const auto *chart = dynamic_cast<const BarChart *>(built.value().get());
    AURORA_TEST_REQUIRE_NOT_NULL(chart);
    AURORA_TEST_CHECK_EQ(chart->series().size(), 1U);
    AURORA_TEST_CHECK_TRUE(chart->series()[0].values == std::vector<double>{2.0, 4.0, 6.0});
}

AURORA_TEST_CASE(hover_maps_local_position_to_category) {
    // 4 个类目、无留白 → 绘图区 = 320×200，带中心分别在 40 / 120 / 200 / 280。
    BarChart chart{bare_props().set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0, 4.0}}})};
    layout_only(chart);

    MouseEvent e1 = move_at(120.0F, 100.0F);
    chart.on_pointer_event(e1);
    const auto hit = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(hit.second, 1);
    AURORA_TEST_CHECK_EQ(hit.first, 0);

    MouseEvent e2 = move_at(280.0F, 100.0F);
    chart.on_pointer_event(e2);
    const auto hit2 = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(hit2.second, 3);

    // 移出绘图区（负坐标）→ 高亮清除
    MouseEvent e3 = move_at(-10.0F, 100.0F);
    chart.on_pointer_event(e3);
    AURORA_TEST_CHECK_TRUE(chart.hovered_point() == std::nullopt);
}

AURORA_TEST_CASE(hover_splits_grouped_series_by_bar_width) {
    // 两系列分组：band=80、group=56、bar=28 → 带 1 的组内 [92,120) 属系列 0，[120,148) 属系列 1。
    BarChart chart{bare_props().set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0, 4.0}},
                                            ChartSeries{.name = "B", .values = {4.0, 3.0, 2.0, 1.0}}})};
    layout_only(chart);

    MouseEvent in_first = move_at(100.0F, 100.0F);
    chart.on_pointer_event(in_first);
    const auto hit_first = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(hit_first.first, 0);

    MouseEvent in_second = move_at(130.0F, 100.0F);
    chart.on_pointer_event(in_second);
    const auto hit_second = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(hit_second.first, 1);
}

AURORA_TEST_CASE(release_fires_on_point_tapped) {
    BarChart chart{bare_props().set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0, 4.0}}})};
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
    BarChart chart{bare_props().set_series({ChartSeries{.name = "A", .values = {NAN, INFINITY}}})};
    layout_only(chart);
    MouseEvent e = move_at(40.0F, 100.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(golden_bar_chart_matches_baseline) {
    auto chart = std::make_shared<BarChart>(
        BarChartProps()
            .set_series({ChartSeries{.name = "2024", .values = {12.0, 18.0, 9.0, 24.0, 15.0}},
                         ChartSeries{.name = "2025", .values = {16.0, 14.0, 20.0, 18.0, 22.0}}})
            .set_categories({"Mon", "Tue", "Wed", "Thu", "Fri"})
            .set_axis_y(ChartAxisSpec{.label = "sales", .tick_count = 5}));
    chart->set_modifier(Modifier{}.width(320.0F).height(200.0F));

    Node root{Column{Node{chart}}};
    const std::filesystem::path current_path =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_bar.png";

    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, static_cast<int>(AURORA_WIDTH), static_cast<int>(AURORA_HEIGHT),
                                           current_path.string().c_str())
                                 .ok());

    // 传入 root 以启用归因：失败时报告会说清差异落在哪个控件的盒子里。
    golden::compare_or_update("chart_bar", current_path, &root);
}

// ===========================================================================
// 权威表的读消费面 + 结构体 / 结构体数组行与两枚带界数值行的形态（属性单一声明迁移批）
// ===========================================================================

AURORA_TEST_CASE(barchart_clear_unstates_each_row_and_drops_every_key) {
    // 十行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且生效值回到该行回落基线」。
    BarChartProbe d;
    d.BarChartProps::set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}}})
        .set_categories({"a", "b"})
        .set_stacked(true)
        .set_bar_width_ratio(0.4F)
        .set_bar_corner_radius(6.0F)
        .set_show_crosshair(false)
        .set_axis_x(ChartAxisSpec{.visible = false, .tick_count = 3})
        .set_axis_y(ChartAxisSpec{.label = "y", .tick_count = 3, .min = 0.0, .max = 9.0})
        .set_legend(ChartLegendSpec{.visible = false, .position = LegendPosition::Right})
        .set_padding(EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F});
    Json stated = Json::object();
    d.serialize_props(stated);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 十行全表态 ⇒ 十键逐键在盘
    }

    d.clear(BarChartProps::series_row());
    AURORA_TEST_CHECK_FALSE(d.present(BarChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(d.series().empty());

    d.clear(BarChartProps::categories_row());
    AURORA_TEST_CHECK_TRUE(d.categories().empty());

    d.clear(BarChartProps::stacked_row());
    d.clear(BarChartProps::show_crosshair_row());
    AURORA_TEST_CHECK_FALSE(d.stacked());  // 回落基线为关
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());  // 回落基线为开

    d.clear(BarChartProps::bar_width_ratio_row());
    d.clear(BarChartProps::bar_corner_radius_row());
    AURORA_TEST_CHECK_NEAR(d.bar_width_ratio(), 0.7F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.bar_corner_radius(), 2.0F, 1e-4F);

    // 结构化行的回落点是各自默认构造，不是「清零」
    d.clear(BarChartProps::axis_x_row());
    d.clear(BarChartProps::axis_y_row());
    AURORA_TEST_CHECK_TRUE(d.axis_x().visible);
    AURORA_TEST_CHECK_EQ(d.axis_y().tick_count, 5);
    AURORA_TEST_CHECK_FALSE(d.axis_y().min.has_value());

    d.clear(BarChartProps::legend_row());
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);

    d.clear(BarChartProps::padding_row());
    AURORA_TEST_CHECK_NEAR(d.padding().left, 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.padding().bottom, 8.0F, 1e-4F);

    Json cleared = Json::object();
    d.serialize_props(cleared);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }
}

AURORA_TEST_CASE(barchart_empty_array_stated_differs_from_unstated) {
    // 两枚数组行的两态语义：`set_series({})` 是「显式要一组没有数据的系列」，与「未表态」取值同、
    // 意图不同——前者写键（盘上是空数组），后者不写键。迁移前两者都是「空 vector」，分不开。
    BarChartProbe stated;
    stated.BarChartProps::set_series({});
    stated.set_categories({});
    AURORA_TEST_CHECK_TRUE(stated.present(BarChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(stated.present(BarChartProps::categories_row()));
    Json with_key = Json::object();
    stated.serialize_props(with_key);
    const Json *arr = require_child(with_key, "series");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_TRUE(arr->is_array());
    AURORA_TEST_CHECK_EQ(arr->size(), 0U);
    const Json *cats = require_child(with_key, "categories");
    AURORA_TEST_REQUIRE_NOT_NULL(cats);
    AURORA_TEST_CHECK_EQ(cats->size(), 0U);
    AURORA_TEST_CHECK_TRUE(stated.series().empty());

    BarChartProbe const bare;
    AURORA_TEST_CHECK_FALSE(bare.present(BarChartProps::series_row()));
    Json without_key = Json::object();
    bare.serialize_props(without_key);
    AURORA_TEST_CHECK_FALSE(without_key.contains("series"));
    AURORA_TEST_CHECK_FALSE(without_key.contains("categories"));
    AURORA_TEST_CHECK_TRUE(bare.series().empty());  // 生效值同（该行回落基线是空序列）

    // 往返不塌态：盘上的空数组读回来仍是「已表态的空序列」。
    BarChartProbe round;
    round.deserialize_props(with_key);
    AURORA_TEST_CHECK_TRUE(round.present(BarChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(round.present(BarChartProps::categories_row()));
    AURORA_TEST_CHECK_TRUE(round.series().empty());
}

AURORA_TEST_CASE(barchart_struct_rows_state_whole_value_not_field_merge) {
    // 结构体行按**整值**表态：写一次是把整只规格装进槽里，不是「只改那一个字段、其余留旧」。
    // 迁移前那是裸字段，`p.axis_x.visible = false` 这类逐字段改写还能成立；现在只能整只装入。
    BarChartProbe d;
    d.set_axis_x(ChartAxisSpec{.visible = false, .label = "x", .tick_count = 7});
    AURORA_TEST_CHECK_TRUE(d.present(BarChartProps::axis_x_row()));
    AURORA_TEST_CHECK_FALSE(d.axis_x().visible);
    AURORA_TEST_CHECK_TRUE(d.axis_x().label == "x");
    AURORA_TEST_CHECK_EQ(d.axis_x().tick_count, 7);
    AURORA_TEST_CHECK_TRUE(d.axis_x().show_grid_lines);  // 未点名的字段取该行回落基线（默认构造）

    d.set_axis_x(ChartAxisSpec{.label = "x2"});  // 整值替换：上一轮的 visible=false 不残留
    AURORA_TEST_CHECK_TRUE(d.axis_x().visible);
    AURORA_TEST_CHECK_EQ(d.axis_x().tick_count, 5);
    AURORA_TEST_CHECK_TRUE(d.axis_x().label == "x2");

    // 同值再写不落账：结构化行的等值短路靠成员比较，故三枚规格都补齐了 `operator==`。
    const BuildContext ctx;
    BarChart chart;
    chart.mount(ctx);
    BarChartDirty dirty;
    dirty.attach_to(chart);
    const ChartLegendSpec leg{.visible = true, .position = LegendPosition::Bottom};
    chart.set_legend(leg);
    const int paints_after = dirty.paints;
    const int layouts_after = dirty.layouts;
    chart.set_legend(leg);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after);
    AURORA_TEST_CHECK_TRUE(chart.legend() == leg);
}

AURORA_TEST_CASE(barchart_constant_rows_ignore_theme_scope) {
    // 十行皆常量档 ⇒ 没有 `x(ctx)` 第二件：无参读件就是生效值，明 / 暗两档主题下与库内
    // `resolve(row, ctx)` 取值全同（缺这组断言则「窄面不丢语义」只是断言）。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    BarChartProbe d;
    d.BarChartProps::set_series({ChartSeries{.name = "A", .values = {1.0, 4.0, 2.0}}})
        .set_categories({"a", "b", "c"})
        .set_bar_width_ratio(0.5F)
        .set_axis_y(ChartAxisSpec{.min = 0.0, .max = 5.0});

    AURORA_TEST_CHECK_TRUE(d.series() == d.resolve(BarChartProps::series_row(), light));
    AURORA_TEST_CHECK_TRUE(d.series() == d.resolve(BarChartProps::series_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.categories() == d.resolve(BarChartProps::categories_row(), light));
    AURORA_TEST_CHECK_TRUE(d.categories() == d.resolve(BarChartProps::categories_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.stacked() == d.resolve(BarChartProps::stacked_row(), light));
    AURORA_TEST_CHECK_TRUE(d.stacked() == d.resolve(BarChartProps::stacked_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.bar_width_ratio(), d.resolve(BarChartProps::bar_width_ratio_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.bar_width_ratio(), d.resolve(BarChartProps::bar_width_ratio_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.bar_corner_radius(), d.resolve(BarChartProps::bar_corner_radius_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.bar_corner_radius(), d.resolve(BarChartProps::bar_corner_radius_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_TRUE(d.show_crosshair() == d.resolve(BarChartProps::show_crosshair_row(), light));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair() == d.resolve(BarChartProps::show_crosshair_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == d.resolve(BarChartProps::axis_x_row(), light));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == d.resolve(BarChartProps::axis_x_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.axis_y() == d.resolve(BarChartProps::axis_y_row(), light));
    AURORA_TEST_CHECK_TRUE(d.axis_y() == d.resolve(BarChartProps::axis_y_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(BarChartProps::legend_row(), light));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(BarChartProps::legend_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.padding().right, d.resolve(BarChartProps::padding_row(), dark).right, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().top, d.resolve(BarChartProps::padding_row(), light).top, 1e-6F);
    // 显式域随整值表态：两档主题下读回来都是那条域，不被作用域改写。
    AURORA_TEST_CHECK_TRUE(d.axis_y().min.has_value());
    AURORA_TEST_CHECK_TRUE(d.axis_y().max.has_value());
    AURORA_TEST_CHECK_NEAR(require_value(d.axis_y().max), 5.0, 1e-6);
}

AURORA_TEST_CASE(barchart_full_key_json_restores_every_effective_value) {
    // 十行全表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态；把它灌回一份空白控件，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    const BuildContext ctx;
    BarChart src{BarChartProps()
                     .set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}},
                                  ChartSeries{.name = "B", .values = {1.0, 3.0, 2.0}}})
                     .set_categories({"x", "y", "z"})
                     .set_stacked(true)
                     .set_bar_width_ratio(0.5F)
                     .set_bar_corner_radius(4.0F)
                     .set_show_crosshair(false)
                     .set_axis_x(ChartAxisSpec{.visible = false, .tick_count = 3})
                     .set_axis_y(ChartAxisSpec{.label = "v", .tick_count = 4, .min = 0.0, .max = 9.0})
                     .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom})
                     .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F})};
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }
    AURORA_TEST_CHECK_NEAR(require_field<float>(legacy, "bar_width_ratio"), 0.5F, 1e-4F);

    BarChartProbe dst;
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::categories_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::stacked_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::bar_width_ratio_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::bar_corner_radius_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::axis_x_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::axis_y_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(BarChartProps::padding_row()));

    AURORA_TEST_CHECK_TRUE(dst.series() == src.series());
    AURORA_TEST_CHECK_TRUE(dst.categories() == src.categories());
    AURORA_TEST_CHECK_TRUE(dst.stacked());
    AURORA_TEST_CHECK_NEAR(dst.bar_width_ratio(), 0.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.bar_corner_radius(), 4.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(dst.show_crosshair());
    AURORA_TEST_CHECK_TRUE(dst.axis_x() == src.axis_x());
    AURORA_TEST_CHECK_TRUE(dst.axis_y() == src.axis_y());
    AURORA_TEST_CHECK_TRUE(dst.legend() == src.legend());
    AURORA_TEST_CHECK_NEAR(dst.padding().left, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.padding().bottom, 5.0F, 1e-4F);

    const auto box_size = bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H));
    LayoutEngine::layout(src, box_size, ctx);
    LayoutEngine::layout(dst, box_size, ctx);
    AURORA_TEST_REQUIRE_NEAR(src.size().width, dst.size().width, 1e-4F);
    AURORA_TEST_REQUIRE_NEAR(src.size().height, dst.size().height, 1e-4F);
    const Rect box{.origin = Point{}, .size = src.size()};
    const auto px_src = render_to_pixels(src, box, ctx, AURORA_PIX_W, AURORA_PIX_H);
    const auto px_dst = render_to_pixels(dst, box, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_EQ(px_src.size(), px_dst.size());
    AURORA_TEST_CHECK_TRUE(px_src == px_dst);
}

AURORA_TEST_CASE(barchart_invalid_json_rejects_checked_rows_while_struct_rows_stay_lenient) {
    // 已登记校验的行：两枚数值行的 `float` 校验按描述符上下界判定，越界值一律不写入、撤表态、
    // 生效值退回该行回落基线，且降级经 `Diagnostics` 上报 —— 迁移前这里是「夹取写入」，本轮改成拒绝。
    // 结构化行（series / categories / axis_x / axis_y / legend）相反：`chart_common.h` 的登记刻意不带
    // `validate`，按既有宽容口径逐项回退默认且照常表态、不产生降级上报——与迁移前逐字一致。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    BarChartProbe d;
    d.BarChartProps::set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}}})
        .set_bar_width_ratio(0.7F)
        .set_bar_corner_radius(2.0F);

    // ① 上界越界（柱宽占比是占比，>1 无意义）⇒ 不写入、撤表态、退回 0.7，不钳成 1。
    Json above_max = Json::object();
    above_max.set("bar_width_ratio", Json{5.0});
    d.deserialize_props(above_max);
    AURORA_TEST_CHECK_FALSE(d.present(BarChartProps::bar_width_ratio_row()));
    AURORA_TEST_CHECK_NEAR(d.bar_width_ratio(), 0.7F, 1e-4F);

    // ② 下界越界（圆角为负 / 占比低于下限）⇒ 两行同样退回各自基线。
    Json below_min = Json::object();
    below_min.set("bar_corner_radius", Json{-4.0});
    below_min.set("bar_width_ratio", Json{0.001});
    d.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(d.present(BarChartProps::bar_corner_radius_row()));
    AURORA_TEST_CHECK_NEAR(d.bar_corner_radius(), 2.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(d.present(BarChartProps::bar_width_ratio_row()));
    AURORA_TEST_CHECK_NEAR(d.bar_width_ratio(), 0.7F, 1e-4F);

    // ③ 形态不符：开关喂串 ⇒ 两行同样撤表态退回基线。
    Json wrong_shape = Json::object();
    wrong_shape.set("stacked", Json{"yes"});
    wrong_shape.set("show_crosshair", Json{"no"});
    d.deserialize_props(wrong_shape);
    AURORA_TEST_CHECK_FALSE(d.present(BarChartProps::stacked_row()));
    AURORA_TEST_CHECK_FALSE(d.stacked());
    AURORA_TEST_CHECK_FALSE(d.present(BarChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());

    const std::size_t reported = Diagnostics::count();
    AURORA_TEST_CHECK_TRUE(reported >= 5U);  // ①②③共五处降级逐一上报（不是静默吞掉）

    // ④ 结构体数组行宽容通路：非对象元素整项跳过，其余照常入列且**仍表态**、不新增上报。
    d.clear(BarChartProps::series_row());
    Json with_holes = Json::object();
    Json raw = Json::array();
    Json one = Json::object();
    one.set("name", Json{"A"});
    Json vals = Json::array();
    vals.push_back(1.5);
    one.set("values", vals);
    raw.push_back(one);
    raw.push_back(Json{"skip-me"});  // 非对象元素 ⇒ 整项跳过
    with_holes.set("series", raw);
    d.deserialize_props(with_holes);
    AURORA_TEST_CHECK_TRUE(d.present(BarChartProps::series_row()));
    AURORA_TEST_CHECK_EQ(d.series().size(), 1U);
    AURORA_TEST_CHECK_TRUE(d.series()[0].name == "A");
    AURORA_TEST_CHECK_TRUE(d.series()[0].values == std::vector<double>{1.5});
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);  // 宽容解析不报降级

    // ⑤ 结构体行整块形态不符（轴 / 图例喂成非对象）⇒ 退该行默认构造，但仍表态（默认构造也是意图）。
    Json not_object = Json::object();
    not_object.set("axis_x", Json{"{not-an-object}"});
    not_object.set("legend", Json{7.0});
    d.deserialize_props(not_object);
    AURORA_TEST_CHECK_TRUE(d.present(BarChartProps::axis_x_row()));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == BarChartProps::axis_x_default());
    AURORA_TEST_CHECK_TRUE(d.present(BarChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(d.legend() == BarChartProps::legend_default());
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑥ 轴对象里的字段级畸形：类型不符逐项回退默认、`tick_count` 最小夹到 2，整行仍表态。
    Json partial = Json::object();
    Json ax = Json::object();
    ax.set("visible", Json{"no"});  // 非 bool ⇒ 取默认（可见）
    ax.set("tick_count", Json{1});  // 越界 ⇒ 夹到 2
    partial.set("axis_y", ax);
    d.deserialize_props(partial);
    AURORA_TEST_CHECK_TRUE(d.present(BarChartProps::axis_y_row()));
    AURORA_TEST_CHECK_TRUE(d.axis_y().visible);
    AURORA_TEST_CHECK_EQ(d.axis_y().tick_count, 2);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑦ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("bar_width_ratio", Json{0.3});
    good.set("bar_corner_radius", Json{1.0});
    Json cat = Json::array();
    cat.push_back("a");
    cat.push_back("b");
    good.set("categories", cat);
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(BarChartProps::bar_width_ratio_row()));
    AURORA_TEST_CHECK_NEAR(d.bar_width_ratio(), 0.3F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.bar_corner_radius(), 1.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(d.categories() == std::vector<std::string>{"a", "b"});

    Diagnostics::take();
}

AURORA_TEST_CASE(barchart_mounted_writers_mark_declared_tiers_and_repaint_with_new_pixels) {
    // 挂载后的写入按行声明的档位标脏：两枚只进绘制的行落一笔，八枚进布局的行两档各落一笔。
    BarChart chart;
    const BuildContext ctx;
    chart.mount(ctx);
    BarChartDirty dirty;
    dirty.attach_to(chart);

    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;
    chart.set_show_crosshair(false);  // 纯绘制档
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);
    chart.set_bar_corner_radius(6.0F);  // 纯绘制档
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 2);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);

    const int paints_before = dirty.paints;
    const int layouts_before = dirty.layouts;
    chart.BarChartProps::set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}});  // 域 + 几何 + 柱体
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 1);

    // 迁移前两枚手写 setter 只标绘制档，几何缓存因此可能停在旧值；表行补上布局档后两档齐落。
    chart.set_stacked(true);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 2);
    chart.set_bar_width_ratio(0.5F);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 3);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 3);

    chart.set_axis_y(ChartAxisSpec{.min = 0.0, .max = 8.0});  // 结构化行同样两档齐落
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 4);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 4);

    chart.set_padding(EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 5);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 5);

    // 同值守卫的另一半：同值再写不落账（结构体数组行靠成员比较短路）。
    chart.BarChartProps::set_categories({"a", "b", "c"});
    const int layouts_after_cat = dirty.layouts;
    const int paints_after_cat = dirty.paints;
    chart.BarChartProps::set_categories({"a", "b", "c"});
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_cat);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_cat);

    // 「取到新画面」：数据域真的换了，柱高随之翻转，不是只记了一笔脏。
    const auto box_size = bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H));
    LayoutEngine::layout(chart, box_size, ctx);
    AURORA_TEST_CHECK_NEAR(chart.size().width, static_cast<float>(AURORA_PIX_W), 1e-4F);
    const auto px_a =
        render_to_pixels(chart, Rect{.origin = Point{}, .size = chart.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_FALSE(px_a.empty());
    chart.BarChartProps::set_series({ChartSeries{.name = "A", .values = {4.0, 2.0, 0.0}}});
    LayoutEngine::layout(chart, box_size, ctx);
    const auto px_b =
        render_to_pixels(chart, Rect{.origin = Point{}, .size = chart.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_CHECK_FALSE(px_a == px_b);

    // 表外回调槽不受属性面影响：赋值即生效，既不标脏也不进序列化。
    const int paints_before_slot = dirty.paints;
    chart.on_point_tapped = [](int, int) -> void {};
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before_slot);
    Json props = Json::object();
    chart.serialize_props(props);
    AURORA_TEST_CHECK_FALSE(props.contains("on_point_tapped"));
}

AURORA_TEST_CASE(barchart_widget_writer_replays_grow_in_without_second_answer) {
    // 控件侧那枚同名覆写：值仍由同一行给出（不是第二个答案），额外动作只有 grow-in 重播。
    // 探针只为把 `present` 的表态位点名可读；写件与读件走的仍是 `BarChart` 的生产面。
    BarChartProbe chart;
    const std::vector<ChartSeries> data{{ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}}}};
    chart.set_series(data);  // 控件面写件：返回 BarChart&，值走属性面同一条 assign
    AURORA_TEST_CHECK_TRUE(chart.series() == data);
    AURORA_TEST_CHECK_TRUE(chart.present(BarChartProps::series_row()));
    Json props = Json::object();
    chart.serialize_props(props);
    const Json *arr = require_child(props, "series");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_EQ(arr->size(), 1U);
    // 撤表态后回落基线仍只有一条通路：控件侧覆写不另存一份值。
    chart.clear(BarChartProps::series_row());
    AURORA_TEST_CHECK_TRUE(chart.series().empty());
}

AURORA_TEST_CASE(paint_and_interaction_branches) {
    Painter painter;
    const BuildContext ctx;
    auto paint_at = [&painter, &ctx](Widget &w) -> void {
        w.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = w.size()}, ctx);
    };

    // 1) 空数据：无界约束走 300x200 缺省画布；plot 内 Move 因 series 空命中早返回。
    BarChart empty{};
    empty.layout(Constraints{}, ctx);
    paint_at(empty);
    MouseEvent on_empty = move_at(40.0F, 100.0F);
    empty.on_pointer_event(on_empty);
    AURORA_TEST_CHECK_FALSE(empty.hovered_point().has_value());

    // 2) 显式 y 域（from_explicit）+ x 轴标签 + 图例 Bottom：几何预留与绘制全路径。
    BarChart chart{bare_props()
                       .set_series({ChartSeries{.name = "A", .values = {1.0, 3.0}},
                                    ChartSeries{.name = "B", .values = {2.0, NAN}}})
                       .set_axis_x(ChartAxisSpec{.visible = true, .label = "cat"})
                       .set_axis_y(ChartAxisSpec{.label = "v", .tick_count = 4, .min = 0.0, .max = 4.0})
                       .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom})};
    layout_only(chart);
    chart.on_hover_change(true);
    paint_at(chart);

    // 3) 堆叠模式：y 域按类目求和、命中按 y 分段定位系列、悬停值框 + 十字准线绘制。
    BarChart stacked{bare_props()
                         .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}},
                                      ChartSeries{.name = "B", .values = {2.0, 1.0}}})
                         .set_stacked(true)};
    layout_only(stacked);
    MouseEvent hit_bar = move_at(40.0F, 190.0F);  // 带内、堆叠底部段（系列 A）
    stacked.on_pointer_event(hit_bar);
    const auto stacked_hit = aurora::testing::require_value(stacked.hovered_point());
    AURORA_TEST_CHECK_EQ(stacked_hit.first, 0);
    paint_at(stacked);  // 值框（夹边）+ 准线
    AURORA_TEST_CHECK_FALSE(stacked.accessibility_value().empty());
    AURORA_TEST_CHECK_TRUE(stacked.accessibility_role() == AccessibilityRole::Image);

    // 4) 图例 Top（默认位）命中：优先于数据点 + 联动降透明绘制；悬停离开清除。
    BarChart top{bare_props()
                     .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}},
                                  ChartSeries{.name = "B", .values = {2.0, 1.0}}})
                     .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Top})};
    layout_only(top);
    MouseEvent legend_move = move_at(2.0F, 2.0F);
    top.on_pointer_event(legend_move);
    AURORA_TEST_CHECK_TRUE(top.hovered_legend().has_value());
    AURORA_TEST_CHECK_FALSE(top.hovered_point().has_value());
    paint_at(top);
    top.on_hover_change(false);
    AURORA_TEST_CHECK_FALSE(top.hovered_legend().has_value());

    // 5) 图例 Right：右侧图例带几何（宽 500 收窄绘图区）+ 纵排命中区生成。
    BarChart right{bare_props()
                       .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}},
                                    ChartSeries{.name = "B", .values = {2.0, 1.0}}})
                       .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})};
    Constraints wide;
    wide.min = Size{.width = 0.0F, .height = 0.0F};
    wide.max = Size{.width = 500.0F, .height = 200.0F};
    right.layout(wide, ctx);
    paint_at(right);

    // 6) Press 事件不属 Move/Release：回落基类，不改变悬停态。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 40.0F, .y = 190.0F};
    stacked.on_pointer_event(press);
}

AURORA_TEST_CASE(series_value_fallbacks_are_safe) {
    // 系列越界 / 类目越界 / 非有限值：安全回退 0.0，布局与悬停不产生 NaN 几何。
    BarChart chart{bare_props()
                       .set_series({ChartSeries{.name = "short", .values = {1.0}},  // 类目数取 max(标签, 值) = 2
                                    ChartSeries{.name = "nan", .values = {NAN, INFINITY}}})
                       .set_categories({"x", "y"})};
    layout_only(chart);
    MouseEvent e = move_at(200.0F, 100.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_TRUE(chart.hovered_point().has_value());
    Painter painter;
    chart.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = chart.size()}, BuildContext{});
}

}  // namespace aurora::test_cases::utest_bar_chart

/// 测试类型: unit
/// 目标单元: include/aurora/widget/scatter_chart.h
/// 测试说明: 覆盖 ScatterChart——defaults / describe_static / 序列化往返（points 为 [[x,y]] 数组）/
/// 工厂 from_json 重建、最近点欧氏距离命中（阈值 = dot_radius + 4dp）、on_point_tapped、
/// 空数据与 NaN 点跳过，以及像素 golden 基线（chart_scatter.png，受 AURORA_UPDATE_GOLDEN 控制）。
/// 属性单一声明迁移后另覆盖权威表六行的读消费面：逐行撤表态该键退出序列化、显式空序列与未表态
/// 同值不同意、结构体行按整值表态而非字段合并、六行皆常量档故两档主题下与库内解析全同、全量键 JSON
/// 逐行复原生效值并落同一帧像素；本型的口径分叉在两处——两枚标量行（准线开关 / 图内留白）走 `bool`
/// 与 `EdgeInsets` 自带的校验，故形态不符**不写入并撤表态**（迁移前是「非 bool 静默跳过、保留现值」
/// 与「非对象烘成四边全 0 且照常取值」）；而散点数组与双轴、图例三行走 `chart_common.h` 登记的宽容
/// 通路（逐项回退默认、照常表态、不产生降级上报），散点数组行的等值短路另依赖 `ScatterSeries` 与
/// `ChartPoint` 成对补齐的判等运算

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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

namespace aurora::test_cases::utest_scatter_chart {

using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;
using aurora::testing::require_value;

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 320;
constexpr int AURORA_HEIGHT = 200;
constexpr int AURORA_PIX_W = 160;
constexpr int AURORA_PIX_H = 100;

/// 权威表六行的键名（声明顺序 = schema 顺序）。
constexpr const char *AURORA_ALL_KEYS[6] = {"series", "show_crosshair", "axis_x", "axis_y", "legend", "padding"};

/// @brief 关掉双轴与图例、清零留白：绘图区即整块画布，点位与像素可精确预期。
/// @return 只表态四行（数据行 `series` 仍未表态）的属性对象。
[[nodiscard]] auto bare_props() -> ScatterChartProps {
    return ScatterChartProps()
        .set_axis_x(ChartAxisSpec{.visible = false})
        .set_axis_y(ChartAxisSpec{.visible = false})
        .set_legend(ChartLegendSpec{.visible = false})
        .set_padding(EdgeInsets{});
}

/// @brief 显式双轴域 [0,4]：令数据点 (2,2) 落在绘图区正中。
/// @return 不可见、五刻度、域 [0,4] 的轴规格。
[[nodiscard]] auto square_axes() -> ChartAxisSpec {
    return ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0};
}

/// @brief 单系列单点 (2,2) 的数据集（配合 `square_axes` 得到确定的绘图几何）。
/// @return 只含一枚点、名为 "P" 的散点系列。
[[nodiscard]] auto single_series() -> std::vector<ScatterSeries> {
    return {ScatterSeries{.name = "P", .points = {ChartPoint{.x = 2.0, .y = 2.0}}}};
}

[[nodiscard]] auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class ScatterChartProbe final : public ScatterChart {
  public:
    using ScatterChart::present;
    using ScatterChart::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct ScatterChartDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(ScatterChart &target) -> void {
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

/// @brief 布局一帧（无头，无 Application）。
/// @param w 待布局的控件。
auto layout_only(Widget &w) -> void {
    constexpr BuildContext ctx;
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = static_cast<float>(AURORA_WIDTH), .height = static_cast<float>(AURORA_HEIGHT)};
    w.layout(c, ctx);
}

[[nodiscard]] auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
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

[[nodiscard]] auto render_chart(const ScatterChartProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<ScatterChart>(props);
    chart->set_modifier(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_scatter.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

}  // namespace

AURORA_TEST_CASE(defaults_match_declared_property_defaults) {
    const ScatterChartProps d = ScatterChart::defaults();
    // 六行皆未表态 ⇒ 生效值逐行等于该行回落基线（与迁移前字段声明处默认值逐值相同）。
    AURORA_TEST_CHECK_TRUE(d.series().empty());
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());
    AURORA_TEST_CHECK_TRUE(d.axis_x().visible);
    AURORA_TEST_CHECK_EQ(d.axis_x().tick_count, 5);
    AURORA_TEST_CHECK_FALSE(d.axis_x().min.has_value());
    AURORA_TEST_CHECK_TRUE(d.axis_y().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding().left), 8.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding().bottom), 8.0, 1e-6);

    ScatterChart const c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"ScatterChart"});
    AURORA_TEST_CHECK_TRUE(c.wants_click());
    AURORA_TEST_CHECK_FALSE(c.hovered_point().has_value());

    // 未表态 ⇒ 一律不写键（回落语义不外泄）；控件级 width/height/show 由基类写。
    Json props = Json::object();
    c.serialize_props(props);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(props.contains(key));
    }
}

AURORA_TEST_CASE(describe_static_is_complete) {
    const WidgetDescriptor d = ScatterChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "ScatterChart");
    AURORA_TEST_CHECK_TRUE(d.children_policy == "none");
    AURORA_TEST_CHECK_EQ(d.properties.size(), 6U);
    AURORA_TEST_CHECK_TRUE(d.events.front() == "on_point_tapped");

    auto find = [&d](const std::string &key) -> const PropDescriptor * {
        const auto it = std::ranges::find_if(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
        return it == d.properties.end() ? nullptr : &*it;
    };
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(find(key) != nullptr);
    }
    // 类型名与 JSON 类型一律由 `chart_common.h` / `props.h` 的登记派生（迁移前散点数组与三行规格手写成 "Json"）。
    const PropDescriptor *secs = find("series");
    AURORA_TEST_REQUIRE_NOT_NULL(secs);
    AURORA_TEST_CHECK_TRUE(secs->type == "vector<ScatterSeries>");
    AURORA_TEST_CHECK_TRUE(secs->json_type == "array");
    for (const char *key : {"axis_x", "axis_y"}) {
        const PropDescriptor *axis = find(key);
        AURORA_TEST_REQUIRE_NOT_NULL(axis);
        AURORA_TEST_CHECK_TRUE(axis->type == "ChartAxisSpec");
        AURORA_TEST_CHECK_TRUE(axis->json_type == "object");
    }
    const PropDescriptor *leg = find("legend");
    AURORA_TEST_REQUIRE_NOT_NULL(leg);
    AURORA_TEST_CHECK_TRUE(leg->type == "ChartLegendSpec");
    AURORA_TEST_CHECK_TRUE(leg->json_type == "object");
    // 两枚标量行走自带校验的登记，但本型两行都没有额外约束 ⇒ 上下界留空。
    const PropDescriptor *cross = find("show_crosshair");
    AURORA_TEST_REQUIRE_NOT_NULL(cross);
    AURORA_TEST_CHECK_TRUE(cross->type == "bool");
    AURORA_TEST_CHECK_TRUE(cross->json_type == "boolean");
    AURORA_TEST_CHECK_TRUE(cross->min_value.empty() && cross->max_value.empty());
    const PropDescriptor *pad = find("padding");
    AURORA_TEST_REQUIRE_NOT_NULL(pad);
    AURORA_TEST_CHECK_TRUE(pad->type == "EdgeInsets");
    AURORA_TEST_CHECK_TRUE(pad->json_type == "object");

    AURORA_TEST_CHECK_FALSE(d.invariants.empty());
    AURORA_TEST_CHECK_FALSE(d.examples.empty());
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    ScatterSeries s{};
    s.name = "P";
    s.points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 3.0, .y = 4.0}};
    s.dot_radius = 5.0F;
    ScatterChart const src{ScatterChartProps().set_series({s})};
    Json props = Json::object();
    src.serialize_props(props);
    const auto &series = *require_child(props, "series");
    AURORA_TEST_REQUIRE_TRUE(series.is_array());
    AURORA_TEST_CHECK_EQ(require_child_at(series, 0)->at("points")->size(), 2U);

    ScatterChart dst{};
    dst.deserialize_props(props);
    const auto dst_series = dst.series();
    AURORA_TEST_REQUIRE_EQ(dst_series.size(), 1U);
    AURORA_TEST_CHECK_NEAR(dst_series[0].points[1].y, 4.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst_series[0].dot_radius), 5.0, 1e-6);

    serialization::register_core_widgets();
    Json node = Json::object();
    node.set("type", "ScatterChart");
    node.set("props", props);
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_REQUIRE_NOT_NULL(dynamic_cast<const ScatterChart *>(built.value().get()));
}

AURORA_TEST_CASE(nearest_point_hit_by_euclidean_distance) {
    // 显式双轴域 [0,4] 使几何确定：点 (2,2) → (160,100)，hover 命中 index 0。
    ScatterChart chart{bare_props().set_axis_x(square_axes()).set_axis_y(square_axes()).set_series(single_series())};
    layout_only(chart);

    MouseEvent at_center = move_at(160.0F, 100.0F);
    chart.on_pointer_event(at_center);
    const auto center_pt = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(center_pt.second, 0);

    MouseEvent far = move_at(10.0F, 10.0F);  // 超出命中半径
    chart.on_pointer_event(far);
    AURORA_TEST_CHECK_FALSE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(release_fires_on_point_tapped) {
    ScatterChart chart{bare_props().set_axis_x(square_axes()).set_axis_y(square_axes()).set_series(single_series())};
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

    const std::vector<ScatterSeries> data{
        ScatterSeries{.name = "P", .points = {ChartPoint{.x = NAN, .y = 1.0}, ChartPoint{.x = 2.0, .y = 2.0}}}};
    ScatterChart chart{bare_props().set_axis_x(square_axes()).set_axis_y(square_axes()).set_series(data)};
    layout_only(chart);
    MouseEvent e = move_at(160.0F, 100.0F);  // 点 (2,2) 在 [0,4] 域下 → (160,100)
    chart.on_pointer_event(e);
    const auto nan_pt = require_value(chart.hovered_point());
    AURORA_TEST_CHECK_EQ(nan_pt.second, 1);  // NaN 点被跳过
}

AURORA_TEST_CASE(golden_scatter_chart_matches_baseline) {
    ScatterSeries a{};
    a.name = "A";
    a.points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 2.5, .y = 4.0}, ChartPoint{.x = 4.0, .y = 3.0},
                ChartPoint{.x = 5.5, .y = 6.0}};
    ScatterSeries b{};
    b.name = "B";
    b.points = {ChartPoint{.x = 1.5, .y = 1.0}, ChartPoint{.x = 3.0, .y = 2.5}, ChartPoint{.x = 5.0, .y = 4.5}};
    const ScatterChartProps p = ScatterChartProps()
                                    .set_series({a, b})
                                    .set_axis_x(ChartAxisSpec{.label = "x", .tick_count = 5})
                                    .set_axis_y(ChartAxisSpec{.label = "y", .tick_count = 5});
    golden::compare_or_update("chart_scatter", render_chart(p));
}

AURORA_TEST_CASE(paint_and_interaction_branches) {
    Painter painter;
    const BuildContext ctx;
    auto paint_at = [&painter, &ctx](Widget &w) -> void {
        w.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = w.size()}, ctx);
    };

    // 1) 空数据：无界约束走 300x200 缺省画布。
    ScatterChart empty{};
    empty.layout(Constraints{}, ctx);
    paint_at(empty);

    // 2) 非有限点跳过 + 图例 Bottom 几何与可见轴（仅验证绘制不炸）。
    const std::vector<ScatterSeries> mixed{
        ScatterSeries{.name = "A",
                      .points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = NAN, .y = 1.0},
                                 ChartPoint{.x = 3.0, .y = INFINITY}, ChartPoint{.x = 3.0, .y = 3.0}}},
        ScatterSeries{.name = "B", .points = {ChartPoint{.x = 2.0, .y = 2.0}}}};
    ScatterChart chart{bare_props()
                           .set_series(mixed)
                           .set_axis_x(ChartAxisSpec{})
                           .set_axis_y(ChartAxisSpec{})
                           .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom})
                           .set_padding(EdgeInsets{.left = 8.0F, .top = 8.0F, .right = 8.0F, .bottom = 8.0F})};
    layout_only(chart);
    chart.on_hover_change(true);
    paint_at(chart);

    // 3) 裸配置（图例关）悬停数据点 → 准线 / 高亮绘制 + accessibility_value。
    //    显式双轴域 [0,4]：点 (2,2) → (160,100)，hover 命中 index 0。
    ScatterChart hover{bare_props().set_axis_x(square_axes()).set_axis_y(square_axes()).set_series(single_series())};
    layout_only(hover);
    MouseEvent hit = move_at(160.0F, 100.0F);
    hover.on_pointer_event(hit);
    AURORA_TEST_REQUIRE_TRUE(hover.hovered_point().has_value());
    paint_at(hover);
    AURORA_TEST_CHECK_FALSE(hover.accessibility_value().empty());
    AURORA_TEST_CHECK_TRUE(hover.accessibility_role() == AccessibilityRole::Image);

    // 4) 图例 Top（默认位）命中：优先于数据点 + 降透明绘制；悬停离开清除。
    const std::vector<ScatterSeries> two{ScatterSeries{.name = "A", .points = {ChartPoint{.x = 1.0, .y = 1.0}}},
                                         ScatterSeries{.name = "B", .points = {ChartPoint{.x = 2.0, .y = 2.0}}}};
    ScatterChart top{
        bare_props().set_series(two).set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Top})};
    layout_only(top);
    MouseEvent legend_move = move_at(2.0F, 2.0F);
    top.on_pointer_event(legend_move);
    AURORA_TEST_CHECK_FALSE(top.hovered_point().has_value());  // 图例优先，不再解析数据点
    paint_at(top);
    top.on_hover_change(false);
    AURORA_TEST_CHECK_FALSE(top.hovered_point().has_value());

    // 5) 图例 Right：纵排命中区生成（宽 500 收窄绘图区）。
    ScatterChart right{
        bare_props().set_series(two).set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})};
    Constraints wide;
    wide.min = Size{.width = 0.0F, .height = 0.0F};
    wide.max = Size{.width = 500.0F, .height = 200.0F};
    right.layout(wide, ctx);
    paint_at(right);

    // 6) Press 事件不属 Move/Release：回落基类。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 2.0F, .y = 2.0F};
    hover.on_pointer_event(press);
}

AURORA_TEST_CASE(scatterchart_clear_unstates_each_row_and_drops_every_key) {
    // 撤表态是逐行的公共通用件：六行各撤一次，该键退出序列化、生效值退回该行回落基线。
    ScatterChartProbe chart;
    chart.ScatterChartProps::set_series(single_series())
        .set_show_crosshair(false)
        .set_axis_x(square_axes())
        .set_axis_y(square_axes())
        .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})
        .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F});

    Json full = Json::object();
    chart.serialize_props(full);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(full.contains(key));
    }

    chart.clear(ScatterChartProps::series_row());
    chart.clear(ScatterChartProps::show_crosshair_row());
    chart.clear(ScatterChartProps::axis_x_row());
    chart.clear(ScatterChartProps::axis_y_row());
    chart.clear(ScatterChartProps::legend_row());
    chart.clear(ScatterChartProps::padding_row());

    Json cleared = Json::object();
    chart.serialize_props(cleared);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));
    }
    // 回落基线逐个复现，不是全零：开关回 true、留白回四边 8.0、规格回默认构造。
    AURORA_TEST_CHECK_TRUE(chart.series().empty());
    AURORA_TEST_CHECK_TRUE(chart.show_crosshair());
    AURORA_TEST_CHECK_TRUE(chart.axis_x().visible);
    AURORA_TEST_CHECK_TRUE(chart.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_NEAR(chart.padding().left, 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(chart.padding().bottom, 8.0F, 1e-4F);
}

AURORA_TEST_CASE(scatterchart_empty_array_stated_differs_from_unstated) {
    // 显式空序列与未表态的生效值相同（都不画），但意图不同：前者写键、后者不写键。
    ScatterChartProbe stated;
    stated.ScatterChartProps::set_series({});
    AURORA_TEST_CHECK_TRUE(stated.present(ScatterChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(stated.series().empty());
    Json stated_json = Json::object();
    stated.serialize_props(stated_json);
    AURORA_TEST_CHECK_TRUE(stated_json.contains("series"));
    AURORA_TEST_REQUIRE_TRUE(require_child(stated_json, "series")->is_array());
    AURORA_TEST_CHECK_EQ(require_child(stated_json, "series")->size(), 0U);

    ScatterChartProbe const unstated;
    AURORA_TEST_CHECK_FALSE(unstated.present(ScatterChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(unstated.series().empty());
    Json unstated_json = Json::object();
    unstated.serialize_props(unstated_json);
    AURORA_TEST_CHECK_FALSE(unstated_json.contains("series"));

    // 往返保住这份区别：空数组 JSON 灌回控件仍表态，而不是退化成「没设」。
    ScatterChartProbe roundtrip;
    roundtrip.deserialize_props(stated_json);
    AURORA_TEST_CHECK_TRUE(roundtrip.present(ScatterChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(roundtrip.series().empty());
}

AURORA_TEST_CASE(scatterchart_struct_rows_state_whole_value_not_field_merge) {
    // 结构体行按整值表态：第二次写入整只替换，不把字段并进上一次的值里。
    ScatterChartProbe chart;
    chart.ScatterChartProps::set_axis_x(ChartAxisSpec{.visible = true, .label = "x", .tick_count = 7})
        .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom});
    AURORA_TEST_CHECK_TRUE(chart.axis_x().label == "x");
    AURORA_TEST_CHECK_EQ(chart.axis_x().tick_count, 7);

    chart.set_axis_x(ChartAxisSpec{});  // 整值替换：标签与刻度数一起回到默认
    AURORA_TEST_CHECK_TRUE(chart.axis_x().label.empty());
    AURORA_TEST_CHECK_EQ(chart.axis_x().tick_count, 5);
    AURORA_TEST_CHECK_TRUE(chart.axis_x().visible);
    // 另一行不受影响：按行隔离，不是按对象共享。
    AURORA_TEST_CHECK_TRUE(chart.legend().position == LegendPosition::Bottom);

    // 等值短路真的走到最内层：点数相同、只有一枚坐标参与比较时也必须判为不等。
    const std::vector<ScatterSeries> a{ScatterSeries{.name = "P", .points = {ChartPoint{.x = 1.0, .y = NAN}}}};
    const std::vector<ScatterSeries> b{ScatterSeries{.name = "P", .points = {ChartPoint{.x = 1.0, .y = 2.0}}}};
    AURORA_TEST_CHECK_FALSE(a == b);
    chart.ScatterChartProps::set_series(a);
    chart.ScatterChartProps::set_series(b);
    AURORA_TEST_REQUIRE_EQ(chart.series().size(), 1U);
    AURORA_TEST_CHECK_NEAR(chart.series()[0].points[0].y, 2.0, 1e-12);
    // 判等的另一半：NaN 与 NaN 也判不等（`double` 按 `==`），故「同样畸形」仍会被当作新值写入。
    AURORA_TEST_CHECK_FALSE(ChartPoint{.x = NAN, .y = 0.0} == ChartPoint{.x = NAN, .y = 0.0});
    AURORA_TEST_CHECK_TRUE(ChartPoint{.x = 1.0, .y = 2.0} == ChartPoint{.x = 1.0, .y = 2.0});
}

AURORA_TEST_CASE(scatterchart_constant_rows_ignore_theme_scope) {
    // 六行皆为常量档回落点：配色取自主题与色板而非属性面，故生效值与光暗作用域无关，
    // 且库内解析件 resolve(row, ctx) 与无参读件给出同一个答案。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    ScatterChartProbe d;
    d.ScatterChartProps::set_series({ScatterSeries{.name = "A", .points = {ChartPoint{.x = 1.0, .y = 1.0}}}})
        .set_show_crosshair(false)
        .set_axis_x(square_axes())
        .set_axis_y(square_axes())
        .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})
        .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F});

    AURORA_TEST_CHECK_TRUE(d.series() == d.resolve(ScatterChartProps::series_row(), light));
    AURORA_TEST_CHECK_TRUE(d.series() == d.resolve(ScatterChartProps::series_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair() == d.resolve(ScatterChartProps::show_crosshair_row(), light));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair() == d.resolve(ScatterChartProps::show_crosshair_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == d.resolve(ScatterChartProps::axis_x_row(), light));
    AURORA_TEST_CHECK_TRUE(d.axis_y() == d.resolve(ScatterChartProps::axis_y_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(ScatterChartProps::legend_row(), light));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(ScatterChartProps::legend_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.padding().right, d.resolve(ScatterChartProps::padding_row(), dark).right, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().top, d.resolve(ScatterChartProps::padding_row(), light).top, 1e-6F);
    // 两档主题下逐行同值 ⇒ 换主题不该改动任何一行已表态的生效值。
    AURORA_TEST_CHECK_TRUE(d.resolve(ScatterChartProps::axis_x_row(), light) ==
                           d.resolve(ScatterChartProps::axis_x_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.resolve(ScatterChartProps::padding_row(), light).left,
                           d.resolve(ScatterChartProps::padding_row(), dark).left, 1e-6F);
}

AURORA_TEST_CASE(scatterchart_full_key_json_restores_every_effective_value) {
    // 六行全表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态；把它灌回一份空白控件，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    const BuildContext ctx;
    ScatterChart src{
        ScatterChartProps()
            .set_series(
                {ScatterSeries{.name = "A", .points = {ChartPoint{.x = 1.0, .y = 2.0}, ChartPoint{.x = 3.0, .y = 4.0}}},
                 ScatterSeries{.name = "B", .points = {ChartPoint{.x = 2.0, .y = 1.0}}, .dot_radius = 6.0F}})
            .set_show_crosshair(false)
            .set_axis_x(ChartAxisSpec{.label = "x", .tick_count = 5})
            .set_axis_y(ChartAxisSpec{.label = "y", .tick_count = 4})
            .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})
            .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F})};
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }
    AURORA_TEST_CHECK_FALSE(require_field<bool>(legacy, "show_crosshair"));
    AURORA_TEST_CHECK_NEAR(require_field<float>(*require_child(legacy, "padding"), "left"), 2.0F, 1e-4F);

    ScatterChartProbe dst;
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(ScatterChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ScatterChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ScatterChartProps::axis_x_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ScatterChartProps::axis_y_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ScatterChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(ScatterChartProps::padding_row()));

    AURORA_TEST_CHECK_TRUE(dst.series() == src.series());
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

AURORA_TEST_CASE(scatterchart_invalid_json_rejects_checked_rows_while_struct_rows_stay_lenient) {
    // 已登记校验的两枚标量行：`bool` 与 `EdgeInsets` 的校验随描述符生效，形态不符一律不写入、
    // 撤表态、生效值退回该行回落基线，且降级经 `Diagnostics` 上报 —— 迁移前这里是「非 bool 静默跳过、
    // 保留现值」与「非对象整值烘成四边全 0 且照常取值」，本轮统一改成拒绝。
    // 结构化行（series / axis_x / axis_y / legend）相反：`chart_common.h` 的登记刻意不带 `validate`，
    // 按既有宽容口径逐项回退默认且照常表态、不产生降级上报——与迁移前逐字一致。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    ScatterChartProbe d;
    d.ScatterChartProps::set_show_crosshair(false).set_padding(EdgeInsets{});

    // ① 开关喂串 ⇒ 不写入、撤表态，生效值退回基线 true（不是保留上一次写入的 false）。
    Json bad_bool = Json::object();
    bad_bool.set("show_crosshair", Json{"yes"});
    d.deserialize_props(bad_bool);
    AURORA_TEST_CHECK_FALSE(d.present(ScatterChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());

    // ② 留白喂标量（迁移前会整值烘成四边全 0 并照常取值）⇒ 同样拒写、撤表态、退回基线 8.0。
    Json scalar_pad = Json::object();
    scalar_pad.set("padding", Json{6.0});
    d.deserialize_props(scalar_pad);
    AURORA_TEST_CHECK_FALSE(d.present(ScatterChartProps::padding_row()));
    AURORA_TEST_CHECK_NEAR(d.padding().left, 8.0F, 1e-4F);

    // ③ 留白对象内的字段级畸形：类型不符与负值各拒一次。
    Json string_field = Json::object();
    Json left_str = Json::object();
    left_str.set("left", Json{"wide"});
    string_field.set("padding", left_str);
    d.deserialize_props(string_field);
    AURORA_TEST_CHECK_FALSE(d.present(ScatterChartProps::padding_row()));

    Json negative = Json::object();
    Json top_neg = Json::object();
    top_neg.set("top", Json{-4.0});
    negative.set("padding", top_neg);
    d.deserialize_props(negative);
    AURORA_TEST_CHECK_FALSE(d.present(ScatterChartProps::padding_row()));
    AURORA_TEST_CHECK_NEAR(d.padding().top, 8.0F, 1e-4F);

    const std::size_t reported = Diagnostics::count();
    AURORA_TEST_CHECK_TRUE(reported >= 4U);  // ①②③共四处降级逐一上报（不是静默吞掉）

    // ④ 散点数组行宽容通路：非对象元素整项跳过，长度不足与非数值的数组项整项丢弃，
    //    其余照常入列且**仍表态**、不新增上报。
    d.clear(ScatterChartProps::series_row());
    Json with_holes = Json::object();
    Json raw = Json::array();
    Json one = Json::object();
    one.set("name", Json{"A"});
    Json pts = Json::array();
    Json good_pt = Json::array();
    good_pt.push_back(1.0);
    good_pt.push_back(2.0);
    pts.push_back(good_pt);
    Json short_pt = Json::array();
    short_pt.push_back(3.0);  // 长度不足 2 ⇒ 整项丢弃
    pts.push_back(short_pt);
    Json text_pt = Json::array();
    text_pt.push_back("x");
    text_pt.push_back(1.0);  // 首元素非数值 ⇒ 整项丢弃
    pts.push_back(text_pt);
    one.set("points", pts);
    raw.push_back(one);
    raw.push_back(Json{"skip-me"});  // 非对象元素 ⇒ 整项跳过
    with_holes.set("series", raw);
    d.deserialize_props(with_holes);
    AURORA_TEST_CHECK_TRUE(d.present(ScatterChartProps::series_row()));
    AURORA_TEST_REQUIRE_EQ(d.series().size(), 1U);
    AURORA_TEST_CHECK_TRUE(d.series()[0].name == "A");
    AURORA_TEST_REQUIRE_EQ(d.series()[0].points.size(), 1U);
    AURORA_TEST_CHECK_NEAR(d.series()[0].points[0].x, 1.0, 1e-9);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);  // 宽容解析不报降级

    // ⑤ 结构体行整块形态不符（轴喂成非对象）⇒ 退该行默认构造，但仍表态（默认构造也是意图）。
    Json axis_not_object = Json::object();
    axis_not_object.set("axis_x", Json{"{not-an-object}"});
    d.deserialize_props(axis_not_object);
    AURORA_TEST_CHECK_TRUE(d.present(ScatterChartProps::axis_x_row()));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == ChartAxisSpec{});
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑥ 轴对象里的字段级畸形：类型不符回退默认、显式域仍收录，整行照常表态。
    Json axis_partial = Json::object();
    Json ax = Json::object();
    ax.set("visible", Json{"no"});  // 非 bool ⇒ 取默认（可见）
    ax.set("tick_count", Json{"five"});  // 非数值 ⇒ 取默认 5
    ax.set("min", Json{0.0});
    axis_partial.set("axis_y", ax);
    d.deserialize_props(axis_partial);
    AURORA_TEST_CHECK_TRUE(d.present(ScatterChartProps::axis_y_row()));
    AURORA_TEST_CHECK_TRUE(d.axis_y().visible);
    AURORA_TEST_CHECK_EQ(d.axis_y().tick_count, 5);
    AURORA_TEST_CHECK_TRUE(d.axis_y().min.has_value());
    AURORA_TEST_CHECK_NEAR(d.axis_y().min.value_or(-1.0), 0.0, 1e-9);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑦ 图例位置未知 ⇒ 按既有口径回退 Top，整行仍表态。
    Json legend_partial = Json::object();
    Json leg = Json::object();
    leg.set("visible", Json{true});
    leg.set("position", Json{"Diagonal"});
    legend_partial.set("legend", leg);
    d.deserialize_props(legend_partial);
    AURORA_TEST_CHECK_TRUE(d.present(ScatterChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑧ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("show_crosshair", Json{false});
    Json pad = Json::object();
    pad.set("left", Json{3.0});
    pad.set("top", Json{3.0});
    pad.set("right", Json{3.0});
    pad.set("bottom", Json{3.0});
    good.set("padding", pad);
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(ScatterChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_FALSE(d.show_crosshair());
    AURORA_TEST_CHECK_TRUE(d.present(ScatterChartProps::padding_row()));
    AURORA_TEST_CHECK_NEAR(d.padding().right, 3.0F, 1e-4F);

    Diagnostics::take();
}

AURORA_TEST_CASE(scatterchart_mounted_writers_mark_declared_tiers_and_repaint_with_new_pixels) {
    // 挂载后的写入按行声明的档位标脏：一枚只进绘制的行落一笔，五行都进布局的行两档各落一笔。
    ScatterChart chart;
    const BuildContext ctx;
    chart.mount(ctx);
    ScatterChartDirty dirty;
    dirty.attach_to(chart);

    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;
    chart.set_show_crosshair(false);  // 纯绘制档（迁移前根本没有写件，只能经字段直写或 JSON 抵达）
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);

    const int paints_before = dirty.paints;
    const int layouts_before = dirty.layouts;
    chart.ScatterChartProps::set_series(single_series());  // 域计算 + 几何预留 + 画点
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 1);

    // 双轴与图例迁移前只标布局，但三者都被绘制趟直接读（刻度画不画、色块与名称），故本轮补绘制档
    // —— 单标一档会让另一趟停在旧值。留白随图表族同键口径补绘制档。
    chart.ScatterChartProps::set_axis_x(square_axes());
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 2);
    chart.ScatterChartProps::set_axis_y(square_axes());
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 3);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 3);
    chart.set_legend(ChartLegendSpec{.visible = false});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 4);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 4);
    chart.set_padding(EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 5);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 5);

    // 「取到新画面」：数据点真的换了位置，绘图区与点位随之移动，不是只记了一笔脏。
    const auto box_size = bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H));
    LayoutEngine::layout(chart, box_size, ctx);
    AURORA_TEST_CHECK_NEAR(chart.size().width, static_cast<float>(AURORA_PIX_W), 1e-4F);
    const auto px_a =
        render_to_pixels(chart, Rect{.origin = Point{}, .size = chart.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_FALSE(px_a.empty());
    chart.ScatterChartProps::set_series(
        {ScatterSeries{.name = "P", .points = {ChartPoint{.x = 3.5, .y = 0.5}}}});  // 移到右下角
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

AURORA_TEST_CASE(scatterchart_widget_writer_replays_grow_in_without_second_answer) {
    // 控件侧那枚同名覆写：值仍由同一行给出（不是第二个答案），额外动作只有 grow-in 重播。
    // 探针只为把 `present` 的表态位点名可读；写件与读件走的仍是 `ScatterChart` 的生产面。
    ScatterChartProbe chart;
    const BuildContext ctx;
    chart.mount(ctx);
    const std::vector<ScatterSeries> data = single_series();
    chart.set_series(data);  // 控件面写件：返回 ScatterChart&，值走属性面同一条 assign
    AURORA_TEST_CHECK_TRUE(chart.series() == data);
    AURORA_TEST_CHECK_TRUE(chart.present(ScatterChartProps::series_row()));
    Json props = Json::object();
    chart.serialize_props(props);
    const Json *arr = require_child(props, "series");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_EQ(arr->size(), 1U);
    // 撤表态后回落基线仍只有一条通路：控件侧覆写不另存一份值。
    chart.clear(ScatterChartProps::series_row());
    AURORA_TEST_CHECK_TRUE(chart.series().empty());
    // 无头上下文没有运行中的 Animator ⇒ grow-in 落终态、重播是 no-op，故 Display List 仍可缓存
    // （重播只在真帧循环里可见，本用例只锁「不另存值」这条不变量）。
    AURORA_TEST_CHECK_TRUE(chart.can_cache_display_list());
}

}  // namespace aurora::test_cases::utest_scatter_chart

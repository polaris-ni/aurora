/// 测试类型: unit
/// 目标单元: include/aurora/widget/line_chart.h
/// 测试说明: 覆盖 LineChart——defaults / describe_static / 序列化往返 / 工厂 from_json 重建、
/// hover 最近点命中（等距 x = 索引，域与渲染同源）、on_point_tapped 触发、空数据与 NaN 降级、
/// 以及像素 golden 基线（chart_line.png，受 AURORA_GOLDEN_DIR / MAX_DIFF / MAX_PIXELS / UPDATE_GOLDEN 控制）。
/// 属性单一声明迁移后另覆盖权威表十行的读消费面，以及本轮新解的「结构体行 / 结构体数组行」形态：
/// 逐行撤表态该键退出序列化、显式空序列与未表态同值不同意（前者写键、后者不写）、结构体行按整值表态
/// 而非字段合并、十行皆常量档故两档主题下与库内解析全同、全量键 JSON 逐行复原生效值并落同一帧像素、
/// 非法 JSON 逐档降级而结构化行仍走宽容通路（逐项回退默认且照常表态、不产生降级上报）、
/// 挂载后按行档位标脏并真的取到新画面

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
#include "aurora/widget/line_chart.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_line_chart {

using aurora::testing::require_child;
using aurora::testing::require_field;
using aurora::testing::require_value;

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 320;
constexpr int AURORA_HEIGHT = 200;

constexpr int AURORA_PIX_W = 160;
constexpr int AURORA_PIX_H = 100;

/// 权威表十行的键名（声明顺序 = schema 顺序）。
constexpr const char *AURORA_ALL_KEYS[10] = {"series",         "categories", "show_dots", "line_width", "dot_radius",
                                             "show_crosshair", "axis_x",     "axis_y",    "legend",     "padding"};

/// @brief 关掉轴与图例、清零留白：绘图区 = 整个控件，命中几何可精确预期。
[[nodiscard]] auto bare_props() -> LineChartProps {
    return LineChartProps()
        .set_axis_x(ChartAxisSpec{.visible = false})
        .set_axis_y(ChartAxisSpec{.visible = false})
        .set_legend(ChartLegendSpec{.visible = false})
        .set_padding(EdgeInsets{});
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
    chart->set_modifier(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp = std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_line.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
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
class LineChartProbe final : public LineChart {
  public:
    using LineChart::present;
    using LineChart::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct LineChartDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(LineChart &target) -> void {
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

AURORA_TEST_CASE(defaults_and_identity) {
    const LineChartProps d = LineChart::defaults();
    // 十行皆未表态 ⇒ 生效值逐行等于该行回落基线。
    AURORA_TEST_CHECK_TRUE(d.series().empty());
    AURORA_TEST_CHECK_TRUE(d.categories().empty());
    AURORA_TEST_CHECK_TRUE(d.show_dots());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.line_width()), 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.dot_radius()), 3.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());
    AURORA_TEST_CHECK_TRUE(d.axis_x().visible);
    AURORA_TEST_CHECK_EQ(d.axis_y().tick_count, 5);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding().left), 8.0, 1e-6);

    LineChart const c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"LineChart"});
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
    const WidgetDescriptor d = LineChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "LineChart");
    AURORA_TEST_CHECK_EQ(d.properties.size(), 10U);
    auto find = [&d](const std::string &key) -> const PropDescriptor * {
        const auto it = std::ranges::find_if(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
        return it == d.properties.end() ? nullptr : &*it;
    };
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(find(key) != nullptr);
    }
    // 结构化行的类型名与 JSON 类型由 `chart_common.h` 的编解码登记给出（迁移前轴行手写成 "Json"）。
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
    // 下界约束随表行走：0 是合法值（「显式要一条不画出来的线」），故只登记 min、不登记 max。
    const PropDescriptor *width = find("line_width");
    AURORA_TEST_REQUIRE_NOT_NULL(width);
    AURORA_TEST_CHECK_TRUE(width->min_value == "0");
    AURORA_TEST_CHECK_TRUE(width->max_value.empty());
    AURORA_TEST_CHECK_FALSE(d.events.empty());
    AURORA_TEST_CHECK_TRUE(d.events.front() == "on_point_tapped");
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    LineChart const src{
        LineChartProps()
            .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}, .color = Color{9, 8, 7, 255}}})
            .set_categories({"a", "b", "c"})
            .set_show_dots(false)
            .set_line_width(3.5F)
            .set_show_crosshair(false)};
    Json props = Json::object();
    src.serialize_props(props);
    const Json *arr = require_child(props, "series");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_EQ(arr->size(), 1U);

    LineChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.series()[0].name == "A");
    AURORA_TEST_CHECK_TRUE(dst.series()[0].values == std::vector<double>{1.0, 2.0, 3.0});
    AURORA_TEST_CHECK(dst.series()[0].color.has_value());
    AURORA_TEST_CHECK(dst.series()[0].color == std::optional<Color>{Color{9, 8, 7, 255}});
    AURORA_TEST_CHECK_FALSE(dst.show_dots());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.line_width()), 3.5, 1e-6);
    AURORA_TEST_CHECK_FALSE(dst.show_crosshair());

    // 未表态的行不写键（迁移前恒写全量，现按表态位收敛）
    AURORA_TEST_CHECK_FALSE(props.contains("axis_x"));
    AURORA_TEST_CHECK_FALSE(props.contains("padding"));

    serialization::register_core_widgets();
    Json node = Json::object();
    node.set("type", "LineChart");
    node.set("props", props);
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    const auto *chart = dynamic_cast<const LineChart *>(built.value().get());
    AURORA_TEST_REQUIRE_NOT_NULL(chart);
    AURORA_TEST_CHECK_EQ(chart->series().size(), 1U);
}

AURORA_TEST_CASE(hover_snaps_to_nearest_point_index) {
    // 显式 y 域 [0,4] 使几何确定：3 点等距 x = 0 / 160 / 320，值 {0,2,4} 映射 y = 200 / 100 / 0。
    // x 由索引线性映射、y 由显式域线性映射，与渲染同源（见 nearest_point）。
    LineChart chart{bare_props()
                        .set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}})
                        .set_axis_y(ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0})};
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
    LineChart chart{LineChartProps()
                        .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}},
                                     ChartSeries{.name = "B", .values = {2.0, 1.0}}})
                        .set_padding(EdgeInsets{})
                        .set_axis_x(ChartAxisSpec{.visible = false})
                        .set_axis_y(ChartAxisSpec{.visible = false})};
    layout_only(chart);

    // 图例在顶部（padding=0 ⇒ 图例带紧贴 y=0），命中图例项即联动，不再解析数据点
    MouseEvent on_legend = move_at(2.0F, 2.0F);
    chart.on_pointer_event(on_legend);
    AURORA_TEST_CHECK_TRUE(chart.hovered_legend().has_value());
    AURORA_TEST_CHECK_FALSE(chart.hovered_point().has_value());
}

AURORA_TEST_CASE(release_fires_on_point_tapped) {
    LineChart chart{bare_props()
                        .set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}})
                        .set_axis_y(ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0})};
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

    LineChart chart{bare_props().set_series({ChartSeries{.name = "A", .values = {NAN, INFINITY, 1.0}}})};
    layout_only(chart);
    AURORA_TEST_CHECK_FALSE(chart.accessibility_label().empty());
}

AURORA_TEST_CASE(golden_line_chart_matches_baseline) {
    const LineChartProps p =
        LineChartProps()
            .set_series({ChartSeries{.name = "cpu", .values = {12.0, 18.0, 9.0, 24.0, 15.0, 20.0}},
                         ChartSeries{.name = "mem", .values = {8.0, 10.0, 14.0, 12.0, 18.0, 16.0}}})
            .set_categories({"Mon", "Tue", "Wed", "Thu", "Fri", "Sat"})
            .set_axis_y(ChartAxisSpec{.label = "%", .tick_count = 5});
    golden::compare_or_update("chart_line", render_chart(p));
}

// ===========================================================================
// 权威表的读消费面 + 结构体 / 结构体数组行的新形态（属性单一声明迁移批）
// ===========================================================================

AURORA_TEST_CASE(linechart_clear_unstates_each_row_and_drops_every_key) {
    // 十行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且生效值回到该行回落基线」。
    LineChartProbe d;
    d.LineChartProps::set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}}})
        .set_categories({"a", "b"})
        .set_show_dots(false)
        .set_line_width(4.0F)
        .set_dot_radius(6.0F)
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

    d.clear(LineChartProps::series_row());
    AURORA_TEST_CHECK_FALSE(d.present(LineChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(d.series().empty());

    d.clear(LineChartProps::categories_row());
    AURORA_TEST_CHECK_TRUE(d.categories().empty());

    d.clear(LineChartProps::show_dots_row());
    d.clear(LineChartProps::show_crosshair_row());
    AURORA_TEST_CHECK_TRUE(d.show_dots());  // 回落基线为开
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());

    d.clear(LineChartProps::line_width_row());
    d.clear(LineChartProps::dot_radius_row());
    AURORA_TEST_CHECK_NEAR(d.line_width(), 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), 3.0F, 1e-4F);

    // 结构化行的回落点是各自默认构造，不是「清零」
    d.clear(LineChartProps::axis_x_row());
    d.clear(LineChartProps::axis_y_row());
    AURORA_TEST_CHECK_TRUE(d.axis_x().visible);
    AURORA_TEST_CHECK_EQ(d.axis_y().tick_count, 5);
    AURORA_TEST_CHECK_FALSE(d.axis_y().min.has_value());

    d.clear(LineChartProps::legend_row());
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);

    d.clear(LineChartProps::padding_row());
    AURORA_TEST_CHECK_NEAR(d.padding().left, 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.padding().bottom, 8.0F, 1e-4F);

    Json cleared = Json::object();
    d.serialize_props(cleared);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }
}

AURORA_TEST_CASE(linechart_empty_array_stated_differs_from_unstated) {
    // 两枚数组行的两态语义：`set_series({})` 是「显式要一条没有数据的序列」，与「未表态」取值同、
    // 意图不同——前者写键（盘上是空数组），后者不写键。迁移前两者都是「空 vector」，分不开。
    LineChartProbe stated;
    stated.LineChartProps::set_series({});
    stated.set_categories({});
    AURORA_TEST_CHECK_TRUE(stated.present(LineChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(stated.present(LineChartProps::categories_row()));
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

    LineChartProbe const bare;
    AURORA_TEST_CHECK_FALSE(bare.present(LineChartProps::series_row()));
    Json without_key = Json::object();
    bare.serialize_props(without_key);
    AURORA_TEST_CHECK_FALSE(without_key.contains("series"));
    AURORA_TEST_CHECK_FALSE(without_key.contains("categories"));
    AURORA_TEST_CHECK_TRUE(bare.series().empty());  // 生效值同（该行回落基线是空序列）

    // 往返不塌态：盘上的空数组读回来仍是「已表态的空序列」。
    LineChartProbe round;
    round.deserialize_props(with_key);
    AURORA_TEST_CHECK_TRUE(round.present(LineChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(round.present(LineChartProps::categories_row()));
    AURORA_TEST_CHECK_TRUE(round.series().empty());
}

AURORA_TEST_CASE(linechart_struct_rows_state_whole_value_not_field_merge) {
    // 结构体行按**整值**表态：写一次是把整只规格装进槽里，不是「只改那一个字段、其余留旧」。
    // 迁移前那是裸字段，`p.axis_x.visible = false` 这类逐字段改写还能成立；现在只能整只装入。
    LineChartProbe d;
    d.set_axis_x(ChartAxisSpec{.visible = false, .label = "x", .tick_count = 7});
    AURORA_TEST_CHECK_TRUE(d.present(LineChartProps::axis_x_row()));
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
    LineChart chart;
    chart.mount(ctx);
    LineChartDirty dirty;
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

AURORA_TEST_CASE(linechart_constant_rows_ignore_theme_scope) {
    // 十行皆常量档 ⇒ 没有 `x(ctx)` 第二件：无参读件就是生效值，明 / 暗两档主题下与库内
    // `resolve(row, ctx)` 取值全同（缺这组断言则「窄面不丢语义」只是断言）。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    LineChartProbe d;
    d.LineChartProps::set_series({ChartSeries{.name = "A", .values = {1.0, 4.0, 2.0}}})
        .set_categories({"a", "b", "c"})
        .set_line_width(3.0F)
        .set_axis_y(ChartAxisSpec{.min = 0.0, .max = 5.0});

    AURORA_TEST_CHECK_TRUE(d.series() == d.resolve(LineChartProps::series_row(), light));
    AURORA_TEST_CHECK_TRUE(d.series() == d.resolve(LineChartProps::series_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.categories() == d.resolve(LineChartProps::categories_row(), light));
    AURORA_TEST_CHECK_TRUE(d.categories() == d.resolve(LineChartProps::categories_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.show_dots() == d.resolve(LineChartProps::show_dots_row(), light));
    AURORA_TEST_CHECK_TRUE(d.show_dots() == d.resolve(LineChartProps::show_dots_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.line_width(), d.resolve(LineChartProps::line_width_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.line_width(), d.resolve(LineChartProps::line_width_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), d.resolve(LineChartProps::dot_radius_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), d.resolve(LineChartProps::dot_radius_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_TRUE(d.show_crosshair() == d.resolve(LineChartProps::show_crosshair_row(), light));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair() == d.resolve(LineChartProps::show_crosshair_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == d.resolve(LineChartProps::axis_x_row(), light));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == d.resolve(LineChartProps::axis_x_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.axis_y() == d.resolve(LineChartProps::axis_y_row(), light));
    AURORA_TEST_CHECK_TRUE(d.axis_y() == d.resolve(LineChartProps::axis_y_row(), dark));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(LineChartProps::legend_row(), light));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(LineChartProps::legend_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.padding().right, d.resolve(LineChartProps::padding_row(), dark).right, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().top, d.resolve(LineChartProps::padding_row(), light).top, 1e-6F);
    // 显式域随整值表态：两档主题下读回来都是那条域，不被作用域改写。
    AURORA_TEST_CHECK_TRUE(d.axis_y().min.has_value());
    AURORA_TEST_CHECK_TRUE(d.axis_y().max.has_value());
    AURORA_TEST_CHECK_NEAR(*d.axis_y().max, 5.0, 1e-6);
}

AURORA_TEST_CASE(linechart_full_key_json_restores_every_effective_value) {
    // 十行全表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态；把它灌回一份空白控件，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    const BuildContext ctx;
    LineChart src{LineChartProps()
                      .set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}},
                                   ChartSeries{.name = "B", .values = {1.0, 3.0, 2.0}}})
                      .set_categories({"x", "y", "z"})
                      .set_show_dots(false)
                      .set_line_width(2.5F)
                      .set_dot_radius(4.0F)
                      .set_show_crosshair(false)
                      .set_axis_x(ChartAxisSpec{.visible = false, .tick_count = 3})
                      .set_axis_y(ChartAxisSpec{.label = "v", .tick_count = 4, .min = 0.0, .max = 4.0})
                      .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom})
                      .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F})};
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }
    AURORA_TEST_CHECK_NEAR(require_field<float>(legacy, "line_width"), 2.5F, 1e-4F);

    LineChartProbe dst;
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::series_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::categories_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::show_dots_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::line_width_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::dot_radius_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::axis_x_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::axis_y_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(LineChartProps::padding_row()));

    AURORA_TEST_CHECK_TRUE(dst.series() == src.series());
    AURORA_TEST_CHECK_TRUE(dst.categories() == src.categories());
    AURORA_TEST_CHECK_FALSE(dst.show_dots());
    AURORA_TEST_CHECK_NEAR(dst.line_width(), 2.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.dot_radius(), 4.0F, 1e-4F);
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

AURORA_TEST_CASE(linechart_invalid_json_degrades_per_row_while_struct_rows_stay_lenient) {
    // 已登记校验的行（线宽 / 半径越界、开关形态不符）：非法值一律不写入、撤表态、生效值退回该行
    // 回落基线，且降级经 `Diagnostics` 上报。
    // 结构化行（series / categories / axis_x / axis_y / legend）与留白行：`chart_common.h` 的四枚登记
    // 刻意不登记 `validate`，按既有宽容口径逐项回退默认且照常表态、不产生降级上报——与迁移前逐字一致。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    LineChartProbe d;
    d.LineChartProps::set_series({ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}}})
        .set_line_width(2.0F)
        .set_dot_radius(4.0F);

    // ① 下界越界（`line_width` / `dot_radius` 的 bounds 为 0）⇒ 撤表态退回各自基线。
    Json below_min = Json::object();
    below_min.set("line_width", Json{-5.0});
    below_min.set("dot_radius", Json{-1.0});
    d.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(d.present(LineChartProps::line_width_row()));
    AURORA_TEST_CHECK_FALSE(d.present(LineChartProps::dot_radius_row()));
    AURORA_TEST_CHECK_NEAR(d.line_width(), 2.0F, 1e-4F);  // 退回基线，不钳成 0
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), 3.0F, 1e-4F);

    // ② 形态不符：开关喂串 ⇒ 两行同样撤表态退回基线。
    Json wrong_shape = Json::object();
    wrong_shape.set("show_dots", Json{"yes"});
    wrong_shape.set("show_crosshair", Json{"no"});
    d.deserialize_props(wrong_shape);
    AURORA_TEST_CHECK_FALSE(d.present(LineChartProps::show_dots_row()));
    AURORA_TEST_CHECK_TRUE(d.show_dots());
    AURORA_TEST_CHECK_FALSE(d.present(LineChartProps::show_crosshair_row()));
    AURORA_TEST_CHECK_TRUE(d.show_crosshair());

    const std::size_t reported = Diagnostics::count();
    AURORA_TEST_CHECK_TRUE(reported >= 4U);  // ①②共四处降级逐一上报（不是静默吞掉）

    // ③ 结构体数组行宽容通路：非对象元素整项跳过，其余照常入列且**仍表态**、不新增上报。
    d.clear(LineChartProps::series_row());
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
    AURORA_TEST_CHECK_TRUE(d.present(LineChartProps::series_row()));
    AURORA_TEST_CHECK_EQ(d.series().size(), 1U);
    AURORA_TEST_CHECK_TRUE(d.series()[0].name == "A");
    AURORA_TEST_CHECK_TRUE(d.series()[0].values == std::vector<double>{1.5});
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);  // 宽容解析不报降级

    // ④ 结构体行整块形态不符（轴 / 图例喂成非对象）⇒ 退该行默认构造，但仍表态（默认构造也是意图）。
    Json not_object = Json::object();
    not_object.set("axis_x", Json{"{not-an-object}"});
    not_object.set("legend", Json{7.0});
    d.deserialize_props(not_object);
    AURORA_TEST_CHECK_TRUE(d.present(LineChartProps::axis_x_row()));
    AURORA_TEST_CHECK_TRUE(d.axis_x() == LineChartProps::axis_x_default());
    AURORA_TEST_CHECK_TRUE(d.present(LineChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(d.legend() == LineChartProps::legend_default());
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑤ 轴对象里的字段级畸形：类型不符逐项回退默认、`tick_count` 最小夹到 2，整行仍表态。
    Json partial = Json::object();
    Json ax = Json::object();
    ax.set("visible", Json{"no"});  // 非 bool ⇒ 取默认（可见）
    ax.set("tick_count", Json{1});  // 越界 ⇒ 夹到 2
    partial.set("axis_y", ax);
    d.deserialize_props(partial);
    AURORA_TEST_CHECK_TRUE(d.present(LineChartProps::axis_y_row()));
    AURORA_TEST_CHECK_TRUE(d.axis_y().visible);
    AURORA_TEST_CHECK_EQ(d.axis_y().tick_count, 2);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑥ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("line_width", Json{3.0});
    good.set("dot_radius", Json{1.0});
    Json cat = Json::array();
    cat.push_back("a");
    cat.push_back("b");
    good.set("categories", cat);
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(LineChartProps::line_width_row()));
    AURORA_TEST_CHECK_NEAR(d.line_width(), 3.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), 1.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(d.categories() == std::vector<std::string>{"a", "b"});

    Diagnostics::take();
}

AURORA_TEST_CASE(linechart_mounted_writers_mark_declared_tiers_and_repaint_with_new_pixels) {
    // 挂载后的写入按行声明的档位标脏：四枚开关 / 尺寸行只落绘制档，六枚影响几何的行两档各落一笔。
    LineChart chart;
    const BuildContext ctx;
    chart.mount(ctx);
    LineChartDirty dirty;
    dirty.attach_to(chart);

    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;
    chart.set_show_dots(false);  // 纯绘制档
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);

    const int paints_before = dirty.paints;
    const int layouts_before = dirty.layouts;
    chart.LineChartProps::set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}});  // 域 + 几何 + 描线
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 1);

    chart.set_axis_y(ChartAxisSpec{.min = 0.0, .max = 8.0});  // 结构化行同样两档齐落
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 2);

    chart.set_padding(EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 3);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 3);

    // 同值守卫的另一半：同值再写不落账（结构体数组行靠成员比较短路）。
    chart.LineChartProps::set_categories({"a", "b", "c"});
    const int layouts_after_cat = dirty.layouts;
    const int paints_after_cat = dirty.paints;
    chart.LineChartProps::set_categories({"a", "b", "c"});
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_cat);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_cat);

    // 「取到新画面」：数据域真的换了，折线走向随之翻转，不是只记了一笔脏。
    const auto box_size = bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H));
    LayoutEngine::layout(chart, box_size, ctx);
    AURORA_TEST_CHECK_NEAR(chart.size().width, static_cast<float>(AURORA_PIX_W), 1e-4F);
    const auto px_a =
        render_to_pixels(chart, Rect{.origin = Point{}, .size = chart.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_FALSE(px_a.empty());
    chart.LineChartProps::set_series({ChartSeries{.name = "A", .values = {4.0, 2.0, 0.0}}});
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

AURORA_TEST_CASE(linechart_widget_writer_replays_grow_in_without_second_answer) {
    // 控件侧那枚同名覆写：值仍由同一行给出（不是第二个答案），额外动作只有 grow-in 重播。
    // 探针只为把 `present` 的表态位点名可读；写件与读件走的仍是 `LineChart` 的生产面。
    LineChartProbe chart;
    const std::vector<ChartSeries> data{{ChartSeries{.name = "A", .values = {1.0, 2.0, 3.0}}}};
    chart.set_series(data);  // 控件面写件：返回 LineChart&，值走属性面同一条 assign
    AURORA_TEST_CHECK_TRUE(chart.series() == data);
    AURORA_TEST_CHECK_TRUE(chart.present(LineChartProps::series_row()));
    Json props = Json::object();
    chart.serialize_props(props);
    const Json *arr = require_child(props, "series");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_EQ(arr->size(), 1U);
    // 撤表态后回落基线仍只有一条通路：控件侧覆写不另存一份值。
    chart.clear(LineChartProps::series_row());
    AURORA_TEST_CHECK_TRUE(chart.series().empty());
}

AURORA_TEST_CASE(paint_and_interaction_branches) {
    Painter painter;
    const BuildContext ctx;
    auto paint_at = [&painter, &ctx](Widget &w) -> void {
        w.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = w.size()}, ctx);
    };

    // 1) 空数据：无界约束走 300x200 缺省画布；plot 内 Move 因 series 空命中早返回。
    LineChart empty{};
    empty.layout(Constraints{}, ctx);
    paint_at(empty);
    MouseEvent on_empty = move_at(160.0F, 100.0F);
    empty.on_pointer_event(on_empty);
    AURORA_TEST_CHECK_FALSE(empty.hovered_point().has_value());

    // 2) 图例 Bottom 几何预留 + show_dots=false 绘制路径（不悬停，只验证绘制不炸）。
    LineChart chart{bare_props()
                        .set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}},
                                     ChartSeries{.name = "B", .values = {NAN, 1.0, 2.0}}})
                        .set_axis_y(ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0})
                        .set_show_dots(false)
                        .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom})};
    layout_only(chart);
    chart.on_hover_change(true);
    paint_at(chart);

    // 3) 裸配置（图例关）悬停数据点 → 十字准线 / 高亮绘制 + accessibility_value。
    LineChart hover{bare_props()
                        .set_series({ChartSeries{.name = "A", .values = {0.0, 2.0, 4.0}}})
                        .set_axis_y(ChartAxisSpec{.visible = false, .tick_count = 5, .min = 0.0, .max = 4.0})};
    layout_only(hover);
    MouseEvent hit = move_at(160.0F, 100.0F);
    hover.on_pointer_event(hit);
    AURORA_TEST_REQUIRE_TRUE(hover.hovered_point().has_value());
    paint_at(hover);
    AURORA_TEST_CHECK_FALSE(hover.accessibility_value().empty());
    AURORA_TEST_CHECK_TRUE(hover.accessibility_role() == AccessibilityRole::Image);

    // 4) 图例 Top（默认位）命中：优先于数据点 + 降透明绘制；悬停离开清除。
    LineChart top{bare_props()
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

    // 5) 图例 Right：纵排命中区生成（宽 500 收窄绘图区）。
    LineChart right{bare_props()
                        .set_series({ChartSeries{.name = "A", .values = {1.0, 2.0}},
                                     ChartSeries{.name = "B", .values = {2.0, 1.0}}})
                        .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})};
    Constraints wide;
    wide.min = Size{.width = 0.0F, .height = 0.0F};
    wide.max = Size{.width = 500.0F, .height = 200.0F};
    right.layout(wide, ctx);
    paint_at(right);

    // 6) Press 事件不属 Move/Release：回落基类。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 160.0F, .y = 100.0F};
    hover.on_pointer_event(press);
}

}  // namespace aurora::test_cases::utest_line_chart

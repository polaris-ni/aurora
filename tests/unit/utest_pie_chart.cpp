/// 测试类型: unit
/// 目标单元: include/aurora/widget/pie_chart.h
/// 测试说明: 覆盖 PieChart——defaults / describe_static / 序列化往返（含扇区对象数组与图例规格）、
/// 工厂 from_json 重建、极坐标命中（半径 + 角度定位扇区）、on_section_tapped、Σ≤0 与负值降级、
/// 图例命中优先，以及像素 golden 基线（chart_pie.png，受 AURORA_UPDATE_GOLDEN 控制）。
/// 属性单一声明迁移后另覆盖权威表七行的读消费面：逐行撤表态该键退出序列化、显式空扇区序列与未表态
/// 同值不同意、结构体行按整值表态而非字段合并、七行皆常量档故两档主题下与库内解析全同、全量键 JSON
/// 逐行复原生效值并落同一帧像素；本型的口径分叉在两处——两枚数值行（内径比 / 扇区间隙）登记了约束且
/// `float` 校验随描述符约束生效，故 JSON 越界值**不写入并撤表态**（迁移前是夹取写入），而扇区数组与
/// 图例规格两行仍走 `chart_common.h` 登记的宽容通路（逐项回退默认、照常表态、不产生降级上报）

#include <algorithm>
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

namespace aurora::test_cases::utest_pie_chart {

using aurora::testing::require_child;
using aurora::testing::require_field;
using aurora::testing::require_value;

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 320;
constexpr int AURORA_HEIGHT = 200;

constexpr int AURORA_PIX_W = 160;
constexpr int AURORA_PIX_H = 100;

/// 权威表七行的键名（声明顺序 = schema 顺序）。
constexpr const char *AURORA_ALL_KEYS[7] = {
    "sections", "center_space_ratio", "start_angle", "show_percentage_labels", "section_gap", "legend", "padding"};

/// @brief 关掉图例、清零留白：圆心与半径可精确预期。
/// @return 只表态图例（关）与留白（零）两行、扇区行仍未表态的属性对象。
[[nodiscard]] auto bare_props() -> PieChartProps {
    return PieChartProps().set_legend(ChartLegendSpec{.visible = false}).set_padding(EdgeInsets{});
}

/// @brief 布局一帧（无头，无 Application）。
/// @param w 待布局的控件。
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
class PieChartProbe final : public PieChart {
  public:
    using PieChart::present;
    using PieChart::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct PieChartDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(PieChart &target) -> void {
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

[[nodiscard]] auto render_chart(const PieChartProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<PieChart>(props);
    chart->set_modifier(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp = std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_pie.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

}  // namespace

AURORA_TEST_CASE(defaults_match_declared_property_defaults) {
    const PieChartProps d = PieChart::defaults();
    // 七行皆未表态 ⇒ 生效值逐行等于该行回落基线（与迁移前字段声明处默认值逐值相同）。
    AURORA_TEST_CHECK_TRUE(d.sections().empty());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.center_space_ratio()), 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.start_angle()), -90.0, 1e-6);
    AURORA_TEST_CHECK_FALSE(d.show_percentage_labels());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.section_gap()), 2.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding().left), 8.0, 1e-6);

    PieChart c{};
    AURORA_TEST_CHECK_TRUE(c.type_name() == std::string{"PieChart"});
    AURORA_TEST_CHECK_TRUE(c.wants_click());
    AURORA_TEST_CHECK_FALSE(c.hovered_section().has_value());

    // 未表态 ⇒ 一律不写键（回落语义不外泄）；控件级 width/height/show 由基类写。
    Json props = Json::object();
    c.serialize_props(props);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(props.contains(key));
    }
}

AURORA_TEST_CASE(describe_static_lists_all_props_and_events) {
    const WidgetDescriptor d = PieChart::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "PieChart");
    AURORA_TEST_CHECK_TRUE(d.children_policy == "none");
    AURORA_TEST_CHECK_EQ(d.properties.size(), 7U);

    auto find = [&d](const std::string &key) -> const PropDescriptor * {
        const auto it = std::ranges::find_if(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
        return it == d.properties.end() ? nullptr : &*it;
    };
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(find(key) != nullptr);
    }
    // 扇区数组行的类型名与 JSON 类型由 `chart_common.h` 本批新登记的 PropCodec 给出（迁移前那两行手写成 "Json"）。
    const PropDescriptor *secs = find("sections");
    AURORA_TEST_REQUIRE_NOT_NULL(secs);
    AURORA_TEST_CHECK_TRUE(secs->type == "vector<PieSection>");
    AURORA_TEST_CHECK_TRUE(secs->json_type == "array");
    const PropDescriptor *leg = find("legend");
    AURORA_TEST_REQUIRE_NOT_NULL(leg);
    AURORA_TEST_CHECK_TRUE(leg->type == "ChartLegendSpec");
    AURORA_TEST_CHECK_TRUE(leg->json_type == "object");
    // 两枚数值行的约束随表行走：内径比是「占比」故上下界齐登，扇区间隙只登记下界。
    const PropDescriptor *ratio = find("center_space_ratio");
    AURORA_TEST_REQUIRE_NOT_NULL(ratio);
    AURORA_TEST_CHECK_TRUE(ratio->min_value == "0");
    AURORA_TEST_CHECK_TRUE(ratio->max_value == "0.95");
    const PropDescriptor *gap = find("section_gap");
    AURORA_TEST_REQUIRE_NOT_NULL(gap);
    AURORA_TEST_CHECK_TRUE(gap->min_value == "0");
    AURORA_TEST_CHECK_TRUE(gap->max_value.empty());

    AURORA_TEST_CHECK_TRUE(d.events.front() == "on_section_tapped");
    AURORA_TEST_CHECK_TRUE(d.invariants.front() == "0 <= center_space_ratio <= 0.95");
}

AURORA_TEST_CASE(props_roundtrip_including_section_array) {
    PieChart src{PieChartProps()
                     .set_sections({PieSection{.name = "A", .value = 3.0, .color = Color{1, 2, 3, 255}},
                                    PieSection{.name = "B", .value = 1.0}})
                     .set_center_space_ratio(0.5F)
                     .set_start_angle(0.0F)
                     .set_show_percentage_labels(true)
                     .set_section_gap(3.0F)
                     .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})
                     .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F})};
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_REQUIRE_TRUE(props.contains("sections"));
    const auto *const arr = require_child(props, "sections");
    AURORA_TEST_REQUIRE_TRUE(arr->is_array());
    AURORA_TEST_CHECK_EQ(arr->size(), 2U);

    PieChart dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(dst.sections().size(), 2U);
    AURORA_TEST_CHECK_TRUE(dst.sections()[0].name == "A");
    AURORA_TEST_REQUIRE_TRUE(dst.sections()[0].color.has_value());
    AURORA_TEST_CHECK_TRUE(require_value(dst.sections()[0].color) == Color{1, 2, 3, 255});
    AURORA_TEST_CHECK_FALSE(dst.sections()[1].color.has_value());
    AURORA_TEST_CHECK_NEAR(dst.sections()[1].value, 1.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.center_space_ratio()), 0.5, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.start_angle()), 0.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(dst.show_percentage_labels());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.section_gap()), 3.0, 1e-6);
    AURORA_TEST_CHECK_TRUE(dst.legend().position == LegendPosition::Right);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(dst.padding().bottom), 5.0, 1e-6);
}

AURORA_TEST_CASE(factory_rebuilds_from_json) {
    serialization::register_core_widgets();
    Json props = Json::object();
    props.set("sections", pie_section_vector_to_json({PieSection{.name = "S", .value = 6.0}}));
    Json node = Json::object();
    node.set("type", "PieChart");
    node.set("props", props);

    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    const auto *chart = dynamic_cast<const PieChart *>(built.value().get());
    AURORA_TEST_REQUIRE_NOT_NULL(chart);
    AURORA_TEST_CHECK_EQ(chart->sections().size(), 1U);
    AURORA_TEST_CHECK_TRUE(chart->sections()[0].name == "S");
    AURORA_TEST_CHECK_NEAR(chart->sections()[0].value, 6.0, 1e-6);
}

AURORA_TEST_CASE(polar_hit_maps_angle_to_section) {
    // 无留白：中心 (160,100)、外径 100（min(320,200)/2）、12 点起；A 占 75% ⇒ 0..270°，B ⇒ 270..360°。
    PieChart chart{
        bare_props().set_sections({PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}})};
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
    PieChart chart{
        bare_props().set_sections({PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}})};
    layout_only(chart);

    int got = -1;
    chart.on_section_tapped = [&got](int idx) -> void { got = idx; };
    MouseEvent e = release_at(160.0F, 40.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(got, 0);
}

AURORA_TEST_CASE(non_positive_total_falls_back_safely) {
    PieChart chart{
        bare_props().set_sections({PieSection{.name = "z", .value = 0.0}, PieSection{.name = "n", .value = -5.0}})};
    layout_only(chart);
    AURORA_TEST_CHECK_FALSE(chart.accessibility_label().empty());
    MouseEvent e = move_at(160.0F, 40.0F);
    chart.on_pointer_event(e);
    AURORA_TEST_CHECK_FALSE(chart.hovered_section().has_value());  // Σ ≤ 0 ⇒ 无扇区可命中
}

AURORA_TEST_CASE(golden_pie_chart_matches_baseline) {
    const PieChartProps p =
        PieChartProps()
            .set_sections({PieSection{.name = "mobile", .value = 45.0}, PieSection{.name = "desktop", .value = 30.0},
                           PieSection{.name = "tablet", .value = 15.0}, PieSection{.name = "other", .value = 10.0}})
            .set_center_space_ratio(0.45F)
            .set_show_percentage_labels(true);
    golden::compare_or_update("chart_pie", render_chart(p));
}

// ===========================================================================
// 权威表的读消费面 + 扇区数组 / 图例结构体行与两枚带界数值行的形态（属性单一声明迁移批）
// ===========================================================================

AURORA_TEST_CASE(piechart_clear_unstates_each_row_and_drops_every_key) {
    // 七行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且生效值回到该行回落基线」。
    PieChartProbe d;
    d.PieChartProps::set_sections({PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}})
        .set_center_space_ratio(0.5F)
        .set_start_angle(0.0F)
        .set_show_percentage_labels(true)
        .set_section_gap(4.0F)
        .set_legend(ChartLegendSpec{.visible = false, .position = LegendPosition::Right})
        .set_padding(EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F});
    Json stated = Json::object();
    d.serialize_props(stated);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 七行全表态 ⇒ 七键逐键在盘
    }

    d.clear(PieChartProps::sections_row());
    AURORA_TEST_CHECK_FALSE(d.present(PieChartProps::sections_row()));
    AURORA_TEST_CHECK_TRUE(d.sections().empty());

    d.clear(PieChartProps::center_space_ratio_row());
    AURORA_TEST_CHECK_NEAR(d.center_space_ratio(), 0.0F, 1e-4F);  // 实心饼图形态

    d.clear(PieChartProps::start_angle_row());
    AURORA_TEST_CHECK_NEAR(d.start_angle(), -90.0F, 1e-4F);

    d.clear(PieChartProps::show_percentage_labels_row());
    AURORA_TEST_CHECK_FALSE(d.show_percentage_labels());

    d.clear(PieChartProps::section_gap_row());
    AURORA_TEST_CHECK_NEAR(d.section_gap(), 2.0F, 1e-4F);

    // 结构化行的回落点是各自默认构造，不是「清零」
    d.clear(PieChartProps::legend_row());
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);

    d.clear(PieChartProps::padding_row());
    AURORA_TEST_CHECK_NEAR(d.padding().left, 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.padding().bottom, 8.0F, 1e-4F);

    Json cleared = Json::object();
    d.serialize_props(cleared);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }
}

AURORA_TEST_CASE(piechart_empty_array_stated_differs_from_unstated) {
    // 数组行的两态语义：`set_sections({})` 是「显式要一组没有数据的扇区」，与「未表态」取值同、
    // 意图不同——前者写键（盘上是空数组），后者不写键。迁移前两者都是「空 vector」，分不开。
    PieChartProbe stated;
    stated.PieChartProps::set_sections({});
    AURORA_TEST_CHECK_TRUE(stated.present(PieChartProps::sections_row()));
    Json with_key = Json::object();
    stated.serialize_props(with_key);
    const Json *arr = require_child(with_key, "sections");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_TRUE(arr->is_array());
    AURORA_TEST_CHECK_EQ(arr->size(), 0U);
    AURORA_TEST_CHECK_TRUE(stated.sections().empty());

    PieChartProbe bare;
    AURORA_TEST_CHECK_FALSE(bare.present(PieChartProps::sections_row()));
    Json without_key = Json::object();
    bare.serialize_props(without_key);
    AURORA_TEST_CHECK_FALSE(without_key.contains("sections"));
    AURORA_TEST_CHECK_TRUE(bare.sections().empty());  // 生效值同（该行回落基线是空序列）

    // 往返不塌态：盘上的空数组读回来仍是「已表态的空序列」。
    PieChartProbe round;
    round.deserialize_props(with_key);
    AURORA_TEST_CHECK_TRUE(round.present(PieChartProps::sections_row()));
    AURORA_TEST_CHECK_TRUE(round.sections().empty());
}

AURORA_TEST_CASE(piechart_struct_rows_state_whole_value_not_field_merge) {
    // 结构体行按**整值**表态：写一次是把整只规格装进槽里，不是「只改那一个字段、其余留旧」。
    // 迁移前那是裸字段，`p.legend.visible = false` 这类逐字段改写还能成立；现在只能整只装入。
    PieChartProbe d;
    d.set_legend(ChartLegendSpec{.visible = false, .position = LegendPosition::Bottom});
    AURORA_TEST_CHECK_TRUE(d.present(PieChartProps::legend_row()));
    AURORA_TEST_CHECK_FALSE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Bottom);

    d.set_legend(ChartLegendSpec{.position = LegendPosition::Right});  // 整值替换：上一轮的关不残留
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Right);

    // 同值再写不落账：结构化行的等值短路靠成员比较，故 `ChartLegendSpec` 与本批补上的
    // `PieSection` 都带 `operator==`（后者是「结构体数组进属性表」的必配项）。
    const BuildContext ctx;
    PieChart chart;
    chart.mount(ctx);
    PieChartDirty dirty;
    dirty.attach_to(chart);
    const ChartLegendSpec leg{.visible = true, .position = LegendPosition::Bottom};
    chart.set_legend(leg);
    const int paints_after_leg = dirty.paints;
    const int layouts_after_leg = dirty.layouts;
    chart.set_legend(leg);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_leg);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_leg);
    AURORA_TEST_CHECK_TRUE(chart.legend() == leg);

    const std::vector<PieSection> data{PieSection{.name = "A", .value = 1.0}, PieSection{.name = "B", .value = 2.0}};
    chart.PieChartProps::set_sections(data);  // 数组行：整份同值序列再写一次同样不落账
    const int paints_after_secs = dirty.paints;
    const int layouts_after_secs = dirty.layouts;
    chart.PieChartProps::set_sections(data);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_secs);
    AURORA_TEST_CHECK_EQ(dirty.layouts, layouts_after_secs);
}

AURORA_TEST_CASE(piechart_constant_rows_ignore_theme_scope) {
    // 七行皆常量档 ⇒ 没有 `x(ctx)` 第二件：无参读件就是生效值，明 / 暗两档主题下与库内
    // `resolve(row, ctx)` 取值全同（缺这组断言则「窄面不丢语义」只是断言）。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    PieChartProbe d;
    d.PieChartProps::set_sections({PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}})
        .set_center_space_ratio(0.4F)
        .set_start_angle(30.0F)
        .set_show_percentage_labels(true)
        .set_section_gap(1.0F)
        .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom});

    AURORA_TEST_CHECK_TRUE(d.sections() == d.resolve(PieChartProps::sections_row(), light));
    AURORA_TEST_CHECK_TRUE(d.sections() == d.resolve(PieChartProps::sections_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.center_space_ratio(), d.resolve(PieChartProps::center_space_ratio_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.center_space_ratio(), d.resolve(PieChartProps::center_space_ratio_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.start_angle(), d.resolve(PieChartProps::start_angle_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.start_angle(), d.resolve(PieChartProps::start_angle_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_TRUE(d.show_percentage_labels() == d.resolve(PieChartProps::show_percentage_labels_row(), light));
    AURORA_TEST_CHECK_TRUE(d.show_percentage_labels() == d.resolve(PieChartProps::show_percentage_labels_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.section_gap(), d.resolve(PieChartProps::section_gap_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.section_gap(), d.resolve(PieChartProps::section_gap_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(PieChartProps::legend_row(), light));
    AURORA_TEST_CHECK_TRUE(d.legend() == d.resolve(PieChartProps::legend_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.padding().right, d.resolve(PieChartProps::padding_row(), dark).right, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().top, d.resolve(PieChartProps::padding_row(), light).top, 1e-6F);
    // 配色不属属性面：扇区未点名颜色时取的是继承主题的色板，与本表七行的常量档回落互不干扰。
    AURORA_TEST_CHECK_FALSE(d.sections()[0].color.has_value());
}

AURORA_TEST_CASE(piechart_full_key_json_restores_every_effective_value) {
    // 七行全表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态；把它灌回一份空白控件，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    const BuildContext ctx;
    PieChart src{
        PieChartProps()
            .set_sections({PieSection{.name = "mobile", .value = 45.0}, PieSection{.name = "desktop", .value = 30.0},
                           PieSection{.name = "other", .value = 25.0}})
            .set_center_space_ratio(0.4F)
            .set_start_angle(-45.0F)
            .set_show_percentage_labels(true)
            .set_section_gap(1.0F)
            .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})
            .set_padding(EdgeInsets{.left = 2.0F, .top = 3.0F, .right = 4.0F, .bottom = 5.0F})};
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }
    AURORA_TEST_CHECK_NEAR(require_field<float>(legacy, "center_space_ratio"), 0.4F, 1e-4F);

    PieChartProbe dst;
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::sections_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::center_space_ratio_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::start_angle_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::show_percentage_labels_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::section_gap_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(PieChartProps::padding_row()));

    AURORA_TEST_CHECK_TRUE(dst.sections() == src.sections());
    AURORA_TEST_CHECK_NEAR(dst.center_space_ratio(), 0.4F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.start_angle(), -45.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(dst.show_percentage_labels());
    AURORA_TEST_CHECK_NEAR(dst.section_gap(), 1.0F, 1e-4F);
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

AURORA_TEST_CASE(piechart_invalid_json_rejects_checked_rows_while_struct_rows_stay_lenient) {
    // 已登记校验的行：两枚数值行的 `float` 校验按描述符上下界判定，越界值一律不写入、撤表态、
    // 生效值退回该行回落基线，且降级经 `Diagnostics` 上报 —— 迁移前这里是「夹取写入」，本轮改成拒绝。
    // 结构化行（sections / legend）相反：`chart_common.h` 的登记刻意不带 `validate`，按既有宽容口径
    // 逐项回退默认且照常表态、不产生降级上报——与迁移前逐字一致。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    PieChartProbe d;
    d.PieChartProps::set_sections({PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}})
        .set_center_space_ratio(0.0F)
        .set_section_gap(2.0F);

    // ① 上界越界（内径比 > 0.95 时环壁退化）⇒ 不写入、撤表态、退回 0.0，不钳成 0.95。
    Json above_max = Json::object();
    above_max.set("center_space_ratio", Json{1.8});
    d.deserialize_props(above_max);
    AURORA_TEST_CHECK_FALSE(d.present(PieChartProps::center_space_ratio_row()));
    AURORA_TEST_CHECK_NEAR(d.center_space_ratio(), 0.0F, 1e-4F);

    // ② 下界越界（间隙为负 / 内径比为负）⇒ 两行同样退回各自基线。
    Json below_min = Json::object();
    below_min.set("section_gap", Json{-3.0});
    below_min.set("center_space_ratio", Json{-0.5});
    d.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(d.present(PieChartProps::section_gap_row()));
    AURORA_TEST_CHECK_NEAR(d.section_gap(), 2.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(d.present(PieChartProps::center_space_ratio_row()));
    AURORA_TEST_CHECK_NEAR(d.center_space_ratio(), 0.0F, 1e-4F);

    // ③ 形态不符：开关喂串 ⇒ 同样撤表态退回基线。
    Json wrong_shape = Json::object();
    wrong_shape.set("show_percentage_labels", Json{"yes"});
    d.deserialize_props(wrong_shape);
    AURORA_TEST_CHECK_FALSE(d.present(PieChartProps::show_percentage_labels_row()));
    AURORA_TEST_CHECK_FALSE(d.show_percentage_labels());

    const std::size_t reported = Diagnostics::count();
    AURORA_TEST_CHECK_TRUE(reported >= 4U);  // ①②③共四处降级逐一上报（不是静默吞掉）

    // ④ 结构体数组行宽容通路：非对象元素整项跳过，其余照常入列且**仍表态**、不新增上报。
    d.clear(PieChartProps::sections_row());
    Json with_holes = Json::object();
    Json raw = Json::array();
    Json one = Json::object();
    one.set("name", Json{"A"});
    one.set("value", Json{2.5});
    raw.push_back(one);
    raw.push_back(Json{"skip-me"});  // 非对象元素 ⇒ 整项跳过
    with_holes.set("sections", raw);
    d.deserialize_props(with_holes);
    AURORA_TEST_CHECK_TRUE(d.present(PieChartProps::sections_row()));
    AURORA_TEST_CHECK_EQ(d.sections().size(), 1U);
    AURORA_TEST_CHECK_TRUE(d.sections()[0].name == "A");
    AURORA_TEST_CHECK_NEAR(d.sections()[0].value, 2.5, 1e-6);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);  // 宽容解析不报降级

    // ⑤ 结构体行整块形态不符（图例喂成非对象）⇒ 退该行默认构造，但仍表态（默认构造也是意图）。
    Json not_object = Json::object();
    not_object.set("legend", Json{"{not-an-object}"});
    d.deserialize_props(not_object);
    AURORA_TEST_CHECK_TRUE(d.present(PieChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(d.legend() == PieChartProps::legend_default());
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑥ 图例对象里的字段级畸形：类型不符回退默认、位置未知值回退 Top，整行仍表态。
    Json partial = Json::object();
    Json leg = Json::object();
    leg.set("visible", Json{"no"});  // 非 bool ⇒ 取默认（可见）
    leg.set("position", Json{"Diagonal"});  // 枚举未知名 ⇒ 回退 Top
    partial.set("legend", leg);
    d.deserialize_props(partial);
    AURORA_TEST_CHECK_TRUE(d.present(PieChartProps::legend_row()));
    AURORA_TEST_CHECK_TRUE(d.legend().visible);
    AURORA_TEST_CHECK_TRUE(d.legend().position == LegendPosition::Top);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑦ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("center_space_ratio", Json{0.45});
    good.set("section_gap", Json{0.0});
    good.set("start_angle", Json{0.0});
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(PieChartProps::center_space_ratio_row()));
    AURORA_TEST_CHECK_NEAR(d.center_space_ratio(), 0.45F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.section_gap(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.start_angle(), 0.0F, 1e-4F);

    Diagnostics::take();
}

AURORA_TEST_CASE(piechart_mounted_writers_mark_declared_tiers_and_repaint_with_new_pixels) {
    // 挂载后的写入按行声明的档位标脏：三枚只进绘制的行落一笔，四枚进布局的行两档各落一笔。
    PieChart chart;
    const BuildContext ctx;
    chart.mount(ctx);
    PieChartDirty dirty;
    dirty.attach_to(chart);

    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;
    chart.set_show_percentage_labels(true);  // 纯绘制档
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);
    chart.set_section_gap(4.0F);  // 纯绘制档（迁移前根本没有写件，只能经字段直写或 JSON 抵达）
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 2);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);
    chart.set_start_angle(0.0F);  // 纯绘制档（命中反查另属交互趟、不属脏档位）
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 3);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);

    const int paints_before = dirty.paints;
    const int layouts_before = dirty.layouts;
    chart.PieChartProps::set_sections({PieSection{.name = "A", .value = 3.0},  // 占比 + 图例带几何
                                       PieSection{.name = "B", .value = 1.0}});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 1);

    // 迁移前两枚手写 setter 各只标一档（内径比只标绘制、图例只标布局），而两者都被布局期的
    // compute_geometry **和**绘制趟直接读（内半径在那趟算定、图例可见性与名称在两趟都要）⇒ 单标
    // 一档会让另一趟停在旧值；表行补上缺的那档后两档齐落。
    chart.set_center_space_ratio(0.5F);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 2);
    chart.set_legend(ChartLegendSpec{.visible = false});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 3);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 3);

    // 留白随图表族同键口径补绘制档（本型绘制趟经缓存的几何参与）。
    chart.set_padding(EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 4);
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 4);

    // 「取到新画面」：数据域真的换了，扇区边界随之翻转，不是只记了一笔脏。
    const auto box_size = bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H));
    LayoutEngine::layout(chart, box_size, ctx);
    AURORA_TEST_CHECK_NEAR(chart.size().width, static_cast<float>(AURORA_PIX_W), 1e-4F);
    const auto px_a =
        render_to_pixels(chart, Rect{.origin = Point{}, .size = chart.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_FALSE(px_a.empty());
    chart.PieChartProps::set_sections({PieSection{.name = "A", .value = 1.0},  // 占比倒置
                                       PieSection{.name = "B", .value = 3.0}});
    LayoutEngine::layout(chart, box_size, ctx);
    const auto px_b =
        render_to_pixels(chart, Rect{.origin = Point{}, .size = chart.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_CHECK_FALSE(px_a == px_b);

    // 表外回调槽不受属性面影响：赋值即生效，既不标脏也不进序列化。
    const int paints_before_slot = dirty.paints;
    chart.on_section_tapped = [](int) -> void {};
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_before_slot);
    Json props = Json::object();
    chart.serialize_props(props);
    AURORA_TEST_CHECK_FALSE(props.contains("on_section_tapped"));
}

AURORA_TEST_CASE(piechart_widget_writer_replays_grow_in_without_second_answer) {
    // 控件侧那枚同名覆写：值仍由同一行给出（不是第二个答案），额外动作只有 grow-in 重播。
    // 探针只为把 `present` 的表态位点名可读；写件与读件走的仍是 `PieChart` 的生产面。
    PieChartProbe chart;
    const BuildContext ctx;
    chart.mount(ctx);
    const std::vector<PieSection> data{PieSection{.name = "A", .value = 3.0}, PieSection{.name = "B", .value = 1.0}};
    chart.set_sections(data);  // 控件面写件：返回 PieChart&，值走属性面同一条 assign
    AURORA_TEST_CHECK_TRUE(chart.sections() == data);
    AURORA_TEST_CHECK_TRUE(chart.present(PieChartProps::sections_row()));
    Json props = Json::object();
    chart.serialize_props(props);
    const Json *arr = require_child(props, "sections");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_EQ(arr->size(), 2U);
    // 撤表态后回落基线仍只有一条通路：控件侧覆写不另存一份值。
    chart.clear(PieChartProps::sections_row());
    AURORA_TEST_CHECK_TRUE(chart.sections().empty());
    // 无头上下文没有运行中的 Animator ⇒ grow-in 落终态、重播是 no-op，故 Display List 仍可缓存
    // （重播只在真帧循环里可见，本用例只锁「不另存值」这条不变量）。
    AURORA_TEST_CHECK_TRUE(chart.can_cache_display_list());
}

AURORA_TEST_CASE(paint_and_interaction_branches) {
    Painter painter;
    const BuildContext ctx;
    auto paint_at = [&painter, &ctx](Widget &w) -> void {
        w.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = w.size()}, ctx);
    };

    // 1) 空数据：无界约束走 240x200 缺省画布；无扇区绘制早返回。
    PieChart empty{};
    empty.layout(Constraints{}, ctx);
    paint_at(empty);

    // 2) 零值扇区（sweep ≤ gap 跳过）+ 百分比标签 + 图例 Bottom：绘制全路径。
    PieChart chart{bare_props()
                       .set_sections({PieSection{.name = "A", .value = 3.0}, PieSection{.name = "z", .value = 0.0},
                                      PieSection{.name = "B", .value = 1.0}})
                       .set_show_percentage_labels(true)
                       .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Bottom})};
    layout_only(chart);
    chart.on_hover_change(true);
    paint_at(chart);

    // 3) 悬停扇区 → 描弧高亮 + 值框绘制 + accessibility_value 文本。
    MouseEvent hit = move_at(160.0F, 40.0F);  // 正上方命中扇区 0
    chart.on_pointer_event(hit);
    AURORA_TEST_REQUIRE_TRUE(chart.hovered_section().has_value());
    paint_at(chart);
    AURORA_TEST_CHECK_FALSE(chart.accessibility_value().empty());
    AURORA_TEST_CHECK_TRUE(chart.accessibility_role() == AccessibilityRole::Image);

    // 4) 图例 Bottom 命中 → 图例优先于扇区 + 联动降透明绘制；悬停离开清除。
    MouseEvent legend_move = move_at(4.0F, 195.0F);
    chart.on_pointer_event(legend_move);
    AURORA_TEST_CHECK_FALSE(chart.hovered_section().has_value());  // 图例优先
    paint_at(chart);
    chart.on_hover_change(false);
    AURORA_TEST_CHECK_FALSE(chart.hovered_section().has_value());

    // 5) 图例 Right：纵排命中区（宽 500 让图例项放得下）。
    PieChart right{bare_props()
                       .set_sections({PieSection{.name = "A", .value = 1.0}, PieSection{.name = "B", .value = 1.0}})
                       .set_legend(ChartLegendSpec{.visible = true, .position = LegendPosition::Right})};
    Constraints wide;
    wide.min = Size{.width = 0.0F, .height = 0.0F};
    wide.max = Size{.width = 500.0F, .height = 200.0F};
    right.layout(wide, ctx);
    MouseEvent legend_right = move_at(470.0F, 4.0F);
    right.on_pointer_event(legend_right);
    AURORA_TEST_CHECK_FALSE(right.hovered_section().has_value());  // 图例命中优先
    paint_at(right);

    // 6) Press 事件不属 Move/Release：回落基类。
    MouseEvent press;
    press.action = MouseAction::Press;
    press.local_position = Point{.x = 160.0F, .y = 40.0F};
    chart.on_pointer_event(press);

    // 7) 环图 + 负值扇区：内孔让半径判定多一条拒绝支，负值不参与占比。
    PieChart donut{bare_props()
                       .set_sections({PieSection{.name = "A", .value = 2.0}, PieSection{.name = "n", .value = -5.0},
                                      PieSection{.name = "B", .value = 2.0}})
                       .set_center_space_ratio(0.6F)};
    layout_only(donut);
    MouseEvent in_hole = move_at(160.0F, 100.0F);  // 圆心 ⇒ 落在内孔半径之内
    donut.on_pointer_event(in_hole);
    AURORA_TEST_CHECK_FALSE(donut.hovered_section().has_value());
    paint_at(donut);
}

}  // namespace aurora::test_cases::utest_pie_chart

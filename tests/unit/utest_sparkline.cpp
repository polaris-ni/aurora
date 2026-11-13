/// 测试类型: unit
/// 目标单元: include/aurora/widget/sparkline.h
/// 测试说明: 覆盖 Sparkline（最薄图表：无轴 / 无网格 / 无图例 / 无交互）——defaults /
/// describe_static / 序列化往返（values 数组 + 可选 color）/ 工厂 from_json 重建、
/// 空数据与单点 / 全等值不崩、以及像素 golden 基线（chart_sparkline.png，受 AURORA_UPDATE_GOLDEN 控制）。
/// 属性单一声明迁移后另覆盖权威表的读消费面与数组值行（`std::vector<double>`）的新形态：逐行撤表态
/// 该键退出序列化、显式空序列与未表态同值不同意（前者写键、后者不写）、六行皆常量档故两档主题下与
/// 库内解析全同、全量键 JSON 逐行复原生效值并落同一帧像素、非法 JSON 逐档降级而数组行走宽容通路
/// （逐项跳过且照常表态、不产生降级上报）、挂载后按行档位标脏并真的取到新像素

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
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
#include "aurora/widget/sparkline.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_sparkline {
using aurora::testing::require_child;
using aurora::testing::require_field;

namespace golden = aurora::testing::golden;

namespace {

constexpr int AURORA_WIDTH = 120;
constexpr int AURORA_HEIGHT = 40;

constexpr int AURORA_PIX_W = 60;
constexpr int AURORA_PIX_H = 24;

/// 权威表六行的键名（声明顺序 = schema 顺序）。
constexpr const char *AURORA_ALL_KEYS[6] = {"values", "color", "line_width", "show_end_dot", "dot_radius", "padding"};

[[nodiscard]] auto render_chart(const SparklineProps &props) -> std::filesystem::path {
    auto chart = std::make_shared<Sparkline>(props);
    chart->set_modifier(Modifier{}.width(static_cast<float>(AURORA_WIDTH)).height(static_cast<float>(AURORA_HEIGHT)));
    Node root{Column{Node{chart}}};
    const std::filesystem::path tmp =
        std::filesystem::path(testing::isolation::temp_dir()) / "current_chart_sparkline.png";
    AURORA_TEST_REQUIRE_TRUE(render_to_png(root, AURORA_WIDTH, AURORA_HEIGHT, tmp.string().c_str()).ok());
    return tmp;
}

/// 造一个注入了指定主题的构建上下文（根级环境，无父指针）。
/// 返回 holder + ctx：`ctx.env` 指向 holder 内的 Environment，二者须在同作用域内使用。
auto themed_ctx(const Theme &t) -> std::pair<std::shared_ptr<Environment>, BuildContext> {
    auto env = std::make_shared<Environment>();
    env->set_local(t);
    BuildContext ctx;
    ctx.env = env.get();
    return {std::move(env), ctx};
}

/// 探针：把只在库内消费的读件提到可读位置（`present` 表态位、`resolve` 带上下文解析）。
/// 两者都不升公共面，故判据只能经派生类点名读取；读到的仍是生产侧同一条函数，不是用例另算一遍。
class SparklineProbe final : public Sparkline {
  public:
    using Sparkline::present;
    using Sparkline::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct SparklineDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(Sparkline &target) -> void {
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

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 直绘一遍（非录制）并把整块画布读回，供逐像素比对。
auto render_to_pixels(Widget &w, const Rect &box, const BuildContext &ctx, int cw, int ch) -> std::vector<Color> {
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
    const SparklineProps d = Sparkline::defaults();
    // 六行皆未表态 ⇒ 生效值逐行等于该行回落基线。
    AURORA_TEST_CHECK_TRUE(d.values().empty());
    AURORA_TEST_CHECK(d.color() == chart_palette(0U));
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.line_width()), 1.5, 1e-6);
    AURORA_TEST_CHECK_TRUE(d.show_end_dot());
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.dot_radius()), 2.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(d.padding().left), 2.0, 1e-6);

    const Sparkline s{};
    AURORA_TEST_CHECK_TRUE(s.type_name() == std::string{"Sparkline"});
    AURORA_TEST_CHECK_TRUE(s.accessibility_label() == std::string{"Sparkline, 0 points"});
    AURORA_TEST_CHECK_TRUE(s.accessibility_value().empty());

    // 未表态 ⇒ 一律不写键（回落语义不外泄）；控件级 width/height/show 由基类写。
    Json props = Json::object();
    s.serialize_props(props);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(props.contains(key));
    }
}

AURORA_TEST_CASE(describe_static_is_complete) {
    const WidgetDescriptor d = Sparkline::describe_static();
    AURORA_TEST_CHECK_TRUE(d.name == "Sparkline");
    AURORA_TEST_CHECK_TRUE(d.events.empty());  // 无交互 ⇒ 无事件
    auto has = [&d](const std::string &key) -> bool {
        return std::ranges::any_of(d.properties, [&key](const PropDescriptor &p) { return p.name == key; });
    };
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(has(key));
    }
}

AURORA_TEST_CASE(props_roundtrip_and_factory) {
    Sparkline const src{SparklineProps()
                            .set_values({1.0, 3.0, 2.0, 5.0})
                            .set_color(Color{10, 20, 30, 255})
                            .set_line_width(2.5F)
                            .set_show_end_dot(false)};
    Json props = Json::object();
    src.serialize_props(props);
    const Json *values = require_child(props, "values");
    AURORA_TEST_REQUIRE_NOT_NULL(values);
    AURORA_TEST_CHECK_EQ(values->size(), 4U);

    Sparkline dst{};
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.values() == std::vector<double>{1.0, 3.0, 2.0, 5.0});
    AURORA_TEST_CHECK(dst.color() == Color{10, 20, 30, 255});
    AURORA_TEST_CHECK_NEAR(dst.line_width(), 2.5F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(dst.show_end_dot());

    // 未设色 ⇒ 不输出 color 键（保留「按色板取色」语义）
    Sparkline plain{};
    plain.set_values({1.0, 2.0});
    Json plain_props = Json::object();
    plain.serialize_props(plain_props);
    AURORA_TEST_CHECK_FALSE(plain_props.contains("color"));

    serialization::register_core_widgets();
    Json node = Json::object();
    node.set("type", "Sparkline");
    node.set("props", props);
    const auto built = serialization::from_json(node);
    AURORA_TEST_REQUIRE_TRUE(built.ok());
    AURORA_TEST_REQUIRE_NOT_NULL(dynamic_cast<const Sparkline *>(built.value().get()));
}

AURORA_TEST_CASE(degenerate_data_is_safe) {
    // 空值 / 单点 / 全等值：均不得崩溃，golden 路径亦应呈现空或平线
    Sparkline empty{};
    empty.set_values({});
    AURORA_TEST_CHECK_TRUE(empty.accessibility_label() == std::string{"Sparkline, 0 points"});
    AURORA_TEST_CHECK_TRUE(render_chart(SparklineProps{}).empty() == false);
    AURORA_TEST_CHECK_TRUE(render_chart(SparklineProps{}.set_values({2.0})).empty() == false);
    AURORA_TEST_CHECK_TRUE(render_chart(SparklineProps{}.set_values({2.0, 2.0, 2.0})).empty() == false);
}

AURORA_TEST_CASE(golden_sparkline_matches_baseline) {
    const SparklineProps p = SparklineProps()
                                 .set_values({4.0, 6.0, 3.0, 8.0, 5.0, 9.0, 7.0, 11.0})
                                 .set_line_width(2.0F)
                                 .set_dot_radius(2.5F);
    golden::compare_or_update("chart_sparkline", render_chart(p));
}

// ===========================================================================
// 权威表的读消费面 + 数组值行的新形态（属性单一声明迁移批）
// ===========================================================================

AURORA_TEST_CASE(sparkline_clear_unstates_each_row_and_drops_every_key) {
    // 六行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且生效值回到该行回落基线」。
    SparklineProbe d;
    d.set_values({1.0, 2.0, 3.0})
        .set_color(Color{0x11, 0x22, 0x33, 0x44})
        .set_line_width(4.0F)
        .set_show_end_dot(false)
        .set_dot_radius(6.0F)
        .set_padding(EdgeInsets{.left = 1.0F, .top = 2.0F, .right = 3.0F, .bottom = 4.0F});
    Json stated = Json::object();
    d.serialize_props(stated);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 六行全表态 ⇒ 六键逐键在盘
    }

    d.clear(SparklineProps::values_row());
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::values_row()));
    AURORA_TEST_CHECK_TRUE(d.values().empty());

    d.clear(SparklineProps::color_row());
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::color_row()));
    AURORA_TEST_CHECK(d.color() == chart_palette(0U));

    d.clear(SparklineProps::line_width_row());
    d.clear(SparklineProps::dot_radius_row());
    AURORA_TEST_CHECK_NEAR(d.line_width(), 1.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), 2.0F, 1e-4F);

    d.clear(SparklineProps::show_end_dot_row());
    AURORA_TEST_CHECK_TRUE(d.show_end_dot());  // 回落基线为开

    d.clear(SparklineProps::padding_row());
    AURORA_TEST_CHECK_NEAR(d.padding().left, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.padding().bottom, 2.0F, 1e-4F);

    Json cleared = Json::object();
    d.serialize_props(cleared);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }
}

AURORA_TEST_CASE(sparkline_empty_array_stated_differs_from_unstated) {
    // 数组行的两态语义：`set_values({})` 是「显式要一条没有数据的序列」，与「未表态」取值同、意图不同
    // ——前者写键（盘上是空数组），后者不写键。迁移前两者都是「空 vector」，无法区分；现由表态位区分。
    SparklineProbe stated;
    stated.set_values({});
    AURORA_TEST_CHECK_TRUE(stated.present(SparklineProps::values_row()));
    Json with_key = Json::object();
    stated.serialize_props(with_key);
    AURORA_TEST_CHECK_TRUE(with_key.contains("values"));
    const Json *arr = require_child(with_key, "values");
    AURORA_TEST_REQUIRE_NOT_NULL(arr);
    AURORA_TEST_CHECK_TRUE(arr->is_array());
    AURORA_TEST_CHECK_EQ(arr->size(), 0U);
    AURORA_TEST_CHECK_TRUE(stated.values().empty());

    SparklineProbe const bare;
    AURORA_TEST_CHECK_FALSE(bare.present(SparklineProps::values_row()));
    Json without_key = Json::object();
    bare.serialize_props(without_key);
    AURORA_TEST_CHECK_FALSE(without_key.contains("values"));
    AURORA_TEST_CHECK_TRUE(bare.values().empty());  // 生效值同（该行回落基线是空序列）

    // 往返不塌态：盘上的空数组读回来仍是「已表态的空序列」。
    SparklineProbe round;
    round.deserialize_props(with_key);
    AURORA_TEST_CHECK_TRUE(round.present(SparklineProps::values_row()));
    AURORA_TEST_CHECK_TRUE(round.values().empty());
}

AURORA_TEST_CASE(sparkline_constant_rows_ignore_theme_scope) {
    // 六行皆常量档 ⇒ 没有 `x(ctx)` 第二件：无参读件就是生效值，明 / 暗两档主题下与库内
    // `resolve(row, ctx)` 取值全同（缺这组断言则「窄面不丢语义」只是断言）。
    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    SparklineProbe d;
    d.set_values({5.0, 1.0, 4.0}).set_line_width(3.0F);  // 表态后仍须与解析同值

    AURORA_TEST_CHECK_TRUE(d.values() == d.resolve(SparklineProps::values_row(), light));
    AURORA_TEST_CHECK_TRUE(d.values() == d.resolve(SparklineProps::values_row(), dark));
    AURORA_TEST_CHECK(d.color() == d.resolve(SparklineProps::color_row(), light));
    AURORA_TEST_CHECK(d.color() == d.resolve(SparklineProps::color_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.line_width(), d.resolve(SparklineProps::line_width_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.line_width(), d.resolve(SparklineProps::line_width_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_TRUE(d.show_end_dot() == d.resolve(SparklineProps::show_end_dot_row(), light));
    AURORA_TEST_CHECK_TRUE(d.show_end_dot() == d.resolve(SparklineProps::show_end_dot_row(), dark));
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), d.resolve(SparklineProps::dot_radius_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), d.resolve(SparklineProps::dot_radius_row(), dark), 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().right, d.resolve(SparklineProps::padding_row(), dark).right, 1e-6F);
    AURORA_TEST_CHECK_NEAR(d.padding().top, d.resolve(SparklineProps::padding_row(), light).top, 1e-6F);
    // 取色走内置色板而非注册的 `ChartPalette` 扩展 ⇒ 基线就是色板首色，换档不动（与迁移前同口径）。
    AURORA_TEST_CHECK(d.color() == chart_palette(0U));
}

AURORA_TEST_CASE(sparkline_full_key_json_restores_every_effective_value) {
    // 逐行表态的属性对象，其序列化输出即迁移前「全量键」的键集与取值形态；把它灌回一份空白控件，
    // 每行都必须重新表态且生效值逐键相等，最终落同一帧像素。
    const BuildContext ctx;
    Sparkline src{SparklineProps()
                      .set_values({4.0, 6.0, 3.0, 8.0, 5.0})
                      .set_color(Color{0x12, 0x34, 0x56, 0x78})
                      .set_line_width(2.0F)
                      .set_show_end_dot(false)
                      .set_dot_radius(3.5F)
                      .set_padding(EdgeInsets{.left = 1.0F, .top = 3.0F, .right = 5.0F, .bottom = 2.0F})};
    Json legacy = Json::object();
    src.serialize_props(legacy);
    for (const char *key : AURORA_ALL_KEYS) {
        AURORA_TEST_CHECK_TRUE(legacy.contains(key));
    }
    AURORA_TEST_CHECK_NEAR(require_field<float>(legacy, "line_width"), 2.0F, 1e-4F);

    SparklineProbe dst;
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(SparklineProps::values_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(SparklineProps::color_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(SparklineProps::line_width_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(SparklineProps::show_end_dot_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(SparklineProps::dot_radius_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(SparklineProps::padding_row()));
    AURORA_TEST_CHECK_TRUE(dst.values() == std::vector<double>{4.0, 6.0, 3.0, 8.0, 5.0});
    AURORA_TEST_CHECK(dst.color() == Color{0x12, 0x34, 0x56, 0x78});
    AURORA_TEST_CHECK_NEAR(dst.line_width(), 2.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(dst.show_end_dot());
    AURORA_TEST_CHECK_NEAR(dst.dot_radius(), 3.5F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.padding().left, 1.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.padding().top, 3.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.padding().right, 5.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.padding().bottom, 2.0F, 1e-4F);

    LayoutEngine::layout(src, bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H)), ctx);
    LayoutEngine::layout(dst, bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H)), ctx);
    AURORA_TEST_REQUIRE_NEAR(src.size().width, dst.size().width, 1e-4F);
    AURORA_TEST_REQUIRE_NEAR(src.size().height, dst.size().height, 1e-4F);
    const Rect box{.origin = Point{}, .size = src.size()};
    const auto px_src = render_to_pixels(src, box, ctx, AURORA_PIX_W, AURORA_PIX_H);
    const auto px_dst = render_to_pixels(dst, box, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_EQ(px_src.size(), px_dst.size());
    AURORA_TEST_CHECK_TRUE(px_src == px_dst);
}

AURORA_TEST_CASE(sparkline_invalid_json_degrades_per_row_while_array_stays_lenient) {
    // 已登记校验的行（线宽 / 半径越界、颜色 / 留白 / 开关形态不符）：非法值一律不写入、撤表态、
    // 生效值退回该行回落基线，且降级经 `Diagnostics` 上报。
    // 数组行（`values`）：`PropCodec<std::vector<double>>` 刻意不登记 `validate`，按既有宽容口径
    // 逐项跳过非数值元素并照常表态，不产生降级上报——与迁移前的解析行为逐字一致。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级

    SparklineProbe d;
    d.set_values({1.0, 2.0, 3.0}).set_line_width(2.0F).set_dot_radius(4.0F);

    // ① 下界越界（`line_width` / `dot_radius` 的 bounds 为 0）⇒ 撤表态退回各自基线。
    Json below_min = Json::object();
    below_min.set("line_width", Json{-5.0});
    below_min.set("dot_radius", Json{-1.0});
    d.deserialize_props(below_min);
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::line_width_row()));
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::dot_radius_row()));
    AURORA_TEST_CHECK_NEAR(d.line_width(), 1.5F, 1e-4F);  // 退回基线，不钳成 0 也不留 2.0
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), 2.0F, 1e-4F);

    // ② 形态不符：颜色喂串、留白喂数、开关喂串 ⇒ 三行同样撤表态退回基线。
    Json wrong_shape = Json::object();
    wrong_shape.set("color", Json{"red"});
    wrong_shape.set("padding", Json{7.0});
    wrong_shape.set("show_end_dot", Json{"yes"});
    d.deserialize_props(wrong_shape);
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::color_row()));
    AURORA_TEST_CHECK(d.color() == chart_palette(0U));
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::padding_row()));
    AURORA_TEST_CHECK_NEAR(d.padding().left, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(d.present(SparklineProps::show_end_dot_row()));
    AURORA_TEST_CHECK_TRUE(d.show_end_dot());

    const std::size_t reported = Diagnostics::count();
    AURORA_TEST_CHECK_TRUE(reported >= 5U);  // ①②共五处降级逐一上报（不是静默吞掉）

    // ③ 数组行宽容通路：非数值元素逐项跳过，其余照常入列且**仍表态**、不新增上报。
    d.clear(SparklineProps::values_row());
    Json with_holes = Json::object();
    Json raw = Json::array();
    raw.push_back(1.5);
    raw.push_back("skip-me");
    raw.push_back(2.5);
    raw.push_back(json::Value{true});  // Bool 构造是 explicit，且 `Value` 在 `aurora::json` 下
    with_holes.set("values", raw);
    d.deserialize_props(with_holes);
    AURORA_TEST_CHECK_TRUE(d.present(SparklineProps::values_row()));
    AURORA_TEST_CHECK_TRUE(d.values() == std::vector<double>{1.5, 2.5});
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);  // 宽容解析不报降级

    // ④ 数组行整块形态不符（喂串而非数组）⇒ 退空序列，但仍表态（空序列也是用户意图）。
    Json not_array = Json::object();
    not_array.set("values", Json{"[1,2]"});
    d.deserialize_props(not_array);
    AURORA_TEST_CHECK_TRUE(d.present(SparklineProps::values_row()));
    AURORA_TEST_CHECK_TRUE(d.values().empty());
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), reported);

    // ⑤ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    Json good_arr = Json::array();
    good_arr.push_back(7.0);
    good_arr.push_back(8.0);
    good.set("values", good_arr);
    good.set("line_width", Json{3.0});
    good.set("dot_radius", Json{1.0});
    d.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(d.present(SparklineProps::line_width_row()));
    AURORA_TEST_CHECK_NEAR(d.line_width(), 3.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.dot_radius(), 1.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(d.values() == std::vector<double>{7.0, 8.0});

    Diagnostics::take();
}

AURORA_TEST_CASE(sparkline_mounted_writers_mark_declared_tiers_and_repaint_with_new_pixels) {
    // 挂载后的写入按行声明的档位标脏：五行 `.paint()` 只落绘制档，`padding` 一行 `.layout()` 只落布局档。
    Sparkline s;
    const BuildContext ctx;
    s.mount(ctx);
    SparklineDirty dirty;
    dirty.attach_to(s);

    const int paints0 = dirty.paints;
    const int layouts0 = dirty.layouts;
    s.set_values({2.0, 4.0, 1.0});  // 数组行：纯绘制档
    AURORA_TEST_CHECK_EQ(dirty.paints - paints0, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts0, 0);

    const int paints_before = dirty.paints;
    const int layouts_before = dirty.layouts;
    s.set_padding(EdgeInsets{.left = 4.0F, .top = 4.0F, .right = 4.0F, .bottom = 4.0F});  // 内缩绘图矩形 ⇒ 仍是绘制档
    AURORA_TEST_CHECK_EQ(dirty.paints - paints_before, 1);
    AURORA_TEST_CHECK_EQ(dirty.layouts - layouts_before, 0);

    const Color next{0x12, 0x34, 0x56, 0xFF};
    s.set_color(next);  // 同值守卫的另一半：同值再写不落账（数组行同样成立）
    const int paints_after_color = dirty.paints;
    s.set_color(next);
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_color);
    s.set_values({2.0, 4.0, 1.0});
    AURORA_TEST_CHECK_EQ(dirty.paints, paints_after_color);

    // 「取到新画面」：落笔的线真的换了色，不是只记了一笔脏。
    LayoutEngine::layout(s, bounded(static_cast<float>(AURORA_PIX_W), static_cast<float>(AURORA_PIX_H)), ctx);
    AURORA_TEST_CHECK_NEAR(s.size().width, static_cast<float>(AURORA_PIX_W), 1e-4F);
    const auto px = render_to_pixels(s, Rect{.origin = Point{}, .size = s.size()}, ctx, AURORA_PIX_W, AURORA_PIX_H);
    AURORA_TEST_REQUIRE_FALSE(px.empty());
    bool any_line_pixel = false;
    for (const Color &c : px) {
        if (c == next) {
            any_line_pixel = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(any_line_pixel);
}

}  // namespace aurora::test_cases::utest_sparkline

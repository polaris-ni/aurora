/// 测试类型: unit
/// 目标单元: include/aurora/widget/grid.h
/// 测试说明: 覆盖 Grid 网格布局——行优先落位与列宽/行高聚合、间距计入总尺寸、
/// 有界宽度均分单元格、默认回落（1 列 / 4px 间距）、写件原样存值而布局入口保底 1 列、
/// 属性序列化往返、自描述。属性单一声明迁移后另覆盖权威表的读消费面：逐行撤表态该键退出
/// 序列化、两枚常量档读件在明 / 暗两档与库内解析全同且不存在带上下文的第二件、全量表态的
/// JSON 逐行复原生效值并落同一帧几何、非法 JSON 逐档降级（越界 / 形态不符拒写并撤表态，
/// 降级经 Diagnostics 上报）、挂载后写入只落布局档且同值短路

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/diagnostics.h"
#include "aurora/environment/environment.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/grid.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_grid {

using aurora::testing::require_field;

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto box_expand_w(float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::expand());
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
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
class GridProbe final : public Grid {
  public:
    using Grid::Grid;
    using Grid::present;
    using Grid::resolve;
};

/// 标脏观察器：`Widget::on_dirty` 的 bool 入参把两档合一，这里拆成两枚计数器。
struct GridDirty {
    int paints = 0;  ///< 绘制档落账次数
    int layouts = 0;  ///< 布局档落账次数

    /// @brief 把观察回调挂到目标控件上（挂上即归零，用例只断增量）。
    /// @param target 待观察的控件。
    auto attach_to(Grid &target) -> void {
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

/// 「带上下文的列数读件」形态：常量档行不应存在这第二件。
/// 用概念而非裸 `requires` 式判定：GCC 在非模板上下文把非法简单要求判成硬错误而非 false。
template <typename W>
concept ScopedColumnsReader = requires(const W &probe, const BuildContext &scope) { probe.columns(scope); };

/// 同上，对间距行。
template <typename W>
concept ScopedGapReader = requires(const W &probe, const BuildContext &scope) { probe.gap(scope); };

}  // namespace

AURORA_TEST_CASE(grid_places_row_major_with_per_column_width) {
    // 2 列、gap=10：A(50x20) B(30x10) / C(20x30)。
    // 列宽 [50,30]、行高 [20,30]；总尺寸 90x60。
    Grid grid(std::vector<Node>{box(50.0F, 20.0F), box(30.0F, 10.0F), box(20.0F, 30.0F)},
              GridProps().set_columns(2).set_gap(10.0F));
    AURORA_TEST_CHECK_EQ(std::string{grid.type_name()}, "Grid");

    LayoutEngine::layout(grid, bounded(500.0F, 500.0F));
    const Size s = grid.size();
    AURORA_TEST_CHECK_NEAR(s.width, 90.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 60.0F, 1e-4F);

    const auto &kids = grid.child_nodes();
    AURORA_TEST_REQUIRE_EQ(kids.size(), 3U);
    // idx0 (r0,c0)：(0,0)；idx1 (r0,c1)：x=50+10=60；idx2 (r1,c0)：y=20+10=30。
    AURORA_TEST_CHECK_NEAR(kids[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[0].bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[1].bounds().origin.x, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[1].bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[2].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(kids[2].bounds().origin.y, 30.0F, 1e-4F);
}

AURORA_TEST_CASE(grid_bounded_width_equalizes_cell_measurement) {
    // 有界宽 200、2 列、gap=10 → 单元格测量宽 (200-10)/2=95：
    // Expand 子项被撑到 95，总宽 95+95+10=200。
    Grid grid(std::vector<Node>{box_expand_w(20.0F), box_expand_w(30.0F)}, GridProps().set_columns(2).set_gap(10.0F));
    LayoutEngine::layout(grid, bounded(200.0F, 500.0F));
    const Size s = grid.size();
    AURORA_TEST_CHECK_NEAR(s.width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[0].bounds().size.width, 95.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[1].bounds().size.width, 95.0F, 1e-4F);
}

AURORA_TEST_CASE(grid_handles_uneven_last_row) {
    // 3 项 2 列：第二行只有 1 项，不崩溃且行高取该行实际子项。
    Grid grid(std::vector<Node>{box(50.0F, 20.0F), box(50.0F, 20.0F), box(50.0F, 40.0F)},
              GridProps().set_columns(2).set_gap(4.0F));
    LayoutEngine::layout(grid, bounded(500.0F, 500.0F));
    const Size s = grid.size();
    AURORA_TEST_CHECK_NEAR(s.width, 104.0F, 1e-4F);  // 50+50+4
    AURORA_TEST_CHECK_NEAR(s.height, 64.0F, 1e-4F);  // 20+40+4
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[2].bounds().origin.y, 24.0F, 1e-4F);
}

AURORA_TEST_CASE(grid_initializer_list_uses_fallback_defaults) {
    // 扁平罗列子项即「两行全未表态」：生效值取回落点（1 列 / 4px），布局按单列垂直排。
    Grid grid{box(10.0F, 10.0F), box(20.0F, 10.0F)};
    AURORA_TEST_REQUIRE_EQ(grid.child_nodes().size(), 2U);
    AURORA_TEST_CHECK_EQ(grid.columns(), 1);
    AURORA_TEST_CHECK_NEAR(grid.gap(), 4.0F, 1e-6F);

    LayoutEngine::layout(grid, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(grid.size().width, 20.0F, 1e-4F);  // 单列 = 最宽子项
    AURORA_TEST_CHECK_NEAR(grid.size().height, 24.0F, 1e-4F);  // 10 + 4 + 10
    AURORA_TEST_CHECK_NEAR(grid.child_nodes()[1].bounds().origin.y, 14.0F, 1e-4F);
}

AURORA_TEST_CASE(grid_writers_store_raw_and_layout_keeps_one_column_floor) {
    // 写件是纯 DTO 语义：非法值原样存入；「小于 1 视为 1」的保底在布局入口，
    // 合法性把关在反序列化通道（见下一条用例）。
    Grid grid{box(10.0F, 10.0F), box(20.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(grid.set_columns(0).columns(), 0);
    LayoutEngine::layout(grid, bounded(200.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(grid.size().width, 20.0F, 1e-4F);  // 0 按 1 列保底
    AURORA_TEST_CHECK_NEAR(grid.size().height, 24.0F, 1e-4F);  // 10 + 4 + 10

    grid.set_columns(-3);
    AURORA_TEST_CHECK_EQ(grid.columns(), -3);
    grid.set_gap(-1.0F);
    AURORA_TEST_CHECK_NEAR(grid.gap(), -1.0F, 0.0F);  // 负间距原样存（下界由不变量与反序列化把关）
}

// ===========================================================================
// 权威表的读消费面（属性单一声明迁移批）：逐行撤表态 / 两档恒等 / 全量键复原 /
// 非法值降级 / 挂载后逐档标脏
// ===========================================================================

AURORA_TEST_CASE(grid_clear_unstates_each_row_and_drops_every_key) {
    // 两行逐行「表态 ⇒ 该键在盘 ⇒ clear ⇒ 该键退盘且取值回到该行回落点」。
    // 子项不进表：序列化里既没有 children，也没有 cells。
    GridProbe grid(std::vector<Node>{box(10.0F, 10.0F), box(20.0F, 10.0F)});
    grid.set_columns(3).set_gap(7.0F);
    Json stated = Json::object();
    grid.serialize_props(stated);
    for (const char *key : {"columns", "gap"}) {
        AURORA_TEST_CHECK_TRUE(stated.contains(key));  // 两行全表态 ⇒ 两键逐键在盘
    }
    for (const char *key : {"children", "cells"}) {
        AURORA_TEST_CHECK_FALSE(stated.contains(key));  // 表外名字（子项）不是属性键
    }

    grid.clear(GridProps::columns_row());
    AURORA_TEST_CHECK_FALSE(grid.present(GridProps::columns_row()));
    AURORA_TEST_CHECK_EQ(grid.columns(), 1);  // 回落基线
    grid.clear(GridProps::gap_row());
    AURORA_TEST_CHECK_FALSE(grid.present(GridProps::gap_row()));
    AURORA_TEST_CHECK_NEAR(grid.gap(), 4.0F, 1e-6F);

    Json cleared = Json::object();
    grid.serialize_props(cleared);
    for (const char *key : {"columns", "gap"}) {
        AURORA_TEST_CHECK_FALSE(cleared.contains(key));  // 意图集合里再无一键
    }
    AURORA_TEST_CHECK_EQ(grid.child_count(), 2U);  // 撤表态不动子项所有权
}

AURORA_TEST_CASE(grid_constant_rows_read_identically_under_light_and_dark) {
    // 恒等关系的正面取证：两行都是常量档，没有 `x(ctx)` 第二件，其无参读件**就是**生效值——
    // 明 / 暗两档主题下与库内 `resolve(row, ctx)` 取值全同。
    static_assert(!ScopedColumnsReader<Grid>);
    static_assert(!ScopedGapReader<Grid>);

    const auto [env_l, light] = themed_ctx(Theme::light());
    const auto [env_d, dark] = themed_ctx(Theme::dark());
    GridProbe grid;
    AURORA_TEST_CHECK_EQ(grid.columns(), grid.resolve(GridProps::columns_row(), light));
    AURORA_TEST_CHECK_EQ(grid.columns(), grid.resolve(GridProps::columns_row(), dark));
    AURORA_TEST_CHECK_NEAR(grid.gap(), grid.resolve(GridProps::gap_row(), light), 1e-6F);
    AURORA_TEST_CHECK_NEAR(grid.gap(), grid.resolve(GridProps::gap_row(), dark), 1e-6F);

    // 表态后仍与解析同值；控件面同名写件返回控件引用，链式表的是「同一个对象」而非第二份属性。
    Grid &chain = grid.set_columns(2).set_gap(3.0F);
    AURORA_TEST_CHECK(&chain == static_cast<Grid *>(&grid));
    AURORA_TEST_CHECK_EQ(grid.columns(), 2);
    AURORA_TEST_CHECK_EQ(grid.columns(), grid.resolve(GridProps::columns_row(), light));
    AURORA_TEST_CHECK_NEAR(grid.gap(), 3.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(grid.gap(), grid.resolve(GridProps::gap_row(), dark), 1e-6F);
}

AURORA_TEST_CASE(grid_full_key_json_restores_every_effective_value) {
    // 逐行表态的属性对象，其序列化输出灌回一份空白控件：每行都重新表态且生效值逐键相等，
    // 最终落同一帧几何（每个子项的布局盒逐值相同）。
    Grid src(std::vector<Node>{box(50.0F, 20.0F), box(30.0F, 10.0F), box(20.0F, 30.0F)},
             GridProps().set_columns(2).set_gap(10.0F));
    Json legacy = Json::object();
    src.serialize_props(legacy);
    AURORA_TEST_CHECK_EQ(require_field<int>(legacy, "columns"), 2);
    AURORA_TEST_CHECK_NEAR(require_field<float>(legacy, "gap"), 10.0F, 1e-6F);

    GridProbe dst(std::vector<Node>{box(50.0F, 20.0F), box(30.0F, 10.0F), box(20.0F, 30.0F)});
    dst.deserialize_props(legacy);
    AURORA_TEST_CHECK_TRUE(dst.present(GridProps::columns_row()));
    AURORA_TEST_CHECK_TRUE(dst.present(GridProps::gap_row()));
    AURORA_TEST_CHECK_EQ(dst.columns(), 2);
    AURORA_TEST_CHECK_NEAR(dst.gap(), 10.0F, 1e-6F);

    const BuildContext ctx;
    LayoutEngine::layout(src, bounded(500.0F, 500.0F), ctx);
    LayoutEngine::layout(dst, bounded(500.0F, 500.0F), ctx);
    AURORA_TEST_REQUIRE_NEAR(src.size().width, dst.size().width, 1e-4F);
    AURORA_TEST_REQUIRE_NEAR(src.size().height, dst.size().height, 1e-4F);
    AURORA_TEST_REQUIRE_EQ(src.child_nodes().size(), dst.child_nodes().size());
    for (std::size_t i = 0; i < src.child_nodes().size(); ++i) {
        const Rect a = src.child_nodes()[i].bounds();
        const Rect b = dst.child_nodes()[i].bounds();
        AURORA_TEST_CHECK_NEAR(a.origin.x, b.origin.x, 1e-4F);
        AURORA_TEST_CHECK_NEAR(a.origin.y, b.origin.y, 1e-4F);
        AURORA_TEST_CHECK_NEAR(a.size.width, b.size.width, 1e-4F);
        AURORA_TEST_CHECK_NEAR(a.size.height, b.size.height, 1e-4F);
    }
}

AURORA_TEST_CASE(grid_invalid_json_degrades_to_row_fallback) {
    // 非法值一律不写入：可空行撤表态退回该行回落点，且降级经 `Diagnostics` 上报。
    Diagnostics::take();  // 清掉前序用例残留，本条只看自己产生的降级
    const std::size_t baseline = Diagnostics::count();

    // ① 列数越界（下界 1）⇒ 撤表态，生效值回 1，而不是把 0 烘进字段。
    GridProbe grid;
    grid.set_columns(2);
    Json zero = Json::object();
    zero.set("columns", Json{0});
    grid.deserialize_props(zero);
    AURORA_TEST_CHECK_FALSE(grid.present(GridProps::columns_row()));
    AURORA_TEST_CHECK_EQ(grid.columns(), 1);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline + 1U);

    // ② 间距越界（下界 0）⇒ 同样撤表态并退回 4px 基线。
    grid.set_gap(9.0F);
    Json negative = Json::object();
    negative.set("gap", Json{-3.0});
    grid.deserialize_props(negative);
    AURORA_TEST_CHECK_FALSE(grid.present(GridProps::gap_row()));
    AURORA_TEST_CHECK_NEAR(grid.gap(), 4.0F, 1e-6F);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline + 2U);

    // ③ 形态不符（喂串）⇒ 同样拒写并撤表态。
    grid.set_columns(2);
    Json wrong_type = Json::object();
    wrong_type.set("columns", Json{"wide"});
    grid.deserialize_props(wrong_type);
    AURORA_TEST_CHECK_FALSE(grid.present(GridProps::columns_row()));
    AURORA_TEST_CHECK_EQ(grid.columns(), 1);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline + 3U);

    // ④ 反面对照（防空转）：同一路径灌合法值仍照常写入并表态。
    Json good = Json::object();
    good.set("columns", Json{3});
    good.set("gap", Json{8.0});
    grid.deserialize_props(good);
    AURORA_TEST_CHECK_TRUE(grid.present(GridProps::columns_row()));
    AURORA_TEST_CHECK_TRUE(grid.present(GridProps::gap_row()));
    AURORA_TEST_CHECK_EQ(grid.columns(), 3);
    AURORA_TEST_CHECK_NEAR(grid.gap(), 8.0F, 1e-6F);
    AURORA_TEST_CHECK_EQ(Diagnostics::count(), baseline + 3U);  // 合法值不再上报

    Diagnostics::take();
}

AURORA_TEST_CASE(grid_mounted_writers_mark_layout_tier_only) {
    // 两行都只声明布局档：挂载后每次写入都只落布局档，绘制档不落账；同值写入由行上的判等守卫短路。
    Grid grid;
    const BuildContext ctx;
    grid.mount(ctx);
    GridDirty dirty;
    dirty.attach_to(grid);

    grid.set_columns(2);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
    grid.set_columns(2);  // 同值：判等短路，不落账
    AURORA_TEST_CHECK_EQ(dirty.layouts, 1);
    grid.set_gap(6.0F);
    AURORA_TEST_CHECK_EQ(dirty.layouts, 2);
    AURORA_TEST_CHECK_EQ(dirty.paints, 0);
}

AURORA_TEST_CASE(describe_reports_invariants) {
    const auto d = Grid::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Grid");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
    AURORA_TEST_REQUIRE_EQ(d.invariants.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{d.invariants[0]}, "columns >= 1");
    AURORA_TEST_CHECK_EQ(std::string{d.invariants[1]}, "gap >= 0");

    // 属性项全部由权威表发射：按 `rows()` 点名顺序恰两行。
    AURORA_TEST_REQUIRE_EQ(d.properties.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].name}, "columns");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].name}, "gap");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[0].min_value}, "1");
    AURORA_TEST_CHECK_EQ(std::string{d.properties[1].min_value}, "0");
}

}  // namespace aurora::test_cases::utest_grid
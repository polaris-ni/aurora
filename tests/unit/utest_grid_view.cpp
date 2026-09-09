/// 测试类型: unit
/// 目标单元: include/aurora/widget/grid_view.h
/// 测试说明: 覆盖 GridView——默认不变量、非法 count/列数/格高钳制降级、行数向上取整与内容高、
/// 视口填充与单元格整形、按需构建仅可见行（含末行不满格）、滚动偏移钳制与滚轮步进、
/// 单元格网格落位、序列化与自描述

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/layout/layout_engine.h"
#include "aurora/widget/grid_view.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_grid_view {

namespace {

/// 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char* override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints& c, const BuildContext& /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter& /*p*/, const Rect& /*bounds*/, const BuildContext& /*ctx*/) -> void override {}

  private:
    float w_;
    float h_;
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 单元格构建观测器：记录构建序号并保留各单元格实例（虚拟化回收后仍可读其几何）。
struct BuildRecorder {
    std::map<int, std::shared_ptr<FixedBox>> items;
    std::vector<int> built_order;

    auto builder() -> GridView::ItemBuilder {
        return [this](int index) -> Node {
            built_order.push_back(index);
            auto box = std::make_shared<FixedBox>(100.0F, 96.0F);
            items.emplace(index, std::move(box));
            return Node{items.at(index)};
        };
    }
};

}  // namespace

AURORA_TEST_CASE(default_grid_view_invariants) {
    GridView grid;
    AURORA_TEST_CHECK_EQ(std::string{grid.type_name()}, "GridView");
    AURORA_TEST_CHECK_EQ(grid.count(), 0);
    AURORA_TEST_CHECK_EQ(grid.columns(), 1);
    AURORA_TEST_CHECK_NEAR(grid.cell_extent(), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(grid.row_count(), 0);
    AURORA_TEST_CHECK_NEAR(grid.content_height(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(grid.live_item_count(), 0U);
}

AURORA_TEST_CASE(ctor_degrades_invalid_arguments) {
    // 非正列数降级为 1（经 Diagnostics::degraded 上报）。
    GridView zero_columns{9, 0, {}, 96.0F};
    AURORA_TEST_CHECK_EQ(zero_columns.columns(), 1);

    // 非正格高降级为 96。
    GridView bad_extent{9, 3, {}, -1.0F};
    AURORA_TEST_CHECK_NEAR(bad_extent.cell_extent(), 96.0F, 1e-4F);

    // 负 count 钳为 0。
    GridView negative{-2, 3, {}, 96.0F};
    AURORA_TEST_CHECK_EQ(negative.count(), 0);

    GridView normal{30, 3, {}, 96.0F};
    AURORA_TEST_CHECK_EQ(normal.count(), 30);
    AURORA_TEST_CHECK_EQ(normal.columns(), 3);
    AURORA_TEST_CHECK_NEAR(normal.cell_extent(), 96.0F, 1e-4F);
}

AURORA_TEST_CASE(row_count_rounds_up_and_content_height) {
    GridView full{30, 3, {}, 96.0F};
    AURORA_TEST_CHECK_EQ(full.row_count(), 10);
    AURORA_TEST_CHECK_NEAR(full.content_height(), 960.0F, 1e-4F);

    GridView remainder{10, 3, {}, 96.0F};
    AURORA_TEST_CHECK_EQ(remainder.row_count(), 4);  // ceil(10 / 3)
    AURORA_TEST_CHECK_NEAR(remainder.content_height(), 384.0F, 1e-4F);

    GridView exact{9, 3, {}, 96.0F};
    AURORA_TEST_CHECK_EQ(exact.row_count(), 3);
    AURORA_TEST_CHECK_NEAR(exact.content_height(), 288.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_fills_viewport_and_sizes_cells) {
    BuildRecorder rec;
    GridView grid{30, 3, rec.builder(), 96.0F};
    grid.set_cache_extent(0.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(grid.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grid.size().height, 300.0F, 1e-4F);
    // 单元格被格约束（min == max = 视口宽/列数 × 格高）强制整形。
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->size().width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->size().height, 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(11)->size().width, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(virtualization_builds_visible_rows_only) {
    BuildRecorder rec;
    GridView grid{30, 3, rec.builder(), 96.0F};
    grid.set_cache_extent(0.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_EQ(grid.visible_row_range().first, 0);
    AURORA_TEST_CHECK_EQ(grid.visible_row_range().second, 4);  // ceil(300 / 96)
    AURORA_TEST_CHECK_EQ(grid.live_item_count(), 12U);  // 行 0..3 × 3 列
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 12U);  // 只构建可见行，而非 count=30

    // 约束不变的重复布局不重建任何单元格。
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 12U);
    AURORA_TEST_CHECK_EQ(grid.live_item_count(), 12U);
}

AURORA_TEST_CASE(partial_last_row_builds_only_existing_cells) {
    BuildRecorder rec;
    GridView grid{7, 3, rec.builder(), 96.0F};
    grid.set_cache_extent(0.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_EQ(grid.row_count(), 3);
    AURORA_TEST_CHECK_EQ(grid.visible_row_range().second, 3);  // 钳到 row_count
    AURORA_TEST_CHECK_EQ(grid.live_item_count(), 7U);  // 末行只有第 6 号一格
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 7U);
}

AURORA_TEST_CASE(scroll_offset_clamps_to_content) {
    // 断言 live_item_count 需要真实构建器：空 builder 下虚拟化永不实例化单元格。
    BuildRecorder rec;
    GridView grid{30, 3, rec.builder(), 96.0F};
    grid.set_cache_extent(0.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(grid.max_scroll_offset(), 660.0F, 1e-4F);  // 960 - 300
    grid.set_scroll_offset(99999.0F);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 660.0F, 1e-4F);
    grid.set_scroll_offset(-5.0F);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 0.0F, 1e-4F);

    // 滚到底后的可见行窗口：660/96=6.875 → 行 6..9。
    grid.set_scroll_offset(660.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_EQ(grid.visible_row_range().first, 6);
    AURORA_TEST_CHECK_EQ(grid.visible_row_range().second, 10);
    AURORA_TEST_CHECK_EQ(grid.live_item_count(), 12U);  // 行 6..8 满格 9 + 末行 3 格
}

AURORA_TEST_CASE(wheel_scroll_steps_offset) {
    GridView grid{30, 3, {}, 96.0F};
    grid.set_cache_extent(0.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    grid.set_scroll_offset(660.0F);
    // delta_y 正方向为向上滚动：offset 减小一个滚轮步进 40。
    ScrollEvent e;
    e.delta_y = 1.0F;
    grid.on_scroll(e);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 620.0F, 1e-4F);
    ScrollEvent down;
    down.delta_y = -1.0F;
    grid.on_scroll(down);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 660.0F, 1e-4F);
}

AURORA_TEST_CASE(cell_positions_follow_grid) {
    BuildRecorder rec;
    GridView grid{30, 3, rec.builder(), 96.0F};
    grid.set_cache_extent(0.0F);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));

    Painter p;
    p.begin(300, 300);
    grid.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = 300.0F}},
               BuildContext{});
    // 索引 → (行, 列)：0 → (0,0)，2 → (0,2)，5 → (1,2)；列宽 100，格高 96。
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->paint_bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->paint_bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(2)->paint_bounds().origin.x, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(2)->paint_bounds().origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(5)->paint_bounds().origin.x, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(5)->paint_bounds().origin.y, 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(5)->paint_bounds().size.width, 100.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_and_describe) {
    GridView src{9, 2, {}, 80.0F};
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["count"].get<int>(), 9);
    AURORA_TEST_CHECK_EQ(props["columns"].get<int>(), 2);
    AURORA_TEST_CHECK_NEAR(props["cell_extent"].get<float>(), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["scroll_offset"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["cache_extent"].get<float>(), 200.0F, 1e-4F);

    const auto d = GridView::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "GridView");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool columns_required = false;
    bool columns_min_one = false;
    for (const auto& p : d.properties) {
        if (p.name == "columns") {
            columns_required = p.required;
            columns_min_one = (p.min_value == "1");
        }
    }
    AURORA_TEST_CHECK_TRUE(columns_required);
    AURORA_TEST_CHECK_TRUE(columns_min_one);
    AURORA_TEST_CHECK_FALSE(d.invariants.empty());
}

}  // namespace aurora::test_cases::utest_grid_view

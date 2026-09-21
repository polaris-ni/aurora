/// 测试类型: unit
/// 目标单元: include/aurora/widget/grid_view.h
/// 测试说明: 覆盖 GridView——默认不变量、非法 count/列数/格高钳制降级、行数向上取整与内容高、
/// 视口填充与单元格整形、按需构建仅可见行（含末行不满格）、滚动偏移钳制与滚轮步进、
/// 单元格网格落位、序列化与自描述、snap/paging 逐帧收位、reduce-motion 直落、offset_signal 发布
///
/// 行窗口与偏移的换算按 `item_extent = 96` 的整行推进（滚轮步进 40 为控件内部常量）。

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
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

/// @brief 假想帧钟：单调递增，令自驱动的收位滑动逐帧可测（不依赖墙钟抖动）。
auto frame_clock() -> std::chrono::steady_clock::time_point& {
    static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{};
    return now;
}

/// @brief 推进 n 帧（每帧 16ms），驱动 snap 收位 / scroll_to 短滑动。
auto pump(GridView& grid, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        grid.tick(frame_clock());
    }
}

/// @brief 等滑动走完：滑满时长 150ms + 余量。
auto settle(GridView& grid) -> void { pump(grid, 16); }

/// @brief 滚轮事件入口（step 为控件内部常量 40，故 1 单位 = 40dp）。
auto wheel(GridView& grid, float delta_y) -> ScrollEvent {
    ScrollEvent e;
    e.delta_y = delta_y;
    grid.on_scroll(e);
    return e;
}

/// @brief reduce-motion 守卫：作用域内开启，离开时复原进程级设置（单例，测试须自清）。
class ReduceMotionGuard final {
  public:
    ReduceMotionGuard() : saved_(current_accessibility_settings()) {
        AccessibilitySettings s = saved_;
        s.reduce_motion = true;
        set_accessibility_settings(s);
    }
    ~ReduceMotionGuard() { set_accessibility_settings(saved_); }
    ReduceMotionGuard(const ReduceMotionGuard&) = delete;
    auto operator=(const ReduceMotionGuard&) -> ReduceMotionGuard& = delete;
    ReduceMotionGuard(ReduceMotionGuard&&) = delete;
    auto operator=(ReduceMotionGuard&&) -> ReduceMotionGuard& = delete;

  private:
    AccessibilitySettings saved_;
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

AURORA_TEST_CASE(snap_glide_advances_every_frame_until_row_boundary) {
    // 格高 96、snap extent 192（两行一站）：滚轮落在 120 → 收位到 192，须逐帧推进到终点。
    GridView grid{30, 3, {}, 96.0F};
    grid.set_snap(ScrollSnap{.extent = 192.0F});
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));  // 内容 960 / 视口 300 → [0, 660]

    wheel(grid, -3.0F);  // 3 单位 × 40dp = 120
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 120.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(grid.is_gliding());

    pump(grid, 1);
    AURORA_TEST_CHECK_TRUE(grid.scroll_offset() > 120.0F && grid.scroll_offset() < 192.0F);
    AURORA_TEST_CHECK_TRUE(grid.is_gliding());  // 中间帧仍在滑动（曾在此处被作废而冻结）

    settle(grid);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 192.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(grid.is_gliding());
}

AURORA_TEST_CASE(snap_paging_pages_by_viewport_height) {
    GridView grid{30, 3, {}, 96.0F};
    grid.set_snap(ScrollSnap::page());  // 视口高 300 = 一页
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));

    wheel(grid, -2.0F);  // 80 < 半页 → 回弹本页
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 80.0F, 1e-4F);
    settle(grid);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 0.0F, 1e-4F);

    wheel(grid, -5.0F);  // 200 > 半页 → 下一页 300
    settle(grid);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 300.0F, 1e-4F);

    // 末段不足一页：以整页对齐为准（600），不因滚轮夹在 660 而停在非对齐点。
    wheel(grid, -10.0F);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 660.0F, 1e-4F);
    settle(grid);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 600.0F, 1e-4F);
}

AURORA_TEST_CASE(reduce_motion_snaps_and_jumps_without_intermediate_frames) {
    ReduceMotionGuard guard;
    GridView grid{30, 3, {}, 96.0F};
    grid.set_snap(ScrollSnap{.extent = 192.0F});
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));

    wheel(grid, -3.0F);
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 192.0F, 1e-4F);  // 直落对齐点
    AURORA_TEST_CHECK_FALSE(grid.is_gliding());

    grid.scroll_to(0.0F);  // animate=true 同样短路：状态与走完一致
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(grid.is_gliding());
}

AURORA_TEST_CASE(offset_signal_follows_programmatic_and_glide_frames) {
    GridView grid{30, 3, {}, 96.0F};
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));

    SignalView<float>& offset = grid.offset_signal();  // 懒创建：初值取当前偏移
    AURORA_TEST_CHECK_NEAR(offset.get(), 0.0F, 1e-4F);

    grid.set_scroll_offset(100.0F);
    AURORA_TEST_CHECK_NEAR(offset.get(), 100.0F, 1e-4F);
    wheel(grid, 1.0F);  // 向上 40dp
    AURORA_TEST_CHECK_NEAR(offset.get(), 60.0F, 1e-4F);

    grid.scroll_to(400.0F);  // 滑动期逐帧发布
    pump(grid, 1);
    AURORA_TEST_CHECK_TRUE(offset.get() > 60.0F && offset.get() < 400.0F);
    settle(grid);
    AURORA_TEST_CHECK_NEAR(offset.get(), 400.0F, 1e-4F);
}

AURORA_TEST_CASE(snap_properties_are_serialized) {
    GridView src{9, 3, {}, 96.0F};
    src.set_snap(ScrollSnap{.extent = 288.0F, .alignment = ScrollSnapAlignment::End});
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["snap_extent"].get<float>(), 288.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(props["snap_paging"].get<bool>());
    AURORA_TEST_CHECK_EQ(props["snap_alignment"].get<std::string>(), std::string{"End"});

    // 吸附开关的可观测性：本控件以 set_snap 接线（builder 属运行时回调，from_json 不重建条目）。
    AURORA_TEST_CHECK_TRUE(src.snap().enabled(300.0F));
    GridView off{9, 3, {}, 96.0F};
    AURORA_TEST_CHECK_FALSE(off.snap().enabled(300.0F));
}

}  // namespace aurora::test_cases::utest_grid_view

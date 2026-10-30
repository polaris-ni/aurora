/// 测试类型: unit
/// 目标单元: include/aurora/widget/grid_view.h
/// 测试说明: 覆盖 GridView——默认不变量、非法 count/列数/格高钳制降级、行数向上取整与内容高、
/// 视口填充与单元格整形、按需构建仅可见行（含末行不满格）、滚动偏移钳制与滚轮步进、
/// 单元格网格落位、序列化与自描述、反序列化回填标量属性（含非法值降级、显式偏移优先于按键恢复）、
/// snap/paging 逐帧收位、reduce-motion 直落、offset_signal 发布、
/// 应用内全局时间缩放的 GridView 消费点（到位帧数按倍率折半且终值不变 / 冻结段零重绘请求且解冻续跑）
///
/// 行窗口与偏移的换算按 `item_extent = 96` 的整行推进（滚轮步进 40 为控件内部常量）。

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/grid_view.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_grid_view {

using aurora::testing::require_child;
using aurora::testing::require_field;

namespace {

/// 固定尺寸哑控件：布局返回构造时给定的自然尺寸（经约束钳制），绘制无副作用。
class FixedBox final : public Widget {
  public:
    FixedBox(float w, float h) : w_(w), h_(h) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = w_, .height = h_});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

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
auto frame_clock() -> std::chrono::steady_clock::time_point & {
    static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{};
    return now;
}

/// @brief 推进 n 帧（每帧 16ms），驱动 snap 收位 / scroll_to 短滑动。
auto pump(GridView &grid, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        grid.tick(frame_clock());
    }
}

/// @brief 等滑动走完：滑满时长 150ms + 余量。
auto settle(GridView &grid) -> void { pump(grid, 16); }

/// @brief 滚轮事件入口（step 为控件内部常量 40，故 1 单位 = 40dp）。
auto wheel(GridView &grid, float delta_y) -> ScrollEvent {
    ScrollEvent e;
    e.delta_y = delta_y;
    grid.on_scroll(e);
    return e;
}

/// @brief 缩放 / 冻结判据的固定帧步长：62500µs = 1/16 秒（二进制精确，跨倍率帧数可整除推导）。
constexpr auto AURORA_SCALE_FRAME_STEP = std::chrono::microseconds(62500);

/// @brief 按固定帧步长推进一帧（假想帧钟，不读墙钟）——收位滑动的逐帧入口。
template <typename WidgetT>
auto pump_scaled_frame(WidgetT &w) -> void {
    frame_clock() += AURORA_SCALE_FRAME_STEP;
    w.tick(frame_clock());
}

/// @brief 按固定帧步长推进到收位滑动结束（`is_gliding()` 转假），返回真正喂进去的帧数。
/// @tparam WidgetT 被驱动的控件类型（须有 `tick(time_point)` 与 `is_gliding()`）。
/// @param w 目标控件。
/// @param budget 帧预算上限（防呆：正常路径 1–3 帧，超预算即记账失败）。
/// @return 到位用掉的帧数；超预算时返回 `budget + 1`（并已就地记一次失败）。
template <typename WidgetT>
auto pump_until_glide_ends(WidgetT &w, int budget = 32) -> int {
    for (int n = 1; n <= budget; ++n) {
        pump_scaled_frame(w);
        if (!w.is_gliding()) {
            return n;
        }
    }
    AURORA_TEST_FAIL("snap glide never finished within the frame budget");
    return budget + 1;
}

/// @brief 到位帧数的推导模型（倍率只乘**一层**）：`ScrollGlide` 累计满 0.15 秒即刻位，每帧步长 1/16 秒。
///
/// 1x / 2x / 4x 依次为 3 / 2 / 1 帧（0.0625、0.125、0.1875 越线）。若消费点与 `scaled_frame_dt`
/// 两层都乘，倍率变成平方：2x 档每帧 0.25 秒 ⇒ 只需 1 帧，与下表的 2 帧不符。
/// @param scale `Animator::set_time_scale` 的倍率（调用方须传正值）。
/// @return 从滑动起步（elapsed 已归零）到收位所需的固定步长帧数。
constexpr auto expected_glide_frames(double scale) -> int {
    constexpr double frame_seconds = 1.0 / 16.0;  // 与 `AURORA_SCALE_FRAME_STEP` 同值的秒数
    constexpr double duration_seconds = 0.15;  // `ScrollGlide::default_duration_s()`
    double elapsed = 0.0;
    for (int n = 1; n <= 64; ++n) {
        elapsed += frame_seconds * scale;
        if (elapsed >= duration_seconds) {
            return n;
        }
    }
    return 0;
}

/// @brief 重绘请求计数器：作用域内把控件的 `on_dirty` 换成计数回调，出作用域还原原回调。
///
/// `Widget::request_frame` 一定会调用挂在控件自身的 `on_dirty`（即便 `layout_parent_` 为空），
/// 所以它是「本段里请求了多少帧重绘」的直接观测量——判据 11(b) 的「冻结不空转」就落在这个计数上。
class ScopedDirtyCounter final {
  public:
    /// @brief 接上计数回调并保存此前挂在 `on_dirty` 上的回调（通常为空）。
    /// @param w 观察目标控件（非拥有引用，须比本对象存活更久）。
    explicit ScopedDirtyCounter(Widget &w) : widget_(w), saved_(std::move(w.on_dirty)) {
        w.on_dirty = [this](bool /*layout*/) -> void { ++requests_; };
    }
    ScopedDirtyCounter(const ScopedDirtyCounter &) = delete;
    auto operator=(const ScopedDirtyCounter &) -> ScopedDirtyCounter & = delete;
    ScopedDirtyCounter(ScopedDirtyCounter &&) = delete;
    auto operator=(ScopedDirtyCounter &&) -> ScopedDirtyCounter & = delete;
    ~ScopedDirtyCounter() { widget_.on_dirty = std::move(saved_); }

    /// @brief 本作用域内累计的重绘请求数。
    /// @return `on_dirty` 被调用的次数。
    [[nodiscard]] auto count() const -> int { return requests_; }

  private:
    Widget &widget_;
    std::function<void(bool)> saved_;
    int requests_ = 0;
};

/// @brief reduce-motion 守卫：作用域内开启，离开时复原进程级设置（单例，测试须自清）。
class ReduceMotionGuard final {
  public:
    ReduceMotionGuard() : saved_(current_accessibility_settings()) {
        AccessibilitySettings s = saved_;
        s.reduce_motion = true;
        set_accessibility_settings(s);
    }
    ~ReduceMotionGuard() { set_accessibility_settings(saved_); }
    ReduceMotionGuard(const ReduceMotionGuard &) = delete;
    auto operator=(const ReduceMotionGuard &) -> ReduceMotionGuard & = delete;
    ReduceMotionGuard(ReduceMotionGuard &&) = delete;
    auto operator=(ReduceMotionGuard &&) -> ReduceMotionGuard & = delete;

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
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "count"), 9);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "columns"), 2);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "cell_extent"), 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "scroll_offset"), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "cache_extent"), 200.0F, 1e-4F);

    const auto d = GridView::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "GridView");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool columns_required = false;
    bool columns_min_one = false;
    for (const auto &p : d.properties) {
        if (p.name == "columns") {
            columns_required = p.required;
            columns_min_one = (p.min_value == "1");
        }
    }
    AURORA_TEST_CHECK_TRUE(columns_required);
    AURORA_TEST_CHECK_TRUE(columns_min_one);
    AURORA_TEST_CHECK_FALSE(d.invariants.empty());
}

AURORA_TEST_CASE(deserialize_props_restores_every_scalar) {
    // 静态 JSON 回填属性（工厂重建路径）：几何/缓存/吸附逐项还原，二次序列化幂等。
    Json props = Json::object();
    props.set("count", 30);
    props.set("columns", 3);
    props.set("cell_extent", 96.0F);
    props.set("cache_extent", 300.0F);
    props.set("scroll_offset", 192.0F);
    props.set("restore_key", "demo.gallery");
    props.set("snap_paging", Json{true});
    props.set("snap_alignment", "End");

    GridView grid;
    grid.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(grid.count(), 30);
    AURORA_TEST_CHECK_EQ(grid.columns(), 3);
    AURORA_TEST_CHECK_EQ(grid.row_count(), 10);
    AURORA_TEST_CHECK_NEAR(grid.cell_extent(), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(grid.snap().paging);
    AURORA_TEST_CHECK_TRUE(grid.snap().alignment == ScrollSnapAlignment::End);
    AURORA_TEST_CHECK_EQ(grid.restore_key(), std::string{"demo.gallery"});

    Json again = Json::object();
    grid.serialize_props(again);
    for (const char *key : {"count", "columns", "cell_extent", "cache_extent", "restore_key", "snap_paging"}) {
        AURORA_TEST_CHECK_TRUE(*require_child(again, key) == *require_child(props, key));
    }
}

AURORA_TEST_CASE(deserialize_degrades_nonpositive_geometry) {
    // 反序列化路径与构造器同一降级判据：非正列数回落 1、非正格高回落 96、负项数归零。
    Json props = Json::object();
    props.set("count", -1);
    props.set("columns", 0);
    props.set("cell_extent", 0.0F);

    GridView grid;
    grid.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(grid.count(), 0);
    AURORA_TEST_CHECK_EQ(grid.columns(), 1);
    AURORA_TEST_CHECK_EQ(grid.row_count(), 0);

    Json out = Json::object();
    grid.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "cell_extent"), 96.0F, 1e-4F);
}

AURORA_TEST_CASE(deserialized_offset_wins_over_restore_key) {
    // 树里写明的偏移是「声明的状态」，restore_key 的意义是「树没写时记住位置」⇒ 前者优先。
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("demo.grid", 480.0F);

    Json props = Json::object();
    props.set("count", 30);
    props.set("columns", 3);
    props.set("cell_extent", 96.0F);
    props.set("restore_key", "demo.grid");
    props.set("scroll_offset", 192.0F);

    GridView grid;
    grid.deserialize_props(props);
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));  // 内容 960 / 视口 300 → [0, 660]
    AURORA_TEST_CHECK_NEAR(grid.scroll_offset(), 192.0F, 1e-3F);
    AURORA_TEST_CHECK_EQ(grid.live_item_count(), static_cast<std::size_t>(0));  // 无 builder：齐备但暂无条目

    // 只声明 restore_key（无显式偏移）时，按键恢复照常生效；宿主随后挂 builder，窗口按恢复的偏移构建。
    Json keyed_only = Json::object();
    keyed_only.set("count", 30);
    keyed_only.set("columns", 3);
    keyed_only.set("cell_extent", 96.0F);
    keyed_only.set("restore_key", "demo.grid");
    BuildRecorder rec;
    GridView restored;
    restored.deserialize_props(keyed_only);
    restored.set_item_builder(rec.builder());
    LayoutEngine::layout(restored, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(restored.scroll_offset(), 480.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(restored.live_item_count() > 0U);
    AURORA_TEST_CHECK_FALSE(rec.items.contains(0));  // 偏移 480：第 0 行早已滚出
    AURORA_TEST_CHECK_TRUE(rec.items.contains(6));  // 行 2 起（含 cache_extent 缓冲）
    storage.clear_all();
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

    const ReadOnlyState<float> offset = grid.offset_signal();  // 懒创建：初值取当前偏移
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
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "snap_extent"), 288.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(require_field<bool>(props, "snap_paging"));
    AURORA_TEST_CHECK_EQ(require_field<std::string>(props, "snap_alignment"), std::string{"End"});

    // 吸附开关的可观测性：本控件以 set_snap 接线（builder 属运行时回调，from_json 不重建条目）。
    AURORA_TEST_CHECK_TRUE(src.snap().enabled(300.0F));
    GridView off{9, 3, {}, 96.0F};
    AURORA_TEST_CHECK_FALSE(off.snap().enabled(300.0F));
}

// ---- 应用内全局时间缩放：ScrollGlide 的 GridView 消费点（判据 9 / 10 / 11(b)）----

// @brief 判据 9/10（GridView 行）：收位滑动的到位帧数按倍率折半、终值逐位不变（倍率只乘一层）。
AURORA_TEST_CASE(grid_view_glide_frame_count_follows_time_scale) {
    // 推导同 utest_scroll：帧步长 1/16 秒（二进制精确）、`ScrollGlide` 缺省 0.15 秒 ⇒ 1x/2x/4x = 3/2/1 帧。
    // 首帧的裸步长是「锚点未置」时的 1/60 兜底值，与后续帧不等长会污染取整，故每档先喂一帧兑现它，
    // 再在途重定向（`scroll_to` 的 `start` 归零 elapsed、保留锚点），计数段内每帧便恰为固定步长。
    Animator anim;
    const testing::ScopedCurrentAnimator current{anim};
    GridView grid{30, 3, {}, 96.0F};  // 10 行 × 96 = 内容 960 / 视口 300 ⇒ 可滚 [0, 660]
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));
    AURORA_TEST_REQUIRE_EQ(grid.max_scroll_offset(), 660.0F);

    // 同一棵树、同一目标，只换倍率：复位 → 起滑动 → 兑现兜底帧 → 重定向 → 计数到位帧。
    auto frames_at = [&grid, &anim](double scale) -> int {
        anim.set_time_scale(scale);
        grid.scroll_to(300.0F, false);  // 即时复位（同时作废在途滑动）
        AURORA_TEST_CHECK(grid.scroll_to(660.0F));
        pump_scaled_frame(grid);
        AURORA_TEST_CHECK(grid.scroll_to(660.0F));  // 在途重定向：elapsed 归零、锚点已就位
        return pump_until_glide_ends(grid);
    };

    const int n_1x = frames_at(1.0);
    const float end_1x = grid.scroll_offset();
    const int n_2x = frames_at(2.0);
    const float end_2x = grid.scroll_offset();
    const int n_4x = frames_at(4.0);
    const float end_4x = grid.scroll_offset();

    AURORA_TEST_CHECK_EQ(n_1x, expected_glide_frames(1.0));
    AURORA_TEST_CHECK_EQ(n_2x, expected_glide_frames(2.0));
    AURORA_TEST_CHECK_EQ(n_4x, expected_glide_frames(4.0));
    AURORA_TEST_CHECK_EQ(n_1x, 3);
    // 判据 9：帧数恰为 1x 档的一半。双层缩放（倍率平方）会让 2x 档每帧吃 0.25 秒 ⇒ 一帧到位，
    // 与这里的 2 帧冲突即转红——2x 档正是抓出「两层都乘」的那一档。
    AURORA_TEST_CHECK_EQ(n_2x, (n_1x + 1) / 2);
    AURORA_TEST_CHECK_EQ(n_4x, (n_1x + 3) / 4);
    AURORA_TEST_CHECK(end_1x == end_2x && end_2x == end_4x);  // 只改步长、不改终值
    AURORA_TEST_CHECK_FALSE(grid.is_gliding());
    AURORA_TEST_CHECK_EQ(end_1x, 660.0F);  // 落位即夹取到的可滚末端

    anim.set_time_scale(1.0);  // 倍率还原后行为与原先一致（判据 9 的无残留半句）
    grid.scroll_to(300.0F, false);
    AURORA_TEST_CHECK(grid.scroll_to(660.0F));
    pump_scaled_frame(grid);
    AURORA_TEST_CHECK(grid.scroll_to(660.0F));
    AURORA_TEST_CHECK_EQ(pump_until_glide_ends(grid), n_1x);
}

// @brief 判据 11(b) + 8 的 0.0 档（GridView 行）：冻结段不推进、零重绘请求，解冻按剩余帧数续跑到同一终值。
AURORA_TEST_CASE(grid_view_glide_freeze_holds_progress_and_emits_no_redraw) {
    Animator anim;
    const testing::ScopedCurrentAnimator current{anim};
    GridView grid{30, 3, {}, 96.0F};  // 可滚 [0, 660]
    LayoutEngine::layout(grid, bounded(300.0F, 300.0F));

    AURORA_TEST_CHECK(grid.scroll_to(660.0F));
    pump_scaled_frame(grid);  // 兑现首帧的 1/60 兜底步长
    AURORA_TEST_CHECK(grid.scroll_to(660.0F));  // 在途重定向 ⇒ 其后每帧恰为 1/16 秒
    pump_scaled_frame(grid);  // 推进一帧：elapsed = 1/16 秒
    AURORA_TEST_REQUIRE_TRUE(grid.is_gliding());
    const float frozen_from = grid.scroll_offset();

    anim.set_time_scale(0.0);
    AURORA_TEST_CHECK_EQ(anim.time_scale(), 0.0);
    ScopedDirtyCounter dirty{grid};
    for (int i = 0; i < 8; ++i) {
        pump_scaled_frame(grid);
    }
    AURORA_TEST_CHECK_EQ(grid.scroll_offset(), frozen_from);  // 冻结段一步都不挪
    AURORA_TEST_CHECK_TRUE(grid.is_gliding());  // 在途标志按实现保留（不该被摘除）
    AURORA_TEST_CHECK_EQ(dirty.count(), 0);  // 判据 11(b)：停着不动也不满帧唤醒（行窗口不重建）

    anim.set_time_scale(1.0);  // 解冻重武装：进度不丢，剩余 2 帧（0.125 → 0.1875 越线）收位
    AURORA_TEST_CHECK_EQ(pump_until_glide_ends(grid), 2);
    AURORA_TEST_CHECK_GT(dirty.count(), 0);  // 对照：冻结段的 0 不是「回调没接线」
    AURORA_TEST_CHECK_FALSE(grid.is_gliding());
    AURORA_TEST_CHECK_EQ(grid.scroll_offset(), 660.0F);  // 与不冻结路径的终值逐位一致
}

}  // namespace aurora::test_cases::utest_grid_view

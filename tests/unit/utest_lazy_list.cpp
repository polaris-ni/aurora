/// 测试类型: unit
/// 目标单元: include/aurora/widget/lazy_list.h
/// 测试说明: 覆盖 LazyList——默认不变量、count/行高参数钳制与降级、按需构建仅可见窗口条目（实例复用）、
/// cache_extent 窗口、滚动偏移钳制与 scroll_to_item、滚轮步进、滚出窗口回收重建、序列化与自描述、
/// 反序列化回填标量属性（含非法值降级、显式偏移优先于 restore_key 恢复）、
/// snap/paging 收位短滑动（逐帧推进至终点对齐）、reduce-motion 直落、offset_signal 发布、滚轮余量上冒

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/scroll_storage.h"
#include "aurora/core/accessibility.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/lazy_list.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_lazy_list {

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

/// 条目构建观测器：记录构建序号并保留各条目实例（虚拟化实例被回收后仍可读其几何）。
struct BuildRecorder {
    std::map<int, std::shared_ptr<FixedBox>> items;
    std::vector<int> built_order;

    auto builder() -> LazyList::ItemBuilder {
        return [this](int index) -> Node {
            built_order.push_back(index);
            auto box = std::make_shared<FixedBox>(300.0F, 48.0F);
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

/// @brief 推进 n 帧（每帧 16ms），驱动 snap 收位 / scroll-to 短滑动。
auto pump(LazyList &list, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        list.tick(frame_clock());
    }
}

/// @brief 等滑动走完：滑满时长 150ms + 余量。
auto settle(LazyList &list) -> void { pump(list, 16); }

/// @brief 滚轮事件入口（step 为控件内部常量 40，故 1 单位 = 40dp）。
auto wheel(LazyList &list, float delta_y) -> ScrollEvent {
    ScrollEvent e;
    e.delta_y = delta_y;
    list.on_scroll(e);
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
    ReduceMotionGuard(const ReduceMotionGuard &) = delete;
    auto operator=(const ReduceMotionGuard &) -> ReduceMotionGuard & = delete;
    ReduceMotionGuard(ReduceMotionGuard &&) = delete;
    auto operator=(ReduceMotionGuard &&) -> ReduceMotionGuard & = delete;

  private:
    AccessibilitySettings saved_;
};

}  // namespace

AURORA_TEST_CASE(default_lazy_list_invariants) {
    LazyList list;
    AURORA_TEST_CHECK_EQ(std::string{list.type_name()}, "LazyList");
    AURORA_TEST_CHECK_EQ(list.count(), 0);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(list.visible_range().first, 0);
    AURORA_TEST_CHECK_EQ(list.visible_range().second, 0);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 0U);
}

AURORA_TEST_CASE(ctor_clamps_count_and_degrades_extent) {
    LazyList negative{-5, {}, 48.0F};
    AURORA_TEST_CHECK_EQ(negative.count(), 0);
    AURORA_TEST_CHECK_NEAR(negative.content_height(), 0.0F, 1e-4F);

    // 非正行高降级为默认 48（经 Diagnostics::degraded 上报）。
    LazyList degraded{5, {}, -1.0F};
    AURORA_TEST_CHECK_NEAR(degraded.content_height(), 240.0F, 1e-4F);

    LazyList normal{100, {}, 48.0F};
    AURORA_TEST_CHECK_NEAR(normal.content_height(), 4800.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_builds_only_visible_window) {
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};  // 默认 cache_extent = 200
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(list.size().height, 400.0F, 1e-4F);
    const auto [first, last] = list.visible_range();
    AURORA_TEST_CHECK_EQ(first, 0);
    AURORA_TEST_CHECK_EQ(last, 13);  // ceil((400 + 200) / 48)
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 13U);
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 13U);  // 只构建窗口内条目，而非 count=100

    // 约束不变的重复布局不重建任何条目（实例复用）。
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 13U);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 13U);
}

AURORA_TEST_CASE(cache_extent_zero_builds_exact_window) {
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(list.visible_range().first, 0);
    AURORA_TEST_CHECK_EQ(list.visible_range().second, 9);  // ceil(400 / 48)
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 9U);
    // 存活条目被条目约束（min == max = 视口宽 × 行高）强制整形。
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->size().width, 300.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(0)->size().height, 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(rec.items.at(8)->size().height, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_offset_setter_clamps_to_content) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_NEAR(list.max_scroll_offset(), 4400.0F, 1e-4F);  // 4800 - 400
    list.set_scroll_offset(-10.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    list.set_scroll_offset(99999.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-4F);
}

AURORA_TEST_CASE(scroll_to_item_aligns_index_top) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.scroll_to_item(50);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 2400.0F, 1e-4F);  // 50 * 48
    // 越界索引钳到 [0, count-1]，偏移再钳到内容范围：99*48=4752 → 4400。
    list.scroll_to_item(1000);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-4F);
    list.scroll_to_item(-3);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(wheel_scroll_steps_offset) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.set_scroll_offset(200.0F);
    // delta_y 正方向为向上滚动：offset 减小一个滚轮步进 40。
    ScrollEvent e;
    e.delta_y = 1.0F;
    list.on_scroll(e);
    AURORA_TEST_CHECK_TRUE(e.is_handled);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 160.0F, 1e-4F);
    ScrollEvent down;
    down.delta_y = -1.0F;
    list.on_scroll(down);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 200.0F, 1e-4F);
}

AURORA_TEST_CASE(scrolling_recycles_and_rebuilds_window) {
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 9U);  // 窗口 0..8

    list.set_scroll_offset(960.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 9U);  // 新窗口 20..28
    AURORA_TEST_CHECK_EQ(rec.built_order.size(), 18U);  // 旧窗口全部回收、新窗口全部新建

    // 绘制后条目落位于内容坐标 - 滚动偏移处。
    Painter p;
    p.begin(300, 400);
    list.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = 400.0F}},
               BuildContext{});
    AURORA_TEST_CHECK_NEAR(rec.items.at(20)->paint_bounds().origin.y, 0.0F, 1e-4F);  // 20*48 - 960
    AURORA_TEST_CHECK_NEAR(rec.items.at(24)->paint_bounds().origin.y, 192.0F, 1e-4F);  // 24*48 - 960
    AURORA_TEST_CHECK_NEAR(rec.items.at(24)->paint_bounds().size.height, 48.0F, 1e-4F);
}

AURORA_TEST_CASE(serialize_props_and_describe_metadata) {
    LazyList src{7, {}, 24.0F};
    src.set_cache_extent(0.0F);
    src.set_scroll_offset(96.0F);
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["count"].get<int>(), 7);
    AURORA_TEST_CHECK_NEAR(props["item_extent"].get<float>(), 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["scroll_offset"].get<float>(), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["cache_extent"].get<float>(), 0.0F, 1e-4F);

    const auto d = LazyList::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "LazyList");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "none");
    bool count_required = false;
    for (const auto &p : d.properties) {
        if (p.name == "count") {
            count_required = p.required;
        }
    }
    AURORA_TEST_CHECK_TRUE(count_required);
    AURORA_TEST_CHECK_FALSE(d.invariants.empty());
}

AURORA_TEST_CASE(deserialize_props_restores_every_scalar) {
    // 静态 JSON 回填属性（工厂重建路径）：几何/缓存/吸附逐项还原，二次序列化幂等。
    Json props;
    props["count"] = 100;
    props["item_extent"] = 48.0F;
    props["cache_extent"] = 300.0F;
    props["scroll_offset"] = 240.0F;
    props["restore_key"] = "demo.feed";
    props["snap_extent"] = 240.0F;
    props["snap_paging"] = false;
    props["snap_alignment"] = "Center";

    LazyList list;
    list.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(list.count(), 100);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 4800.0F, 1e-3F);  // count × item_extent
    AURORA_TEST_CHECK_NEAR(list.snap().extent, 240.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(list.snap().alignment == ScrollSnapAlignment::Center);
    AURORA_TEST_CHECK_EQ(list.restore_key(), std::string{"demo.feed"});

    Json again;
    list.serialize_props(again);
    for (const char *key : {"count", "item_extent", "cache_extent", "snap_extent", "restore_key"}) {
        AURORA_TEST_CHECK_TRUE(again[key] == props[key]);
    }
}

AURORA_TEST_CASE(deserialize_degrades_nonpositive_geometry) {
    // 反序列化路径与构造器同一降级判据：非正行高回落 48、负项数归零（不把非法值直写进控件）。
    Json props;
    props["count"] = -5;
    props["item_extent"] = 0.0F;
    props["cache_extent"] = -10.0F;

    LazyList list;
    list.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(list.count(), 0);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 0.0F, 1e-4F);  // count 归零 → 无内容

    Json out;
    list.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(out["item_extent"].get<float>(), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(out["cache_extent"].get<float>(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(deserialized_offset_wins_over_restore_key) {
    // 树里写明的偏移是「声明的状态」，restore_key 的意义是「树没写时记住位置」⇒ 前者优先。
    // 且重建实例在挂 builder 前不该有条目（虚拟化窗口在无 builder 时为空）。
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("demo.list", 900.0F);

    Json props;
    props["count"] = 100;
    props["item_extent"] = 48.0F;
    props["restore_key"] = "demo.list";
    props["scroll_offset"] = 240.0F;

    LazyList list;
    list.deserialize_props(props);
    LayoutEngine::layout(list, bounded(320.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 240.0F, 1e-3F);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), static_cast<std::size_t>(0));  // 无 builder：属性齐备、暂无条目

    // 只声明 restore_key（无显式偏移）时，按键恢复照常生效。
    Json keyed_only;
    keyed_only["count"] = 100;
    keyed_only["item_extent"] = 48.0F;
    keyed_only["restore_key"] = "demo.list";
    LazyList restored;
    restored.deserialize_props(keyed_only);
    LayoutEngine::layout(restored, bounded(320.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(restored.scroll_offset(), 900.0F, 1e-3F);
    storage.clear_all();
}

AURORA_TEST_CASE(snap_glide_advances_every_frame_until_row_boundary) {
    // 行高 48、snap extent 240（5 行一站）：滚轮落在 160 → 收位到 240，且必须逐帧推进到终点。
    BuildRecorder rec;
    LazyList list{100, rec.builder(), 48.0F};
    list.set_snap(ScrollSnap{.extent = 240.0F});
    list.set_cache_extent(0.0F);  // 关预取：窗口起点即偏移所在行
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    wheel(list, -4.0F);  // 4 单位 × 40dp = 160
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 160.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(list.is_gliding());

    pump(list, 1);
    AURORA_TEST_CHECK_TRUE(list.scroll_offset() > 160.0F && list.scroll_offset() < 240.0F);
    AURORA_TEST_CHECK_TRUE(list.is_gliding());  // 中间帧仍在滑动（曾在此处被作废而冻结）

    settle(list);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 240.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());
    AURORA_TEST_CHECK_TRUE(list.live_item_count() > 0U);  // 滑动帧照常虚拟化

    // 窗口随滑动位移重建（对齐点 240 → 首项 index 5 起）。
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_CHECK_EQ(list.visible_range().first, 5);
}

AURORA_TEST_CASE(snap_paging_pages_by_viewport_height_and_bounces_back) {
    LazyList list{100, {}, 48.0F};
    list.set_snap(ScrollSnap::page());  // 视口高 400 = 一页
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    wheel(list, -3.0F);  // 120 < 半页 → 回弹本页
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 120.0F, 1e-4F);
    settle(list);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);

    wheel(list, -6.0F);  // 240 > 半页 → 进下一页 400
    settle(list);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 400.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());

    // 配置读取与关闭：snap() 回读；置 extent<=0 且非分页 = 关闭，滚轮不再收位。
    AURORA_TEST_CHECK_TRUE(list.snap().paging);
    list.set_snap(ScrollSnap{});
    AURORA_TEST_CHECK_FALSE(list.snap().enabled(400.0F));
    wheel(list, -2.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 480.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());
}

AURORA_TEST_CASE(reduce_motion_snaps_without_intermediate_frames) {
    ReduceMotionGuard guard;
    LazyList list{100, {}, 48.0F};
    list.set_snap(ScrollSnap{.extent = 240.0F});
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    wheel(list, -4.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 240.0F, 1e-4F);  // 直落对齐点
    AURORA_TEST_CHECK_FALSE(list.is_gliding());

    list.scroll_to_item(30, true);  // animate=true 同样短路
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 1440.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());
}

AURORA_TEST_CASE(scroll_to_item_supports_instant_and_animated) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    list.scroll_to_item(10);  // 缺省即时
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 480.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());

    list.scroll_to_item(20, true);  // 动画：先起滑动，位置待逐帧推进
    AURORA_TEST_CHECK_TRUE(list.is_gliding());
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 480.0F, 1e-4F);
    settle(list);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 960.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.is_gliding());

    // scroll_to 同口径：越界夹到 max = 4800 - 400。
    AURORA_TEST_CHECK_TRUE(list.scroll_to(99999.0F, false));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(list.scroll_to(4400.0F, false));
}

AURORA_TEST_CASE(offset_signal_follows_wheel_glide_and_programmatic) {
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    SignalView<float> &offset = list.offset_signal();  // 懒创建：初值取当前偏移
    AURORA_TEST_CHECK_NEAR(offset.get(), 0.0F, 1e-4F);

    list.set_scroll_offset(100.0F);
    AURORA_TEST_CHECK_NEAR(offset.get(), 100.0F, 1e-4F);
    wheel(list, 1.0F);  // 向上 40dp
    AURORA_TEST_CHECK_NEAR(offset.get(), 60.0F, 1e-4F);

    list.scroll_to(400.0F);  // 滑动期逐帧发布
    pump(list, 1);
    AURORA_TEST_CHECK_TRUE(offset.get() > 60.0F && offset.get() < 400.0F);
    settle(list);
    AURORA_TEST_CHECK_NEAR(offset.get(), 400.0F, 1e-4F);
}

AURORA_TEST_CASE(wheel_margin_bubbles_up_when_clamped_at_edges) {
    // 嵌套滚动协调：端点被夹掉的量以 remaining_y 回传（单位同 delta_y，保留符号）。
    LazyList list{100, {}, 48.0F};
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    const ScrollEvent at_top = wheel(list, 5.0F);
    AURORA_TEST_CHECK_TRUE(at_top.is_handled);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(at_top.remaining_y, 5.0F, 1e-4F);

    list.set_scroll_offset(4400.0F);  // 到底
    const ScrollEvent at_bottom = wheel(list, -2.0F);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4400.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(at_bottom.remaining_y, -2.0F, 1e-4F);

    const ScrollEvent mid = wheel(list, 3.0F);  // 中途全量消费
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4280.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(mid.remaining_y, 0.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_lazy_list

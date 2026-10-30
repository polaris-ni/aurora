/// 测试类型: unit
/// 目标单元: include/aurora/widget/lazy_list.h
/// 测试说明: 覆盖 LazyList——默认不变量、count/行高参数钳制与降级、按需构建仅可见窗口条目（实例复用）、
/// cache_extent 窗口、滚动偏移钳制与 scroll_to_item、滚轮步进、滚出窗口回收重建、序列化与自描述、
/// 反序列化回填标量属性（含非法值降级、显式偏移优先于 restore_key 恢复）、
/// snap/paging 收位短滑动（逐帧推进至终点对齐）、reduce-motion 直落、offset_signal 发布、滚轮余量上冒、
/// 绘制盒与命中盒同源（对齐/非对齐滚动位下点中的条目==可见的条目）、
/// 应用内全局时间缩放的 LazyList 消费点（到位帧数按倍率折半且终值不变 / 冻结段零重绘请求且解冻续跑）

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
#include "aurora/widget/lazy_list.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_lazy_list {

using aurora::testing::require_child;
using aurora::testing::require_field;
using aurora::testing::require_value;

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

    /// 可点击条目：仅让条目进入命中链，用于「绘制盒 == 命中盒」对账。
    auto clickable_builder() -> LazyList::ItemBuilder {
        return [this](int index) -> Node {
            built_order.push_back(index);
            auto box = std::make_shared<FixedBox>(300.0F, 48.0F);
            box->set_modifier(Modifier{}.clickable([]() -> void {}));
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

// 绘制盒与命中盒必须同源：虚拟化条目的可见位置与点击落点解析出的条目一致。
// 对齐偏移（行高整数倍）与非对齐偏移（滚轮 40dp 步进的自然落点）都要成，
// 否则「看得见的那一行」与「点中的那一行」会错开若干行（TC-WIDGET-008 的断言）。
AURORA_TEST_CASE(hit_test_shares_paint_origin_across_offsets) {
    BuildRecorder rec;
    LazyList list{100, rec.clickable_builder(), 48.0F};
    list.set_cache_extent(0.0F);
    const Rect viewport{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = 400.0F}};

    int probed = 0;
    int guarded = 0;
    for (const float offset : {240.0F, 260.0F}) {
        list.set_scroll_offset(offset);
        LayoutEngine::layout(list, bounded(300.0F, 400.0F));
        Painter p;
        p.begin(300, 400);
        list.paint(p, viewport, BuildContext{});

        const int first = static_cast<int>(offset / 48.0F);
        for (int idx = first; (static_cast<float>(idx) * 48.0F) - offset < viewport.size.height; ++idx) {
            const float top = (static_cast<float>(idx) * 48.0F) - offset;
            const float probe = top + 24.0F;  // 盒中心
            if (probe < 0.0F || probe >= viewport.size.height) {
                continue;
            }
            ++probed;
            const Rect pb = rec.items.at(idx)->paint_bounds();
            AURORA_TEST_CHECK_NEAR(pb.origin.y, top, 1e-4F);
            const auto chain = list.hit_test_chain(Point{.x = 150.0F, .y = probe}, viewport, BuildContext{});
            AURORA_TEST_CHECK_FALSE(chain.empty());
            if (chain.empty()) {
                continue;
            }
            AURORA_TEST_CHECK_TRUE(chain.back().ptr == static_cast<Widget *>(rec.items.at(idx).get()));
            // 命中链记录的全局 origin 即该条目的绘制盒 origin（同一原点，无行高/内缩错位）
            AURORA_TEST_CHECK_NEAR(chain.back().origin.y, pb.origin.y, 1e-4F);
            AURORA_TEST_CHECK_NEAR(chain.back().origin.x, pb.origin.x, 1e-4F);
            if (idx > first) {
                // 上沿外 1dp 属前一行，不得解析到本条目
                ++guarded;
                const auto above = list.hit_test_chain(Point{.x = 150.0F, .y = top - 1.0F}, viewport, BuildContext{});
                auto *item = static_cast<Widget *>(rec.items.at(idx).get());
                AURORA_TEST_CHECK_TRUE(above.empty() || above.back().ptr != item);
            }
        }
    }
    // 防空转：两处偏移都必须真正探到若干整行，且都做过「上一行边界」反证
    AURORA_TEST_CHECK(probed >= 16);
    AURORA_TEST_CHECK(guarded >= 14);
}

AURORA_TEST_CASE(serialize_props_and_describe_metadata) {
    LazyList src{7, {}, 24.0F};
    src.set_cache_extent(0.0F);
    src.set_scroll_offset(96.0F);
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<int>(props, "count"), 7);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "item_extent"), 24.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "scroll_offset"), 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "cache_extent"), 0.0F, 1e-4F);

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
    Json props = Json::object();
    props.set("count", 100);
    props.set("item_extent", 48.0F);
    props.set("cache_extent", 300.0F);
    props.set("scroll_offset", 240.0F);
    props.set("restore_key", "demo.feed");
    props.set("snap_extent", 240.0F);
    props.set("snap_paging", Json{false});
    props.set("snap_alignment", "Center");

    LazyList list;
    list.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(list.count(), 100);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 4800.0F, 1e-3F);  // count × item_extent
    AURORA_TEST_CHECK_NEAR(list.snap().extent, 240.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(list.snap().alignment == ScrollSnapAlignment::Center);
    AURORA_TEST_CHECK_EQ(list.restore_key(), std::string{"demo.feed"});

    Json again = Json::object();
    list.serialize_props(again);
    for (const char *key : {"count", "item_extent", "cache_extent", "snap_extent", "restore_key"}) {
        AURORA_TEST_CHECK_TRUE(*require_child(again, key) == *require_child(props, key));
    }
}

AURORA_TEST_CASE(deserialize_degrades_nonpositive_geometry) {
    // 反序列化路径与构造器同一降级判据：非正行高回落 48、负项数归零（不把非法值直写进控件）。
    Json props = Json::object();
    props.set("count", -5);
    props.set("item_extent", 0.0F);
    props.set("cache_extent", -10.0F);

    LazyList list;
    list.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(list.count(), 0);
    AURORA_TEST_CHECK_NEAR(list.content_height(), 0.0F, 1e-4F);  // count 归零 → 无内容

    Json out = Json::object();
    list.serialize_props(out);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "item_extent"), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(out, "cache_extent"), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(deserialized_offset_wins_over_restore_key) {
    // 树里写明的偏移是「声明的状态」，restore_key 的意义是「树没写时记住位置」⇒ 前者优先。
    // 且重建实例在挂 builder 前不该有条目（虚拟化窗口在无 builder 时为空）。
    auto &storage = ScrollStorage::instance();
    storage.clear_all();
    storage.write("demo.list", 900.0F);

    Json props = Json::object();
    props.set("count", 100);
    props.set("item_extent", 48.0F);
    props.set("restore_key", "demo.list");
    props.set("scroll_offset", 240.0F);

    LazyList list;
    list.deserialize_props(props);
    LayoutEngine::layout(list, bounded(320.0F, 200.0F));
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 240.0F, 1e-3F);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), static_cast<std::size_t>(0));  // 无 builder：属性齐备、暂无条目

    // 只声明 restore_key（无显式偏移）时，按键恢复照常生效。
    Json keyed_only = Json::object();
    keyed_only.set("count", 100);
    keyed_only.set("item_extent", 48.0F);
    keyed_only.set("restore_key", "demo.list");
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

    const ReadOnlyState<float> offset = list.offset_signal();  // 懒创建：初值取当前偏移
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

// ---- 运行期改条目数（set_count）----
AURORA_TEST_CASE(set_count_keeps_surviving_items_and_recycles_rest) {
    // 口径：下标仍在新范围内的存活条目**不重建**（状态保持），越界条目立即回收。
    // 观测手段：`BuildRecorder` 持有每个条目实例的强引用，故地址比对可判「同一实例」；
    // 存活集用 `live_item_count()` + 构建序列（`built_order`）间接钉住——
    // LazyList 的 `child_nodes()` 恒空（虚拟化条目不经children_ 暴露），故不用它做判据。
    BuildRecorder rec;
    LazyList list(100, rec.builder(), 48.0F);
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    AURORA_TEST_REQUIRE(rec.items.size() > 1U);

    const std::size_t live_before = list.live_item_count();
    const std::size_t built_before = rec.built_order.size();
    // 记录改count 前已构建条目的实例地址。
    std::map<int, FixedBox *> addr_before;
    for (const auto &kv : rec.items) {
        addr_before.emplace(kv.first, kv.second.get());
    }

    list.set_count(60);
    AURORA_TEST_CHECK_EQ(list.count(), 60);
    // 回收：视口（400dp ≈ 8.3 行）内的存活条目数不得超过新count。
    AURORA_TEST_CHECK(list.live_item_count() <= static_cast<std::size_t>(60));
    // 缩count 后重排：已构建条目不重复构建（状态保持的直接证据）。
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    for (std::size_t i = built_before; i < rec.built_order.size(); ++i) {
        // 新构建只能来自先前未构建的下标；同一实例不得重建。
        AURORA_TEST_CHECK(!addr_before.contains(rec.built_order[i]));
    }
    AURORA_TEST_CHECK(live_before > 0U);
}

AURORA_TEST_CASE(set_count_clamps_offset_to_new_content_range) {
    // 口径：条目数减少后偏移越界须按新范围夹取，否则列表停在内容末尾之外的空白处。
    BuildRecorder rec;
    LazyList list(100, rec.builder(), 48.0F);
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.set_scroll_offset(4000.0F);  // 内容高4800 − 视口 400 = 4400，故 4000 合法
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 4000.0F, 1e-4F);

    list.set_count(20);  // 新内容高 960 ⇒ 可滚上限 560
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), list.max_scroll_offset(), 1e-4F);
    AURORA_TEST_CHECK(list.scroll_offset() <= 560.0F + 1e-3F);
}

AURORA_TEST_CASE(set_count_treats_negative_as_zero) {
    // 口径：负值按 0 处理（与带参构造、deserialize_props 同一口径，不分叉）。
    BuildRecorder rec;
    LazyList list(10, rec.builder(), 48.0F);
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    list.set_count(-5);
    AURORA_TEST_CHECK_EQ(list.count(), 0);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), 0U);
    AURORA_TEST_CHECK_NEAR(list.scroll_offset(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(set_count_grow_rebuilds_new_items_only) {
    // 口径：扩容只构建新进入窗口的条目，已有存活条目不重复构建。
    BuildRecorder rec;
    LazyList list(5, rec.builder(), 48.0F);
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    const int built_before = static_cast<int>(rec.built_order.size());
    AURORA_TEST_CHECK(built_before > 0);

    list.set_count(8);
    AURORA_TEST_CHECK_EQ(list.count(), 8);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    // 新构建的下标皆落在新增区间 [5, 8)。
    for (auto i = static_cast<std::size_t>(built_before); i < rec.built_order.size(); ++i) {
        AURORA_TEST_CHECK(rec.built_order[i] >= 5);
        AURORA_TEST_CHECK(rec.built_order[i] < 8);
    }
}

AURORA_TEST_CASE(set_count_is_idempotent_on_same_value) {
    // 同值不改变任何状态（不回收、不标脏）。
    BuildRecorder rec;
    LazyList list(10, rec.builder(), 48.0F);
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));
    const std::size_t live_before = list.live_item_count();
    list.set_count(10);
    AURORA_TEST_CHECK_EQ(list.live_item_count(), live_before);
    AURORA_TEST_CHECK_EQ(list.count(), 10);
}

// ---- VerticalViewSize 数据源（LazyList） ----
AURORA_TEST_CASE(vertical_view_size_reports_visible_fraction_of_content) {
    // 判据①（数据源，LazyList 源）：视口 100 / 内容 400（10 行 × 40） ⇒ 25%。
    BuildRecorder rec;
    LazyList list{10, rec.builder(), 40.0F};
    LayoutEngine::layout(list, bounded(300.0F, 100.0F));
    const auto range = require_value(list.accessibility_scroll());
    AURORA_TEST_CHECK_NEAR(range.viewport, 100.0, 1e-3);
    AURORA_TEST_CHECK_NEAR(range.content, 400.0, 1e-3);
    AURORA_TEST_CHECK_NEAR(aurora::compute_vertical_view_size(range), 25.0, 1e-6);
}

// ---- 应用内全局时间缩放：ScrollGlide 的 LazyList 消费点（判据 9 / 10 / 11(b)）----

// @brief 判据 9/10（LazyList 行）：收位滑动的到位帧数按倍率折半、终值逐位不变（倍率只乘一层）。
AURORA_TEST_CASE(lazy_list_glide_frame_count_follows_time_scale) {
    // 推导同 utest_scroll：帧步长 1/16 秒（二进制精确）、`ScrollGlide` 缺省 0.15 秒 ⇒ 1x/2x/4x = 3/2/1 帧。
    // 首帧的裸步长是「锚点未置」时的 1/60 兜底值，与后续帧不等长会污染取整，故每档先喂一帧兑现它，
    // 再在途重定向（`scroll_to` 的 `start` 归零 elapsed、保留锚点），计数段内每帧便恰为固定步长。
    Animator anim;
    const testing::ScopedCurrentAnimator current{anim};
    LazyList list{100, {}, 48.0F};  // 内容 4800 / 视口 400 ⇒ 可滚 [0, 4400]
    list.set_cache_extent(0.0F);  // 关预取：滑动帧只做窗口重建，不掺入额外脏源
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    // 同一棵树、同一目标，只换倍率：复位 → 起滑动 → 兑现兜底帧 → 重定向 → 计数到位帧。
    auto frames_at = [&list, &anim](double scale) -> int {
        anim.set_time_scale(scale);
        list.scroll_to(2000.0F, false);  // 即时复位（同时作废在途滑动）
        AURORA_TEST_CHECK(list.scroll_to(4400.0F));
        pump_scaled_frame(list);
        AURORA_TEST_CHECK(list.scroll_to(4400.0F));  // 在途重定向：elapsed 归零、锚点已就位
        return pump_until_glide_ends(list);
    };

    const int n_1x = frames_at(1.0);
    const float end_1x = list.scroll_offset();
    const int n_2x = frames_at(2.0);
    const float end_2x = list.scroll_offset();
    const int n_4x = frames_at(4.0);
    const float end_4x = list.scroll_offset();

    AURORA_TEST_CHECK_EQ(n_1x, expected_glide_frames(1.0));
    AURORA_TEST_CHECK_EQ(n_2x, expected_glide_frames(2.0));
    AURORA_TEST_CHECK_EQ(n_4x, expected_glide_frames(4.0));
    AURORA_TEST_CHECK_EQ(n_1x, 3);
    // 判据 9：帧数恰为 1x 档的一半。双层缩放（倍率平方）会让 2x 档每帧吃 0.25 秒 ⇒ 一帧到位，
    // 与这里的 2 帧冲突即转红——2x 档正是抓出「两层都乘」的那一档。
    AURORA_TEST_CHECK_EQ(n_2x, (n_1x + 1) / 2);
    AURORA_TEST_CHECK_EQ(n_4x, (n_1x + 3) / 4);
    AURORA_TEST_CHECK(end_1x == end_2x && end_2x == end_4x);  // 只改步长、不改终值
    AURORA_TEST_CHECK_FALSE(list.is_gliding());
    AURORA_TEST_CHECK_EQ(end_1x, 4400.0F);  // 落位即夹取到的可滚末端

    anim.set_time_scale(1.0);  // 倍率还原后行为与原先一致（判据 9 的无残留半句）
    list.scroll_to(2000.0F, false);
    AURORA_TEST_CHECK(list.scroll_to(4400.0F));
    pump_scaled_frame(list);
    AURORA_TEST_CHECK(list.scroll_to(4400.0F));
    AURORA_TEST_CHECK_EQ(pump_until_glide_ends(list), n_1x);
}

// @brief 判据 11(b) + 8 的 0.0 档（LazyList 行）：冻结段不推进、零重绘请求，解冻按剩余帧数续跑到同一终值。
AURORA_TEST_CASE(lazy_list_glide_freeze_holds_progress_and_emits_no_redraw) {
    Animator anim;
    const testing::ScopedCurrentAnimator current{anim};
    LazyList list{100, {}, 48.0F};  // 可滚 [0, 4400]
    list.set_cache_extent(0.0F);
    LayoutEngine::layout(list, bounded(300.0F, 400.0F));

    AURORA_TEST_CHECK(list.scroll_to(4400.0F));
    pump_scaled_frame(list);  // 兑现首帧的 1/60 兜底步长
    AURORA_TEST_CHECK(list.scroll_to(4400.0F));  // 在途重定向 ⇒ 其后每帧恰为 1/16 秒
    pump_scaled_frame(list);  // 推进一帧：elapsed = 1/16 秒
    AURORA_TEST_REQUIRE_TRUE(list.is_gliding());
    const float frozen_from = list.scroll_offset();

    anim.set_time_scale(0.0);
    AURORA_TEST_CHECK_EQ(anim.time_scale(), 0.0);
    ScopedDirtyCounter dirty{list};
    for (int i = 0; i < 8; ++i) {
        pump_scaled_frame(list);
    }
    AURORA_TEST_CHECK_EQ(list.scroll_offset(), frozen_from);  // 冻结段一步都不挪
    AURORA_TEST_CHECK_TRUE(list.is_gliding());  // 在途标志按实现保留（不该被摘除）
    AURORA_TEST_CHECK_EQ(dirty.count(), 0);  // 判据 11(b)：停着不动也不满帧唤醒（虚拟化窗口不重建）

    anim.set_time_scale(1.0);  // 解冻重武装：进度不丢，剩余 2 帧（0.125 → 0.1875 越线）收位
    AURORA_TEST_CHECK_EQ(pump_until_glide_ends(list), 2);
    AURORA_TEST_CHECK_GT(dirty.count(), 0);  // 对照：冻结段的 0 不是「回调没接线」
    AURORA_TEST_CHECK_FALSE(list.is_gliding());
    AURORA_TEST_CHECK_EQ(list.scroll_offset(), 4400.0F);  // 与不冻结路径的终值逐位一致
}

}  // namespace aurora::test_cases::utest_lazy_list

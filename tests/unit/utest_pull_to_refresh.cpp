/// 测试类型: unit
/// 目标单元: include/aurora/widget/pull_to_refresh.h
/// 测试说明: 下拉刷新容器——滚轮余量消费与「子树在顶部」门控、刷新中冻结滚轮、
/// 橡皮筋双曲阻尼（近 1:1 起段、渐近 max_pull 不越界、拖满 max_pull 恰达缺省阈值）、
/// 拖拽劫持与松手裁决（达阈值触发 on_refresh / 未达阈值回弹归零）、驻留高度与 finish_refresh 收拢、
/// reduce-motion 直落端点、指示器覆盖层像素可见性、describe/序列化往返

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "aurora/core/accessibility.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/pull_to_refresh.h"
#include "aurora/widget/scroll.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_pull_to_refresh {

namespace {

/// 纯色哑控件：整盒填红，用于观测顶部覆盖层是否真的盖住了内容。
class RedBox final : public Widget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "RedBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 200.0F, .height = 200.0F});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, Color{255, 0, 0, 255});
    }
};

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// 堆上持有容器（Widget 不可移动赋值），刷新次数用 shared_ptr 计数供回调自增。
struct Tree {
    std::unique_ptr<PullToRefresh> ptr;
    std::shared_ptr<int> refreshes;
};

/// 无滚动子树的下拉容器：child_at_top() 恒真，专测拖拽/滚轮通道本身。
auto make_bare(float threshold = 64.0F, float max_pull = 128.0F) -> Tree {
    auto refreshes = std::make_shared<int>(0);
    auto ptr = std::make_unique<PullToRefresh>(
        PullToRefreshProps{.child = Node{std::make_shared<RedBox>()}, .threshold = threshold, .max_pull = max_pull});
    ptr->on_refresh([refreshes] { ++(*refreshes); });
    LayoutEngine::layout(*ptr, bounded(200.0F, 200.0F));
    return Tree{.ptr = std::move(ptr), .refreshes = std::move(refreshes)};
}

/// 单调递增的假想帧钟：控件 tick 取墙钟差，测试须保证时间不回拨（否则 dt 为负、glide 倒退）。
auto frame_clock() -> std::chrono::steady_clock::time_point & {
    static std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    return t;
}

/// 以 16ms 步长推进 `frames` 帧（模拟渲染循环逐帧 tick）。
auto pump(PullToRefresh &ptr, int frames) -> void {
    for (int i = 0; i < frames; ++i) {
        frame_clock() += std::chrono::milliseconds(16);
        ptr.tick(frame_clock());
    }
}

/// 推进足够帧数让回弹/收拢 glide 收敛（默认时长 150ms → 16 帧 = 256ms 有余）。
auto settle(PullToRefresh &ptr) -> void { pump(ptr, 16); }

/// 合成并派发一个鼠标事件（返回是否命中）。
auto send(EventDispatcher &d, PullToRefresh &ptr, MouseAction action, float x, float y) -> bool {
    MouseEvent e;
    e.action = action;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};
    return d.dispatch_mouse(ptr, e);
}

/// 喂一个滚轮增量（delta_y 以滚轮单位计）。
auto wheel(PullToRefresh &ptr, float delta_y) -> ScrollEvent {
    ScrollEvent e;
    e.delta_y = delta_y;
    ptr.on_scroll(e);
    return e;
}

}  // namespace

AURORA_TEST_CASE(defaults_and_describe) {
    const PullToRefresh d;
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, std::string{"PullToRefresh"});
    AURORA_TEST_CHECK_NEAR(d.threshold, 64.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.max_pull, 128.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.pull_distance(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d.progress(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(d.is_gliding());
    AURORA_TEST_CHECK_EQ(static_cast<int>(d.state()), static_cast<int>(PullToRefreshState::Idle));
    // 包装层本身不向读屏透传滚动 range（读屏直接驱动内部滚动子树）
    AURORA_TEST_CHECK_FALSE(d.accessibility_scroll().has_value());

    const auto desc = PullToRefresh::describe_static();
    AURORA_TEST_CHECK_EQ(desc.name, std::string{"PullToRefresh"});
    AURORA_TEST_CHECK_EQ(desc.children_policy, std::string{"single"});
    AURORA_TEST_CHECK_EQ(desc.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(desc.events[0], std::string{"on_refresh"});
    bool found_threshold = false;
    for (const auto &prop : desc.properties) {
        if (prop.name == "threshold") {
            found_threshold = true;
            AURORA_TEST_CHECK_EQ(prop.default_value, std::string{"64.0"});
        }
    }
    AURORA_TEST_CHECK_TRUE(found_threshold);
}

AURORA_TEST_CASE(threshold_and_max_pull_serialize_round_trip) {
    PullToRefresh src;
    src.threshold = 40.0F;
    src.max_pull = 90.0F;
    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(props["threshold"].get<float>(), 40.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(props["max_pull"].get<float>(), 90.0F, 1e-4F);

    PullToRefresh dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_NEAR(dst.threshold, 40.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(dst.max_pull, 90.0F, 1e-4F);
}

AURORA_TEST_CASE(wheel_margin_pulls_and_rubber_bands_are_capped) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;

    // 2 个滚轮单位 × 缺省 step 16dp = 32dp 物理位移 → 阻尼后 32·128/(32+128) = 25.6dp
    const ScrollEvent a = wheel(ptr, 2.0F);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 25.6F, 1e-2F);
    AURORA_TEST_CHECK_NEAR(ptr.progress(), 25.6F / 64.0F, 1e-3F);
    AURORA_TEST_CHECK_TRUE(a.is_handled);
    AURORA_TEST_CHECK_NEAR(a.remaining_y, 0.0F, 1e-6F);  // 余量被吃掉，不再继续上冒
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Pulling));

    // 余量按单位累加，且被 max_pull 硬截顶（橡皮筋渐近值亦不越界）
    for (int i = 0; i < 20; ++i) {
        wheel(ptr, 30.0F);
    }
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() <= ptr.max_pull + 1e-4F);
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() > 100.0F);

    // 负增量（向下滚、露出下方内容）不属下拉语义：原样放行、不改状态
    const float before = ptr.pull_distance();
    const ScrollEvent down = wheel(ptr, -3.0F);
    AURORA_TEST_CHECK_FALSE(down.is_handled);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), before, 1e-4F);
}

AURORA_TEST_CASE(wheel_channel_gated_by_child_scroll_position) {
    auto scroll = std::make_shared<Scroll>();
    scroll->add(Node{std::make_shared<RedBox>()});  // 内容自然高 200
    PullToRefresh ptr{
        PullToRefreshProps{.child = Node{scroll}, .threshold = 64.0F, .max_pull = 128.0F}};
    int refreshes = 0;
    ptr.on_refresh([&refreshes] { ++refreshes; });
    LayoutEngine::layout(ptr, bounded(200.0F, 100.0F));  // 视口高 100 < 内容 200 → 可滚 100

    // 子树在顶部：正增量归本容器 → 下拉
    const ScrollEvent top = wheel(ptr, 1.0F);
    AURORA_TEST_CHECK_TRUE(top.is_handled);
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() > 0.0F);

    // 子树已离开顶部：正增量应由子级吃掉，本容器不再劫持
    AURORA_TEST_CHECK_TRUE(scroll->set_offset(80.0F));
    const float held = ptr.pull_distance();
    const ScrollEvent mid = wheel(ptr, 1.0F);
    AURORA_TEST_CHECK_FALSE(mid.is_handled);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), held, 1e-4F);
    AURORA_TEST_CHECK_EQ(refreshes, 0);
}

AURORA_TEST_CASE(refreshing_state_freezes_wheel) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;

    // 拖过大行程后松手 → 进入 Refreshing（驻留半阈值）
    send(d, ptr, MouseAction::Press, 100.0F, 20.0F);
    send(d, ptr, MouseAction::Move, 100.0F, 180.0F);
    send(d, ptr, MouseAction::Release, 100.0F, 180.0F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Refreshing));
    const float parked = ptr.pull_distance();

    // 刷新中冻结：滚轮既不拉伸下拉也不消费事件
    const ScrollEvent frozen = wheel(ptr, 10.0F);
    AURORA_TEST_CHECK_FALSE(frozen.is_handled);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), parked, 1e-6F);

    // 刷新完成后收拢：回到 Idle，回调不会被重复触发
    ptr.finish_refresh();
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Idle));
    AURORA_TEST_CHECK_EQ(*t.refreshes, 1);
}

AURORA_TEST_CASE(drag_hijacks_past_threshold_then_refreshes_and_settles) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;

    AURORA_TEST_CHECK_TRUE(send(d, ptr, MouseAction::Press, 100.0F, 20.0F));
    // 竖向拖过 slop（8dp）后锁主轴；160dp 行程 → 阻尼后 71.1dp > 阈值 64
    send(d, ptr, MouseAction::Move, 100.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() > 0.0F);
    send(d, ptr, MouseAction::Move, 100.0F, 180.0F);
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() >= ptr.threshold);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Pulling));

    send(d, ptr, MouseAction::Release, 100.0F, 180.0F);
    AURORA_TEST_CHECK_EQ(*t.refreshes, 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Refreshing));
    AURORA_TEST_CHECK_TRUE(ptr.is_gliding());

    // 驻留高度 = min(pull, 半阈值)：glide 收敛后停在那里转圈，不回零
    settle(ptr);
    AURORA_TEST_CHECK_FALSE(ptr.is_gliding());
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 32.0F, 1e-3F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Refreshing));

    // 数据回来后收拢：回弹到 0 并回到 Idle（不再触发回调）
    ptr.finish_refresh();
    settle(ptr);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Idle));
    AURORA_TEST_CHECK_FALSE(ptr.is_gliding());
    AURORA_TEST_CHECK_EQ(*t.refreshes, 1);
}

AURORA_TEST_CASE(short_drag_bounces_back_without_refresh) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;

    send(d, ptr, MouseAction::Press, 100.0F, 100.0F);
    send(d, ptr, MouseAction::Move, 100.0F, 120.0F);  // 20dp → 阻尼后 17.3dp，未达阈值
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), (20.0F * 128.0F) / 148.0F, 1e-2F);
    send(d, ptr, MouseAction::Release, 100.0F, 120.0F);

    AURORA_TEST_CHECK_EQ(*t.refreshes, 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Idle));
    settle(ptr);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(hijacked_gesture_follows_back_and_releases_quietly) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;

    send(d, ptr, MouseAction::Press, 100.0F, 20.0F);
    send(d, ptr, MouseAction::Move, 100.0F, 120.0F);  // 劫持：100dp → 阻尼后 80dp
    const float peak = ptr.pull_distance();
    AURORA_TEST_CHECK_NEAR(peak, (100.0F * 128.0F) / 228.0F, 1e-2F);

    // 回落段仍跟手（允许拉回更低），手势仍归下拉所有
    send(d, ptr, MouseAction::Move, 100.0F, 60.0F);
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() < peak);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), (40.0F * 128.0F) / 168.0F, 1e-2F);

    send(d, ptr, MouseAction::Release, 100.0F, 60.0F);
    AURORA_TEST_CHECK_EQ(*t.refreshes, 0);  // 未达阈值：静默回弹
    settle(ptr);
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 0.0F, 1e-4F);
}

AURORA_TEST_CASE(reduce_motion_drops_setback_without_frames) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;
    send(d, ptr, MouseAction::Press, 100.0F, 20.0F);
    send(d, ptr, MouseAction::Move, 100.0F, 180.0F);
    send(d, ptr, MouseAction::Release, 100.0F, 180.0F);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Refreshing));
    AURORA_TEST_CHECK_TRUE(ptr.is_gliding());  // 默认走短滑动收拢

    auto saved = current_accessibility_settings();
    saved.reduce_motion = true;
    set_accessibility_settings(saved);

    ptr.finish_refresh();
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 0.0F, 1e-6F);  // 直落端点，不产生中间帧
    AURORA_TEST_CHECK_FALSE(ptr.is_gliding());
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Idle));

    set_accessibility_settings(saved);  // 单例须自行复原
}

AURORA_TEST_CASE(indicator_overlay_paints_over_content) {
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;

    constexpr BuildContext ctx;
    const Rect view{.origin = Point{}, .size = Size{.width = 200.0F, .height = 200.0F}};

    Painter clean;
    clean.begin(200, 200);
    ptr.paint(clean, view, ctx);
    const Color bare = clean.get_pixel(100, 5);  // 无下拉：顶部即内容红
    AURORA_TEST_CHECK_EQ(static_cast<int>(bare.r), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(bare.g), 0);

    wheel(ptr, 10.0F);  // 160dp 行程 → 71dp 指示器带高
    AURORA_TEST_CHECK_TRUE(ptr.pull_distance() > 60.0F);

    Painter covered;
    covered.begin(200, 200);
    ptr.paint(covered, view, ctx);
    const Color over = covered.get_pixel(100, 5);  // 带内：92% 白压红 → 高绿蓝
    AURORA_TEST_CHECK_TRUE(over.g > 200);
    AURORA_TEST_CHECK_TRUE(over.b > 200);
    // 带外仍是内容原色：覆盖层不改布局盒、不推挤内容
    const Color below = covered.get_pixel(100, 150);
    AURORA_TEST_CHECK_EQ(static_cast<int>(below.r), 255);
    AURORA_TEST_CHECK_EQ(static_cast<int>(below.g), 0);
}

}  // namespace aurora::test_cases::utest_pull_to_refresh

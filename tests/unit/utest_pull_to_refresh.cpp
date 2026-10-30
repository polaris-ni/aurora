/// 测试类型: unit
/// 目标单元: include/aurora/widget/pull_to_refresh.h
/// 测试说明: 下拉刷新容器——滚轮余量消费与「子树在顶部」门控、刷新中冻结滚轮、
/// 橡皮筋双曲阻尼（近 1:1 起段、渐近 max_pull 不越界、拖满 max_pull 恰达缺省阈值）、
/// 拖拽劫持与松手裁决（达阈值触发 on_refresh / 未达阈值回弹归零）、驻留高度与 finish_refresh 收拢、
/// reduce-motion 直落端点、指示器覆盖层像素可见性、describe/序列化往返、
/// 应用内全局时间缩放的 PullToRefresh 消费点（回弹到位帧数按倍率折半且驻留高度不变 /
/// 冻结段零重绘请求且解冻续跑）

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/core/accessibility.h"
#include "aurora/environment/environment.h"
#include "aurora/event/dispatcher.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/theming/theme.h"
#include "aurora/widget/pull_to_refresh.h"
#include "aurora/widget/scroll.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_pull_to_refresh {
using aurora::testing::require_field;

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

/// @brief 缩放 / 冻结判据的固定帧步长：62500µs = 1/16 秒（二进制精确，跨倍率帧数可整除推导）。
constexpr auto AURORA_SCALE_FRAME_STEP = std::chrono::microseconds(62500);

/// @brief 按固定帧步长推进一帧（假想帧钟，不读墙钟）——回弹滑动的逐帧入口。
template <typename WidgetT>
auto pump_scaled_frame(WidgetT &w) -> void {
    frame_clock() += AURORA_SCALE_FRAME_STEP;
    w.tick(frame_clock());
}

/// @brief 按固定帧步长推进到回弹滑动结束（`is_gliding()` 转假），返回真正喂进去的帧数。
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
    AURORA_TEST_FAIL("setback glide never finished within the frame budget");
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

/// @brief 一次完整下拉手势：`y0` 起按、拖到 `y1`、原地松手（裁决按既有语义走）。
/// @param d 事件派发器（与既有拖拽用例同形，可跨手势复用）。
/// @param ptr 目标容器。
/// @param y0 起按纵坐标（dp）。
/// @param y1 松手纵坐标（dp）；与 `y0` 之差即物理行程。
/// @return 三个事件里是否至少有一个被接管。
auto drag(EventDispatcher &d, PullToRefresh &ptr, float y0, float y1) -> bool {
    bool handled = send(d, ptr, MouseAction::Press, 100.0F, y0);
    handled = send(d, ptr, MouseAction::Move, 100.0F, y1) || handled;
    return send(d, ptr, MouseAction::Release, 100.0F, y1) || handled;
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
    Json props = Json::object();
    src.serialize_props(props);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "threshold"), 40.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(require_field<float>(props, "max_pull"), 90.0F, 1e-4F);

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
    PullToRefresh ptr{PullToRefreshProps{.child = Node{scroll}, .threshold = 64.0F, .max_pull = 128.0F}};
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

    const auto saved = current_accessibility_settings();  // 原值留作恢复源（勿污染副本）
    auto reduced = saved;
    reduced.reduce_motion = true;
    set_accessibility_settings(reduced);

    ptr.finish_refresh();
    AURORA_TEST_CHECK_NEAR(ptr.pull_distance(), 0.0F, 1e-6F);  // 直落端点，不产生中间帧
    AURORA_TEST_CHECK_FALSE(ptr.is_gliding());
    AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Idle));

    set_accessibility_settings(saved);  // 单例须自行复原（reduce_motion 是进程级全局，泄漏会级联打红后续文件）
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

// ---- 主题跟随（迁移批）：未显式设色 → 角色回退；显式设色 → 不被主题覆盖 ----

AURORA_TEST_CASE(pull_to_refresh_colors_follow_theme_when_unset) {
    // 两态语义：未显式设色时 resolve 取主题角色；主题翻转 ⇒ 解析值翻转（换肤生效）。
    const BuildContext bare;  // 无注入：inherit_theme 回落 light（与 OS 未表态同口径）
    PullToRefresh ptr{PullToRefreshProps{.child = Node{std::make_shared<RedBox>()}}};
    AURORA_TEST_CHECK(ptr.resolve_indicator_color(bare) == Color::blue());  // light primary = (0,0,255)
    AURORA_TEST_CHECK(ptr.resolve_band_color(bare) == Color::white());  // light surface = white
    AURORA_TEST_CHECK(ptr.resolve_refreshing_text_color(bare) == Color{0x47, 0x47, 0x47});  // light on_surface_variant

    const auto [env, dark] = themed_ctx(Theme::dark());
    AURORA_TEST_CHECK(ptr.resolve_indicator_color(dark) == Color{0xC0, 0xC1, 0xFF});  // dark primary
    AURORA_TEST_CHECK(ptr.resolve_band_color(dark) == Color{0x20, 0x21, 0x24});  // dark surface
    AURORA_TEST_CHECK(ptr.resolve_refreshing_text_color(dark) == Color{0xC6, 0xC6, 0xC6});  // dark on_surface_variant
}

AURORA_TEST_CASE(pull_to_refresh_explicit_colors_override_theme) {
    // 对照组：显式设色不被主题覆盖（两态语义的另一半）。
    const auto [env, dark] = themed_ctx(Theme::dark());
    PullToRefresh ptr{PullToRefreshProps{.child = Node{std::make_shared<RedBox>()}}};
    ptr.indicator_color(Color{0xE0, 0x1B, 0x24});  // error 红，刻意区别于两档 primary
    ptr.band_color(Color{0x12, 0x34, 0x56});
    ptr.refreshing_text_color(Color{0x77, 0x88, 0x99});
    AURORA_TEST_CHECK(ptr.resolve_indicator_color(dark) == Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK(ptr.resolve_band_color(dark) == Color{0x12, 0x34, 0x56});
    AURORA_TEST_CHECK(ptr.resolve_refreshing_text_color(dark) == Color{0x77, 0x88, 0x99});
}

// ---- 应用内全局时间缩放：ScrollGlide 的 PullToRefresh 消费点（判据 9 / 10 / 11(b)）----

// @brief 判据 9/10（PullToRefresh 行）：回弹滑动的到位帧数按倍率折半、驻留高度逐位不变（倍率只乘一层）。
AURORA_TEST_CASE(pull_to_refresh_setback_frame_count_follows_time_scale) {
    // 推导同 utest_scroll：帧步长 1/16 秒（二进制精确）、`ScrollGlide` 缺省 0.15 秒 ⇒ 1x/2x/4x = 3/2/1 帧。
    // 首帧的裸步长是「锚点未置」时的 1/60 兜底值，与后续帧不等长会污染取整，故每档先用一次**未达阈值**
    // 的短回弹（目标 0）喂一帧把兜底兑现掉，再在途重定向为「拖过阈值 ⇒ 回弹到驻留高度 32」
    // （`begin_setback_glide` 的 `start` 归零 elapsed、保留 `last_tick_` 锚点），计数段内每帧恰为固定步长。
    Animator anim;
    const testing::ScopedCurrentAnimator current{anim};
    auto t = make_bare();  // threshold 64 / max_pull 128 ⇒ 驻留高度 min(pull, 32) = 32
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;

    // 每档返回 {到位帧数, 收位终值}：终值必须在收拢（finish_refresh）之前取，否则一律是 0。
    auto frames_at = [&ptr, &d, &anim](double scale) -> std::pair<int, float> {
        anim.set_time_scale(scale);
        drag(d, ptr, 100.0F, 120.0F);  // ① 20dp 行程：未达阈值 ⇒ 回弹归零，只为兑现兜底首帧
        AURORA_TEST_CHECK_MSG(ptr.is_gliding(), "setback glide should arm on the sub-threshold release");
        pump_scaled_frame(ptr);
        AURORA_TEST_CHECK(drag(d, ptr, 20.0F, 180.0F));  // ② 160dp ⇒ 阻尼后 71.1dp 过阈值，在途改指 32
        AURORA_TEST_CHECK_EQ(static_cast<int>(ptr.state()), static_cast<int>(PullToRefreshState::Refreshing));
        AURORA_TEST_CHECK_TRUE(ptr.is_gliding());
        const int frames = pump_until_glide_ends(ptr);
        const float endpoint = ptr.pull_distance();
        ptr.finish_refresh();  // 收拢回 Idle，下一档从同一初态起步
        settle(ptr);
        return {frames, endpoint};
    };

    const auto [n_1x, end_1x] = frames_at(1.0);
    const auto [n_2x, end_2x] = frames_at(2.0);
    const auto [n_4x, end_4x] = frames_at(4.0);

    AURORA_TEST_CHECK_EQ(n_1x, expected_glide_frames(1.0));
    AURORA_TEST_CHECK_EQ(n_2x, expected_glide_frames(2.0));
    AURORA_TEST_CHECK_EQ(n_4x, expected_glide_frames(4.0));
    AURORA_TEST_CHECK_EQ(n_1x, 3);
    // 判据 9：帧数恰为 1x 档的一半。双层缩放（倍率平方）会让 2x 档每帧吃 0.25 秒 ⇒ 一帧到位，
    // 与这里的 2 帧冲突即转红——2x 档正是抓出「两层都乘」的那一档。
    AURORA_TEST_CHECK_EQ(n_2x, (n_1x + 1) / 2);
    AURORA_TEST_CHECK_EQ(n_4x, (n_1x + 3) / 4);
    AURORA_TEST_CHECK(end_1x == end_2x && end_2x == end_4x);  // 只改步长、不改终值
    AURORA_TEST_CHECK_EQ(end_1x, 32.0F);  // 驻留高度即回弹终点
    AURORA_TEST_CHECK_FALSE(ptr.is_gliding());
    AURORA_TEST_CHECK_EQ(*t.refreshes, 3);  // 三档各触发一次刷新回调（路径未被跳过）

    anim.set_time_scale(1.0);  // 倍率还原后行为与原先一致（判据 9 的无残留半句）
    drag(d, ptr, 100.0F, 120.0F);
    pump_scaled_frame(ptr);
    AURORA_TEST_CHECK(drag(d, ptr, 20.0F, 180.0F));
    AURORA_TEST_CHECK_EQ(pump_until_glide_ends(ptr), n_1x);
    ptr.finish_refresh();
    settle(ptr);
}

// @brief 判据 11(b) + 8 的 0.0 档（PullToRefresh 行）：冻结段不推进、零重绘请求，解冻按剩余帧数续跑到同高。
AURORA_TEST_CASE(pull_to_refresh_setback_freeze_holds_progress_and_emits_no_redraw) {
    Animator anim;
    const testing::ScopedCurrentAnimator current{anim};
    auto t = make_bare();
    PullToRefresh &ptr = *t.ptr;
    EventDispatcher d;

    drag(d, ptr, 100.0F, 120.0F);  // 兑现首帧的 1/60 兜底步长
    pump_scaled_frame(ptr);
    AURORA_TEST_CHECK(drag(d, ptr, 20.0F, 180.0F));  // 在途重定向：回弹目标 32
    AURORA_TEST_CHECK_TRUE(ptr.is_gliding());
    pump_scaled_frame(ptr);  // 推进一帧：elapsed = 1/16 秒
    AURORA_TEST_REQUIRE_TRUE(ptr.is_gliding());
    const float frozen_from = ptr.pull_distance();

    anim.set_time_scale(0.0);
    AURORA_TEST_CHECK_EQ(anim.time_scale(), 0.0);
    ScopedDirtyCounter dirty{ptr};
    for (int i = 0; i < 8; ++i) {
        pump_scaled_frame(ptr);
    }
    AURORA_TEST_CHECK_EQ(ptr.pull_distance(), frozen_from);  // 冻结段一步都不挪
    AURORA_TEST_CHECK_TRUE(ptr.is_gliding());  // 在途标志按实现保留（不该被摘除）
    AURORA_TEST_CHECK_EQ(dirty.count(), 0);  // 判据 11(b)：回弹与 spinner 同时停手，不「满帧唤醒」

    anim.set_time_scale(1.0);  // 解冻重武装：进度不丢，剩余 2 帧（0.125 → 0.1875 越线）到位
    AURORA_TEST_CHECK_EQ(pump_until_glide_ends(ptr), 2);
    AURORA_TEST_CHECK_GT(dirty.count(), 0);  // 对照：冻结段的 0 不是「回调没接线」
    AURORA_TEST_CHECK_FALSE(ptr.is_gliding());
    AURORA_TEST_CHECK_EQ(ptr.pull_distance(), 32.0F);  // 与不冻结路径的终值逐位一致
    ptr.finish_refresh();
    settle(ptr);
}

}  // namespace aurora::test_cases::utest_pull_to_refresh

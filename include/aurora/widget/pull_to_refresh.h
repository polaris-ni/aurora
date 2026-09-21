#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "aurora/core/accessibility.h"
#include "aurora/event/gesture.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/scroll_viewport.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 下拉刷新状态机（PullToRefresh 三态）。
enum class PullToRefreshState : std::uint8_t {
    Idle,  ///< 空闲（指示器收起）
    Pulling,  ///< 下拉中（橡皮筋跟手）
    Refreshing,  ///< 刷新中（spinner 持续旋转，待 `finish_refresh()` 收拢）
};

/// @brief PullToRefresh 属性（聚合，继承式双模 API：字段即控件公有成员）。
struct PullToRefreshProps {
    Node child;
    float threshold = 64.0F;  ///< 触发刷新的下拉距离（dp，超过后松手触发）
    float max_pull = 128.0F;  ///< 橡皮筋下拉上限（dp）
};

/**
 * @brief 下拉刷新容器（specification/04-widget.md）：包住一个滚动子树，顶部继续下拉时
 * 积累下拉距离，越过阈值松手触发 `on_refresh` 回调，`finish_refresh()` 收拢回弹。
 *
 * 两条输入通道（均要求滚动子树已在顶部）：
 * - **拖拽**：`DragRecognizer` 垂直锁定主轴后 0.45 系数橡皮筋映射（远端渐硬）；
 * - **滚轮余量**：嵌套滚动协调下，子级把到顶后未消费的量经 `ScrollEvent::remaining_y`
 *   上冒给本容器（本容器在命中链上位于子滚动控件更浅层）。
 *
 * 指示器以**覆盖层**绘制在视口顶部（不动布局盒、不推挤内容，spec §6.5「动画不改布局」），
 * 回弹/收拢走共享 `ScrollGlide` 短滑动时序（自驱动 tick，不占 `Animator`）；
 * reduce-motion 下直落端点、不产生中间帧（对齐 `AnimationController::tick` 短路语义）。
 *
 * 对标 Flutter `RefreshIndicator`、Material 3 pull-to-refresh、iOS UIRefreshControl。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: yes（阈值可序列化；`on_refresh` 回调属运行时接线，重建后须重挂）
 */
class PullToRefresh : public SingleChild, public PullToRefreshProps {
  public:
    PullToRefresh() = default;
    explicit PullToRefresh(Node child) {
        if (child) {
            child_ = std::move(child);
        }
    }
    explicit PullToRefresh(PullToRefreshProps props) {
        if (props.child) {
            child_ = std::move(props.child);
        }
        threshold = props.threshold;
        max_pull = props.max_pull;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "PullToRefresh"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "PullToRefresh",
            .properties =
                {
                    {.name = "threshold",
                     .type = "float",
                     .default_value = "64.0",
                     .required = false,
                     .note = "触发刷新的下拉距离(dp)"},
                    {.name = "max_pull",
                     .type = "float",
                     .default_value = "128.0",
                     .required = false,
                     .note = "橡皮筋下拉上限(dp)"},
                },
            .events = {"on_refresh"},
            .children_policy = "single",
            .examples = {"au::PullToRefresh(au::Scroll{...}) /* 顶部下拉触发刷新 */"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 注册刷新回调（松手越过阈值时触发一次；回调内异步取数，完成后调 `finish_refresh`）。
    auto on_refresh(std::function<void()> cb) -> PullToRefresh & {
        on_refresh_ = std::move(cb);
        return *this;
    }

    /// @brief 结束刷新态：指示器收拢回弹（reduce-motion 下直落 0）。数据加载完成后调用。
    auto finish_refresh() -> void {
        if (state_ != PullToRefreshState::Refreshing) {
            return;
        }
        state_ = PullToRefreshState::Idle;
        begin_setback_glide(0.0F);
    }

    [[nodiscard]] auto state() const -> PullToRefreshState { return state_; }
    /// @brief 当前下拉距离（dp，覆盖层高度；测试/联动观测点）。
    [[nodiscard]] auto pull_distance() const -> float { return pull_; }
    /// @brief 触发进度（pull/threshold，可 >1；绑定指示器旋转角度/透明度用）。
    [[nodiscard]] auto progress() const -> float { return threshold > 0.0F ? pull_ / threshold : 0.0F; }
    /// @brief 是否正在回弹滑动（测试观测点）。
    [[nodiscard]] auto is_gliding() const -> bool { return glide_.active; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["threshold"] = threshold;
        props["max_pull"] = max_pull;
    }
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("threshold")) {
            threshold = props["threshold"].get<float>();
        }
        if (props.contains("max_pull")) {
            max_pull = props["max_pull"].get<float>();
        }
    }

    /// @brief 滚轮余量消费（嵌套滚动协调）：派发器把子级到顶后冒上来的余量改写为
    ///        `delta_y` 再交给本容器（`remaining_y` 每跳重置，不作为入口信号）。
    ///        仅吃「向上滚且子树已在顶部」的量——门控不通过时按原样回传 `remaining_y`
    ///        继续上冒（派发器把「未写余量」视为全量消费，静默 return 会吞掉事件）。
    auto on_scroll(ScrollEvent &e) -> void override {
        if (state_ == PullToRefreshState::Refreshing || e.delta_y <= 0.0F || !child_at_top()) {
            e.remaining_y = e.delta_y;  // 本容器不接：原量交还更浅层可滚动祖先
            return;
        }
        // 滚轮余量以「单位」计（全库约定 offset -= delta × step），换算成 dp 再进橡皮筋，
        // 否则滚轮通道要几百个单位才够阈值。缺省 step=16dp（ScrollProps::step 同值）。
        const float grown = rubber(e.delta_y * 16.0F);
        if (grown <= 0.0F) {
            return;
        }
        pull_ = std::clamp(pull_ + grown, 0.0F, max_pull);
        state_ = PullToRefreshState::Pulling;
        e.is_handled = true;
        e.remaining_y = 0.0F;  // 余量已被下拉吃掉
        glide_.active = false;
        mark_needs_paint();
        request_frame(false);
    }

    /// @brief 本容器参与滚动派发（仅消费子级余量）。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

  protected:
    /// @brief 视口透传布局：子树取本容器盒尺寸（滚动子树自管内滚偏移）。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 320.0F, .height = 480.0F};
        }
        if (child_) {
            const Constraints inner{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
        }
        return c.constrain(self);
    }

    auto on_pointer_event(MouseEvent &e) -> void override {
        drag_.on_mouse(e);
        if (e.action == MouseAction::Press && child_at_top()) {
            drag_consumed_ = false;  // 顶部起按：开启下一次下拉候选
        }
        const bool vertical_down = drag_.axis() == DragAxis::Vertical && drag_.delta().y > 0.0F;
        if (e.action == MouseAction::Move && drag_.is_dragging() && vertical_down && !drag_consumed_ &&
            child_at_top() && state_ != PullToRefreshState::Refreshing) {
            pull_ = std::clamp(rubber(drag_.delta().y), 0.0F, max_pull);
            state_ = PullToRefreshState::Pulling;
            glide_.active = false;
            drag_consumed_ = true;  // 本手势归下拉所有：之后即使回落也不再劫持子级点击
            mark_needs_paint();
            request_frame(false);
        } else if (e.action == MouseAction::Move && drag_.is_dragging() && drag_consumed_) {
            // 已劫持手势：回落段继续跟手（允许拉回 0），反向超阈值时放弃劫持交还子级。
            if (drag_.delta().y <= 0.0F) {
                drag_consumed_ = false;
            } else {
                pull_ = std::clamp(rubber(drag_.delta().y), 0.0F, max_pull);
                mark_needs_paint();
                request_frame(false);
            }
        }
        if (e.action == MouseAction::Release && drag_consumed_) {
            drag_consumed_ = false;
            release_pull();
        }
        if (drag_consumed_ || state_ == PullToRefreshState::Refreshing) {
            e.is_handled = true;  // 下拉/刷新手势期消费指针事件，子级不误触点击
        } else {
            SingleChild::on_pointer_event(e);  // 未劫持：修饰链照常下发子树
        }
        if (e.action == MouseAction::Release) {
            drag_.reset();
        }
    }

    auto on_pointer_event(TouchEvent &e) -> void override {
        drag_.on_touch(e);
        SingleChild::on_pointer_event(e);  // 修饰链照常
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        SingleChild::on_paint(p, bounds, ctx);
        if (pull_ <= 0.4F) {
            return;
        }
        // 顶部覆盖层带：半透明底 + 旋转弧线 spinner（不动布局盒）。
        const Color accent = inherit_theme(ctx).primary;
        const Rect band{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y},
                        .size = Size{.width = bounds.size.width, .height = pull_}};
        const double prev_alpha = p.global_alpha();
        p.set_alpha(prev_alpha * 0.92);
        p.fill_rect(band, Color{255, 255, 255, 255});
        p.set_alpha(prev_alpha);
        const float angle = spin_angle_;
        p.stroke_arc(
            Point{.x = band.origin.x + (band.size.width / 2.0F), .y = band.origin.y + (band.size.height / 2.0F)}, 10.0F,
            2.5F, angle, angle + (4.712389F * std::clamp(progress(), 0.25F, 1.0F)), accent);
        if (state_ == PullToRefreshState::Refreshing) {
            const float th = render::FontEngine::measure_height(Font{.size_pt = 11.0F});
            p.draw_text(Rect{.origin = Point{.x = band.origin.x, .y = band.origin.y + pull_ - th - 4.0F},
                             .size = Size{.width = band.size.width, .height = th}},
                        "刷新中…", Font{.size_pt = 11.0F}, Color{120, 120, 120, 255});
        }
    }

    /// @brief 回弹/旋转逐帧推进（自驱动 tick，同 Scroll/Dismissible 模式）。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        SingleChild::tick_gestures(now);
        const double dt =
            last_tick_.has_value() ? std::chrono::duration<double>(now - *last_tick_).count() : (1.0 / 60.0);
        last_tick_ = now;
        bool busy = false;
        if (state_ == PullToRefreshState::Refreshing) {
            spin_angle_ += static_cast<float>(dt) * 6.0F;  // ~1 圈/秒
            if (spin_angle_ > 6.2831853F) {
                spin_angle_ -= 6.2831853F;
            }
            mark_needs_paint();
            request_frame(false);  // 旋转帧 = 活跃帧，防滑入空闲深睡
            busy = true;
        }
        if (glide_.active) {
            const float v = std::clamp(glide_.tick(dt), 0.0F, max_pull);
            if (!glide_.active) {
                needs_gesture_tick_ = false;
            }
            if (v != pull_) {
                pull_ = v;
                mark_needs_paint();
                request_frame(false);
            }
            busy = true;
        }
        if (!busy) {
            needs_gesture_tick_ = false;
            last_tick_.reset();
        }
    }

  private:
    /// @brief 橡皮筋（阻尼）映射：物理位移(dp) → 指示器位移(dp)。
    ///        双曲阻尼 `raw·max/(raw+max)`：起段近 1:1 跟手，渐近 `max_pull` 且恒不越界；
    ///        拖满 `max_pull` 恰达半上限 —— 与缺省 `threshold = max_pull/2` 对齐（可触发）。
    [[nodiscard]] auto rubber(float raw) const -> float {
        const float limited = std::max(0.0F, raw);
        if (max_pull <= 0.0F) {
            return 0.0F;
        }
        return std::min(max_pull, (limited * max_pull) / (limited + max_pull));
    }

    /// @brief 滚动子树是否位于顶部：找最近的可滚动后代（实现 `accessibility_scroll`），
    ///        position<=min 即顶部；无可滚动后代视为「在顶部」（静态内容可整体下拉）。
    [[nodiscard]] auto child_at_top() const -> bool {
        if (!child_) {
            return true;
        }
        const auto range = first_scrollable_of(child_.widget());
        return !range.has_value() || range->position <= range->min + 0.5;
    }

    static auto first_scrollable_of(const Widget &w) -> std::optional<AccessibilityScrollRange> {
        if (const auto range = w.accessibility_scroll(); range.has_value()) {
            return range;
        }
        std::optional<AccessibilityScrollRange> found;
        w.for_each_child([&found](const Widget &c) {
            if (!found.has_value()) {
                found = first_scrollable_of(c);
            }
        });
        return found;
    }

    /// @brief 松手裁决：进度达阈值 → 进入刷新态并回弹到驻留高度（约半阈值）；否则回弹归零。
    auto release_pull() -> void {
        if (pull_ >= threshold && on_refresh_) {
            state_ = PullToRefreshState::Refreshing;
            needs_gesture_tick_ = true;  // spinner 逐帧旋转依赖 tick（回弹目标可能与当前值相等而早退）
            on_refresh_();
            begin_setback_glide(std::min(pull_, threshold * 0.5F));
        } else {
            state_ = PullToRefreshState::Idle;
            begin_setback_glide(0.0F);
        }
    }

    /// @brief 启动回弹滑动；reduce-motion 下直落目标（无中间帧）。
    auto begin_setback_glide(float target) -> void {
        if (target == pull_) {
            return;
        }
        if (current_accessibility_settings().reduce_motion) {
            glide_.active = false;
            pull_ = target;
            mark_needs_paint();
            request_frame(false);
            return;
        }
        glide_.start(pull_, target);
        needs_gesture_tick_ = true;
        request_frame(false);
    }

    std::function<void()> on_refresh_;
    DragRecognizer drag_;
    bool drag_consumed_ = false;  ///< 本手势已被下拉劫持（子级点击不再参与）
    PullToRefreshState state_ = PullToRefreshState::Idle;
    float pull_ = 0.0F;  ///< 当前下拉距离（dp）
    float spin_angle_ = 0.0F;  ///< spinner 累计旋转角（弧度）
    ScrollGlide glide_;  ///< 回弹短滑动时序
    std::optional<std::chrono::steady_clock::time_point> last_tick_;
};

}  // namespace aurora

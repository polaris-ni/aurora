#pragma once

#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

#include "aurora/animation/spring.h"
#include "aurora/core/accessibility.h"
#include "aurora/event/event.h"
#include "aurora/state/state.h"

namespace aurora {

/**
 * @brief 拖动主轴（`DragRecognizer` 在超 slop 瞬间锁定，之后仅输出该轴分量）。
 *
 * 独立于 widget 层的 `Orientation`（event 层不依赖 widget 头）；语义为「位移主方向」
 * 而非「控件摆放方向」，二者不可互换。
 */
enum class DragAxis : std::uint8_t {
    None,  ///< 未起拖（未超 slop）
    Horizontal,
    Vertical,
};

/**
 * @brief 捏合（Pinch）手势识别器：追踪双指距离变化，输出缩放比例。
 *
 * 用法：
 * @code
 *   PinchRecognizer pinch;
 *   // 每帧触摸事件：
 *   pinch.on_touch(touch_event);
 *   float scale = pinch.scale();  // 相对初始距离的缩放比
 * @endcode
 */
class PinchRecognizer {
  public:
    /// @brief 处理触摸事件，更新内部状态。
    /// 并发场景下锁定一对 pointer id（激活瞬间取前两个活跃点），后续始终追踪该对，
    /// 避免第三指插入导致距离跳变（仍回退到前两活跃点距离以保持稳健）。
    auto on_touch(const TouchEvent &e) -> void {
        if (e.active_count() < 2) {
            reset();
            return;
        }
        auto pa = e.point_by_id(id_a_);
        auto pb = e.point_by_id(id_b_);
        if (!pa || !pb || !pa->is_active || !pb->is_active) {
            id_a_ = id_b_ = -1;  // 锁定对失效，重新取前两个活跃点
            for (const auto &p : e.points) {
                if (!p.is_active) {
                    continue;
                }
                if (id_a_ < 0) {
                    id_a_ = p.id;
                } else if (id_b_ < 0) {
                    id_b_ = p.id;
                }
            }
            pa = e.point_by_id(id_a_);
            pb = e.point_by_id(id_b_);
        }
        const float dist = (pa && pb)
                               ? std::sqrt(((pa->position.x - pb->position.x) * (pa->position.x - pb->position.x)) +
                                           ((pa->position.y - pb->position.y) * (pa->position.y - pb->position.y)))
                               : e.pinch_distance();
        if (!active_) {
            initial_distance_ = dist;
            active_ = true;
        }
        current_distance_ = dist;
    }

    /// @brief 当前缩放比例（相对初始双指距离）。未激活时返回 1.0。
    [[nodiscard]] auto scale() const -> float {
        if (!active_ || initial_distance_ < 0.001F) {
            return 1.0F;
        }
        return current_distance_ / initial_distance_;
    }

    /// @brief 是否正在识别中（双指活跃）。
    [[nodiscard]] auto is_active() const -> bool { return active_; }

    /// @brief 重置状态。
    auto reset() -> void {
        active_ = false;
        initial_distance_ = 0.0F;
        current_distance_ = 0.0F;
        id_a_ = id_b_ = -1;
    }

  private:
    bool active_ = false;
    float initial_distance_ = 0.0F;
    float current_distance_ = 0.0F;
    int id_a_ = -1;  ///< 锁定的第一指 pointer id
    int id_b_ = -1;  ///< 锁定的第二指 pointer id
};

/**
 * @brief 旋转手势识别器：追踪双指角度变化，输出旋转增量（度）。
 */
class RotationRecognizer {
  public:
    /// @brief 处理触摸事件，更新内部状态。
    /// 并发场景下锁定一对 pointer id（激活瞬间取前两个活跃点），后续始终追踪该对角度。
    auto on_touch(const TouchEvent &e) -> void {
        if (e.active_count() < 2) {
            reset();
            return;
        }
        auto pa = e.point_by_id(id_a_);
        auto pb = e.point_by_id(id_b_);
        if (!pa || !pb || !pa->is_active || !pb->is_active) {
            id_a_ = id_b_ = -1;  // 锁定对失效，重新取前两个活跃点
            for (const auto &p : e.points) {
                if (!p.is_active) {
                    continue;
                }
                if (id_a_ < 0) {
                    id_a_ = p.id;
                } else if (id_b_ < 0) {
                    id_b_ = p.id;
                }
            }
            pa = e.point_by_id(id_a_);
            pb = e.point_by_id(id_b_);
        }
        const float angle =
            (pa && pb) ? std::atan2(pb->position.y - pa->position.y, pb->position.x - pa->position.x) : e.pinch_angle();
        if (!active_) {
            initial_angle_ = angle;
            active_ = true;
        }
        current_angle_ = angle;
    }

    /// @brief 旋转增量（弧度，归一化到 [-π, π]，相对初始角度）。未激活时返回 0。
    [[nodiscard]] auto angle_delta() const -> float {
        if (!active_) {
            return 0.0F;
        }
        float delta = current_angle_ - initial_angle_;
        // 归一化到 [-π, π]
        while (delta > std::numbers::pi_v<float>) {
            delta -= 2.0F * std::numbers::pi_v<float>;
        }
        while (delta < -std::numbers::pi_v<float>) {
            delta += 2.0F * std::numbers::pi_v<float>;
        }
        return delta;
    }

    /// @brief 是否正在识别中。
    [[nodiscard]] auto is_active() const -> bool { return active_; }

    /// @brief 重置状态。
    auto reset() -> void {
        active_ = false;
        initial_angle_ = 0.0F;
        current_angle_ = 0.0F;
        id_a_ = id_b_ = -1;
    }

  private:
    bool active_ = false;
    float initial_angle_ = 0.0F;
    float current_angle_ = 0.0F;
    int id_a_ = -1;  ///< 锁定的第一指 pointer id
    int id_b_ = -1;  ///< 锁定的第二指 pointer id
};

/**
 * @brief 单指拖动识别器（pointer-agnostic）：超 slop 起拖，按 pointer_id 锁定首按点。
 *
 * 鼠标与触摸统一抽象：`on_mouse` / `on_touch` 双入口喂入（触摸取首个活跃点，单指语义；
 * 中途第二指插入不劫持已锁定的指针——锁定策略与 Pinch/Rotation 同源）。纯识别器：
 * 不接触动画、不持有 State，跟手映射由使用方定义（如 `DragToDismiss`）。
 *
 * 用法：
 * @code
 *   DragRecognizer drag;  // drag.slop = 8.0（逻辑 dp，可调）
 *   // 事件循环：drag.on_mouse(e)（或 on_touch）
 *   if (drag.is_dragging()) { auto d = drag.delta();  // 主轴分量映射
 *   if (松手) { drag.on_mouse(release); drag.reset(); }
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class DragRecognizer {
  public:
    double slop = 8.0;  ///< 起拖阈值（逻辑 dp）：累计位移超过才激活（Android view configuration 量级）

    /// @brief 喂入鼠标事件：Press 记录按下点（并清残留状态），Move 推进，Release 结束本拖。
    auto on_mouse(const MouseEvent &e) -> void {
        if (e.action == MouseAction::Press) {
            begin(0, e.position);  // 鼠标无 pointer_id：统一用 0
        } else if (e.action == MouseAction::Move) {
            advance(0, e.position);
        } else if (e.action == MouseAction::Release) {
            tracking_ = false;  // 松手：保持 delta/axis 供读取，待 reset 清零
        }
    }

    /// @brief 喂入触摸事件：按 pointer_id 锁定首个活跃点（单指语义；双指手势走 Pinch/Rotation）。
    auto on_touch(const TouchEvent &e) -> void {
        if (e.active_count() == 0) {
            tracking_ = false;
            return;
        }
        // 锁定指针：未锁定取首个活跃点；已锁定后其他指针插入不换锁。
        if (tracking_) {
            const auto pt = e.point_by_id(pointer_id_);
            if (pt.has_value() && pt->is_active) {
                advance(pointer_id_, pt->position);
                return;
            }
            // 锁定指针消失：结束本拖（保持 delta 供读取）。
            tracking_ = false;
            return;
        }
        const auto pt = e.point_by_id(e.points.front().id);
        if (pt.has_value() && pt->is_active) {
            begin(pt->id, pt->position);
        }
    }

    /// @brief 是否拖动中（已超 slop 且未松手）。
    [[nodiscard]] auto is_dragging() const -> bool { return tracking_ && axis_ != DragAxis::None; }

    /// @brief 相对按下点的累计位移（逻辑 dp）。起拖后仅含锁定主轴分量（未锁轴 = 原始位移）。
    [[nodiscard]] auto delta() const -> Point {
        if (!tracking_) {
            return current_delta_;
        }
        if (axis_ == DragAxis::Horizontal) {
            return Point{.x = current_delta_.x, .y = 0.0F};
        }
        if (axis_ == DragAxis::Vertical) {
            return Point{.x = 0.0F, .y = current_delta_.y};
        }
        return current_delta_;
    }

    /// @brief 锁定的主轴（未起拖 = None；起拖瞬间按 |dx|>|dy| 锁定，此后不变）。
    [[nodiscard]] auto axis() const -> DragAxis { return axis_; }

    /// @brief 清零状态（松手消费 delta 后调用，开启下一次识别）。
    auto reset() -> void {
        tracking_ = false;
        axis_ = DragAxis::None;
        current_delta_ = Point{};
    }

  private:
    auto begin(int pointer_id, Point pos) -> void {
        pointer_id_ = pointer_id;
        origin_ = pos;
        current_delta_ = Point{};
        axis_ = DragAxis::None;
        tracking_ = true;
    }

    auto advance(int pointer_id, Point pos) -> void {
        if (!tracking_ || pointer_id != pointer_id_) {
            return;
        }
        current_delta_ = Point{.x = pos.x - origin_.x, .y = pos.y - origin_.y};
        if (axis_ == DragAxis::None) {
            const float dist = std::sqrt((current_delta_.x * current_delta_.x)
                                         + (current_delta_.y * current_delta_.y));
            if (dist > static_cast<float>(slop)) {
                // 起拖瞬间锁主轴：|dx| > |dy| 水平，否则垂直（相等取垂直，确定序）。
                axis_ = (std::abs(current_delta_.x) > std::abs(current_delta_.y)) ? DragAxis::Horizontal
                                                                                  : DragAxis::Vertical;
            }
        }
    }

    bool tracking_ = false;
    int pointer_id_ = -1;
    Point origin_{};
    Point current_delta_{};
    DragAxis axis_ = DragAxis::None;
};

/**
 * @brief 拖动消除驱动器：跟手（1:1 直接映射）+ 松手 spring 二选一（回位 / 飞出消除）。
 *
 * 值域 0..1：0 = 原位，1 = 消除阈值。跟手期间 `progress` 直接等于拖动距离 /
 * `travel_distance`（主轴分量、负方向夹取为 0）——**直接操作而非动画**，`reduce_motion`
 * 不干预（无障碍语义：直接操作保持 1:1 响应）。松手（`on_release`）依
 * `progress ≥ threshold` 裁决落点：飞出（spring 到 1，完成后触发 `on_dismissed`）或
 * 回位（spring 到 0，静默）。拖动速度（最近两次事件的位移 / dt，主轴分量）作为
 * spring 初速度，方向与裁决一致时自然加速、相反时自然衰减。
 *
 * `reduce_motion` 对 spring 阶段短路：直接落端点（落 1 触发 dismissed、落 0 静默），
 * 与 `AnimationController::tick` 的短路语义同源。
 *
 * 用法：
 * @code
 *   DragToDismiss dtd(DragAxis::Horizontal, 120.0, SpringDescription{});
 *   // 事件循环：dtd.on_mouse(e)（或 on_touch）；松手时 on_release()。
 *   // 帧循环：dtd.tick(dt)（spring 阶段推进；跟手阶段 no-op）。
 *   dtd.on_dismissed([](){});  // 例如从容器摘除
 *   double p = dtd.progress().get();  // 0..1，绑定位移/透明度
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class DragToDismiss {
  public:
    /// @brief 构造：主轴、消除行程（逻辑 dp，拖满即 progress=1）、spring 参数。
    DragToDismiss(DragAxis axis, double travel_distance_dp, SpringDescription spring)
        : axis_(axis), travel_(travel_distance_dp > 0.0 ? travel_distance_dp : 1.0), spring_desc_(spring) {}

    /// @brief 更新消除行程（如布局后按主轴向尺寸校准；非正值夹取 1.0 防除零）。
    auto set_travel(double travel_distance_dp) -> void {
        travel_ = travel_distance_dp > 0.0 ? travel_distance_dp : 1.0;
    }

    /// @brief 喂鼠标事件（转发内部 DragRecognizer；松手不自动裁决——由 on_release 显式触发）。
    auto on_mouse(const MouseEvent &e) -> void {
        const Point prev = recognizer_.delta();
        recognizer_.on_mouse(e);
        if (e.action == MouseAction::Move) {
            update_velocity(prev, recognizer_.delta());
        }
        sync_progress();
    }

    /// @brief 喂触摸事件（同 on_mouse）。
    auto on_touch(const TouchEvent &e) -> void {
        const Point prev = recognizer_.delta();
        recognizer_.on_touch(e);
        update_velocity(prev, recognizer_.delta());
        sync_progress();
    }

    /// @brief 进度值 State（0..1）：跟手与 spring 共用同一条，绑定到位移/透明度等。
    [[nodiscard]] auto progress() const -> const State<double> & { return progress_; }

    /// @brief 裁决阈值（progress ≥ 此值判飞出；默认 0.5）。
    double threshold = 0.5;

    /// @brief 松手裁决：按 progress 与 spring 速度决定飞出（→1 + on_dismissed）或回位（→0）。
    auto on_release() -> void {
        const double current = progress_.get();
        const bool dismiss = current >= threshold;
        const double end = dismiss ? 1.0 : 0.0;
        // 速度取主轴分量、符号对齐裁决方向：方向一致保留（加速飞出/自然回弹），相反丢弃。
        double v = axis_velocity_;
        if ((v > 0.0) != dismiss) {
            v = 0.0;
        }
        spring_ = SpringSimulation(spring_desc_, current, end, v);
        spring_t_ = 0.0;
        spring_end_ = end;
        animating_ = true;
        recognizer_.reset();  // 识别器清零，开启下一次拖动
    }

    /// @brief 飞出完成回调（progress 到 1 且 spring 静止后触发，一次性语义见实现）。
    auto on_dismissed(std::function<void()> cb) -> void { on_dismissed_ = std::move(cb); }

    /// @brief 帧推进（spring 阶段；跟手阶段 no-op）。到位触发 on_dismissed（仅飞出方向）。
    auto tick(double dt_seconds) -> void {
        if (!animating_) {
            return;
        }
        if (current_accessibility_settings().reduce_motion) {
            // 短路：直接落端点（语义与 AnimationController::tick 的 reduce_motion 分支同源）。
            animating_ = false;
            progress_.set(spring_end_);
            fire_dismissed_if_needed();
            return;
        }
        spring_t_ += dt_seconds;
        const double v = spring_->value(spring_t_);
        progress_.set(std::clamp(v, 0.0, 1.0));
        if (spring_->is_settled(spring_t_)) {
            animating_ = false;
            progress_.set(spring_end_);
            fire_dismissed_if_needed();
        }
    }

    /// @brief spring 阶段是否进行中。
    [[nodiscard]] auto is_animating() const -> bool { return animating_; }

    /// @brief 是否处于跟手阶段（可继续喂事件）。
    [[nodiscard]] auto is_dragging() const -> bool { return recognizer_.is_dragging(); }

    /// @brief 识别器 slop 注入（测试用；常规消费者直接用默认 8dp）。
    auto recognizer_slop_for_test(double slop_dp) -> void { recognizer_.slop = slop_dp; }

  private:
    auto fire_dismissed_if_needed() -> void {
        if (spring_end_ >= 1.0 && on_dismissed_) {
            on_dismissed_();
        }
    }

    // 跟手：识别器主轴位移 → progress（负方向夹取 0，超行程夹取 1）。
    auto sync_progress() -> void {
        if (!animating_ && recognizer_.is_dragging()) {
            const Point d = recognizer_.delta();
            const double along = (axis_ == DragAxis::Horizontal) ? d.x : d.y;
            progress_.set(std::clamp(along / travel_, 0.0, 1.0));
        }
    }

    // 速度估计：本帧位移 / 固定采样假设（事件 dt 由调用节拍决定，识别器不持时钟——
    // 实现期决策 §9.1：steady_clock 自计粒度受事件批处理影响，固定 60Hz 假设足够
    // spring 初速度的方向语义，量级误差由 spring 阻尼自然收敛）。
    auto update_velocity(const Point &prev, const Point &cur) -> void {
        const double d = ((axis_ == DragAxis::Horizontal) ? (cur.x - prev.x) : (cur.y - prev.y));
        axis_velocity_ = d / (1.0 / 60.0);
    }

    DragAxis axis_;
    double travel_ = 1.0;
    SpringDescription spring_desc_;
    DragRecognizer recognizer_;
    State<double> progress_{0.0};
    std::optional<SpringSimulation> spring_;
    double spring_t_ = 0.0;
    double spring_end_ = 0.0;
    double axis_velocity_ = 0.0;
    bool animating_ = false;
    std::function<void()> on_dismissed_;
};

}  // namespace aurora

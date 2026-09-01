#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "aurora/animation/timeline.h"
#include "aurora/state/state.h"

namespace aurora {

/// @brief 动画状态（对应 Flutter `AnimationStatus`）。
enum class AnimationStatus : std::uint8_t {
    Dismissed, ///< 在起点（进度 0）
    Forward,   ///< 正向播放中
    Reverse,   ///< 反向播放中
    Completed, ///< 在终点（进度 1）
};

/**
 * @brief 时间驱动：在 duration 内把线性进度 0→1（或反向）推进（架构 §5.5）。
 *
 * 纯时间线，**不含曲线**——曲线在 `Tween`/`Keyframes` 上应用；控制器只输出原始
 * 归一化进度 t∈[0,1]。每帧由 `Animator::tick` 调用 `tick(dtSeconds)` 推进。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class AnimationController {
  public:
    explicit AnimationController(double duration_seconds, double value = 0.0)
        : m_duration(std::max(duration_seconds, 1e-6)), m_value(value) {}

    [[nodiscard]] auto value() const -> double { return m_value; }
    [[nodiscard]] auto status() const -> AnimationStatus { return m_status; }
    [[nodiscard]] auto duration() const -> double { return m_duration; }
    [[nodiscard]] auto is_dismissed() const -> bool { return m_status == AnimationStatus::Dismissed; }
    [[nodiscard]] auto is_completed() const -> bool { return m_status == AnimationStatus::Completed; }
    [[nodiscard]] auto is_animating() const -> bool {
        return m_status == AnimationStatus::Forward || m_status == AnimationStatus::Reverse;
    }
    /// @brief 本帧 value 是否发生变化（供 Animator 决定是否需要写目标 State）。
    [[nodiscard]] auto dirty() const -> bool { return m_dirty; }
    auto clear_dirty() -> void { m_dirty = false; }

    /// @brief 正向播放（可选从 from 起步）。
    auto forward(double from = -1.0) -> void;

    /// @brief 反向播放。
    auto reverse() -> void;

    /// @brief 复位到 v（不播放，静止）。
    auto reset(double v = 0.0) -> void;

    /// @brief 停止：保持当前进度，静止于最近端点语义。
    auto stop() -> void;

    /// @brief 推进 dt 秒；到达端点时钳制并置对应终态。
    auto tick(double dt_seconds) -> void;

  private:
    double m_duration = 0.0;
    double m_value = 0.0;
    AnimationStatus m_status = AnimationStatus::Dismissed;
    bool m_dirty = false;
};

/**
 * @brief 帧动画管理器（架构 §5.5 `Animator`）。
 *
 * 持有所有 `AnimationController`，在每帧 `tick(dt)` 中推进它们，并把绑定到
 * `State<T>` 的补间结果写入目标（`State::set` → 触发信号定点刷新，动画不另起通道）。
 * 仅当控制器本帧进度变化时才写 State，避免空闲帧的冗余刷新。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class Animator {
  public:
    /// @brief 登记控制器（非拥有，指针）；tick 时推进。
    /// @warning 登记的是裸指针。若控制器的生命周期短于本 `Animator`（典型：作为某个
    ///          widget 的成员），**必须**在其析构前调用 `remove(c)`，否则下一帧
    ///          `tick` 会写入已释放内存。
    auto drive(AnimationController &c) -> void { m_controllers.push_back(&c); }

    /// @brief 注销控制器及其经 `bind()` 建立的所有绑定。
    ///
    /// 与 `drive`/`bind` 配对使用：控制器或绑定目标 `State<T>` 先于 `Animator` 析构时
    /// （如 `NavigatorHost` 这类持有自身控制器、又把它注册进 `Application` 全局
    /// `Animator` 的 widget），须在析构函数中调用本函数摘除登记，避免帧循环
    /// 解引用悬垂指针（use-after-free）。
    /// @param c 待注销的控制器；未登记过时为无操作。
    auto remove(const AnimationController &c) -> void {
        std::erase(m_controllers, &c);
        std::erase_if(m_on_tick, [&c](const Binding &b) -> bool { return b.owner == &c; });
    }

    /// @brief 是否有运行中的控制器（Forward/Reverse），供帧调度决策取值
    /// （CPU 性能专项阶段 A3）：无活跃动画时 idle 帧可阻塞等待事件。
    [[nodiscard]] auto has_active() const -> bool {
        return std::ranges::any_of(
            m_controllers, [](const AnimationController *c) -> bool { return c != nullptr && c->is_animating(); });
    }

    /// @brief 推进所有控制器并应用所有绑定（在帧边界调用一次）。
    auto tick(double dt_seconds) const -> void;

    /// @brief 追加一帧回调（在 tick 中于所有控制器推进后、clear_dirty 前执行）。
    auto add_binding(std::function<void()> fn) -> void {
        m_on_tick.push_back(Binding{ .owner = nullptr, .fn = std::move(fn) });
    }

    /// @brief 绑定 (控制器 + 补间) → 目标 State：每帧把插值写入 State。
    template<typename T> auto bind(AnimationController &c, Tween<T> tw, State<T> &target) -> void {
        drive(c);
        m_on_tick.push_back(Binding{ &c, [&c, tw, &target]() -> auto {
                                        if (c.dirty()) {
                                            target.set(tw.value(c.value()));
                                        }
                                    } });
    }

    /// @brief 绑定 (控制器 + 关键帧) → 目标 State。
    template<typename T> auto bind(AnimationController &c, Keyframes<T> kf, State<T> &target) -> void {
        drive(c);
        m_on_tick.push_back(Binding{ &c, [&c, kf, &target]() -> auto {
                                        if (c.dirty()) {
                                            target.set(kf.value(c.value()));
                                        }
                                    } });
    }

  private:
    /// 一帧回调 + 其归属控制器（`add_binding` 的裸回调归属为空，不参与 `remove`）。
    struct Binding {
        const AnimationController *owner = nullptr;
        std::function<void()> fn;
    };

    std::vector<AnimationController *> m_controllers; ///< 非拥有
    std::vector<Binding> m_on_tick;
};

/**
 * @brief 便捷封装：把 `State<T>` + `Tween<T>` + `AnimationController` 收拢一处，
 * 通过 `attach(animator)` 一键接入帧循环（对应 Flutter `AnimatedBuilder` 的驱动部分）。
 *
 * 内部以 `shared_ptr` 持有驱动载荷（控制器 + 补间 + 目标 + completed 回调），因此本类型
 * 可自由拷贝/移动，**且 `attach` 后即使原句柄离开作用域，帧循环仍安全持有驱动载荷（不悬垂）**——
 * 这正是 `animate()` 能按值返回句柄、又能在 Animator 帧循环中长期驱动的前提。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template<typename T> class AnimatedValue {
  public:
    struct Payload {
        State<T> *target;               ///< 非拥有：目标 State 必须比本动画存活更久
        AnimationController controller; ///< 拥有的驱动控制器
        Tween<T> tween;                 ///< 拥有的补间（含曲线）
        std::function<void()> on_completed;
        bool fired_completed = false;
    };

    AnimatedValue(State<T> &target, Tween<T> tw, double duration_seconds)
        : m(std::make_shared<Payload>(
              Payload{ &target, AnimationController{ duration_seconds }, std::move(tw), {}, false })) {}

    [[nodiscard]] auto controller() -> AnimationController & { return m->controller; }
    [[nodiscard]] auto tween() -> Tween<T> & { return m->tween; }

    /// @brief 开始正向播放（可选从 from∈[0,1] 起步）。
    auto forward(double from = -1.0) -> void { m->controller.forward(from); }

    /// @brief 当前归一化进度 t∈[0,1]（控制器原始输出，未经曲线）。
    [[nodiscard]] auto progress() const -> double { return m->controller.value(); }

    /// @brief 目标 State 当前值（补间插值后的落点）。
    [[nodiscard]] auto current() const -> T { return m->target->get(); }

    [[nodiscard]] auto status() const -> AnimationStatus { return m->controller.status(); }
    [[nodiscard]] auto is_completed() const -> bool { return m->controller.is_completed(); }

    /// @brief 注册"到达终点（Completed）"的一次性回调（重复到达不会重复触发）。
    auto on_completed(std::function<void()> cb) -> void { m->on_completed = std::move(cb); }

    /// @brief 把本动画接入 Animator 的帧循环（驱动与写回委托给 Animator，句柄可离开作用域）。
    auto attach(Animator &a) -> void {
        a.drive(m->controller);
        auto p = m;
        a.add_binding([p]() -> void {
            if (p->controller.dirty()) {
                p->target->set(p->tween.value(p->controller.value()));
                if (p->controller.is_completed() && !p->fired_completed) {
                    p->fired_completed = true;
                    if (p->on_completed) {
                        p->on_completed();
                    }
                }
            }
        });
    }

    /// @brief 自驱动一帧（无 Animator 时手动推进）：推进控制器 → 写回目标 → 触发 completed。
    auto tick(double dt_seconds) -> void {
        m->controller.tick(dt_seconds);
        if (m->controller.dirty()) {
            m->target->set(m->tween.value(m->controller.value()));
            if (m->controller.is_completed() && !m->fired_completed) {
                m->fired_completed = true;
                if (m->on_completed) {
                    m->on_completed();
                }
            }
        }
        m->controller.clear_dirty();
    }

  private:
    std::shared_ptr<Payload> m;
};

/**
 * @brief 统一动画入口（收敛 API 面）：创建一个已起步（forward(0)）的动画句柄。
 *
 * 返回 `AnimatedValue<T>` 句柄（按值，可拷贝/移动）。动画不自动接入全局帧循环——
 * 调用方要么每帧 `handle.tick(dt)` 自驱动，要么 `handle.attach(animator)` 接入某
 * `Animator` 的帧循环（详见 `animate(target, tw, duration, animator)` 重载）。
 *
 * 兼容既有 `AnimationController` / `AnimatedValue` 直接构造（不删除）。
 *
 * @example
 * @code
 *   au::State<double> opacity{ 0.0 };
 *   auto anim = au::animate(opacity, au::Tween<double>{ 0.0, 1.0, au::Curves::ease_in_out() }, 0.3);
 *   // 每帧：anim.tick(dt);  // 或 anim.attach(app.animator());
 * @endcode
 *
 * @note Thread: main-thread only
 */
template<typename T> auto animate(State<T> &target, Tween<T> tw, double duration_s) -> AnimatedValue<T> {
    AnimatedValue<T> av{ target, std::move(tw), duration_s };
    av.forward(0.0);
    return av;
}

/// @brief 同上，并自动接入指定 `Animator` 的帧循环（最常用形态）。
template<typename T>
auto animate(State<T> &target, Tween<T> tw, double duration_s, Animator &anim) -> AnimatedValue<T> {
    AnimatedValue<T> av = animate(target, std::move(tw), duration_s);
    av.attach(anim);
    return av;
}

/**
 * @brief 自包含动画值：拥有自己的 State<T>，可独立 tick 推进。
 *
 * 用法：
 * @code
 *   TweenAnimation<float> anim(0.0f);
 *   anim.animate_to(1.0f, 0.3, Curves::ease_in_out());
 *   // 每帧：anim.tick(dt);
 *   float val = anim.get();  // 0→1 过渡中的当前值
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template<typename T> class TweenAnimation {
  public:
    explicit TweenAnimation(T initial) : m_state(initial), m_current(std::move(initial)) {}

    /// @brief 启动到 target 的动画。
    auto animate_to(T target, double duration_seconds, Curve curve = Curve{}) -> void {
        m_tween = Tween<T>(m_current, std::move(target), std::move(curve));
        m_controller = AnimationController(duration_seconds);
        m_controller.forward(0.0);
        m_animating = true;
    }

    /// @brief 每帧推进（由帧循环调用）。
    auto tick(double dt_seconds) -> void {
        if (!m_animating) {
            return;
        }
        m_controller.tick(dt_seconds);
        m_current = m_tween.value(m_controller.value());
        m_state.set(m_current);
        if (!m_controller.is_animating()) {
            m_animating = false;
        }
    }

    /// @brief 当前值。
    [[nodiscard]] auto get() const -> T { return m_current; }

    /// @brief 是否正在动画中。
    [[nodiscard]] auto is_animating() const -> bool { return m_animating; }

    /// @brief 作为信号访问（供响应式绑定）。
    [[nodiscard]] auto as_signal() -> State<T> & { return m_state; }

  private:
    State<T> m_state;
    T m_current;
    Tween<T> m_tween;
    AnimationController m_controller{ 1.0 };
    bool m_animating = false;
};

} // namespace aurora

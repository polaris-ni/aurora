#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

#include "aurora/animation/animator.h"
#include "aurora/core/accessibility.h"
#include "aurora/state/state.h"

namespace aurora {

/// @brief 弹簧物理描述（参考 Flutter `SpringDescription`）。
///
/// 阻尼谐振子：m·x'' + c·x' + k·x = 0（相对目标位移）。
struct SpringDescription {
    double stiffness = 170.0;  ///< 刚度 k（越大回弹越快）
    double damping = 26.0;  ///< 阻尼 c（越大越稳）
    double mass = 1.0;  ///< 质量 m

    /// @brief 无阻尼固有角频率 ω0。
    /// @return sqrt(stiffness / mass)（rad/s）。
    [[nodiscard]] auto natural_frequency() const -> double { return std::sqrt(stiffness / mass); }

    /// @brief 阻尼比 ζ。
    /// @brief 阻尼比 ζ。
    /// @return damping / (2·sqrt(stiffness·mass))；小于 1 欠阻尼、等于 1 临界、大于 1 过阻尼。
    [[nodiscard]] auto damping_ratio() const -> double { return damping / (2.0 * std::sqrt(stiffness * mass)); }
};

/// @brief 阻尼弹簧模拟：从 start 收敛到 end，给定初速度（单位/秒）。
///
/// 提供任意时刻位置（闭合解，统一覆盖欠阻尼/临界阻尼/过阻尼三种情形），
/// 供 spring 动画在每帧 `tick` 时求值（specification/05-event-navigation.md §6.1，对应 Flutter `SpringSimulation`）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
class SpringSimulation {
  public:
    /// @brief 以弹簧参数、起止位置与初速度构造。
    /// @param spring 弹簧物理描述（刚度/阻尼/质量）。
    /// @param start 起始位置（与目标值同单位域）。
    /// @param end 收敛目标位置。
    /// @param velocity 初速度（单位/秒，默认 0）。
    SpringSimulation(const SpringDescription &spring, double start, double end, double velocity = 0.0)
        : spring_(spring), start_(start), end_(end), velocity_(velocity) {}

    /// @brief 时刻 t（秒）的位置。
    /// @param t 时刻（秒）。
    /// @return 闭合解位置（欠/临界/过阻尼统一覆盖，可能短暂越过 end）。
    [[nodiscard]] auto value(double t) const -> double {
        const double y0 = start_ - end_;
        const double w0 = spring_.natural_frequency();
        const double zeta = spring_.damping_ratio();
        return end_ + displacement(y0, velocity_, w0, zeta, t);
    }

    /// @brief 时刻 t（秒）的速度（数值微分，供 settled 判定）。
    /// @param t 时刻（秒）。
    /// @return 中心差商近似速度（步长 1e-4）。
    [[nodiscard]] auto velocity(double t) const -> double {
        constexpr double h = 1e-4;
        return (value(t + h) - value(t - h)) / (2.0 * h);
    }

    /// @brief 是否已在容差内静止（位置与速度均接近目标）。
    /// @param t 时刻（秒）。
    /// @param tolerance 位置与速度的容差（默认 0.01，与值同单位域）。
    /// @return |value(t)-end| 与 |velocity(t)| 都小于容差时为 true。
    [[nodiscard]] auto is_settled(double t, double tolerance = 0.01) const -> bool {
        return std::abs(value(t) - end_) < tolerance && std::abs(velocity(t)) < tolerance;
    }

    /// @brief 收敛目标位置。
    /// @return 构造时传入的 end。
    [[nodiscard]] auto target() const -> double { return end_; }

  private:
    /// @brief 相对目标的位移 y(t)，y(0)=y0, y'(0)=v0。
    static auto displacement(double y0, double v0, double w0, double zeta, double t) -> double {
        if (t <= 0.0) {
            return y0;
        }
        if (std::abs(zeta - 1.0) < 1e-6) {
            // 临界阻尼：y(t) = (A + B·t)·e^(-w0·t)
            const double a = y0;
            const double b = v0 + (w0 * y0);
            return (a + (b * t)) * std::exp(-w0 * t);
        }
        if (zeta < 1.0) {
            // 欠阻尼：y(t) = e^(-ζw0 t)·(A·cos(ωd t) + B·sin(ωd t))
            const double wd = w0 * std::sqrt(1.0 - (zeta * zeta));
            const double a = y0;
            const double b = (v0 + (zeta * w0 * y0)) / wd;
            return std::exp(-zeta * w0 * t) * ((a * std::cos(wd * t)) + (b * std::sin(wd * t)));
        }
        // 过阻尼：y(t) = A·e^(r1 t) + B·e^(r2 t)
        const double s = w0 * std::sqrt((zeta * zeta) - 1.0);
        const double r1 = (-zeta * w0) + s;
        const double r2 = (-zeta * w0) - s;
        const double a = (v0 - (r2 * y0)) / (r1 - r2);
        const double b = y0 - a;
        return (a * std::exp(r1 * t)) + (b * std::exp(r2 * t));
    }

    SpringDescription spring_;
    double start_ = 0.0;
    double end_ = 0.0;
    double velocity_ = 0.0;
};

/// @brief 弹簧到状态的即插封装：把「每帧 `value(t)` 写 State + `is_settled` 定格停帧」收成一处
/// （specification/05-event-navigation.md §6.1，对应 Flutter 在 `SpringSimulation` 之上的驱动层）。
///
/// `SpringSimulation` 仍是纯求解器（签名与语义不动），本类型只补**驱动**这一层：自持模拟时间轴、
/// 逐帧写目标 `State<double>`、收敛后定格于目标值并停帧（不再增长时间、不再写 State、不再让
/// `Animator::has_active()` 恒真）。漏掉收敛判定是这类手写最常见的缺陷（表现为「定格前多写几帧」
/// 或「永不停帧的空转」），故本封装把判定收在唯一一处。
///
/// 帧步长一律经 `scaled_frame_dt` 漏斗：应用内全局时间缩放对弹簧生效，`0.0`（冻结）时本帧不推进
/// 时间轴、不写 State；`reduce_motion` 优先于缩放，单帧直接落目标并结束（与
/// `AnimationController::tick` 的短路语义同源）。
///
/// 只做标量 `double`：向量 / 矩形 / 颜色弹簧由调用方组合多个实例，本类型不泛化。
/// 弹簧可能越过目标（欠阻尼回弹），默认写出**未夹取**的原值；值域受限的目标（如 0..1 进度）
/// 用 `set_range` 声明夹取窗口。
///
/// 用法：
/// @code
/// au::State<double> progress{0.0};
/// au::SpringAnimation spring{progress};
/// spring.on_settled([]() { /* 收尾 */ });
/// spring.animate_to(1.0, au::SpringDescription{}, /*initial_velocity=*/240.0);
/// // 每帧自驱：spring.tick(dt);  或接入帧循环：spring.attach(app.animator());
/// @endcode
///
/// @note Thread: main-thread only
/// @note Side-effects: writes the bound State<double> each frame while running
/// @note Rebuildable: no
class SpringAnimation {
  public:
    /// @brief 以写回目标构造句柄。
    /// @param target 写入目标：内部持有其**句柄副本**（与来源共享，见 `State` 的复制语义），
    ///               故不要求实参比本弹簧存活更久；副本的写入对来源的全部句柄可见。
    explicit SpringAnimation(const State<double> &target) : m_(std::make_shared<Payload>(Payload{target})) {}

    /// @brief 设定写出值的夹取窗口（弹簧越界回弹时用于值域受限的目标，如 0..1 进度）。
    /// @param lo 下界。
    /// @param hi 上界（`hi < lo` 时按 `lo` 退化，保持确定序）。
    auto set_range(double lo, double hi) const -> void { m_->range = Range{.lo = lo, .hi = std::max(lo, hi)}; }

    /// @brief 起播：从目标 `State` 当前值补间到 `target`。
    /// @param target 收敛目标值（`is_settled` 后定格于此值）。
    /// @param spring 弹簧物理描述（刚度 / 阻尼 / 质量）。
    /// @param initial_velocity 初速度（单位/秒，拖拽速度交接用）；缺省 0。
    /// @param tolerance 收敛容差——**与目标值同单位域**（不是归一化时间），沿用
    ///                  `SpringSimulation::is_settled` 的缺省 `0.01`；量纲随目标变化，调用方据此取值。
    auto animate_to(double target, const SpringDescription &spring, double initial_velocity = 0.0,
                    double tolerance = 0.01) const -> void {
        m_->sim = SpringSimulation(spring, m_->target.get(), target, initial_velocity);
        m_->tolerance = tolerance;
        m_->elapsed = 0.0;
        m_->running = true;
        // 常驻活跃登记：无限循环的时钟控制器让弹簧在途期间 has_active() 恒真，
        // 否则 Application::wait_once 会深睡、把半途弹簧冻在原地。
        m_->clock.reset(0.0);
        m_->clock.repeat(-1);
        m_->clock.forward(0.0);
    }

    /// @brief 自驱动一帧（无 `Animator` 时手动推进；帧步长在此经缩放漏斗折算）。
    /// @param raw_dt_seconds 裸帧间隔（秒）；无运行中 Animator 时按恒等处理，故单测手喂 dt 不受影响。
    auto tick(double raw_dt_seconds) const -> void { m_->advance(scaled_frame_dt(raw_dt_seconds)); }

    /// @brief 接入 `Animator` 帧循环（句柄可离开作用域：帧循环持载荷副本，不悬垂）。
    ///
    /// 经 `add_frame_binding` 登记，归属时钟控制器，故 `Animator::remove(clock)` 会连同本绑定一并
    /// 摘除（同 §6.2 的可注销不变量）。此处收到的 dt 已由 Animator 折算，不再二次经漏斗（防双层缩放）。
    /// @param anim 目标 Animator。
    auto attach(Animator &anim) const -> void {
        anim.drive(m_->clock);
        const auto p = m_;
        anim.add_frame_binding([p](double dt) -> void { p->advance(dt); }, m_->clock);
    }

    /// @brief 本弹簧是否仍在推进（起播后到 `is_settled` 定格 / reduce_motion 短路前）。
    /// @return 在途为 true；此时若已 attach，`Animator::has_active()` 亦为真。
    [[nodiscard]] auto is_running() const -> bool { return m_->running; }

    /// @brief 最近一次起播的收敛目标。
    /// @return `animate_to` 传入的 target（从未起播时为 0.0）。
    [[nodiscard]] auto target() const -> double { return m_->sim ? m_->sim->target() : 0.0; }

    /// @brief 注册收敛（定格）回调：每次 `animate_to` 至多触发一次。
    /// @param cb 回调（覆写式登记；传空 std::function 即清除）。
    auto on_settled(std::function<void()> cb) const -> void { m_->on_settled = std::move(cb); }

    /// @brief 时钟控制器访问（一般不需要；`detach` 场景可用它作 `Animator::remove` 的键）。
    /// @return 内部拥有的登记用控制器引用。
    [[nodiscard]] auto clock() -> AnimationController & { return m_->clock; }

  private:
    /// @brief 写出值夹取窗口（值域受限的目标用）。
    struct Range {
        double lo = 0.0;  ///< 下界
        double hi = 1.0;  ///< 上界
    };

    /// @brief 驱动载荷：句柄按值拷贝共享同一份（同 `AnimatedValue` / `TimelinePlayer` 模式）。
    struct Payload {
        /// @brief 以写回目标构造。
        /// @param t 目标 State 句柄（复制后与本弹簧共享同一来源）。
        explicit Payload(const State<double> &t) : target(t) {}

        /// @brief 推进一帧（入参须是**已折算**的动画时间轴步长：自驱走 `tick`，attach 走绑定回调）。
        /// @param dt 本帧动画时间轴步长（秒）；`<= 0` 表示冻结或零步长，不推进。
        auto advance(double dt) -> void {
            if (!running || !sim.has_value()) {
                return;  // 未在途（含已定格：收敛后不再增长时间、不再写 State，无幽灵帧）
            }
            // reduce_motion 优先：单帧落目标并结束，与 AnimationController::tick 的短路语义一致。
            if (current_accessibility_settings().reduce_motion) {
                settle();
                return;
            }
            if (dt <= 0.0) {
                return;  // 时间缩放冻结：时间轴不推进，State 保持当前值（解冻后从原处续）
            }
            elapsed += dt;
            double v = sim->value(elapsed);
            if (range.has_value()) {
                v = std::clamp(v, range->lo, range->hi);
            }
            target.set(v);
            if (sim->is_settled(elapsed, tolerance)) {
                settle();
            }
        }

        /// @brief 定格于目标值并停帧：写终值、清在途、停掉登记时钟（使 has_active() 转假）、回调一次。
        auto settle() -> void {
            if (sim.has_value()) {
                target.set(sim->target());
            }
            running = false;
            clock.stop();
            if (on_settled) {
                on_settled();
            }
        }

        State<double> target;  ///< 写回目标句柄（与调用方共享同一来源，非裸指针引用）
        AnimationController clock{1.0};  ///< 拥有的登记时钟（进度值不使用，只作活跃性与注销键）
        std::optional<SpringSimulation> sim;  ///< 在途模拟（animate_to 建）
        std::optional<Range> range;  ///< 写出值夹取窗口（未设则原样写出，允许回弹越界）
        double elapsed = 0.0;  ///< 弹簧内部已推进时间（秒）
        double tolerance = 0.01;  ///< 收敛容差（与目标值同单位域）
        bool running = false;  ///< 是否在途（animate_to 置真，settle 清假）
        std::function<void()> on_settled;  ///< 定格回调（每次 animate_to 至多一次）
    };

    std::shared_ptr<Payload> m_;  ///< 驱动载荷（跨句柄拷贝共享）
};

}  // namespace aurora

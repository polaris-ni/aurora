#pragma once

#include <cmath>

namespace aurora {

/**
 * @brief 弹簧物理描述（参考 Flutter `SpringDescription`）。
 *
 * 阻尼谐振子：m·x'' + c·x' + k·x = 0（相对目标位移）。
 */
struct SpringDescription {
    double stiffness = 170.0;  ///< 刚度 k（越大回弹越快）
    double damping = 26.0;  ///< 阻尼 c（越大越稳）
    double mass = 1.0;  ///< 质量 m

    [[nodiscard]] auto natural_frequency() const -> double { return std::sqrt(stiffness / mass); }
    [[nodiscard]] auto damping_ratio() const -> double { return damping / (2.0 * std::sqrt(stiffness * mass)); }
};

/**
 * @brief 阻尼弹簧模拟：从 start 收敛到 end，给定初速度（单位/秒）。
 *
 * 提供任意时刻位置（闭合解，统一覆盖欠阻尼/临界阻尼/过阻尼三种情形），
 * 供 spring 动画在每帧 `tick` 时求值（specification/05-event-navigation.md §6.1，对应 Flutter `SpringSimulation`）。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class SpringSimulation {
  public:
    SpringSimulation(const SpringDescription &spring, double start, double end, double velocity = 0.0)
        : spring_(spring), start_(start), end_(end), velocity_(velocity) {}

    /// @brief 时刻 t（秒）的位置。
    [[nodiscard]] auto value(double t) const -> double {
        const double y0 = start_ - end_;
        const double w0 = spring_.natural_frequency();
        const double zeta = spring_.damping_ratio();
        return end_ + displacement(y0, velocity_, w0, zeta, t);
    }

    /// @brief 时刻 t（秒）的速度（数值微分，供 settled 判定）。
    [[nodiscard]] auto velocity(double t) const -> double {
        constexpr double h = 1e-4;
        return (value(t + h) - value(t - h)) / (2.0 * h);
    }

    /// @brief 是否已在容差内静止（位置与速度均接近目标）。
    [[nodiscard]] auto is_settled(double t, double tolerance = 0.01) const -> bool {
        return std::abs(value(t) - end_) < tolerance && std::abs(velocity(t)) < tolerance;
    }

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

}  // namespace aurora

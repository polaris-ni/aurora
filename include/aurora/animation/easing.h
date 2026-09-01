#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>

namespace aurora {

/// @brief 缓动曲线类型（命名曲线）。
enum class CurveKind : std::uint8_t {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    EaseInSine,
    EaseOutSine,
    EaseInOutSine,
    EaseInQuad,
    EaseOutQuad,
    EaseInOutQuad,
    EaseInCubic,
    EaseOutCubic,
    EaseInOutCubic,
    BounceOut,
    Custom, ///< 由 std::function 定义
};

/**
 * @brief 缓动曲线：将归一化时间 t∈[0,1] 映射为缓动后的进度 [0,1]。
 *
 * 命名曲线用 switch 实现（无堆分配、零成本）；自定义曲线用 `std::function`。
 * 与 Flutter `Curve` / CSS `cubic-bezier` 对应，供 `Tween` 在插值时塑形。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class Curve {
  public:
    Curve() = default;
    explicit Curve(CurveKind k) : m_kind(k) {}
    explicit Curve(std::function<double(double)> fn) : m_kind(CurveKind::Custom), m_fn(std::move(fn)) {}

    /// @brief 计算 t 对应的缓动进度（输入/输出均夹入 [0,1]）。
    [[nodiscard]] auto transform(double t) const -> double {
        t = std::clamp(t, 0.0, 1.0);
        if (m_kind == CurveKind::Custom && m_fn) {
            return std::clamp(m_fn(t), 0.0, 1.0);
        }
        return std::clamp(eval(t), 0.0, 1.0);
    }

    [[nodiscard]] auto kind() const -> CurveKind { return m_kind; }

  private:
    [[nodiscard]] auto eval(double t) const -> double {
        switch (m_kind) {
        case CurveKind::Linear: return t;
        case CurveKind::EaseIn: return t * t * t;
        case CurveKind::EaseOut: return 1.0 - std::pow(1.0 - t, 3.0);
        case CurveKind::EaseInOut: return t < 0.5 ? 4.0 * t * t * t : 1.0 - (std::pow((-2.0 * t) + 2.0, 3.0) / 2.0);
        case CurveKind::EaseInSine: return 1.0 - std::cos(t * std::numbers::pi / 2.0);
        case CurveKind::EaseOutSine: return std::sin(t * std::numbers::pi / 2.0);
        case CurveKind::EaseInOutSine: return -(std::cos(std::numbers::pi * t) - 1.0) / 2.0;
        case CurveKind::EaseInQuad: return t * t;
        case CurveKind::EaseOutQuad: return t * (2.0 - t);
        case CurveKind::EaseInOutQuad: return t < 0.5 ? 2.0 * t * t : 1.0 - (std::pow((-2.0 * t) + 2.0, 2.0) / 2.0);
        case CurveKind::EaseInCubic: return t * t * t;
        case CurveKind::EaseOutCubic: return 1.0 - std::pow(1.0 - t, 3.0);
        case CurveKind::EaseInOutCubic:
            return t < 0.5 ? 4.0 * t * t * t : 1.0 - (std::pow((-2.0 * t) + 2.0, 3.0) / 2.0);
        case CurveKind::BounceOut: return bounce_out(t);
        case CurveKind::Custom: return t;
        }
        return t;
    }

    static auto bounce_out(double t) -> double {
        constexpr double n1 = 7.5625;
        constexpr double d1 = 2.75;
        if (t < 1.0 / d1) {
            return n1 * t * t;
        }
        if (t < 2.0 / d1) {
            t -= 1.5 / d1;
            return (n1 * t * t) + 0.75;
        }
        if (t < 2.5 / d1) {
            t -= 2.25 / d1;
            return (n1 * t * t) + 0.9375;
        }
        t -= 2.625 / d1;
        return (n1 * t * t) + 0.984375;
    }

    CurveKind m_kind = CurveKind::Linear;
    std::function<double(double)> m_fn;
};

/// @brief 常用曲线集合（命名工厂，对应 specification/05-event-navigation.md §6.1）。
struct Curves {
    static auto linear() -> Curve { return Curve{ CurveKind::Linear }; }
    static auto ease_in() -> Curve { return Curve{ CurveKind::EaseIn }; }
    static auto ease_out() -> Curve { return Curve{ CurveKind::EaseOut }; }
    static auto ease_in_out() -> Curve { return Curve{ CurveKind::EaseInOut }; }
    static auto ease_in_sine() -> Curve { return Curve{ CurveKind::EaseInSine }; }
    static auto ease_out_sine() -> Curve { return Curve{ CurveKind::EaseOutSine }; }
    static auto ease_in_out_sine() -> Curve { return Curve{ CurveKind::EaseInOutSine }; }
    static auto ease_in_quad() -> Curve { return Curve{ CurveKind::EaseInQuad }; }
    static auto ease_out_quad() -> Curve { return Curve{ CurveKind::EaseOutQuad }; }
    static auto ease_in_out_quad() -> Curve { return Curve{ CurveKind::EaseInOutQuad }; }
    static auto ease_in_cubic() -> Curve { return Curve{ CurveKind::EaseInCubic }; }
    static auto ease_out_cubic() -> Curve { return Curve{ CurveKind::EaseOutCubic }; }
    static auto ease_in_out_cubic() -> Curve { return Curve{ CurveKind::EaseInOutCubic }; }
    static auto bounce_out() -> Curve { return Curve{ CurveKind::BounceOut }; }
};

} // namespace aurora

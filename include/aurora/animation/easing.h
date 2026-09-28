#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>

namespace aurora {

/// @brief 缓动曲线类型（命名曲线）。
enum class CurveKind : std::uint8_t {
    Linear,  ///< 恒等映射 p(t)=t
    EaseIn,  ///< 三次缓入 p=t³（起慢终快）
    EaseOut,  ///< 三次缓出 p=1-(1-t)³（起快终慢）
    EaseInOut,  ///< 三次两端对称：前半 4t³，后半镜像
    EaseInSine,  ///< 正弦缓入 p=1-cos(πt/2)
    EaseOutSine,  ///< 正弦缓出 p=sin(πt/2)
    EaseInOutSine,  ///< 正弦两端对称 p=-(cos(πt)-1)/2
    EaseInQuad,  ///< 二次缓入 p=t²
    EaseOutQuad,  ///< 二次缓出 p=t(2-t)
    EaseInOutQuad,  ///< 二次两端对称：前半 2t²，后半镜像
    EaseInCubic,  ///< 三次缓入（求值同 EaseIn）
    EaseOutCubic,  ///< 三次缓出（求值同 EaseOut）
    EaseInOutCubic,  ///< 三次两端对称（求值同 EaseInOut）
    BounceOut,  ///< 弹跳缓出：按 n1=7.5625、d1=2.75 四段落地反弹
    Custom,  ///< 由 std::function 定义
};

/// @brief 缓动曲线：将归一化时间 t∈[0,1] 映射为缓动后的进度 [0,1]。
///
/// 命名曲线用 switch 实现（无堆分配、零成本）；自定义曲线用 `std::function`。
/// 与 Flutter `Curve` / CSS `cubic-bezier` 对应，供 `Tween` 在插值时塑形。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
class Curve {
  public:
    Curve() = default;

    /// @brief 以命名曲线种类构造。
    /// @param k 曲线种类；为 Custom 但未配 std::function 时按线性求值。
    explicit Curve(CurveKind k) : kind_(k) {}

    /// @brief 自定义曲线构造：kind_ 置 Custom。
    /// @param fn 进度映射函数；transform 会把其入参与输出均夹入 [0,1]。
    explicit Curve(std::function<double(double)> fn) : kind_(CurveKind::Custom), fn_(std::move(fn)) {}

    /// @brief 计算 t 对应的缓动进度（输入/输出均夹入 [0,1]）。
    /// @param t 归一化时间，[0,1]；越界先夹取。
    /// @return 缓动后的进度，恒在 [0,1]（Custom 且 fn 为空时按线性返回）。
    [[nodiscard]] auto transform(double t) const -> double {
        t = std::clamp(t, 0.0, 1.0);
        if (kind_ == CurveKind::Custom && fn_) {
            return std::clamp(fn_(t), 0.0, 1.0);
        }
        return std::clamp(eval(t), 0.0, 1.0);
    }

    /// @brief 本曲线的种类。
    /// @return 构造时确定的 CurveKind（Custom 不代表 fn 一定非空）。
    [[nodiscard]] auto kind() const -> CurveKind { return kind_; }

  private:
    [[nodiscard]] auto eval(double t) const -> double {
        switch (kind_) {
            case CurveKind::Linear:
                return t;
            case CurveKind::EaseIn:
                return t * t * t;
            case CurveKind::EaseOut:
                return 1.0 - std::pow(1.0 - t, 3.0);
            case CurveKind::EaseInOut:
                return t < 0.5 ? 4.0 * t * t * t : 1.0 - (std::pow((-2.0 * t) + 2.0, 3.0) / 2.0);
            case CurveKind::EaseInSine:
                return 1.0 - std::cos(t * std::numbers::pi / 2.0);
            case CurveKind::EaseOutSine:
                return std::sin(t * std::numbers::pi / 2.0);
            case CurveKind::EaseInOutSine:
                return -(std::cos(std::numbers::pi * t) - 1.0) / 2.0;
            case CurveKind::EaseInQuad:
                return t * t;
            case CurveKind::EaseOutQuad:
                return t * (2.0 - t);
            case CurveKind::EaseInOutQuad:
                return t < 0.5 ? 2.0 * t * t : 1.0 - (std::pow((-2.0 * t) + 2.0, 2.0) / 2.0);
            case CurveKind::EaseInCubic:
                return t * t * t;
            case CurveKind::EaseOutCubic:
                return 1.0 - std::pow(1.0 - t, 3.0);
            case CurveKind::EaseInOutCubic:
                return t < 0.5 ? 4.0 * t * t * t : 1.0 - (std::pow((-2.0 * t) + 2.0, 3.0) / 2.0);
            case CurveKind::BounceOut:
                return bounce_out(t);
            case CurveKind::Custom:
                return t;
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

    CurveKind kind_ = CurveKind::Linear;
    std::function<double(double)> fn_;
};

/// @brief 常用曲线集合（命名工厂，对应 specification/05-event-navigation.md §6.1）。
struct Curves {
    /// @brief 线性曲线（进度与时间成正比）。
    /// @return kind 为 Linear 的 Curve。
    static auto linear() -> Curve { return Curve{CurveKind::Linear}; }

    /// @brief 三次缓入曲线（起慢终快）。
    /// @return kind 为 EaseIn 的 Curve。
    static auto ease_in() -> Curve { return Curve{CurveKind::EaseIn}; }

    /// @brief 三次缓出曲线（起快终慢）。
    /// @return kind 为 EaseOut 的 Curve。
    static auto ease_out() -> Curve { return Curve{CurveKind::EaseOut}; }

    /// @brief 三次两端对称曲线。
    /// @return kind 为 EaseInOut 的 Curve。
    static auto ease_in_out() -> Curve { return Curve{CurveKind::EaseInOut}; }

    /// @brief 正弦缓入曲线。
    /// @return kind 为 EaseInSine 的 Curve。
    static auto ease_in_sine() -> Curve { return Curve{CurveKind::EaseInSine}; }

    /// @brief 正弦缓出曲线。
    /// @return kind 为 EaseOutSine 的 Curve。
    static auto ease_out_sine() -> Curve { return Curve{CurveKind::EaseOutSine}; }

    /// @brief 正弦两端对称曲线。
    /// @return kind 为 EaseInOutSine 的 Curve。
    static auto ease_in_out_sine() -> Curve { return Curve{CurveKind::EaseInOutSine}; }

    /// @brief 二次缓入曲线。
    /// @return kind 为 EaseInQuad 的 Curve。
    static auto ease_in_quad() -> Curve { return Curve{CurveKind::EaseInQuad}; }

    /// @brief 二次缓出曲线。
    /// @return kind 为 EaseOutQuad 的 Curve。
    static auto ease_out_quad() -> Curve { return Curve{CurveKind::EaseOutQuad}; }

    /// @brief 二次两端对称曲线。
    /// @return kind 为 EaseInOutQuad 的 Curve。
    static auto ease_in_out_quad() -> Curve { return Curve{CurveKind::EaseInOutQuad}; }

    /// @brief 三次缓入曲线（求值同 ease_in）。
    /// @return kind 为 EaseInCubic 的 Curve。
    static auto ease_in_cubic() -> Curve { return Curve{CurveKind::EaseInCubic}; }

    /// @brief 三次缓出曲线（求值同 ease_out）。
    /// @return kind 为 EaseOutCubic 的 Curve。
    static auto ease_out_cubic() -> Curve { return Curve{CurveKind::EaseOutCubic}; }

    /// @brief 三次两端对称曲线（求值同 ease_in_out）。
    /// @return kind 为 EaseInOutCubic 的 Curve。
    static auto ease_in_out_cubic() -> Curve { return Curve{CurveKind::EaseInOutCubic}; }

    /// @brief 弹跳缓出曲线（落地后分段反弹）。
    /// @return kind 为 BounceOut 的 Curve。
    static auto bounce_out() -> Curve { return Curve{CurveKind::BounceOut}; }
};

}  // namespace aurora

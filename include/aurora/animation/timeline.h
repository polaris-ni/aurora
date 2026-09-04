#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aurora/animation/easing.h"
#include "aurora/core/color.h"
#include "aurora/core/types.h"

namespace aurora {

// ---- 通用插值（lerp） ----
// 算术类型（int/float/double/...）走通用模板；几何/颜色类型走特化重载。
template <typename T>
    requires std::is_arithmetic_v<T>
auto lerp(T a, T b, double t) -> T {
    return static_cast<T>(a + ((b - a) * t));
}

inline auto lerp(const Point &a, const Point &b, double t) -> Point {
    return Point{.x = lerp(a.x, b.x, t), .y = lerp(a.y, b.y, t)};
}

inline auto lerp(const Size &a, const Size &b, double t) -> Size {
    return Size{.width = lerp(a.width, b.width, t), .height = lerp(a.height, b.height, t)};
}

inline auto lerp(const Color &a, const Color &b, double t) -> Color {
    return Color{
        static_cast<uint8_t>(std::lround(lerp(a.r, b.r, t))),
        static_cast<uint8_t>(std::lround(lerp(a.g, b.g, t))),
        static_cast<uint8_t>(std::lround(lerp(a.b, b.b, t))),
        static_cast<uint8_t>(std::lround(lerp(a.a, b.a, t))),
    };
}

inline auto lerp(const EdgeInsets &a, const EdgeInsets &b, double t) -> EdgeInsets {
    return EdgeInsets{.left = lerp(a.left, b.left, t),
                      .top = lerp(a.top, b.top, t),
                      .right = lerp(a.right, b.right, t),
                      .bottom = lerp(a.bottom, b.bottom, t)};
}

inline auto lerp(const Rect &a, const Rect &b, double t) -> Rect {
    return Rect{.origin = lerp(a.origin, b.origin, t), .size = lerp(a.size, b.size, t)};
}

/**
 * @brief 线性补间：在 [begin,end] 间按曲线插值的补间（t∈[0,1] 为归一化进度）。
 *
 * 与 Flutter `Tween` 对应：`value(t)` = `lerp(begin, end, curve.transform(t))`。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template <typename T>
class Tween {
  public:
    Tween() = default;
    Tween(T begin, T end, Curve curve = Curve{})
        : begin_(std::move(begin)), end_(std::move(end)), curve_(std::move(curve)) {}

    /// @brief 计算归一化进度 t 处的插值结果。
    [[nodiscard]] auto value(double t) const -> T { return lerp(begin_, end_, curve_.transform(t)); }

    auto begin() const -> const T & { return begin_; }
    auto end() const -> const T & { return end_; }
    [[nodiscard]] auto curve() const -> const Curve & { return curve_; }
    auto set_begin(T v) -> void { begin_ = std::move(v); }
    auto set_end(T v) -> void { end_ = std::move(v); }
    auto set_curve(Curve c) -> void { curve_ = std::move(c); }

  private:
    T begin_{};
    T end_{};
    Curve curve_;
};

/**
 * @brief 关键帧补间：在 (time,value) 停靠点之间线性插值（time∈[0,1]）。
 *
 * 停靠点按时间排序；区间外用端点值（对应 Flutter `Keyframe` / 时间线）。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
template <typename T>
class Keyframes {
  public:
    struct Stop {
        double time = 0.0;  ///< 归一化时间 [0,1]
        T value;
    };

    Keyframes() = default;
    explicit Keyframes(std::vector<Stop> stops) : stops_(std::move(stops)) {
        std::sort(stops_.begin(), stops_.end(), [](const Stop &a, const Stop &b) -> auto { return a.time < b.time; });
    }

    /// @brief 计算时刻 t 处的值（线性插值相邻停靠点）。
    [[nodiscard]] auto value(double t) const -> T {
        if (stops_.empty()) {
            return T{};
        }
        if (t <= stops_.front().time) {
            return stops_.front().value;
        }
        if (t >= stops_.back().time) {
            return stops_.back().value;
        }
        for (std::size_t i = 1; i < stops_.size(); ++i) {
            if (t <= stops_[i].time) {
                const double a = stops_[i - 1].time;
                const double b = stops_[i].time;
                const double local = (b > a) ? (t - a) / (b - a) : 0.0;
                return lerp(stops_[i - 1].value, stops_[i].value, local);
            }
        }
        return stops_.back().value;
    }

    [[nodiscard]] auto stops() const -> const std::vector<Stop> & { return stops_; }

  private:
    std::vector<Stop> stops_;
};

}  // namespace aurora

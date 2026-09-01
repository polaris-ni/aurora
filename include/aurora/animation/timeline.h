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
template<typename T>
    requires std::is_arithmetic_v<T>
auto lerp(T a, T b, double t) -> T {
    return static_cast<T>(a + ((b - a) * t));
}

inline auto lerp(const Point &a, const Point &b, double t) -> Point {
    return Point{ .x = lerp(a.x, b.x, t), .y = lerp(a.y, b.y, t) };
}

inline auto lerp(const Size &a, const Size &b, double t) -> Size {
    return Size{ .width = lerp(a.width, b.width, t), .height = lerp(a.height, b.height, t) };
}

inline auto lerp(const Color &a, const Color &b, double t) -> Color {
    return Color{
        static_cast<uint8_t>(std::lround(lerp(a.m_r, b.m_r, t))),
        static_cast<uint8_t>(std::lround(lerp(a.m_g, b.m_g, t))),
        static_cast<uint8_t>(std::lround(lerp(a.m_b, b.m_b, t))),
        static_cast<uint8_t>(std::lround(lerp(a.m_a, b.m_a, t))),
    };
}

inline auto lerp(const EdgeInsets &a, const EdgeInsets &b, double t) -> EdgeInsets {
    return EdgeInsets{ .left = lerp(a.left, b.left, t),
                       .top = lerp(a.top, b.top, t),
                       .right = lerp(a.right, b.right, t),
                       .bottom = lerp(a.bottom, b.bottom, t) };
}

inline auto lerp(const Rect &a, const Rect &b, double t) -> Rect {
    return Rect{ .origin = lerp(a.origin, b.origin, t), .size = lerp(a.size, b.size, t) };
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
template<typename T> class Tween {
  public:
    Tween() = default;
    Tween(T begin, T end, Curve curve = Curve{})
        : m_begin(std::move(begin)), m_end(std::move(end)), m_curve(std::move(curve)) {}

    /// @brief 计算归一化进度 t 处的插值结果。
    [[nodiscard]] auto value(double t) const -> T { return lerp(m_begin, m_end, m_curve.transform(t)); }

    auto begin() const -> const T & { return m_begin; }
    auto end() const -> const T & { return m_end; }
    [[nodiscard]] auto curve() const -> const Curve & { return m_curve; }
    auto set_begin(T v) -> void { m_begin = std::move(v); }
    auto set_end(T v) -> void { m_end = std::move(v); }
    auto set_curve(Curve c) -> void { m_curve = std::move(c); }

  private:
    T m_begin{};
    T m_end{};
    Curve m_curve;
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
template<typename T> class Keyframes {
  public:
    struct Stop {
        double time = 0.0; ///< 归一化时间 [0,1]
        T value;
    };

    Keyframes() = default;
    explicit Keyframes(std::vector<Stop> stops) : m_stops(std::move(stops)) {
        std::sort(m_stops.begin(), m_stops.end(), [](const Stop &a, const Stop &b) -> auto { return a.time < b.time; });
    }

    /// @brief 计算时刻 t 处的值（线性插值相邻停靠点）。
    [[nodiscard]] auto value(double t) const -> T {
        if (m_stops.empty()) {
            return T{};
        }
        if (t <= m_stops.front().time) {
            return m_stops.front().value;
        }
        if (t >= m_stops.back().time) {
            return m_stops.back().value;
        }
        for (std::size_t i = 1; i < m_stops.size(); ++i) {
            if (t <= m_stops[i].time) {
                const double a = m_stops[i - 1].time;
                const double b = m_stops[i].time;
                const double local = (b > a) ? (t - a) / (b - a) : 0.0;
                return lerp(m_stops[i - 1].value, m_stops[i].value, local);
            }
        }
        return m_stops.back().value;
    }

    [[nodiscard]] auto stops() const -> const std::vector<Stop> & { return m_stops; }

  private:
    std::vector<Stop> m_stops;
};

} // namespace aurora

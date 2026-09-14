#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <variant>
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
    // 通道先提升 double 再插值：若直接调通用模板 lerp<uint8_t>，浮点结果会在模板内
    // 先截断回 uint8_t（127.5 → 127），外层 lround 的四舍五入就永远轮不到。
    auto mix = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(static_cast<double>(x) + (static_cast<double>(y) - x) * t));
    };
    return Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
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
        // 稳定排序：同时刻停靠点保持插入次序（取值确定、不受排序抖动影响）。
        std::stable_sort(stops_.begin(), stops_.end(),
                         [](const Stop &a, const Stop &b) -> auto { return a.time < b.time; });
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

// ---- 时间轴编排（Flutter++：纯值区间构建器） ----

class TimelineResolved;  // 前置声明：TimelineSpec::build 返回类型（定义见下）

/**
 * @brief 归一化子区间 [begin, end]（对应 Flutter `Interval`）。
 *
 * 区间树求值的核心映射：总进度 t → 本区间局部进度。区间外保持端点语义
 * （t ≤ begin → 0，t ≥ end → 1），与 `Keyframes` 的区间外夹取一致。
 *
 * @note Thread: thread-safe (pure value type)
 */
struct TimelineInterval {
    double begin = 0.0;
    double end = 1.0;

    auto operator==(const TimelineInterval &) const -> bool = default;

    [[nodiscard]] auto contains(double t) const -> bool { return t >= begin && t <= end; }

    /// @brief 总进度 t → 本区间局部进度 [0,1]（区间外夹取端点）。
    /// 零宽区间（begin == end，夹取后的退化段）：t ≥ begin 即完成（1）。
    [[nodiscard]] auto local(double t) const -> double {
        if (t >= end) {
            return 1.0;
        }
        if (t <= begin) {
            return 0.0;
        }
        return (end > begin) ? ((t - begin) / (end - begin)) : 1.0;
    }
};

/**
 * @brief 时间轴构建器：方法链声明子段结构，`build()` 展开为区间表（纯值类型）。
 *
 * 三种组合子覆盖顺序 / 并行 / 交错（Flutter 官方 staggered 模式须手算各子段 Interval
 * 端点的痛点在此由构建器自动完成）：
 *
 * @code
 *   auto tl = au::TimelineSpec::sequence()
 *                 .add(0.2)                                        // 叶子 0
 *                 .add(au::TimelineSpec::parallel()
 *                          .add(0.3)                               // 叶子 1
 *                          .add(0.5))                              // 叶子 2
 *                 .add(0.1)                                        // 叶子 3
 *                 .build();
 *   // 总时长 0.2 + max(0.3, 0.5) + 0.1 = 0.8s；槽位 = 深度优先序 0..3。
 * @endcode
 *
 * 区间计算规则（构建期一次完成）：
 * - `sequence`：游标推进，子树整体平移到当前游标；
 * - `parallel`：子树全部锚定组起点，组长 = max(子时长)，组尾允许间隙；
 * - `staggered(item_dur, gap, count)`：第 i 叶子区间 [i*(item+gap), i*(item+gap)+item]。
 *
 * 叶子时长非正值夹取为 1e-6（同 `AnimationController` 的时长夹取约定）。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class TimelineSpec {
  public:
    /// @brief 空构建器（默认 sequence 语义；可用 assignment 起任一组合子）。
    TimelineSpec() = default;

    /// @brief 顺序组：子段首尾相接。
    [[nodiscard]] static auto sequence() -> TimelineSpec {
        TimelineSpec s;
        s.kind_ = Kind::Sequence;
        return s;
    }

    /// @brief 并行组：子段同起点，组长 = max(子时长)。
    [[nodiscard]] static auto parallel() -> TimelineSpec {
        TimelineSpec s;
        s.kind_ = Kind::Parallel;
        return s;
    }

    /// @brief 交错组（stagger 语法糖）：count 个等长叶子按 gap 递进偏移。
    [[nodiscard]] static auto staggered(double item_duration_s, double gap_s, int count) -> TimelineSpec {
        TimelineSpec s;
        s.kind_ = Kind::Staggered;
        s.item_ = max_duration(item_duration_s);
        s.gap_ = std::max(gap_s, 0.0);
        s.count_ = std::max(count, 0);
        return s;
    }

    /// @brief 追加叶子段（时长秒，非正值夹取 1e-6）。
    auto add(double duration_s) -> TimelineSpec & {
        children_.push_back(Leaf{.duration = max_duration(duration_s)});
        return *this;
    }

    /// @brief 追加子组（嵌套；子组区间随父组规则展开）。
    auto add(TimelineSpec sub) -> TimelineSpec & {
        children_.push_back(std::move(sub));
        return *this;
    }

    /// @brief 本组总时长（秒）：sequence = Σ子，parallel = max(子)，staggered = span。
    [[nodiscard]] auto duration() const -> double {
        switch (kind_) {
            case Kind::Sequence: {
                double sum = 0.0;
                for (const auto &c : children_) {
                    sum += child_duration(c);
                }
                return sum;
            }
            case Kind::Parallel: {
                double span = 0.0;
                for (const auto &c : children_) {
                    span = std::max(span, child_duration(c));
                }
                return span;
            }
            case Kind::Staggered:
                return stagger_span();
        }
        return 0.0;
    }

    /// @brief 展开区间树：叶子按深度优先序获得槽位 0..n-1，各得归一化区间。
    [[nodiscard]] auto build() const -> TimelineResolved;

  private:
    enum class Kind : std::uint8_t { Sequence, Parallel, Staggered };
    struct Leaf {
        double duration = 0.0;
    };

    static auto max_duration(double d) -> double { return std::max(d, 1e-6); }

    [[nodiscard]] auto stagger_span() const -> double {
        if (count_ <= 0) {
            return 0.0;
        }
        return static_cast<double>(count_ - 1) * (item_ + gap_) + item_;
    }

    [[nodiscard]] static auto child_duration(const std::variant<Leaf, TimelineSpec> &c) -> double {
        if (const auto *leaf = std::get_if<Leaf>(&c)) {
            return leaf->duration;
        }
        return std::get<TimelineSpec>(c).duration();
    }

    // 展开递归：offset_s = 组起点相对总时间轴的秒偏移；total_s = 总时长（归一化基准）。
    auto flatten(double offset_s, double total_s, std::vector<TimelineInterval> &out) const -> void {
        switch (kind_) {
            case Kind::Sequence: {
                double cursor = offset_s;
                for (const auto &c : children_) {
                    if (const auto *leaf = std::get_if<Leaf>(&c)) {
                        out.push_back(norm_interval(cursor, leaf->duration, total_s));
                        cursor += leaf->duration;
                    } else {
                        const auto &sub = std::get<TimelineSpec>(c);
                        sub.flatten(cursor, total_s, out);
                        cursor += sub.duration();
                    }
                }
                break;
            }
            case Kind::Parallel: {
                for (const auto &c : children_) {
                    if (const auto *leaf = std::get_if<Leaf>(&c)) {
                        out.push_back(norm_interval(offset_s, leaf->duration, total_s));
                    } else {
                        std::get<TimelineSpec>(c).flatten(offset_s, total_s, out);
                    }
                }
                break;
            }
            case Kind::Staggered: {
                for (int i = 0; i < count_; ++i) {
                    const double start = offset_s + static_cast<double>(i) * (item_ + gap_);
                    out.push_back(norm_interval(start, item_, total_s));
                }
                break;
            }
        }
    }

    static auto norm_interval(double start_s, double dur_s, double total_s) -> TimelineInterval {
        if (total_s <= 0.0) {
            return TimelineInterval{};  // 空 spec：区间退化为全区间 {0,1}
        }
        return TimelineInterval{.begin = std::clamp(start_s / total_s, 0.0, 1.0),
                                .end = std::clamp((start_s + dur_s) / total_s, 0.0, 1.0)};
    }

    Kind kind_ = Kind::Sequence;
    std::vector<std::variant<Leaf, TimelineSpec>> children_;
    // staggered 专用参数
    double item_ = 0.0;
    double gap_ = 0.0;
    int count_ = 0;
};

/**
 * @brief 区间树求值结果：槽位区间表 + 总时长（纯值，拷贝廉价）。
 *
 * 槽位号 = `build()` 时叶子段的深度优先遍历序（与 `add` 调用序一致，构建即确定）。
 * `TimelinePlayer` 按槽位绑定 Tween/State；`local()` 的区间外夹取语义保证未开始的
 * 子段保持 Tween begin 值、已完成的保持 end 值（反向倒放对称成立）。
 *
 * @note Thread: thread-safe (pure value type)
 */
class TimelineResolved {
  public:
    TimelineResolved() = default;

    /// @brief 槽位数（= 叶子段数；空 spec 为 0）。
    [[nodiscard]] auto slot_count() const -> std::size_t { return intervals_.size(); }

    /// @brief 槽位的归一化区间（越界返回全区间 {0,1}，防御式）。
    [[nodiscard]] auto interval(std::size_t slot) const -> TimelineInterval {
        return slot < intervals_.size() ? intervals_[slot] : TimelineInterval{};
    }

    /// @brief 总时长（秒；空 spec 为 0）。
    [[nodiscard]] auto duration() const -> double { return duration_; }

  private:
    friend class TimelineSpec;
    TimelineResolved(std::vector<TimelineInterval> intervals, double duration_s)
        : intervals_(std::move(intervals)), duration_(duration_s) {}

    std::vector<TimelineInterval> intervals_;
    double duration_ = 0.0;
};

inline auto TimelineSpec::build() const -> TimelineResolved {
    const double total = duration();
    std::vector<TimelineInterval> out;
    out.reserve(children_.size());
    flatten(0.0, total, out);
    return TimelineResolved(std::move(out), total);
}

}  // namespace aurora

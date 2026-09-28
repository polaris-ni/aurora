#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "aurora_assert.h"

namespace aurora {

/// @brief 二维点（逻辑像素）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Point {
    float x = 0.0F;  ///< 横坐标（逻辑像素）
    float y = 0.0F;  ///< 纵坐标（逻辑像素）

    /// @brief 逐坐标相加。
    /// @param o 右操作数点。
    /// @return 新点 (x + o.x, y + o.y)。
    [[nodiscard]] constexpr auto operator+(const Point &o) const noexcept -> Point {
        return Point{.x = x + o.x, .y = y + o.y};
    }
    /// @brief 逐坐标相减。
    /// @param o 被减去的点。
    /// @return 新点 (x - o.x, y - o.y)。
    [[nodiscard]] constexpr auto operator-(const Point &o) const noexcept -> Point {
        return Point{.x = x - o.x, .y = y - o.y};
    }
};

/// @brief 尺寸（逻辑像素）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Size {
    float width = 0.0F;  ///< 宽度（逻辑像素）
    float height = 0.0F;  ///< 高度（逻辑像素）

    /// @brief 表示"不限制"的尺寸（用于 wrap_content / 安卓 UNSPECIFIED）。
    /// @return 两轴均为 +inf 的尺寸。
    [[nodiscard]] static constexpr auto infinity() noexcept -> Size {
        return Size{
            .width = std::numeric_limits<float>::infinity(),
            .height = std::numeric_limits<float>::infinity(),
        };
    }

    /// @brief 两轴同乘缩放系数。
    /// @param s 缩放系数。
    /// @return 新尺寸 (width * s, height * s)。
    [[nodiscard]] constexpr auto operator*(float s) const noexcept -> Size {
        return Size{.width = width * s, .height = height * s};
    }
    /// @brief 两轴逐维相加。
    /// @param o 右操作数尺寸。
    /// @return 新尺寸 (width + o.width, height + o.height)。
    [[nodiscard]] constexpr auto operator+(const Size &o) const noexcept -> Size {
        return Size{.width = width + o.width, .height = height + o.height};
    }
    /// @brief 两轴逐维相减。
    /// @param o 被减去的尺寸。
    /// @return 新尺寸 (width - o.width, height - o.height)。
    [[nodiscard]] constexpr auto operator-(const Size &o) const noexcept -> Size {
        return Size{.width = width - o.width, .height = height - o.height};
    }
    /// @brief 两轴是否均为有限值。
    /// @return width 与 height 都不等于 +inf 时为 true。
    [[nodiscard]] constexpr auto is_finite() const noexcept -> bool {
        return width != std::numeric_limits<float>::infinity() && height != std::numeric_limits<float>::infinity();
    }
};

/// @brief 轴对齐矩形（原点 + 尺寸）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Rect {
    Point origin;  ///< 左上角原点坐标
    Size size;  ///< 矩形尺寸

    /// @brief 右边界 x 坐标。
    /// @return origin.x + size.width。
    [[nodiscard]] auto right() const noexcept -> float { return origin.x + size.width; }
    /// @brief 下边界 y 坐标。
    /// @return origin.y + size.height。
    [[nodiscard]] auto bottom() const noexcept -> float { return origin.y + size.height; }

    /// @brief 点是否落在矩形内（含边界）。
    /// @param p 待判定的点。
    /// @return x ∈ [origin.x, right()] 且 y ∈ [origin.y, bottom()] 时为 true。
    [[nodiscard]] auto contains(Point p) const noexcept -> bool {
        return p.x >= origin.x && p.x <= right() && p.y >= origin.y && p.y <= bottom();
    }

    /// @brief 保守相交判定（外接矩形，圆角裁剪亦用此保证不误剔除）。
    /// @param o 另一矩形。
    /// @return 两外接矩形严格重叠（边界相切不算）时为 true。
    [[nodiscard]] auto intersects(const Rect &o) const noexcept -> bool {
        return origin.x < o.right() && right() > o.origin.x && origin.y < o.bottom() && bottom() > o.origin.y;
    }

    /// @brief 矩形相等比较（逐字段；Display List 缓存命中判定用）。
    /// @param o 右操作数矩形。
    /// @return origin 与 size 四个字段全部相等时为 true。
    [[nodiscard]] auto operator==(const Rect &o) const noexcept -> bool {
        return origin.x == o.origin.x && origin.y == o.origin.y && size.width == o.size.width &&
               size.height == o.size.height;
    }

    /// @brief 矩形不等比较（operator== 取反）。
    /// @param o 右操作数矩形。
    /// @return 任一字段不相等时为 true。
    [[nodiscard]] auto operator!=(const Rect &o) const noexcept -> bool { return !(*this == o); }
};

/// @brief 四边内边距。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct EdgeInsets {
    float left = 0.0F;  ///< 左边距（逻辑像素）
    float top = 0.0F;  ///< 上边距（逻辑像素）
    float right = 0.0F;  ///< 右边距（逻辑像素）
    float bottom = 0.0F;  ///< 下边距（逻辑像素）

    /// @brief 左右边距之和。
    /// @return left + right。
    [[nodiscard]] auto horizontal() const noexcept -> float { return left + right; }
    /// @brief 上下边距之和。
    /// @return top + bottom。
    [[nodiscard]] auto vertical() const noexcept -> float { return top + bottom; }
};

/// @brief 尺寸意图（参考安卓 wrap_content / match_parent / exact，但编码为单一枚举）。
enum class LengthKind : std::uint8_t {
    WrapContent,  ///< 按内容决定（max = 无限）
    Expand,  ///< 填满父级可用空间（max = parentSize）
    Fixed,  ///< 精准固定尺寸（min == max）
    Fraction,  ///< 占父级比例（min == max == parent * value）
};

/// @brief 尺寸意图值：AI 直接写在 width/height 属性上。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Length {
    LengthKind kind = LengthKind::WrapContent;  ///< 尺寸意图种类（默认按内容决定）
    float value = 0.0F;  ///< Fixed: 像素；Fraction: 比例(0~1)

    /// @brief 默认构造：WrapContent 意图，value = 0。
    constexpr Length() noexcept = default;
    /// @brief 指定意图种类与载值构造。
    /// @param k 尺寸意图种类。
    /// @param v 附加载值：Fixed 为像素、Fraction 为比例，默认 0。
    constexpr Length(LengthKind k, float v = 0.0F) noexcept : kind(k), value(v) {}  // NOLINT

    /// @brief 构造 WrapContent 意图。
    /// @return kind = WrapContent 的长度值。
    [[nodiscard]] static constexpr auto wrap() noexcept -> Length { return Length{LengthKind::WrapContent}; }
    /// @brief 构造 Expand 意图。
    /// @return kind = Expand 的长度值。
    [[nodiscard]] static constexpr auto expand() noexcept -> Length { return Length{LengthKind::Expand}; }
    /// @brief 构造固定像素长度。
    /// @param px 非负像素值（负值触发调试断言）。
    /// @return kind = Fixed、value = px 的长度值。
    [[nodiscard]] static constexpr auto fixed(float px) noexcept -> Length {
        AURORA_ASSERT(px >= 0.0F, "Length::fixed requires non-negative pixels");
        return Length{LengthKind::Fixed, px};
    }
    /// @brief 构造父级比例长度。
    /// @param f [0, 1] 区间的比例值（越界触发调试断言）。
    /// @return kind = Fraction、value = f 的长度值。
    [[nodiscard]] static constexpr auto ratio(float f) noexcept -> Length {
        AURORA_ASSERT(f >= 0.0F && f <= 1.0F, "Length::ratio requires a fraction in [0, 1]");
        return Length{LengthKind::Fraction, f};
    }
};

/// @brief 布局约束（Flutter 式 min/max，超集安卓三模式）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Constraints {
    Size min;  ///< 尺寸下限
    Size max = Size::infinity();  ///< 尺寸上限（默认无限 = 不限制）

    /// @brief 该轴上限的**供给性质**：true = 只是「剩余可用空间」的按需上限（Flex 主轴给非加权子项即此），
    ///        false = 父级既定槽位（可撑满/展开）。
    /// 「展开自身占满父级」的修饰（`Align`）只在取值为 false 的轴扩张，否则会吞掉 Flex 同列/同行的剩余空间、
    /// 把后续兄弟控件挤出可视区；对标 Flutter `RenderFlex` 给非 flex 子项施加的 `asFlexChild`（主轴 max=无限）。
    /// 纯几何填充（`fill_max_*`）仍按 `max` 取值，不受该标记影响。
    bool loose_width = false;
    bool loose_height = false;  ///< 高度供给性质，含义同 loose_width

    /// @brief 将给定尺寸夹入 [min, max] 区间。
    /// @param s 待夹取的尺寸。
    /// @return 两轴分别 clamp 后的新尺寸。
    [[nodiscard]] auto constrain(const Size &s) const noexcept -> Size {
        Size r;
        r.width = std::clamp(s.width, min.width, max.width);
        r.height = std::clamp(s.height, min.height, max.height);
        return r;
    }

    /// @brief 约束相等比较（布局缓存键，逐字段比较；供给性质改变展开类修饰的取值，故同属缓存键）。
    /// @param o 右操作数约束。
    /// @return min/max 四轴与两个供给性质标记全部相等时为 true。
    [[nodiscard]] auto operator==(const Constraints &o) const noexcept -> bool {
        return min.width == o.min.width && min.height == o.min.height && max.width == o.max.width &&
               max.height == o.max.height && loose_width == o.loose_width && loose_height == o.loose_height;
    }

    /// @brief 约束不等比较（operator== 取反）。
    /// @param o 右操作数约束。
    /// @return 任一字段不相等时为 true。
    [[nodiscard]] auto operator!=(const Constraints &o) const noexcept -> bool { return !(*this == o); }
};

}  // namespace aurora

#include "aurora/core/dimension.h"

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "aurora/core/assert.h"

namespace aurora {

/// @brief 二维点（逻辑像素）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Point {
    float x = 0.0f;
    float y = 0.0f;

    [[nodiscard]] constexpr auto operator+(const Point &o) const noexcept -> Point {
        return Point{ .x = x + o.x, .y = y + o.y };
    }
    [[nodiscard]] constexpr auto operator-(const Point &o) const noexcept -> Point {
        return Point{ .x = x - o.x, .y = y - o.y };
    }
};

/// @brief 尺寸（逻辑像素）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Size {
    float width = 0.0f;
    float height = 0.0f;

    /// @brief 表示"不限制"的尺寸（用于 wrap_content / 安卓 UNSPECIFIED）。
    [[nodiscard]] static constexpr auto infinity() noexcept -> Size {
        return Size{
            .width = std::numeric_limits<float>::infinity(),
            .height = std::numeric_limits<float>::infinity(),
        };
    }

    [[nodiscard]] constexpr auto operator*(float s) const noexcept -> Size {
        return Size{ .width = width * s, .height = height * s };
    }
    [[nodiscard]] constexpr auto operator+(const Size &o) const noexcept -> Size {
        return Size{ .width = width + o.width, .height = height + o.height };
    }
    [[nodiscard]] constexpr auto operator-(const Size &o) const noexcept -> Size {
        return Size{ .width = width - o.width, .height = height - o.height };
    }
    [[nodiscard]] constexpr auto is_finite() const noexcept -> bool {
        return width != std::numeric_limits<float>::infinity() && height != std::numeric_limits<float>::infinity();
    }
};

/// @brief 轴对齐矩形（原点 + 尺寸）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Rect {
    Point origin;
    Size size;

    [[nodiscard]] auto right() const noexcept -> float { return origin.x + size.width; }
    [[nodiscard]] auto bottom() const noexcept -> float { return origin.y + size.height; }

    [[nodiscard]] auto contains(Point p) const noexcept -> bool {
        return p.x >= origin.x && p.x <= right() && p.y >= origin.y && p.y <= bottom();
    }

    /// @brief 保守相交判定（外接矩形，圆角裁剪亦用此保证不误剔除）。
    [[nodiscard]] auto intersects(const Rect &o) const noexcept -> bool {
        return origin.x < o.right() && right() > o.origin.x && origin.y < o.bottom() && bottom() > o.origin.y;
    }

    /// @brief 矩形相等比较（逐字段；Display List 缓存命中判定用）。
    [[nodiscard]] auto operator==(const Rect &o) const noexcept -> bool {
        return origin.x == o.origin.x && origin.y == o.origin.y && size.width == o.size.width &&
               size.height == o.size.height;
    }

    // NOLINTNEXTLINE(*-redundant-parentheses)
    [[nodiscard]] auto operator!=(const Rect &o) const noexcept -> bool { return !(*this == o); }
};

/// @brief 四边内边距。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct EdgeInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] auto horizontal() const noexcept -> float { return left + right; }
    [[nodiscard]] auto vertical() const noexcept -> float { return top + bottom; }
};

/// @brief 尺寸意图（参考安卓 wrap_content / match_parent / exact，但编码为单一枚举）。
enum class LengthKind : std::uint8_t {
    WrapContent, ///< 按内容决定（max = 无限）
    Expand,      ///< 填满父级可用空间（max = parentSize）
    Fixed,       ///< 精准固定尺寸（min == max）
    Fraction,    ///< 占父级比例（min == max == parent * value）
};

/// @brief 尺寸意图值：AI 直接写在 width/height 属性上。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Length {
    LengthKind kind = LengthKind::WrapContent;
    float value = 0.0f; ///< Fixed: 像素；Fraction: 比例(0~1)

    constexpr Length() noexcept = default;
    constexpr Length(LengthKind k, float v = 0.0f) noexcept : kind(k), value(v) {}

    [[nodiscard]] static constexpr auto wrap() noexcept -> Length { return Length{ LengthKind::WrapContent }; }
    [[nodiscard]] static constexpr auto expand() noexcept -> Length { return Length{ LengthKind::Expand }; }
    [[nodiscard]] static constexpr auto fixed(float px) noexcept -> Length {
        AURORA_ASSERT(px >= 0.0f, "Length::fixed requires non-negative pixels");
        return Length{ LengthKind::Fixed, px };
    }
    [[nodiscard]] static constexpr auto ratio(float f) noexcept -> Length {
        AURORA_ASSERT(f >= 0.0f && f <= 1.0f, "Length::ratio requires a fraction in [0, 1]");
        return Length{ LengthKind::Fraction, f };
    }
};

/// @brief 布局约束（Flutter 式 min/max，超集安卓三模式）。
/// @note Thread: thread-safe
/// @note Side-effects: pure
struct Constraints {
    Size min;
    Size max = Size::infinity();

    /// @brief 将给定尺寸夹入 [min, max] 区间。
    [[nodiscard]] auto constrain(const Size &s) const noexcept -> Size {
        Size r;
        r.width = std::clamp(s.width, min.width, max.width);
        r.height = std::clamp(s.height, min.height, max.height);
        return r;
    }

    /// @brief 约束相等比较（布局缓存键，逐字段比较）。
    [[nodiscard]] auto operator==(const Constraints &o) const noexcept -> bool {
        return min.width == o.min.width && min.height == o.min.height && max.width == o.max.width &&
               max.height == o.max.height;
    }

    // NOLINTNEXTLINE(*-redundant-parentheses)
    [[nodiscard]] auto operator!=(const Constraints &o) const noexcept -> bool { return !(*this == o); }
};

} // namespace aurora

#include "aurora/core/dimension.h" // 强类型尺寸工厂 px/dp/percent/fill（规格 §4），在命名空间外引入

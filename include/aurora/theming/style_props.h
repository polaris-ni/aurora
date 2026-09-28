#pragma once

#include <optional>
#include <string>
#include <variant>

#include "aurora/theming/theme.h"

namespace aurora {

/// @brief 令牌引用或具体值（两态样式字段）。
///
/// 轻量泛型字段，用于 `StyleProps`：既能直接填「具体值」（如 `Color::red()`），
/// 也能填「令牌名」（如 `"color.primary"`），渲染时经 `Theme` 解析为具体值。
/// 解析语义（`resolve`）：
/// - 若为令牌名：在 `Theme` 中查到且类型匹配则返回，否则回退 fallback；
/// - 若为具体值：直接返回。
///
/// @tparam T 具体值类型（如 `Color` / `Font` / `double`）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
///
template <typename T>
struct TokenOr {
    /// @brief 两态存储体：`std::string` 表示令牌名，`T` 表示具体值；默认构造落入 T 值态。
    std::variant<std::string, T> v;

    /// @brief 默认构造为「具体默认值」（未设置样式 → 用默认值，而非空令牌名）。
    TokenOr() : v(std::in_place_type<T>) {}
    /// @brief 具体值。
    /// @param concrete 具体值（移入 variant 的 T 值态）。
    TokenOr(T concrete) : v(std::move(concrete)) {}
    /// @brief 令牌名（字符串字面量便捷构造）。
    /// @param name 令牌名，字符串字面量在构造时拷入 std::string 态。
    TokenOr(const char *name) : v(std::string(name)) {}
    /// @brief 令牌名。
    /// @param name 令牌名（移入 variant 的字符串态）。
    TokenOr(std::string name) : v(std::move(name)) {}

    /// @brief 是否为令牌名（而非具体值）。
    /// @return 持有令牌名时为 true，持有具体值时为 false。
    [[nodiscard]] auto is_token() const -> bool { return std::holds_alternative<std::string>(v); }

    /// @brief 令牌名（若是令牌则返回，否则 std::nullopt）。
    /// @return 令牌态下的名字视图；具体值态下为 std::nullopt。
    [[nodiscard]] auto token_name() const -> std::optional<std::string_view> {
        if (auto *s = std::get_if<std::string>(&v)) {
            return *s;
        }
        return std::nullopt;
    }

    /// @brief 具体值（若是具体值则返回，否则 std::nullopt）。
    /// @return 具体值态下的值拷贝；令牌态下为 std::nullopt。
    [[nodiscard]] auto concrete() const -> std::optional<T> {
        if (auto *p = std::get_if<T>(&v)) {
            return *p;
        }
        return std::nullopt;
    }

    /// @brief 经 `Theme` 解析为具体值；令牌缺失或类型不匹配时回退 fallback。
    /// @param theme [in] 用于查找令牌名的主题。
    /// @param fallback [in] 令牌缺失或令牌值类型与 T 不匹配时的回退值。
    /// @return 具体值态返回自身；令牌态返回主题中类型匹配的令牌值，否则返回 fallback。
    [[nodiscard]] auto resolve(const Theme &theme, T fallback) const -> T {
        if (auto *name = std::get_if<std::string>(&v)) {
            if (auto tv = theme.token(*name)) {
                if (auto *pv = std::get_if<T>(&tv->v)) {
                    return *pv;
                }
            }
            return fallback;
        }
        return std::get<T>(v);  // 必为具体值
    }
};

/// @brief `StyleProps` 经 `Theme` 解析后的具体样式（无令牌名，纯值）。
struct ResolvedStyle {
    Color background;  ///< 背景色
    Color foreground;  ///< 前景色
    Font font{};  ///< 字体
    double corner_radius = 0.0;  ///< dp
    double padding = 0.0;  ///< dp
};

/// @brief 轻量样式叠加结构：字段可填「令牌名或具体值」两态（见 `TokenOr<T>`）。
///
/// 不替代控件既有 `XxxProps`，而是作为「主题令牌驱动」的样式层：组件读取最近
/// 祖先 `ThemeScope` 注入的 `Theme` 后，经 `StyleProps::resolve(theme)` 把所有
/// 两态字段解析为 `ResolvedStyle` 具体值用于绘制。不改既有 `ThemeProvider` 注入机制，
/// 仅叠加令牌解析层（specification/07-environment-modifier.md §5.2 StyleProps）。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
///
struct StyleProps {
    TokenOr<Color> background;  ///< 背景色（两态：令牌名或具体值）
    TokenOr<Color> foreground;  ///< 前景色（两态：令牌名或具体值）
    TokenOr<Font> font;  ///< 字体（两态：令牌名或具体值）
    TokenOr<double> corner_radius;  ///< dp
    TokenOr<double> padding;  ///< dp

    /// @brief 经 `Theme` 解析为具体样式；字段缺失/不匹配时按类型取合理默认。
    /// @param theme [in] 用于解析各两态字段令牌名的主题。
    /// @return 全部字段落到具体值的 ResolvedStyle（未解析字段默认 Color{}/Font{}/0.0）。
    [[nodiscard]] auto resolve(const Theme &theme) const -> ResolvedStyle {
        return ResolvedStyle{
            .background = background.resolve(theme, Color{}),
            .foreground = foreground.resolve(theme, Color{}),
            .font = font.resolve(theme, Font{}),
            .corner_radius = corner_radius.resolve(theme, 0.0),
            .padding = padding.resolve(theme, 0.0),
        };
    }
};

}  // namespace aurora

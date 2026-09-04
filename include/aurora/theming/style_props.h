#pragma once

#include <optional>
#include <string>
#include <variant>

#include "aurora/theming/theme.h"

namespace aurora {

/**
 * @brief 令牌引用或具体值（两态样式字段）。
 *
 * 轻量泛型字段，用于 `StyleProps`：既能直接填「具体值」（如 `Color::red()`），
 * 也能填「令牌名」（如 `"color.primary"`），渲染时经 `Theme` 解析为具体值。
 * 解析语义（`resolve`）：
 *   - 若为令牌名：在 `Theme` 中查到且类型匹配则返回，否则回退 fallback；
 *   - 若为具体值：直接返回。
 *
 * @tparam T 具体值类型（如 `Color` / `Font` / `double`）。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 */
template <typename T>
struct TokenOr {
    std::variant<std::string, T> v;

    /// @brief 默认构造为「具体默认值」（未设置样式 → 用默认值，而非空令牌名）。
    TokenOr() : v(std::in_place_type<T>) {}
    /// @brief 具体值。
    TokenOr(T concrete) : v(std::move(concrete)) {}
    /// @brief 令牌名（字符串字面量便捷构造）。
    TokenOr(const char *name) : v(std::string(name)) {}
    /// @brief 令牌名。
    TokenOr(std::string name) : v(std::move(name)) {}

    /// @brief 是否为令牌名（而非具体值）。
    [[nodiscard]] auto is_token() const -> bool { return std::holds_alternative<std::string>(v); }

    /// @brief 令牌名（若是令牌则返回，否则 std::nullopt）。
    [[nodiscard]] auto token_name() const -> std::optional<std::string_view> {
        if (auto *s = std::get_if<std::string>(&v)) {
            return *s;
        }
        return std::nullopt;
    }

    /// @brief 具体值（若是具体值则返回，否则 std::nullopt）。
    [[nodiscard]] auto concrete() const -> std::optional<T> {
        if (auto *p = std::get_if<T>(&v)) {
            return *p;
        }
        return std::nullopt;
    }

    /// @brief 经 `Theme` 解析为具体值；令牌缺失或类型不匹配时回退 fallback。
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
    Color background;
    Color foreground;
    Font font{};
    double corner_radius = 0.0;  ///< dp
    double padding = 0.0;  ///< dp
};

/**
 * @brief 轻量样式叠加结构：字段可填「令牌名或具体值」两态（见 `TokenOr<T>`）。
 *
 * 不替代控件既有 `XxxProps`，而是作为「主题令牌驱动」的样式层：组件读取最近
 * 祖先 `ThemeScope` 注入的 `Theme` 后，经 `StyleProps::resolve(theme)` 把所有
 * 两态字段解析为 `ResolvedStyle` 具体值用于绘制。不改既有 `ThemeProvider` 注入机制，
 * 仅叠加令牌解析层（specification/07-environment-modifier.md §5.2 StyleProps）。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 */
struct StyleProps {
    TokenOr<Color> background;
    TokenOr<Color> foreground;
    TokenOr<Font> font;
    TokenOr<double> corner_radius;  ///< dp
    TokenOr<double> padding;  ///< dp

    /// @brief 经 `Theme` 解析为具体样式；字段缺失/不匹配时按类型取合理默认。
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

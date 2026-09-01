#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include "aurora/core/color.h"
#include "aurora/core/font.h"

namespace aurora {

/**
 * @brief 设计令牌值（Design Token Value）。
 *
 * 一个命名令牌可承载三种语义值之一：颜色、字体、或尺寸（dp 逻辑像素，
 * 用于间距/圆角/线宽等）。经 `Theme::set_token(name, ...)` 登记，
 * 由 `StyleProps` 的 `TokenOr<T>` 字段在渲染时经 `Theme` 解析为具体值。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 */
struct TokenValue {
    std::variant<Color, Font, double> v;

    TokenValue() = default;
    TokenValue(Color c) : v(c) {}
    TokenValue(Font f) : v(std::move(f)) {}
    TokenValue(double d) : v(d) {}

    /// @brief 便捷判定：当前值是否为某种类型（用于解析与诊断）。
    template<typename T> [[nodiscard]] auto is() const -> bool { return std::holds_alternative<T>(v); }

    /// @brief 取具体值（类型匹配则返回，否则 std::nullopt）。
    template<typename T> [[nodiscard]] auto as() const -> std::optional<T> {
        if (auto *p = std::get_if<T>(&v)) {
            return *p;
        }
        return std::nullopt;
    }
};

/**
 * @brief 主题：聚合扁平设计令牌（颜色、字体）+ 命名令牌表。通过 `Provider<Theme>` 注入环境（§4.7）。
 *
 * 对应 REQ §4.5 主题化。`Theme` 同时持有传统扁平字段（`.background` 等，向后兼容）
 * 与可扩展的命名令牌表（`.token()`/`.set_token()`），组件用 `ctx.environment<Theme>()` 读取后
 * 既可直接用扁平字段，也可经 `StyleProps` 解析「令牌名或具体值」两态样式。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: yes, via from_json
 */
struct Theme {
    Color background = Color::white();
    Color primary = Color::blue();
    Color on_primary = Color::white();
    Color text = Color::black();
    Font font;

    /// @brief 命名令牌表：名称 → 设计令牌值（颜色/字体/尺寸）。
    std::unordered_map<std::string, TokenValue> tokens;

    /// @brief 生成一个浅色主题（便于示例/测试）。
    [[nodiscard]] static auto light() -> Theme { return Theme{}; }

    /// @brief 生成一个深色主题。
    [[nodiscard]] static auto dark() -> Theme {
        Theme t;
        t.background = Color::from_rgba(32, 33, 36);
        t.primary = Color::from_rgba(90, 120, 240);
        t.text = Color::white();
        return t;
    }

    /// @brief 返回所有字段均为默认值的主题。用于把"部分字段主题"与本机默认合并，
    /// 或在 `resolve_theme` 作为兜底根主题。
    [[nodiscard]] static auto with_defaults() -> Theme { return Theme{}; }

    /// @brief 登记一个命名令牌（覆盖同名）。
    auto set_token(std::string_view name, TokenValue value) -> void { tokens[std::string(name)] = std::move(value); }

    /// @brief 查询命名令牌；不存在返回 std::nullopt。
    [[nodiscard]] auto token(std::string_view name) const -> std::optional<TokenValue> {
        auto it = tokens.find(std::string(name));
        if (it == tokens.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// @brief 查询命名令牌并强转为目标类型；类型不匹配或不存在时返回 fallback。
    template<typename T> [[nodiscard]] auto token_or(std::string_view name, T fallback) const -> T {
        auto it = tokens.find(std::string(name));
        if (it != tokens.end()) {
            if (auto *p = std::get_if<T>(&it->second.v)) {
                return *p;
            }
        }
        return fallback;
    }
};

} // namespace aurora

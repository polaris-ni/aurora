#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include "aurora/core/color.h"
#include "aurora/core/font.h"

namespace aurora {

/// @brief 设计令牌值（Design Token Value）。
///
/// 一个命名令牌可承载三种语义值之一：颜色、字体、或尺寸（dp 逻辑像素，
/// 用于间距/圆角/线宽等）。经 `Theme::set_token(name, ...)` 登记，
/// 由 `StyleProps` 的 `TokenOr<T>` 字段在渲染时经 `Theme` 解析为具体值。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
///
struct TokenValue {
    std::variant<Color, Font, double> v;  ///< 三态原始值：颜色 / 字体 / 尺寸（dp 逻辑像素）。

    TokenValue() = default;
    /// @brief 以颜色构造令牌。
    /// @param c 颜色值。
    TokenValue(Color c) : v(c) {}
    /// @brief 以字体构造令牌。
    /// @param f 字体值。
    TokenValue(Font f) : v(std::move(f)) {}
    /// @brief 以尺寸构造令牌（dp 逻辑像素）。
    /// @param d 尺寸值。
    TokenValue(double d) : v(d) {}

    /// @brief 便捷判定：当前值是否为某种类型（用于解析与诊断）。
    /// @tparam T 待判定的目标类型（Color / Font / double）。
    /// @return `true` = 当前值持有 `T`。
    template <typename T>
    [[nodiscard]] auto is() const -> bool {
        return std::holds_alternative<T>(v);
    }

    /// @brief 取具体值（类型匹配则返回，否则 std::nullopt）。
    /// @tparam T 期望取出的目标类型（Color / Font / double）。
    /// @return 类型匹配时为持有值包，否则为 `std::nullopt`。
    template <typename T>
    [[nodiscard]] auto as() const -> std::optional<T> {
        if (auto *p = std::get_if<T>(&v)) {
            return *p;
        }
        return std::nullopt;
    }
};

/// @brief 主题：聚合扁平设计令牌（颜色、字体）+ 命名令牌表。通过 `Provider<Theme>`
/// 注入环境（specification/07-environment-modifier.md §5.1）。
///
/// `Theme` 同时持有传统扁平字段（`background` 等，向后兼容）
/// 与可扩展的命名令牌表（`token()`/`set_token()`），组件用 `ctx.environment<Theme>()` 读取后
/// 既可直接用扁平字段，也可经 `StyleProps` 解析「令牌名或具体值」两态样式。
///
/// 豁免 bugprone-exception-escape：Theme 随控件回调网被值拷贝（Provider/StyleProps 转发链上的
/// std::function 深拷贝触发 .clang-tidy 已记录的系统性假告警面——「转入 std::function 的可调用对象
/// 一律判『不应抛出』」），据此对隐式特殊成员误报。抛出仅可能为令牌表/字体分配的 bad_alloc，由顶层兜底。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: yes, via from_json
/// NOLINTNEXTLINE(bugprone-exception-escape)
struct Theme {
    Color background = Color::white();  ///< 窗口/画布底色。
    Color primary = Color::blue();  ///< 主色（强调/交互色）。
    Color on_primary = Color::white();  ///< 叠加于主色之上的前景色。
    Color text = Color::black();  ///< 正文默认色。
    Font font;  ///< 正文默认字体（默认构造 Font）。

    /// @brief 命名令牌表：名称 → 设计令牌值（颜色/字体/尺寸）。
    std::unordered_map<std::string, TokenValue> tokens;

    /// @brief 生成一个浅色主题（便于示例/测试；亦是未挂 `ThemeScope` 的树经 `inherit_theme`
    /// 拿到的兜底主题）。
    /// @return 全字段默认值 + 命名令牌 `focus.ring` = 纯黑（焦点环取极性色，与 `primary` 拉开）。
    /// @note 与 `with_defaults()` 不同形：后者是「逐字段取声明默认值」的裸主题，不登记任何令牌。
    [[nodiscard]] static auto light() -> Theme {
        Theme t;
        t.set_token("focus.ring", TokenValue{Color::black()});
        return t;
    }

    /// @brief 生成一个深色主题。
    /// @return 深底（rgb 32,33,36）+ 蓝紫主色 + 白正文 + 命名令牌 `focus.ring` = 纯白的主题。
    [[nodiscard]] static auto dark() -> Theme {
        Theme t;
        t.background = Color::from_rgba(32, 33, 36);
        t.primary = Color::from_rgba(90, 120, 240);
        t.text = Color::white();
        t.set_token("focus.ring", TokenValue{Color::white()});
        return t;
    }

    /// @brief 返回所有字段均为默认值的主题。用于把"部分字段主题"与本机默认合并，
    /// 或在 `resolve_theme` 作为兜底根主题。
    /// @return 逐字段取声明处默认值的主题实例。
    [[nodiscard]] static auto with_defaults() -> Theme { return Theme{}; }

    /// @brief 登记一个命名令牌（覆盖同名）。
    /// @param name 令牌名。
    /// @param value 令牌值（颜色/字体/尺寸三态之一）。
    auto set_token(std::string_view name, TokenValue value) -> void { tokens[std::string(name)] = std::move(value); }

    /// @brief 查询命名令牌；不存在返回 std::nullopt。
    /// @param name 令牌名。
    /// @return 命中为令牌值，未命中为 `std::nullopt`。
    [[nodiscard]] auto token(std::string_view name) const -> std::optional<TokenValue> {
        const auto it = tokens.find(std::string(name));
        if (it == tokens.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// @brief 查询命名令牌并强转为目标类型；类型不匹配或不存在时返回 fallback。
    /// @tparam T 期望的令牌值类型（Color / Font / double）。
    /// @param name 令牌名。
    /// @param fallback 未命中或类型不符时的兜底值。
    /// @return 命中且类型匹配为令牌值，否则为 `fallback`。
    template <typename T>
    [[nodiscard]] auto token_or(std::string_view name, T fallback) const -> T {
        const auto it = tokens.find(std::string(name));
        if (it != tokens.end()) {
            if (auto *p = std::get_if<T>(&it->second.v)) {
                return *p;
            }
        }
        return fallback;
    }
};

}  // namespace aurora

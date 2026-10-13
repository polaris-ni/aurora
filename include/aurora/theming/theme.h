#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/theming/color_scheme.h"

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
    ///
    /// 基础表（语义上对应亮色档/共享受体）；暗色差异只写进 `dark_tokens`，
    /// 解析经 `token_for(name)`（按 `scheme` 取档）。
    std::unordered_map<std::string, TokenValue> tokens;

    /// @brief 暗色档令牌覆写表：仅登记与亮/共享表不同的键。
    ///
    /// 一个 Theme 自带明暗两套取值（业界共识：角色名一套、明暗两列取值，见 Material 3
    /// color roles 与 Fluent 2 alias tokens 的 light/dark variants）。解析顺序：`scheme`
    /// 为 Dark 时先查本表、未命中回落 `tokens`；其余档位只查 `tokens`。
    std::unordered_map<std::string, TokenValue> dark_tokens;

    /// @brief 本主题的明暗归属档；`Unspecified` 表示「本主题不表态」。
    ///
    /// 明暗三级优先链的中间层：显式样式 > 本字段 > OS 偏好。`light()`/`dark()`
    /// 工厂会置位；手工构造的主题保持 `Unspecified`，语义为「沿用上一优先层」。
    ColorScheme scheme = ColorScheme::Unspecified;

    /// @brief 按给定档位解析令牌：Dark 先查 `dark_tokens` 再回落 `tokens`，其余只查 `tokens`。
    /// @param name 令牌名。
    /// @param s 目标档位（`Unspecified` 与 `Light` 同走基础表）。
    /// @return 命中为令牌值，未命中为 `std::nullopt`。
    [[nodiscard]] auto token_in(std::string_view name, ColorScheme s) const -> std::optional<TokenValue> {
        if (s == ColorScheme::Dark) {
            const auto dit = dark_tokens.find(std::string(name));
            if (dit != dark_tokens.end()) {
                return dit->second;
            }
        }
        return token(name);
    }

    /// @brief 按本主题归属档（`scheme`）解析令牌（`token_in` 的自档便捷形态）。
    /// @param name 令牌名。
    /// @return 命中为令牌值；`scheme` 为 `Unspecified` 时等价于查基础表。
    [[nodiscard]] auto token_for(std::string_view name) const -> std::optional<TokenValue> {
        return token_in(name, scheme);
    }

    /// @brief 按给定档位解析令牌并强转为目标类型；未命中或类型不符返回 fallback。
    /// @tparam T 期望的令牌值类型（Color / Font / double）。
    /// @param name 令牌名。
    /// @param s 目标档位。
    /// @param fallback 兜底值。
    /// @return 命中且类型匹配为令牌值，否则为 `fallback`。
    template <typename T>
    [[nodiscard]] auto token_or_in(std::string_view name, ColorScheme s, T fallback) const -> T {
        if (auto v = token_in(name, s)) {
            if (auto *p = std::get_if<T>(&v->v)) {
                return *p;
            }
        }
        return fallback;
    }

    /// @brief 按本主题归属档解析令牌并强转为目标类型（`token_or_in` 的自档便捷形态）。
    /// @tparam T 期望的令牌值类型（Color / Font / double）。
    /// @param name 令牌名。
    /// @param fallback 兜底值。
    /// @return 命中且类型匹配为令牌值，否则为 `fallback`。
    template <typename T>
    [[nodiscard]] auto token_or_for(std::string_view name, T fallback) const -> T {
        return token_or_in(name, scheme, std::move(fallback));
    }

    /// @brief 生成一个浅色主题（便于示例/测试；亦是未挂 `ThemeScope` 的树经 `inherit_theme`
    /// 拿到的兜底主题）。
    /// @return 全字段默认值 + 命名令牌 `focus.ring` = 纯黑（焦点环取极性色，与 `primary` 拉开）。
    /// @note 与 `with_defaults()` 不同形：后者是「逐字段取声明默认值」的裸主题，不登记任何令牌。
    [[nodiscard]] static auto light() -> Theme {
        Theme t;
        t.scheme = ColorScheme::Light;
        seed_palette(t);
        t.set_token("focus.ring", TokenValue{Color::black()});
        return t;
    }

    /// @brief 登记语义令牌全集与 CSD 组件令牌（亮色基准；暗色差异由 `dark()` 覆写）。
    ///
    /// 语义令牌命名取 Fluent 2 alias tokens / Material 3 color roles 的公共子集；
    /// `csd.*` 是组件覆写位——只有想改窗口装饰某一处时才写，未覆写的键回落语义层。
    /// 数值基准与 `TitleBarStyle::from_preset(CsdPreset::AdwaitaLight/Dark)` 一致，
    /// 保证「从令牌派生」与「直接取预设」两条路输出相同的默认观感。
    /// @param t 待播种的主题（覆写同名既有令牌）。
    static auto seed_palette(Theme &t) -> void {
        // 语义层（亮色基准）
        t.set_token("surface", TokenValue{Color::white()});
        t.set_token("surface.raised", TokenValue{Color{0xF2, 0xF2, 0xF2}});
        t.set_token("text", TokenValue{Color::black()});
        t.set_token("text.muted", TokenValue{Color{0x92, 0x95, 0x95}});
        t.set_token("accent", TokenValue{Color::blue()});
        t.set_token("border", TokenValue{Color{0xDE, 0xDE, 0xDE}});
        t.set_token("hover", TokenValue{Color{0, 0, 0, 32}});
        t.set_token("pressed", TokenValue{Color{0, 0, 0, 56}});
        t.set_token("danger", TokenValue{Color{0xE0, 0x1B, 0x24}});
        // 组件覆写位（CSD；亮色基准 = AdwaitaLight）：取值与 TitleBarStyle::from_preset(AdwaitaLight)
        // 共用同一组数字——保证「从 Theme 派生」与「直接取预设」逐值相同的契约（CHG-010 取色口径更新）。
        t.set_token("csd.title_bar.bg.active", TokenValue{Color{0xFF, 0xFF, 0xFF}});
        t.set_token("csd.title_bar.bg.inactive", TokenValue{Color{0xFA, 0xFA, 0xFB}});
        t.set_token("csd.title_bar.fg.active", TokenValue{Color{0x33, 0x33, 0x34}});
        t.set_token("csd.title_bar.fg.inactive", TokenValue{Color{0x7D, 0x7D, 0x80}});
        t.set_token("csd.title_bar.hover", TokenValue{Color{0, 0, 0, 38}});
        t.set_token("csd.title_bar.pressed", TokenValue{Color{0, 0, 0, 77}});
        t.set_token("csd.title_bar.close.hover", TokenValue{Color{0xE0, 0x1B, 0x24}});
        t.set_token("csd.shadow.color", TokenValue{Color{0, 0, 0, 70}});
    }

    /// @brief 生成一个深色主题。
    /// @return 深底（rgb 32,33,36）+ 蓝紫主色 + 白正文 + 命名令牌 `focus.ring` = 纯白的主题。
    [[nodiscard]] static auto dark() -> Theme {
        Theme t;
        t.scheme = ColorScheme::Dark;
        seed_palette(t);
        t.background = Color::from_rgba(32, 33, 36);
        t.primary = Color::from_rgba(90, 120, 240);
        t.text = Color::white();
        t.set_token("focus.ring", TokenValue{Color::white()});
        // 暗色差异只进覆写表；未覆写的键（如 danger / csd.title_bar.close.hover）跨档共享。
        t.dark_tokens["surface"] = TokenValue{Color{0x30, 0x30, 0x30}};
        t.dark_tokens["surface.raised"] = TokenValue{Color{0x38, 0x38, 0x38}};
        t.dark_tokens["text"] = TokenValue{Color::white()};
        t.dark_tokens["text.muted"] = TokenValue{Color{0x9A, 0x99, 0x96}};
        t.dark_tokens["border"] = TokenValue{Color{0x4A, 0x4A, 0x4A}};
        t.dark_tokens["hover"] = TokenValue{Color{255, 255, 255, 32}};
        t.dark_tokens["pressed"] = TokenValue{Color{255, 255, 255, 56}};
        // 组件覆写位（CSD；暗色基准 = AdwaitaDark）：取值与 TitleBarStyle::from_preset(AdwaitaDark)
        // 共用同一组数字——保证「从 Theme 派生」与「直接取预设」逐值相同的契约（CHG-010 取色口径更新）。
        t.dark_tokens["csd.title_bar.bg.active"] = TokenValue{Color{0x2E, 0x2E, 0x32}};
        t.dark_tokens["csd.title_bar.bg.inactive"] = TokenValue{Color{0x22, 0x22, 0x26}};
        t.dark_tokens["csd.title_bar.fg.active"] = TokenValue{Color::white()};
        t.dark_tokens["csd.title_bar.fg.inactive"] = TokenValue{Color{0x91, 0x91, 0x93}};
        t.dark_tokens["csd.title_bar.hover"] = TokenValue{Color{255, 255, 255, 38}};
        t.dark_tokens["csd.title_bar.pressed"] = TokenValue{Color{255, 255, 255, 77}};
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

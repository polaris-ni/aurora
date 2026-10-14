#pragma once

#include <type_traits>
#include <utility>

#include "aurora/theming/color_roles.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/theming/csd_roles.h"
#include "aurora/theming/theme_extension.h"
#include "aurora/theming/type_scale.h"

namespace aurora {

/// @brief 主题：强类型色角色集 + 排版字阶 + CSD 组件角色 + 类型安全扩展的值语义聚合。
///
/// 经 `Provider<Theme>` / `ThemeScope` 注入环境，组件用 `inherit_theme(ctx)` 读取（见
/// specification/07-environment-modifier.md §5.1）。取代旧的「扁平字段 + stringly 命名令牌表」双入口：
/// - `roles`：Material 3 色角色全集（明暗两列，本实例持一列，由 `scheme` 标注归属档）；
/// - `type_scale`：排版 15 档（不分明暗，`body_medium` 承接旧 `Theme::font`）；
/// - `csd`：窗口装饰配色角色组（与通用角色解耦，组件级覆写面）；
/// - `extensions`：应用自定义令牌以类型为键注册（`set_extension<T>` / `extension<T>()`），无字符串回落。
///
/// 三级明暗优先链不变：显式样式 > `scheme` > OS 偏好（`os_color_scheme()`）；`scheme` 为
/// `Unspecified` 表示本主题不表态，由优先链下层继续解析。渲染路径读角色为直接字段访问，无哈希、
/// 无堆分配（`extensions` 为空时零分配）。
///
/// 豁免 bugprone-exception-escape：Theme 随控件回调网被值拷贝（Provider 转发链上的
/// std::function 深拷贝触发 .clang-tidy 已记录的系统性假告警面——「转入 std::function 的可调用对象
/// 一律判『不应抛出』」），据此对隐式特殊成员误报。抛出仅可能为扩展注册表 `new` 的 bad_alloc，由顶层兜底。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: yes, via to_json / from_json（角色集；扩展不参与）
/// NOLINTNEXTLINE(bugprone-exception-escape)
struct Theme {
    ColorRoles roles = ColorRoles::light();  ///< 色角色全集（本实例持明或暗一列）。
    TypeScale type_scale;  ///< 排版 15 档（不分明暗）。
    CsdRoles csd = CsdRoles::light();  ///< 窗口装饰配色角色组。
    ColorScheme scheme = ColorScheme::Unspecified;  ///< 明暗归属档（三级优先链中间层，语义不变）。
    ThemeExtensions extensions;  ///< 类型安全扩展注册表（空则零分配）。

    /// @brief 生成浅色主题（亦是未挂 `ThemeScope` 的树经 `inherit_theme` 拿到的兜底主题）。
    /// @return `roles`/`csd` 取亮色列、`scheme` = `Light` 的主题。
    [[nodiscard]] static auto light() -> Theme {
        Theme t;
        t.roles = ColorRoles::light();
        t.csd = CsdRoles::light();
        t.scheme = ColorScheme::Light;
        return t;
    }

    /// @brief 生成深色主题。
    /// @return `roles`/`csd` 取暗色列、`scheme` = `Dark` 的主题。
    [[nodiscard]] static auto dark() -> Theme {
        Theme t;
        t.roles = ColorRoles::dark();
        t.csd = CsdRoles::dark();
        t.scheme = ColorScheme::Dark;
        return t;
    }

    /// @brief 返回所有字段取声明默认值的主题：light 基准列 + 无扩展、`scheme` 不表态。
    /// @return 逐字段取默认成员值的主题实例（裸主题即明色基线，非无色空壳）。
    [[nodiscard]] static auto with_defaults() -> Theme { return Theme{}; }

    /// @brief 注册/覆盖类型 T 的扩展（同类型整体替换）。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @param value 扩展值（移入）。
    template <ThemeExtension T>
    auto set_extension(T value) -> void {
        extensions.set(std::move(value));
    }

    /// @brief 返回带类型 T 扩展的副本（同类型整体替换），不修改自身。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @param value 扩展值（移入副本）。
    /// @return 已注册该扩展的主题副本（应用可在根主题播种，如
    ///         `Theme::light().with_extension(ChartPalette::light())`）。
    template <ThemeExtension T>
    [[nodiscard]] auto with_extension(T value) const -> Theme {
        Theme copy = *this;
        copy.extensions.set(std::move(value));
        return copy;
    }

    /// @brief 读取类型 T 的扩展。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @return 已注册则指向其只读实例，否则 `nullptr`。
    template <ThemeExtension T>
    [[nodiscard]] auto extension() const -> const std::remove_cvref_t<T> * {
        return extensions.template get<T>();
    }

    /// @brief 移除类型 T 的扩展。
    /// @tparam T 扩展类型（须满足 `ThemeExtension`）。
    /// @return 曾存在并被移除时为 true。
    template <ThemeExtension T>
    auto remove_extension() -> bool {
        return extensions.remove<T>();
    }
};

}  // namespace aurora

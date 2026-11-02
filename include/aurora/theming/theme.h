#pragma once

#include <type_traits>
#include <utility>

#include "aurora/theming/color_roles.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/theming/control_metrics.h"
#include "aurora/theming/csd_roles.h"
#include "aurora/theming/layout_conventions.h"
#include "aurora/theming/shape_scale.h"
#include "aurora/theming/spacing_scale.h"
#include "aurora/theming/theme_extension.h"
#include "aurora/theming/theme_style.h"
#include "aurora/theming/type_scale.h"

namespace aurora {

/// @brief 主题：强类型色角色集 + 排版字阶 + CSD 组件角色 + 类型安全扩展的值语义聚合。
///
/// 经 `Provider<Theme>` / `ThemeScope` 注入环境，组件用 `inherit_theme(ctx)` 读取（见
/// specification/07-environment-modifier.md §5.1）。取代旧的「扁平字段 + stringly 命名令牌表」双入口：
/// - `style`：本实例所属**风格轴**（身份位，供 CSD 派生与自描述面读取；取值见 `ThemeStyle`）；
/// - `roles`：Material 3 色角色全集（明暗两列，本实例持一列，由 `scheme` 标注归属档）；
/// - `type_scale`：排版 15 档（不分明暗，`body_medium` 承接旧 `Theme::font`）；
/// - `csd`：窗口装饰配色角色组（与通用角色解耦，组件级覆写面）；
/// - `shapes` / `spacing` / `controls`：形状、间距与控件基准度量三组令牌（单位逻辑 dp，不分明暗）；
/// - `conventions`：布局与交互惯例组（按钮序 / 标题对齐 / 菜单栏位置 / 滚动条模式 / 焦点环形状 / 动效倍率）；
/// - `extensions`：应用自定义令牌以类型为键注册（`set_extension<T>` / `extension<T>()`），无字符串回落。
///
/// 两轴正交：`style` 表「用哪套设计语言」，`scheme` 表「这套语言的明暗呈现」，二者互不折平——
/// 一枚风格内含明暗两列取值，一枚 `Theme` 实例仍只持**一列**（与 `ColorRoles` 的「角色名一套、
/// 取值两列」同构）。
///
/// 三级明暗优先链不变：显式样式 > `scheme` > OS 偏好（`os_color_scheme()`）；`scheme` 为
/// `Unspecified` 表示本主题不表态，由优先链下层继续解析。渲染路径读角色与令牌为直接字段访问，
/// 无哈希、无堆分配（`extensions` 为空时零分配）。
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
    ThemeStyle style = ThemeStyle::Material;  ///< 风格轴归属（身份位；供 CSD 派生与自描述面读取）。
    ColorRoles roles = ColorRoles::light();  ///< 色角色全集（本实例持明或暗一列）。
    TypeScale type_scale;  ///< 排版 15 档（不分明暗）。
    CsdRoles csd = CsdRoles::light();  ///< 窗口装饰配色角色组。
    ShapeScale shapes;  ///< 形状档位令牌（圆角，逻辑 dp）。
    SpacingScale spacing;  ///< 间距档位令牌（留白与内边距，逻辑 dp）。
    ControlMetrics controls;  ///< 控件基准度量（控件高、命中下限、焦点环、滚动条等，逻辑 dp）。
    LayoutConventions conventions;  ///< 布局与交互惯例组（枚举 + 动效倍率）。
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

    /// @brief 按「风格 × 明暗」两轴组合给出标定主题。
    ///
    /// 一枚风格内含明暗两列取值，故本函数不要求先选明暗再选风格；`Unspecified` 表示本主题
    /// 对明暗不表态，此时取该风格的**明色列**（与 `resolve_color_scheme` 及 CSD 桥的
    /// 「`Unspecified` ≡ 明色列」单一口径一致），`scheme` 字段仍保持 `Unspecified` 原样。
    ///
    /// 取值口径：只覆盖**有出处**的角色与度量，其余沿用 Material 基线值——不臆造无据数值。
    /// 出处分级（官方一手 / 真机实测 / 官方公式派生）随值写在实现体的注释里。
    ///
    /// @param style 风格轴取值（见 `ThemeStyle`）。
    /// @param scheme 明暗档；`Unspecified` 取该风格明色列且不表态。
    /// @return 该组合的标定主题。`from_style(Material, Light)` 逐位等于 `light()`、
    ///         `from_style(Material, Dark)` 逐位等于 `dark()`、
    ///         `from_style(Material, Unspecified)` 逐位等于 `with_defaults()`。
    [[nodiscard]] static auto from_style(ThemeStyle style, ColorScheme scheme = ColorScheme::Unspecified) -> Theme;

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
        return extensions.get<T>();
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

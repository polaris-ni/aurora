#pragma once

#include <type_traits>
#include <utility>

#include "aurora/theming/color_roles.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/theming/control_metrics.h"
#include "aurora/theming/csd_roles.h"
#include "aurora/theming/layout_conventions.h"
#include "aurora/theming/motion_scale.h"
#include "aurora/theming/shape_scale.h"
#include "aurora/theming/spacing_scale.h"
#include "aurora/theming/theme_extension.h"
#include "aurora/theming/theme_style.h"
#include "aurora/theming/type_scale.h"

namespace aurora {

/// @brief 主题：强类型色角色集 + 排版字阶 + 度量令牌四组 + 动效时长档 + CSD 组件角色 + 布局惯例 + 类型安全
///        扩展的值语义聚合。
///
/// 经 `Provider<Theme>` / `ThemeScope` 注入环境，组件用 `inherit_theme(ctx)` 读取（见
/// specification/07-environment-modifier.md §5.1）。取代旧的「扁平字段 + stringly 命名令牌表」双入口：
/// - `style`：本实例所属**风格轴**（身份位，供 CSD 派生与自描述面读取；取值见 `ThemeStyle`）；
/// - `roles`：Material 3 色角色全集（明暗两列，本实例持一列，由 `scheme` 标注归属档）；
/// - `type_scale`：排版 15 档（不分明暗，`body_medium` 承接旧 `Theme::font`）；
/// - `shapes` / `spacing` / `controls`：形状、间距与控件基准度量三组令牌（单位逻辑 dp，不分明暗）；
/// - `motion`：动效时长六档（不分明暗，单位毫秒；`conventions.motion_scale` 是它的倍率轴，二者相乘消费）；
/// - `conventions`：布局与交互惯例组（按钮序 / 标题对齐 / 菜单栏位置 / 滚动条模式 / 焦点环形状 / 动效倍率）；
/// - `csd`：窗口装饰配色角色组（与通用角色解耦，组件级覆写面）；
/// - `extensions`：应用自定义令牌以类型为键注册（`set_extension<T>` / `extension<T>()`），无字符串回落，
///   且**不参与属性回落**（类型擦除位不可枚举、不可求值成员指针，故只作应用级自定义令牌）。
///
/// 度量令牌各组在自身头文件里登记「成员指针 → 短名」对照表（`AURORA_TOKENS` / `token_name`），供属性行
/// 以成员指针作**活令牌回落点**：短名即 `aurora_api.json` 的 `default_expr` 用名，前缀为
/// `theme.<字段名>.<档名>`（色角色沿用既有的 `theme.<角色>` 拼法）。`conventions` 组是枚举而非标量，
/// 消费方是排布与绘制分支选择，不作回落点，故该组不登记表。
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
/// @note Rebuildable: no（无 to_json / from_json 实现体；经 from_style 两轴解算或 light/dark 预设重建，
///       JSON 载入口属计划面，见 specification/07-environment-modifier.md §5.4）
/// NOLINTNEXTLINE(bugprone-exception-escape)
struct Theme {
    ThemeStyle style = ThemeStyle::Material;  ///< 风格轴归属（身份位；供 CSD 派生与自描述面读取）。
    ColorRoles roles = ColorRoles::light();  ///< 色角色全集（本实例持明或暗一列）。
    TypeScale type_scale;  ///< 排版 15 档（不分明暗）。
    CsdRoles csd = CsdRoles::light();  ///< 窗口装饰配色角色组。
    ShapeScale shapes;  ///< 形状档位令牌（圆角，逻辑 dp）。
    SpacingScale spacing;  ///< 间距档位令牌（留白与内边距，逻辑 dp）。
    ControlMetrics controls;  ///< 控件基准度量（控件高、命中下限、焦点环、滚动条等，逻辑 dp）。
    MotionScale motion;  ///< 动效时长六档（不分明暗；减动效开关置位时快照取零档，属性行的活令牌回落点）。
    LayoutConventions conventions;  ///< 布局与交互惯例组（枚举 + 动效倍率）。
    ColorScheme scheme = ColorScheme::Unspecified;  ///< 明暗归属档（三级优先链中间层，语义不变）。
    ThemeExtensions extensions;  ///< 类型安全扩展注册表（空则零分配）。

    /// @brief 逐字段相等比较（含扩展注册表的逐条判等）；换肤与覆写物化用它判「到底变了没」。
    ///
    /// 度量令牌各组与 `scheme` / `style` 同参与比较，故「只改配色不动度量」也判为变；未注册扩展时该项恒真。
    /// 同值守卫（同值覆写不触发重排重绘）与相关单测按 `codespec/specification/07-environment-modifier.md`
    /// §5.1 的口径断言本比较。
    ///
    /// @param other 比较对象。
    /// @return 全部字段（含扩展）相等时为 true。
    [[nodiscard]] auto operator==(const Theme &other) const -> bool = default;

    /// @brief 生成浅色主题（亦是「无任何注入」时两条读取通路共用的兜底快照：`inherit_theme(ctx)` 与
    ///        `resolve_theme(root, target)`）。
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

    /// @brief 探测「这台机器的设计语言」属于哪套风格（风格轴的唯一运行期入口）。
    ///
    /// 判据表是纯函数 `detail::style_from(PlatformKind, DesktopEnvironment, has_window_backend)`，
    /// 本函数只负责取证据 + 转调：证据源是 `platform().kind` 与 `auto_detect_surface()`，
    /// **不是** `MediaQuery::platform`（后者表渲染后端而非 OS 家族，取错源会让「Linux 桌面 +
    /// 非原生后端」误落回落档）。
    ///
    /// 结果每进程只读一次并缓存（静态局部量），故**风格轴不随运行期环境变化**：换外壳要重启
    /// 应用。明暗轴相反，它经 `resolve_color_scheme` 三级优先链跟随 OS——两轴在这条上的差异是
    /// 有意的，见 specification/07-environment-modifier.md §5.2。
    /// @return 该环境的风格档；无窗口后端 / WASM / 无任何证据时为 `Material`（回落档，非错误态）。
    /// @note Side-effects: reads the process environment（仅 Linux / BSD 分支，且只读一次）。
    /// @note Thread: 静态局部量的首次初始化由语言保证线程安全，读取路径无锁。
    [[nodiscard]] static auto default_style() -> ThemeStyle;

    /// @brief 跟随这台机器的设计语言给出整套标定主题（零参数的默认路径）。
    ///
    /// 等价于 `from_style(default_style())`，即**只定风格轴、不定明暗档**：返回主题的 `scheme`
    /// 是 `Unspecified`（对明暗不表态，取值落明色列），明暗仍交给三级优先链解算。本函数不在
    /// 构造期直读注册表 / 配置文件拿暗色档——那会同时违反「不在每帧直读、创建时读一次」与
    /// 「构造期不隐式读 OS 偏好」两条既有纪律。要一份确定的暗色就写
    /// `from_style(default_style(), os_color_scheme())`，由调用方显式承担那次读取。
    /// @return 该环境风格档的标定主题（`scheme` 不表态）。
    /// @note 与 `XxxProps::defaults()` 同名不同主：后者出的是该控件的属性快照。
    [[nodiscard]] static auto defaults() -> Theme;

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

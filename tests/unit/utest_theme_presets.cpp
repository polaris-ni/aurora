/// 测试类型: unit
/// 目标单元: src/aurora/theming/theme_presets.cpp
/// 测试说明: 覆盖「风格 × 明暗」两轴工厂 Theme::from_style 的四条零回归不变量（Material 三支全等、
/// Unspecified 取明色列且不折平三态）、四套风格标定档的钉值（色锚点 / 字阶重锚 / 形状与控件度量 /
/// 布局惯例）、以及「有出处才覆写」的取值纪律——未标定字段必须逐字段等于 Material 基线。

#include <string>

#include "aurora/core/color.h"
#include "aurora/theming/color_roles.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
#include "aurora/theming/type_scale.h"
#include "aurora/window/title_bar_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_presets {

namespace {

/// @brief 主题逐字段比较（除扩展注册表外的九个值面）；`Theme` 未声明 `operator==`，故在此聚合。
auto same_theme(const Theme &a, const Theme &b) -> bool {
    return a.style == b.style && a.scheme == b.scheme && a.roles == b.roles && a.type_scale == b.type_scale &&
           a.csd == b.csd && a.shapes == b.shapes && a.spacing == b.spacing && a.controls == b.controls &&
           a.conventions == b.conventions;
}

/// @brief 五枚风格档 × 三枚明暗档的取值枚举（表格驱动用例的公共行集）。
constexpr ThemeStyle AURORA_STYLES[] = {ThemeStyle::Material, ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome,
                                        ThemeStyle::Kde};

}  // namespace

AURORA_TEST_CASE(material_arm_is_the_identity_baseline) {
    // 零回归基线（判据 2）：Material 档的三支与既有工厂 / 裸构造逐位全等。
    AURORA_TEST_CHECK_TRUE(same_theme(Theme::from_style(ThemeStyle::Material, ColorScheme::Light), Theme::light()));
    AURORA_TEST_CHECK_TRUE(same_theme(Theme::from_style(ThemeStyle::Material, ColorScheme::Dark), Theme::dark()));
    AURORA_TEST_CHECK_TRUE(
        same_theme(Theme::from_style(ThemeStyle::Material, ColorScheme::Unspecified), Theme::with_defaults()));
    AURORA_TEST_CHECK_TRUE(same_theme(Theme::with_defaults(), Theme{}));
    // 风格轴身份位仍然写入（三支之外唯一的差别）。
    AURORA_TEST_CHECK_EQ(Theme::from_style(ThemeStyle::Material, ColorScheme::Light).style, ThemeStyle::Material);
}

AURORA_TEST_CASE(unspecified_takes_light_column_without_collapsing_scheme) {
    // 三态不塌（判据 5）：Unspecified 取该风格明色列，但 scheme 字段原样保持 Unspecified。
    for (const ThemeStyle style : AURORA_STYLES) {
        const Theme uns = Theme::from_style(style, ColorScheme::Unspecified);
        const Theme lit = Theme::from_style(style, ColorScheme::Light);
        AURORA_TEST_CHECK(uns.roles == lit.roles);
        AURORA_TEST_CHECK(uns.csd == lit.csd);
        AURORA_TEST_CHECK(uns.type_scale == lit.type_scale);
        AURORA_TEST_CHECK(uns.shapes == lit.shapes);
        AURORA_TEST_CHECK(uns.controls == lit.controls);
        AURORA_TEST_CHECK(uns.conventions == lit.conventions);
        AURORA_TEST_CHECK_EQ(uns.scheme, ColorScheme::Unspecified);
        AURORA_TEST_CHECK_EQ(uns.style, style);
    }
    // 明暗两档字段名集合同构（同一 ColorRoles 类型，判据 1）：类型层面成立，此处校实际分叉。
    const Theme w_l = Theme::from_style(ThemeStyle::Windows, ColorScheme::Light);
    const Theme w_d = Theme::from_style(ThemeStyle::Windows, ColorScheme::Dark);
    AURORA_TEST_CHECK(w_l.roles != w_d.roles);
    AURORA_TEST_CHECK(w_l.csd != w_d.csd);
}

AURORA_TEST_CASE(each_style_arm_actually_overrides_the_baseline) {
    // 标定确实生效：四套非 Material 档的页面底色都不得等于 Material 基线（防止「写了表没接上」）。
    const Theme base = Theme::light();
    for (const ThemeStyle style : {ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome, ThemeStyle::Kde}) {
        const Theme t = Theme::from_style(style, ColorScheme::Light);
        AURORA_TEST_CHECK(t.roles.surface != base.roles.surface || t.roles.primary != base.roles.primary);
        AURORA_TEST_CHECK(t.type_scale.body_medium.family != std::string{"sans-serif"});
        AURORA_TEST_CHECK(t.type_scale.body_medium.size_pt != base.type_scale.body_medium.size_pt);
    }
}

AURORA_TEST_CASE(windows_calibrated_anchors) {
    const Theme light = Theme::from_style(ThemeStyle::Windows, ColorScheme::Light);
    const Theme dark = Theme::from_style(ThemeStyle::Windows, ColorScheme::Dark);
    // Fluent 中性面（官方 Common_themeresources 键名见实现体注释）。
    AURORA_TEST_CHECK_EQ(light.roles.surface, Color{0xF3, 0xF3, 0xF3});
    AURORA_TEST_CHECK_EQ(dark.roles.surface, Color{0x20, 0x20, 0x20});
    AURORA_TEST_CHECK_EQ(light.roles.on_surface, Color{0x1A, 0x1A, 0x1A});
    AURORA_TEST_CHECK_EQ(dark.roles.on_surface, Color::white());
    // 强调色取官方变体阶梯（Dark1 / Light2），非未取到出处的 #0078D4。
    AURORA_TEST_CHECK_EQ(light.roles.primary, Color{0x00, 0x5A, 0x9E});
    AURORA_TEST_CHECK_EQ(dark.roles.primary, Color{0x76, 0xB9, 0xED});
    AURORA_TEST_CHECK_EQ(light.roles.error, Color{0xC4, 0x2B, 0x1C});
    // 度量：ControlCornerRadius 4 / 控件基准高 32 / 正文 14px 折 10.5pt。
    AURORA_TEST_CHECK_NEAR(light.shapes.small, 4.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(light.shapes.medium, 8.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(light.controls.control_height, 32.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(light.type_scale.body_medium.size_pt, 10.5F, 1e-4F);
    AURORA_TEST_CHECK_EQ(light.type_scale.body_medium.family, std::string{"Segoe UI Variable Text"});
    // 惯例：Fluent 与 Material 默认在对话框序 / 菜单栏位置 / 滚动条 / 焦点环四档上同向，故不覆写。
    AURORA_TEST_CHECK(light.conventions == LayoutConventions{});
    // CSD 基座同步换档（栏高 32、按钮语言 Windows）。
    AURORA_TEST_CHECK_EQ(light.csd.title_bar_bg_active, TitleBarStyle::from_preset(CsdPreset::WindowsLight).bg_active);
}

AURORA_TEST_CASE(macos_calibrated_anchors) {
    const Theme light = Theme::from_style(ThemeStyle::MacOs, ColorScheme::Light);
    const Theme dark = Theme::from_style(ThemeStyle::MacOs, ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(light.roles.surface, Color{0xEC, 0xEC, 0xEC});
    AURORA_TEST_CHECK_EQ(dark.roles.surface, Color{0x32, 0x32, 0x32});
    AURORA_TEST_CHECK_EQ(light.roles.surface_container, Color::white());
    AURORA_TEST_CHECK_EQ(dark.roles.surface_container, Color{0x1E, 0x1E, 0x1E});
    // 强调色取 AppKit 实测 controlAccentColor（与 HIG 表 Blue 行 0,136,255 冲突，理由见实现体）。
    AURORA_TEST_CHECK_EQ(light.roles.primary, Color{0x00, 0x7A, 0xFF});
    AURORA_TEST_CHECK_NEAR(light.type_scale.body_medium.size_pt, 13.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(light.type_scale.body_medium.family, std::string{"SF Pro Text"});
    // 四档 macOS 惯例分叉：竖排右对齐且接受在上、原生菜单栏、覆盖式滚动条、外扩发光焦点环。
    AURORA_TEST_CHECK_EQ(light.conventions.dialog_button_order, DialogButtonOrder::VerticalRightAligned);
    AURORA_TEST_CHECK_EQ(light.conventions.menu_bar_placement, MenuBarPlacement::NativePreferred);
    AURORA_TEST_CHECK_EQ(light.conventions.scrollbar_mode, ScrollbarMode::Overlay);
    AURORA_TEST_CHECK_EQ(light.conventions.focus_ring_style, FocusRingStyle::OuterGlow);
    // 未取证面（圆角档位、控件基准高）保持 Material 基线，不得被猜测值污染。
    AURORA_TEST_CHECK(light.shapes == ShapeScale{});
    AURORA_TEST_CHECK_EQ(light.controls.control_height, ControlMetrics{}.control_height);
}

AURORA_TEST_CASE(gnome_calibrated_anchors) {
    const Theme light = Theme::from_style(ThemeStyle::Gnome, ColorScheme::Light);
    const Theme dark = Theme::from_style(ThemeStyle::Gnome, ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(light.roles.surface, Color{0xFA, 0xFA, 0xFB});
    AURORA_TEST_CHECK_EQ(dark.roles.surface, Color{0x22, 0x22, 0x26});
    // 强调色：Adwaita blue_3 配白不达 AA，改取同调色板的 blue_4（明暗同值）。
    AURORA_TEST_CHECK_EQ(light.roles.primary, Color{0x1C, 0x71, 0xD8});
    AURORA_TEST_CHECK_EQ(dark.roles.primary, light.roles.primary);
    // 中性面四层阶梯（亮档第五档无出处 → 留基线）。
    AURORA_TEST_CHECK_EQ(light.roles.surface_container_lowest, Color::white());
    AURORA_TEST_CHECK_EQ(light.roles.surface_container_high, Color{0xEB, 0xEB, 0xED});
    AURORA_TEST_CHECK_EQ(light.roles.surface_container_highest, ColorRoles::light().surface_container_highest);
    AURORA_TEST_CHECK_EQ(dark.roles.surface_container_highest, Color{0x36, 0x36, 0x3A});
    AURORA_TEST_CHECK_NEAR(light.type_scale.body_medium.size_pt, 11.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(light.type_scale.body_medium.family, std::string{"Adwaita Sans"});
    AURORA_TEST_CHECK_NEAR(light.shapes.small, 9.0F, 1e-4F);  // $button_radius
    AURORA_TEST_CHECK_NEAR(light.shapes.large, 12.0F, 1e-4F);  // $card_radius
    AURORA_TEST_CHECK_NEAR(light.controls.control_height, 34.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(light.conventions.dialog_button_order, DialogButtonOrder::AcceptLast);
    AURORA_TEST_CHECK_EQ(light.conventions.scrollbar_mode, ScrollbarMode::Overlay);
    // 判据（既有契约保位）：Gnome 档的 CSD 组与 Adwaita 预设逐值相同，等于不覆写。
    AURORA_TEST_CHECK(light.csd == CsdRoles::light());
    AURORA_TEST_CHECK(dark.csd == CsdRoles::dark());
}

AURORA_TEST_CASE(kde_calibrated_anchors) {
    const Theme light = Theme::from_style(ThemeStyle::Kde, ColorScheme::Light);
    const Theme dark = Theme::from_style(ThemeStyle::Kde, ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(light.roles.surface, Color{0xEF, 0xF0, 0xF1});
    AURORA_TEST_CHECK_EQ(dark.roles.surface, Color{0x20, 0x23, 0x26});
    AURORA_TEST_CHECK_EQ(light.roles.primary, Color{0x3D, 0xAE, 0xE9});  // 官方强调色，明暗同值
    AURORA_TEST_CHECK_EQ(dark.roles.primary, light.roles.primary);
    AURORA_TEST_CHECK_EQ(light.roles.focus_ring, Color{0x3D, 0xAE, 0xE9});  // DecorationFocus
    AURORA_TEST_CHECK_NEAR(light.type_scale.body_medium.size_pt, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_EQ(light.type_scale.body_medium.family, std::string{"Noto Sans"});
    AURORA_TEST_CHECK_NEAR(light.shapes.small, 5.0F, 1e-4F);  // Frame_FrameRadius
    AURORA_TEST_CHECK_NEAR(light.controls.control_height, 36.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(light.controls.scrollbar_thickness, 8.0F, 1e-4F);  // ScrollBar_SliderWidth
    // Qt 的 KdeLayout：Yes 在 No 前、Accept 在 Reject 前 ⇒ 接受在前；小部件层只有经典占位滚动条。
    AURORA_TEST_CHECK_EQ(light.conventions.dialog_button_order, DialogButtonOrder::AcceptFirst);
    AURORA_TEST_CHECK_EQ(light.conventions.scrollbar_mode, ScrollbarMode::Classic);
    // CSD 基座换到 Breeze 两档（标题 10pt / 400，配色取 [Colors:Window]）。
    AURORA_TEST_CHECK_EQ(light.csd.title_bar_bg_active, TitleBarStyle::from_preset(CsdPreset::BreezeLight).bg_active);
    AURORA_TEST_CHECK_EQ(dark.csd.title_bar_bg_active, TitleBarStyle::from_preset(CsdPreset::BreezeDark).bg_active);
    AURORA_TEST_CHECK(dark.csd != CsdRoles::dark());  // 与 Adwaita 暗档确实分叉
}

AURORA_TEST_CASE(type_scale_retarget_preserves_ramp_shape) {
    // 字阶重锚 = 族名整组替换 + 十五档按同一比值缩放（分级「派生」），档间比值必须保持。
    const TypeScale base;
    for (const ThemeStyle style : {ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome, ThemeStyle::Kde}) {
        const Theme t = Theme::from_style(style, ColorScheme::Light);
        const float factor = t.type_scale.body_medium.size_pt / base.body_medium.size_pt;
        AURORA_TEST_CHECK_NEAR(t.type_scale.headline_large.size_pt, base.headline_large.size_pt * factor, 1e-3F);
        AURORA_TEST_CHECK_NEAR(t.type_scale.title_medium.size_pt, base.title_medium.size_pt * factor, 1e-3F);
        AURORA_TEST_CHECK_NEAR(t.type_scale.label_small.size_pt, base.label_small.size_pt * factor, 1e-3F);
        // 字重不被重锚触碰（各档沿用 M3 的 400 / 500 分布）。
        AURORA_TEST_CHECK_EQ(t.type_scale.title_medium.weight, base.title_medium.weight);
        AURORA_TEST_CHECK_EQ(t.type_scale.body_medium.weight, base.body_medium.weight);
        // 同风格的明暗两档共用一支字阶（字阶不分明暗）。
        const Theme d = Theme::from_style(style, ColorScheme::Dark);
        AURORA_TEST_CHECK(t.type_scale == d.type_scale);
    }
    // Material 档不动字阶。
    AURORA_TEST_CHECK(Theme::from_style(ThemeStyle::Material, ColorScheme::Light).type_scale == base);
}

AURORA_TEST_CASE(uncalibrated_roles_stay_on_material_baseline) {
    // 取值纪律：只覆盖有出处的角色。四套风格的 secondary / tertiary / inverse 族与
    // surface_dim / surface_bright（除 Gnome 标定过者）在未被取证前逐字段等于 Material 基线。
    for (const ThemeStyle style : {ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Kde}) {
        for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
            const Theme t = Theme::from_style(style, scheme);
            const ColorRoles base = scheme == ColorScheme::Light ? ColorRoles::light() : ColorRoles::dark();
            AURORA_TEST_CHECK(t.roles.secondary == base.secondary);
            AURORA_TEST_CHECK(t.roles.on_secondary == base.on_secondary);
            AURORA_TEST_CHECK(t.roles.tertiary == base.tertiary);
            AURORA_TEST_CHECK(t.roles.inverse_surface == base.inverse_surface);
            AURORA_TEST_CHECK(t.roles.error_container == base.error_container);
            AURORA_TEST_CHECK(t.spacing == SpacingScale{});  // 间距档位四套风格均未取证
            AURORA_TEST_CHECK_NEAR(t.conventions.motion_scale, 1.0F, 1e-4F);
        }
    }
}

AURORA_TEST_CASE(scrollbar_roles_inherit_the_scheme_baseline_in_every_style) {
    // 五套风格档均不覆写滚动条配色：色值只随明暗列切换，风格轴只改占位模式与粗细。
    for (const ThemeStyle style : AURORA_STYLES) {
        for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
            const Theme t = Theme::from_style(style, scheme);
            const ColorRoles base = scheme == ColorScheme::Light ? ColorRoles::light() : ColorRoles::dark();
            AURORA_TEST_CHECK(t.roles.scrollbar_thumb == base.scrollbar_thumb);
            AURORA_TEST_CHECK(t.roles.scrollbar_track == base.scrollbar_track);
        }
    }
}

AURORA_TEST_CASE(all_five_styles_resolve_for_every_scheme) {
    // 五枚风格 × 三枚明暗档共十五格全部有值（无「未实现」空返回）。
    for (const ThemeStyle style : AURORA_STYLES) {
        for (const ColorScheme scheme : {ColorScheme::Unspecified, ColorScheme::Light, ColorScheme::Dark}) {
            const Theme t = Theme::from_style(style, scheme);
            AURORA_TEST_CHECK_EQ(t.style, style);
            AURORA_TEST_CHECK_EQ(t.scheme, scheme);
            AURORA_TEST_CHECK(t.roles.surface != Color{});
            AURORA_TEST_CHECK(!t.type_scale.body_medium.family.empty());
        }
    }
}

}  // namespace aurora::test_cases::utest_theme_presets

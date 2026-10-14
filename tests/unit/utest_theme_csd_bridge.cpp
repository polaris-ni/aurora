/// 测试类型: unit
/// 目标单元: include/aurora/theming/csd_style_bridge.h + theming/color_scheme.h + theming/theme.h
/// 测试说明: 覆盖明暗三级优先解析顺序、工厂置 scheme 与 CSD 角色列、typed 派生与 Adwaita 预设逐值一致的
/// 基准契约（含 Unspecified ≡ 明色列使 from_bare 由暗转明）、以及逐字段覆写经 theme.csd 流入派生结果。

#include "aurora/theming/csd_style_bridge.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_csd_bridge {

AURORA_TEST_CASE(resolve_color_scheme_follows_three_tier_priority) {
    // 三级优先链：显式 > Theme::scheme > OS；逐层「不表态」时下探；全链未指定返回 Unspecified。
    using CS = ColorScheme;
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Light, CS::Dark, CS::Dark), CS::Light);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Dark, CS::Light, CS::Dark), CS::Dark);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Dark, CS::Light), CS::Dark);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Unspecified, CS::Dark), CS::Dark);
    // Unspecified 不等于 Light：中间层未指定时不得被当成亮色提前截断。
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Unspecified, CS::Light), CS::Light);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Unspecified, CS::Unspecified), CS::Unspecified);
}

AURORA_TEST_CASE(factories_set_scheme_and_csd_column) {
    const Theme light = Theme::light();
    const Theme dark = Theme::dark();
    AURORA_TEST_CHECK_EQ(light.scheme, ColorScheme::Light);
    AURORA_TEST_CHECK_EQ(dark.scheme, ColorScheme::Dark);
    // CSD 角色列随工厂取对应预设 pinned 值；跨档共享的 close_hover / shadow 一致。
    AURORA_TEST_CHECK(light.csd == CsdRoles::light());
    AURORA_TEST_CHECK(dark.csd == CsdRoles::dark());
    AURORA_TEST_CHECK_EQ(light.csd.title_bar_close_hover, dark.csd.title_bar_close_hover);
    AURORA_TEST_CHECK_EQ(light.csd.shadow_color, dark.csd.shadow_color);
}

AURORA_TEST_CASE(derived_title_bar_style_matches_presets) {
    // 基准契约：未逐字段定制时，「从 Theme 派生」与「直接取预设」配色字段逐值相同（typed 版，延续 CHG-010）。
    const TitleBarStyle from_dark = title_bar_style_from_theme(Theme::dark());
    const TitleBarStyle adwaita_dark = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(from_dark.bg_active, adwaita_dark.bg_active);
    AURORA_TEST_CHECK_EQ(from_dark.bg_inactive, adwaita_dark.bg_inactive);
    AURORA_TEST_CHECK_EQ(from_dark.fg_active, adwaita_dark.fg_active);
    AURORA_TEST_CHECK_EQ(from_dark.fg_inactive, adwaita_dark.fg_inactive);
    AURORA_TEST_CHECK_EQ(from_dark.hover_tint, adwaita_dark.hover_tint);
    AURORA_TEST_CHECK_EQ(from_dark.pressed_tint, adwaita_dark.pressed_tint);
    AURORA_TEST_CHECK_EQ(from_dark.close_hover, adwaita_dark.close_hover);
    AURORA_TEST_CHECK_EQ(from_dark.button_layout, adwaita_dark.button_layout);

    const TitleBarStyle from_light = title_bar_style_from_theme(Theme::light());
    const TitleBarStyle adwaita_light = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(from_light.bg_active, adwaita_light.bg_active);
    AURORA_TEST_CHECK_EQ(from_light.fg_active, adwaita_light.fg_active);
    AURORA_TEST_CHECK_EQ(from_light.hover_tint, adwaita_light.hover_tint);

    // 行为变化：scheme=Unspecified（with_defaults）基座现按明色（全系统「Unspecified ≡ 明色列」单一口径）。
    const TitleBarStyle from_bare = title_bar_style_from_theme(Theme::with_defaults());
    AURORA_TEST_CHECK_EQ(from_bare.bg_active, adwaita_light.bg_active);
}

AURORA_TEST_CASE(csd_field_override_flows_through_derivation) {
    // 逐字段定制：只改 theme.csd 一个字段，其余仍取基座（取代旧 csd.* 字符串令牌覆写）。
    Theme t = Theme::light();
    t.csd.title_bar_bg_active = Color{0x12, 0x34, 0x56};
    const TitleBarStyle s = title_bar_style_from_theme(t);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0x12, 0x34, 0x56});
    const TitleBarStyle base = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(s.bg_inactive, base.bg_inactive);  // 未覆写 → 明色基座
    AURORA_TEST_CHECK_EQ(s.close_hover, base.close_hover);
    AURORA_TEST_CHECK_EQ(s.button_layout, base.button_layout);
    AURORA_TEST_CHECK_NEAR(s.height, base.height, 1e-4F);
}

AURORA_TEST_CASE(frame_style_derivation_uses_csd_shadow_color) {
    // 窗框派生只把投影基色取 theme.csd.shadow_color；几何字段保持基座（行为定制不走配色面）。
    Theme t = Theme::dark();
    t.csd.shadow_color = Color{0x10, 0x20, 0x30, 0x40};
    WindowFrameStyle base;
    base.shadow_margin_dp = 24;
    const WindowFrameStyle f = window_frame_style_from_theme(t, base);
    AURORA_TEST_CHECK_EQ(f.shadow_color, Color{0x10, 0x20, 0x30, 0x40});
    AURORA_TEST_CHECK_EQ(f.shadow_margin_dp, 24);
    AURORA_TEST_CHECK_EQ(f.shadow_enabled, base.shadow_enabled);
    AURORA_TEST_CHECK_NEAR(f.shadow_blur_radius, base.shadow_blur_radius, 1e-4F);
    AURORA_TEST_CHECK_NEAR(f.corner_radius, base.corner_radius, 1e-4F);

    // 未定制：投影色取 light 列的 csd.shadow_color。
    const WindowFrameStyle untouched = window_frame_style_from_theme(Theme::light(), base);
    AURORA_TEST_CHECK_EQ(untouched.shadow_color, CsdRoles::light().shadow_color);
}

}  // namespace aurora::test_cases::utest_theme_csd_bridge

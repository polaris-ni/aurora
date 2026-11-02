/// 测试类型: unit
/// 目标单元: include/aurora/theming/csd_style_bridge.h + theming/color_scheme.h + theming/theme.h
/// 测试说明: 覆盖明暗三级优先解析顺序、工厂置 scheme 与 CSD 角色列、typed 派生与预设逐值一致的基准契约
/// （含 Unspecified ≡ 明色列使 from_bare 由暗转明）、风格轴 → 八档预设的两轴映射矩阵、from_style 工厂
/// 主题派生与对应预设逐值相同（八档全覆盖）、以及逐字段覆写经 theme.csd 流入派生结果。

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
    // 基准契约：未逐字段定制时，「从 Theme 派生」与「直接取预设」配色字段逐值相同（typed 版，延续 ）。
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

AURORA_TEST_CASE(csd_preset_for_maps_both_axes) {
    // 两轴 → 八档预设的映射矩阵（15 格逐格钉）：Unspecified 一律折到明色列，Material 与
    // Gnome 两支同落 Adwaita——后者是本库无据回落档的既有观感来源，故派生通道零 diff。
    using CS = ColorScheme;
    using ST = ThemeStyle;
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Material, CS::Light), CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Material, CS::Dark), CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Material, CS::Unspecified), CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Gnome, CS::Light), CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Gnome, CS::Dark), CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Gnome, CS::Unspecified), CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Windows, CS::Light), CsdPreset::WindowsLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Windows, CS::Dark), CsdPreset::WindowsDark);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Windows, CS::Unspecified), CsdPreset::WindowsLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::MacOs, CS::Light), CsdPreset::MacLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::MacOs, CS::Dark), CsdPreset::MacDark);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::MacOs, CS::Unspecified), CsdPreset::MacLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Kde, CS::Light), CsdPreset::BreezeLight);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Kde, CS::Dark), CsdPreset::BreezeDark);
    AURORA_TEST_CHECK_EQ(csd_preset_for(ST::Kde, CS::Unspecified), CsdPreset::BreezeLight);
}

AURORA_TEST_CASE(from_style_derivation_matches_each_preset) {
    // 八档全覆盖（四风格 × 明暗，Material 与 Gnome 同档故取 Gnome 代表 Adwaita）：工厂主题的
    // `csd` 组由同一预设搬运，故「from_style 再派生」与「直接取预设」逐值相同——配色七支之外，
    // 基座带来的几何（栏高 / 按钮语言 / 标题度量）也必须一致。
    struct Cell {
        ThemeStyle style;
        ColorScheme scheme;
        CsdPreset preset;
    };
    const Cell cells[] = {
        {.style = ThemeStyle::Gnome, .scheme = ColorScheme::Light, .preset = CsdPreset::AdwaitaLight},
        {.style = ThemeStyle::Gnome, .scheme = ColorScheme::Dark, .preset = CsdPreset::AdwaitaDark},
        {.style = ThemeStyle::Windows, .scheme = ColorScheme::Light, .preset = CsdPreset::WindowsLight},
        {.style = ThemeStyle::Windows, .scheme = ColorScheme::Dark, .preset = CsdPreset::WindowsDark},
        {.style = ThemeStyle::MacOs, .scheme = ColorScheme::Light, .preset = CsdPreset::MacLight},
        {.style = ThemeStyle::MacOs, .scheme = ColorScheme::Dark, .preset = CsdPreset::MacDark},
        {.style = ThemeStyle::Kde, .scheme = ColorScheme::Light, .preset = CsdPreset::BreezeLight},
        {.style = ThemeStyle::Kde, .scheme = ColorScheme::Dark, .preset = CsdPreset::BreezeDark},
    };
    for (const Cell &c : cells) {
        const TitleBarStyle got = title_bar_style_from_theme(Theme::from_style(c.style, c.scheme));
        const TitleBarStyle want = TitleBarStyle::from_preset(c.preset);
        AURORA_TEST_CHECK_EQ(got.bg_active, want.bg_active);
        AURORA_TEST_CHECK_EQ(got.bg_inactive, want.bg_inactive);
        AURORA_TEST_CHECK_EQ(got.fg_active, want.fg_active);
        AURORA_TEST_CHECK_EQ(got.fg_inactive, want.fg_inactive);
        AURORA_TEST_CHECK_EQ(got.hover_tint, want.hover_tint);
        AURORA_TEST_CHECK_EQ(got.pressed_tint, want.pressed_tint);
        AURORA_TEST_CHECK_EQ(got.close_hover, want.close_hover);
        AURORA_TEST_CHECK_EQ(got.button_layout, want.button_layout);
        AURORA_TEST_CHECK_NEAR(got.height, want.height, 1e-4F);
        AURORA_TEST_CHECK_NEAR(got.metrics.title_font_size_pt, want.metrics.title_font_size_pt, 1e-4F);
        AURORA_TEST_CHECK_EQ(got.metrics.title_font_weight, want.metrics.title_font_weight);
        AURORA_TEST_CHECK_EQ(got.center_title, want.center_title);
    }
}

AURORA_TEST_CASE(theme_style_field_drives_csd_base) {
    // 风格轴是身份位：只改 `style` 就换基座（不重写 csd 时，配色仍从 csd 组取，故只换几何）。
    Theme t = Theme::light();
    t.style = ThemeStyle::Windows;
    const TitleBarStyle s = title_bar_style_from_theme(t);
    const TitleBarStyle adwaita = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_NEAR(s.height, 32.0F, 1e-4F);  // Windows 档栏高钉值，非 Adwaita 基准 36
    AURORA_TEST_CHECK_EQ(s.button_layout, TitleBarButtonLayout::Windows);
    AURORA_TEST_CHECK_FALSE(s.center_title);
    // 配色未随基座换档（theme.csd 仍是明色 Adwaita 列）——这是「配色走角色组、几何走预设」
    // 两段式派生的直接后果；工厂主题 from_style 会把两者一起给出。
    AURORA_TEST_CHECK_EQ(s.bg_active, adwaita.bg_active);
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

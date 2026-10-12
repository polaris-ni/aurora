/// 测试类型: unit
/// 目标单元: include/aurora/theming/csd_style_bridge.h + theming/color_scheme.h + theming/theme.h
/// 测试说明: 覆盖明暗三级优先解析顺序、Theme 双档令牌表（dark_tokens 先查后回落）、
/// 语义/组件令牌播种契约、Theme→TitleBarStyle/WindowFrameStyle 派生（含与预设逐值一致的基准）

#include "aurora/theming/csd_style_bridge.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_csd_bridge {

AURORA_TEST_CASE(resolve_color_scheme_follows_three_tier_priority) {
    // 三级优先链：显式 > Theme > OS；逐层「不表态」时下探；全链未指定返回 Unspecified。
    using CS = ColorScheme;
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Light, CS::Dark, CS::Dark), CS::Light);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Dark, CS::Light, CS::Dark), CS::Dark);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Dark, CS::Light), CS::Dark);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Unspecified, CS::Dark), CS::Dark);
    // Unspecified 不等于 Light：中间层未指定时不得被当成亮色提前截断。
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Unspecified, CS::Light), CS::Light);
    AURORA_TEST_CHECK_EQ(resolve_color_scheme(CS::Unspecified, CS::Unspecified, CS::Unspecified), CS::Unspecified);
}

AURORA_TEST_CASE(theme_factories_seed_semantic_and_csd_tokens) {
    // light()/dark() 播种同一套键名（语义 + csd.* 组件位）；dark 的差异只进 dark_tokens。
    const Theme light = Theme::light();
    const Theme dark = Theme::dark();
    for (const char *key : {"surface", "surface.raised", "text", "text.muted", "accent", "border", "hover", "pressed",
                            "danger", "csd.title_bar.bg.active", "csd.title_bar.bg.inactive", "csd.title_bar.fg.active",
                            "csd.title_bar.fg.inactive", "csd.title_bar.hover", "csd.title_bar.pressed",
                            "csd.title_bar.close.hover", "csd.shadow.color"}) {
        AURORA_TEST_CHECK_TRUE(light.token(key).has_value());
        AURORA_TEST_CHECK_TRUE(dark.token(key).has_value());
    }
    // 档位归属：工厂置 scheme；语义色随档反转（surface 白 ↔ 暗灰）。
    AURORA_TEST_CHECK_EQ(light.scheme, ColorScheme::Light);
    AURORA_TEST_CHECK_EQ(dark.scheme, ColorScheme::Dark);
    AURORA_TEST_CHECK_EQ(light.token_or_for("surface", Color{}), Color::white());
    AURORA_TEST_CHECK_NE(dark.token_or_for("surface", Color{}), Color::white());
    // danger / close.hover 跨档共享（未进 dark_tokens，走基础表回落）。
    AURORA_TEST_CHECK_EQ(light.token_or_for("danger", Color{}), Color{0xE0, 0x1B, 0x24});
    AURORA_TEST_CHECK_EQ(dark.token_or_for("danger", Color{}), Color{0xE0, 0x1B, 0x24});
}

AURORA_TEST_CASE(token_resolution_prefers_dark_table_only_in_dark_scheme) {
    // 同一 Theme：Dark 档先查 dark_tokens；Light/Unspecified 档只查基础表（同一键两读不同值）。
    Theme t = Theme::light();
    t.set_token("csd.title_bar.bg.active", TokenValue{Color{0xAA, 0xBB, 0xCC}});
    t.dark_tokens["csd.title_bar.bg.active"] = TokenValue{Color{0x11, 0x22, 0x33}};

    const auto dark_value = t.token_in("csd.title_bar.bg.active", ColorScheme::Dark)->as<Color>();
    const auto light_value = t.token_in("csd.title_bar.bg.active", ColorScheme::Light)->as<Color>();
    AURORA_TEST_CHECK_TRUE(dark_value.has_value());
    AURORA_TEST_CHECK_TRUE(light_value.has_value());
    AURORA_TEST_CHECK_EQ(*dark_value, Color{0x11, 0x22, 0x33});
    AURORA_TEST_CHECK_EQ(*light_value, Color{0xAA, 0xBB, 0xCC});

    // 未进 dark_tokens 的键：Dark 档回落基础表。
    t.dark_tokens.erase("csd.title_bar.bg.active");
    AURORA_TEST_CHECK_EQ(t.token_or_in("csd.title_bar.bg.active", ColorScheme::Dark, Color{}), Color{0xAA, 0xBB, 0xCC});

    // scheme=Unspecified 的手工主题：token_for 走基础表。
    Theme bare;
    AURORA_TEST_CHECK_EQ(bare.scheme, ColorScheme::Unspecified);
    bare.set_token("surface", TokenValue{Color{0x01, 0x02, 0x03}});
    AURORA_TEST_CHECK_EQ(bare.token_or_for("surface", Color{}), Color{0x01, 0x02, 0x03});
}

AURORA_TEST_CASE(derived_title_bar_style_matches_presets_token_for_token) {
    // 基准契约：未覆写任何令牌时，「从 Theme 派生」与「直接取预设」逐值相同——
    // 这是令牌播种数值与预设共用同一组数字的机械证明。
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

    // Unspecified 主题：基座按暗色（与 TitleBarStyle{} 默认观感一致）。
    const TitleBarStyle from_bare = title_bar_style_from_theme(Theme::with_defaults());
    AURORA_TEST_CHECK_EQ(from_bare.bg_active, adwaita_dark.bg_active);
}

AURORA_TEST_CASE(csd_component_tokens_override_derived_style) {
    // 组件覆写位：只写 csd.title_bar.bg.active 一个键，其余字段保持预设基座。
    Theme t = Theme::light();
    t.set_token("csd.title_bar.bg.active", TokenValue{Color{0x12, 0x34, 0x56}});
    const TitleBarStyle s = title_bar_style_from_theme(t);
    AURORA_TEST_CHECK_EQ(s.bg_active, Color{0x12, 0x34, 0x56});
    const TitleBarStyle base = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK_EQ(s.bg_inactive, base.bg_inactive);  // 未覆写 → 基座
    AURORA_TEST_CHECK_EQ(s.close_hover, base.close_hover);
    AURORA_TEST_CHECK_EQ(s.button_layout, base.button_layout);
    AURORA_TEST_CHECK_NEAR(s.height, base.height, 1e-4F);
}

AURORA_TEST_CASE(frame_style_derivation_touches_only_shadow_color) {
    // 窗框派生只动投影基色；厚度/模糊/圆角保持基座（几何属行为定制，不走令牌）。
    Theme t = Theme::dark();
    t.set_token("csd.shadow.color", TokenValue{Color{0x10, 0x20, 0x30, 0x40}});
    WindowFrameStyle base;
    base.shadow_margin_dp = 24;  // 显式基座值须原样保留
    const WindowFrameStyle f = window_frame_style_from_theme(t, base);
    AURORA_TEST_CHECK_EQ(f.shadow_color, Color{0x10, 0x20, 0x30, 0x40});
    AURORA_TEST_CHECK_EQ(f.shadow_margin_dp, 24);
    AURORA_TEST_CHECK_EQ(f.shadow_enabled, base.shadow_enabled);
    AURORA_TEST_CHECK_NEAR(f.shadow_blur_radius, base.shadow_blur_radius, 1e-4F);
    AURORA_TEST_CHECK_NEAR(f.corner_radius, base.corner_radius, 1e-4F);

    // 未覆写令牌：基座逐值保留。
    const WindowFrameStyle untouched = window_frame_style_from_theme(Theme::light(), base);
    AURORA_TEST_CHECK_EQ(untouched.shadow_color, base.shadow_color);
}

}  // namespace aurora::test_cases::utest_theme_csd_bridge

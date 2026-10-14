/// 测试类型: unit
/// 目标单元: include/aurora/theming/csd_roles.h
/// 测试说明: 钉死 CsdRoles 取值与 Adwaita 预设的等价契约——CsdRoles::light() 逐字段等于
/// TitleBarStyle::from_preset(AdwaitaLight) 的配色字段，dark() 等于 AdwaitaDark；这是立骨批
/// 把 csd.* 字符串令牌替换为 typed 角色组后、派生通道「逐值不变量」的取值前提。

#include "aurora/theming/csd_roles.h"
#include "aurora/window/title_bar_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_csd_roles {

AURORA_TEST_CASE(csd_roles_light_matches_adwaita_light_preset) {
    const CsdRoles r = CsdRoles::light();
    const TitleBarStyle p = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    AURORA_TEST_CHECK(r.title_bar_bg_active == p.bg_active);
    AURORA_TEST_CHECK(r.title_bar_bg_inactive == p.bg_inactive);
    AURORA_TEST_CHECK(r.title_bar_fg_active == p.fg_active);
    AURORA_TEST_CHECK(r.title_bar_fg_inactive == p.fg_inactive);
    AURORA_TEST_CHECK(r.title_bar_hover_tint == p.hover_tint);
    AURORA_TEST_CHECK(r.title_bar_pressed_tint == p.pressed_tint);
    AURORA_TEST_CHECK(r.title_bar_close_hover == p.close_hover);
}

AURORA_TEST_CASE(csd_roles_dark_matches_adwaita_dark_preset) {
    const CsdRoles r = CsdRoles::dark();
    const TitleBarStyle p = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
    AURORA_TEST_CHECK(r.title_bar_bg_active == p.bg_active);
    AURORA_TEST_CHECK(r.title_bar_bg_inactive == p.bg_inactive);
    AURORA_TEST_CHECK(r.title_bar_fg_active == p.fg_active);
    AURORA_TEST_CHECK(r.title_bar_fg_inactive == p.fg_inactive);
    AURORA_TEST_CHECK(r.title_bar_hover_tint == p.hover_tint);
    AURORA_TEST_CHECK(r.title_bar_pressed_tint == p.pressed_tint);
    AURORA_TEST_CHECK(r.title_bar_close_hover == p.close_hover);
}

AURORA_TEST_CASE(csd_shadow_color_cross_scheme_shared) {
    // 投影基色跨明暗共享（沿用既有基线 alpha 70）。
    AURORA_TEST_CHECK(CsdRoles::light().shadow_color == Color{0, 0, 0, 70});
    AURORA_TEST_CHECK(CsdRoles::dark().shadow_color == CsdRoles::light().shadow_color);
}

}  // namespace aurora::test_cases::utest_theme_csd_roles

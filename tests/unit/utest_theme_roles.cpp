/// 测试类型: unit
/// 目标单元: include/aurora/theming/color_roles.h
/// 测试说明: 覆盖色角色取值的两条不变量——(1) 每个 on_* 与其配对角色满足 WCAG AA ≥ 4.5:1；
/// (2) 已消费角色取现行主题基准值（观感不变），暗色 primary/on_primary 为 WCAG 修正对；
/// 并断言明暗两列取向相反（暗色表面族显著深于亮色）。

#include <cmath>

#include "aurora/theming/color_roles.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_roles {

namespace {

/// @brief 相对亮度（WCAG 2.x sRGB 线性化加权和）。
auto luminance(const Color &c) -> double {
    const auto lin = [](uint8_t v) {
        const double s = v / 255.0;
        return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * lin(c.r)) + (0.7152 * lin(c.g)) + (0.0722 * lin(c.b));
}

/// @brief 两色对比度（WCAG 定义）。
auto contrast(const Color &a, const Color &b) -> double {
    const double la = luminance(a);
    const double lb = luminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

}  // namespace

AURORA_TEST_CASE(light_roles_preserve_consumed_baseline) {
    // 已消费角色取现行 Theme::light() 基准值，保证机械迁移后观感逐位不变。
    const ColorRoles r = ColorRoles::light();
    AURORA_TEST_CHECK(r.primary == Color{0x00, 0x00, 0xFF});  // 承接旧 primary = Color::blue()
    AURORA_TEST_CHECK(r.on_primary == Color::white());  // 承接旧 on_primary
    AURORA_TEST_CHECK(r.surface == Color::white());  // 承接旧 background（白）
    AURORA_TEST_CHECK(r.on_surface == Color::black());  // 承接旧 text（黑）
    AURORA_TEST_CHECK(r.focus_ring == Color::black());  // 承接旧 focus.ring（亮档纯黑）
    AURORA_TEST_CHECK(r.hover == Color{0, 0, 0, 32});  // 承接 hover 令牌
    AURORA_TEST_CHECK(r.pressed == Color{0, 0, 0, 56});  // 承接 pressed 令牌
    AURORA_TEST_CHECK(r.error == Color{0xE0, 0x1B, 0x24});  // 承接 danger 令牌
}

AURORA_TEST_CASE(dark_roles_preserve_consumed_baseline_except_wcag_fix) {
    // 暗色 surface 取旧 background 暗值（唯一消费者 command_palette 观感不变）；
    // on_surface / focus_ring 取现行；primary / on_primary 为唯一有意的 WCAG 修正对。
    const ColorRoles r = ColorRoles::dark();
    AURORA_TEST_CHECK(r.surface == Color{0x20, 0x21, 0x24});
    AURORA_TEST_CHECK(r.on_surface == Color::white());
    AURORA_TEST_CHECK(r.focus_ring == Color::white());
    AURORA_TEST_CHECK(r.hover == Color{255, 255, 255, 32});
    AURORA_TEST_CHECK(r.pressed == Color{255, 255, 255, 56});
    AURORA_TEST_CHECK(r.primary == Color{0xC0, 0xC1, 0xFF});  // tone-80 提亮
    AURORA_TEST_CHECK(r.on_primary == Color{0x00, 0x2B, 0x73});  // tone-20 深色
}

AURORA_TEST_CASE(all_on_color_pairs_meet_wcag_aa) {
    // 每个 on_* 与其配对角色对比度 ≥ 4.5:1（明暗两列全集）。
    for (const ColorRoles r : {ColorRoles::light(), ColorRoles::dark()}) {
        AURORA_TEST_CHECK(contrast(r.on_primary, r.primary) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_primary_container, r.primary_container) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_secondary, r.secondary) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_secondary_container, r.secondary_container) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_tertiary, r.tertiary) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_tertiary_container, r.tertiary_container) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_error, r.error) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_error_container, r.error_container) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_surface, r.surface) >= 4.5);
        AURORA_TEST_CHECK(contrast(r.on_surface_variant, r.surface_variant) >= 4.5);
    }
}

AURORA_TEST_CASE(dark_surface_family_darker_than_light) {
    // 明暗取向相反：暗色表面族整体亮度显著低于亮色。
    const ColorRoles l = ColorRoles::light();
    const ColorRoles d = ColorRoles::dark();
    AURORA_TEST_CHECK(luminance(d.surface) < luminance(l.surface));
    AURORA_TEST_CHECK(luminance(d.surface_container) < luminance(l.surface_container));
    AURORA_TEST_CHECK(luminance(d.on_surface) > luminance(l.on_surface));
}

}  // namespace aurora::test_cases::utest_theme_roles

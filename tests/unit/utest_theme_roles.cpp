/// 测试类型: unit
/// 目标单元: include/aurora/theming/color_roles.h
/// 测试说明: 覆盖色角色取值的两条不变量——(1) 每个 on_* 与其配对角色满足 WCAG AA ≥ 4.5:1，
/// 断言面从 Material 两列扩到五风格 × 明暗共 10 组（不达者按官方配对记豁免并钉住实测下限）；
/// (2) 已消费角色取现行主题基准值（观感不变），暗色 primary/on_primary 为 WCAG 修正对；
/// 并断言明暗两列取向相反（暗色表面族显著深于亮色）。

#include <string_view>

#include "aurora/theming/color_roles.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
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
    constexpr ColorRoles r = ColorRoles::light();
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
    constexpr ColorRoles r = ColorRoles::dark();
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
    for (const ColorRoles &r : {ColorRoles::light(), ColorRoles::dark()}) {
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
    constexpr ColorRoles l = ColorRoles::light();
    constexpr ColorRoles d = ColorRoles::dark();
    AURORA_TEST_CHECK(luminance(d.surface) < luminance(l.surface));
    AURORA_TEST_CHECK(luminance(d.surface_container) < luminance(l.surface_container));
    AURORA_TEST_CHECK(luminance(d.on_surface) > luminance(l.on_surface));
}

namespace {

/// @brief 十组标准配对（`on_*` 与其容器底色），成员指针让表格只写名不写取值。
struct Pair {
    const char *name;  ///< 配对名（豁免表与失败定位共用；输出须 ASCII）。
    Color ColorRoles::*fg;  ///< 前景角色。
    Color ColorRoles::*bg;  ///< 底色角色。
};

constexpr Pair AURORA_PAIRS[] = {
    {.name = "on_primary/primary", .fg = &ColorRoles::on_primary, .bg = &ColorRoles::primary},
    {.name = "on_primary_container/primary_container",
     .fg = &ColorRoles::on_primary_container,
     .bg = &ColorRoles::primary_container},
    {.name = "on_secondary/secondary", .fg = &ColorRoles::on_secondary, .bg = &ColorRoles::secondary},
    {.name = "on_secondary_container/secondary_container",
     .fg = &ColorRoles::on_secondary_container,
     .bg = &ColorRoles::secondary_container},
    {.name = "on_tertiary/tertiary", .fg = &ColorRoles::on_tertiary, .bg = &ColorRoles::tertiary},
    {.name = "on_tertiary_container/tertiary_container",
     .fg = &ColorRoles::on_tertiary_container,
     .bg = &ColorRoles::tertiary_container},
    {.name = "on_error/error", .fg = &ColorRoles::on_error, .bg = &ColorRoles::error},
    {.name = "on_error_container/error_container",
     .fg = &ColorRoles::on_error_container,
     .bg = &ColorRoles::error_container},
    {.name = "on_surface/surface", .fg = &ColorRoles::on_surface, .bg = &ColorRoles::surface},
    {.name = "on_surface_variant/surface_variant",
     .fg = &ColorRoles::on_surface_variant,
     .bg = &ColorRoles::surface_variant},
};

/// @brief 不达 AA 的配对豁免（照抄上游官方配对的那几支）。
struct Exemption {
    ThemeStyle style;  ///< 风格档。
    ColorScheme scheme;  ///< 明暗档。
    const char *pair;  ///< 配对名（须与 `AURORA_PAIRS` 里的 name 逐字相同）。
    double floor_ratio;  ///< 实测比值下限（钉住现状，取更差的改动即红灯；仍要求 < 4.5，豁免失效要显式撤销）。
};

constexpr Exemption AURORA_EXEMPTIONS[] = {
    // Aqua：controlAccentColor #007AFF 配白 4.02:1，达 AA Large 未达普通文本线；HIG 的 Blue 只有
    // 这一档，改深即是自造色（见 theme_presets.cpp 的 macOS 段）。
    {.style = ThemeStyle::MacOs, .scheme = ColorScheme::Light, .pair = "on_primary/primary", .floor_ratio = 4.0},
    {.style = ThemeStyle::MacOs, .scheme = ColorScheme::Dark, .pair = "on_primary/primary", .floor_ratio = 4.0},
    // Breeze：#3DAEE9 是 Plasma 6 明暗同值的官方强调色，其调色板没有达标深蓝档，选中态白字
    // 2.4x 是 KDE 的真实观感；#DA4453（ForegroundNegative）配白 4.26:1；ForegroundInactive
    // 是「弱化前景」专用色，任何官方底色上都到不了 4.5（见 KDE 段注释）。
    {.style = ThemeStyle::Kde, .scheme = ColorScheme::Light, .pair = "on_primary/primary", .floor_ratio = 2.4},
    {.style = ThemeStyle::Kde, .scheme = ColorScheme::Dark, .pair = "on_primary/primary", .floor_ratio = 2.4},
    {.style = ThemeStyle::Kde, .scheme = ColorScheme::Light, .pair = "on_error/error", .floor_ratio = 4.2},
    {.style = ThemeStyle::Kde, .scheme = ColorScheme::Dark, .pair = "on_error/error", .floor_ratio = 4.2},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Light,
     .pair = "on_surface_variant/surface_variant",
     .floor_ratio = 3.2},
    {.style = ThemeStyle::Kde,
     .scheme = ColorScheme::Dark,
     .pair = "on_surface_variant/surface_variant",
     .floor_ratio = 3.8},
};

/// @brief 查豁免表；未列出者为「必须达 AA」。
auto find_exemption(ThemeStyle style, ColorScheme scheme, const char *pair) -> const Exemption * {
    for (const Exemption &e : AURORA_EXEMPTIONS) {
        if (e.style == style && e.scheme == scheme && std::string_view{e.pair} == pair) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace

AURORA_TEST_CASE(ten_style_combos_meet_wcag_aa) {
    // 判据第六条：五风格 × 明暗两档共 10 组角色，每对 on_* 与配对角色 ≥ 4.5:1，
    // 不达标者按官方配对照抄并记豁免（豁免同时断言「仍未达标」，取到达标值后要显式撤销条目）。
    for (const ThemeStyle style :
         {ThemeStyle::Material, ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome, ThemeStyle::Kde}) {
        for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
            const ColorRoles roles = Theme::from_style(style, scheme).roles;
            for (const Pair &p : AURORA_PAIRS) {
                const double ratio = contrast(roles.*(p.fg), roles.*(p.bg));
                const Exemption *ex = find_exemption(style, scheme, p.name);
                if (ex == nullptr) {
                    AURORA_TEST_CHECK(ratio >= 4.5);
                } else {
                    AURORA_TEST_CHECK(ratio >= ex->floor_ratio);
                    AURORA_TEST_CHECK(ratio < 4.5);
                }
            }
        }
    }
}

}  // namespace aurora::test_cases::utest_theme_roles

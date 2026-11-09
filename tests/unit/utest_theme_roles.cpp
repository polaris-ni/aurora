/// 测试类型: unit
/// 目标单元: include/aurora/theming/color_roles.h
/// 测试说明: 覆盖色角色取值的三条不变量——(1) 已消费角色取现行主题基准值（观感不变），
/// 暗色 primary / on_primary 为 WCAG 修正对；(2) 明暗两列取向相反（暗色表面族显著深于亮色）；
/// (3) 对比度判据不再自带第二份 WCAG 公式：用例调库内 aurora::contrast_ratio，并逐行读
/// include/aurora/theming/detail/contrast_audit.h 的登记表（与 check_theme_contrast 同一张表），按三档
/// （text 4.5:1 / non_text 3.0:1 / not_applicable 不判）裁决五风格 × 明暗共 10 列；
/// 命中预设豁免的格子同时断言「≥ 登记下限且 < 档位线」，取到达标值后必须显式删掉那行。

#include "aurora/core/color.h"
#include "aurora/theming/color_roles.h"
#include "aurora/theming/detail/contrast_audit.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_roles {

namespace {

/// @brief 配对角色成员指针的别名（登记表与豁免表靠它逐字比对同一色对）。
using RoleMember = Color ColorRoles::*;

/// @brief 档位下限；未登记档位返回负值，让调用处断言转红而不是静默放行。
auto tier_floor(detail::ContrastTier tier) -> double {
    for (const detail::ContrastTierFloor &f : detail::AURORA_CONTRAST_TIERS) {
        if (f.tier == tier) {
            return f.min_ratio;
        }
    }
    return -1.0;
}

/// @brief 查该列该色对的预设豁免行；未列出者必须达到档位线。
auto find_preset_floor(ThemeStyle style, ColorScheme scheme, RoleMember fg, RoleMember bg)
    -> const detail::ContrastPresetFloor * {
    for (const detail::ContrastPresetFloor &e : detail::AURORA_CONTRAST_PRESET_FLOORS) {
        if (e.style == style && e.scheme == scheme && e.fg == fg && e.bg == bg) {
            return &e;
        }
    }
    return nullptr;
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

AURORA_TEST_CASE(tier_floors_are_the_wcag_numbers) {
    // 档位线钉在判据的外部真值上（SC 1.4.3 / SC 1.4.11），登记表不得自带一套数字；
    // 重谈档位属规格变更，须与 specification/07-environment-modifier.md §5.1 同批改。
    AURORA_TEST_CHECK(tier_floor(detail::ContrastTier::Text) == 4.5);
    AURORA_TEST_CHECK(tier_floor(detail::ContrastTier::NonText) == 3.0);
    AURORA_TEST_CHECK(tier_floor(detail::ContrastTier::NotApplicable) == 0.0);
    for (const detail::ContrastTierFloor &f : detail::AURORA_CONTRAST_TIERS) {
        AURORA_TEST_CHECK(!f.criterion.empty());
    }
}

AURORA_TEST_CASE(dark_surface_family_darker_than_light) {
    // 明暗取向相反：暗色表面族整体亮度显著低于亮色。亮度取库内唯一实现。
    constexpr ColorRoles l = ColorRoles::light();
    constexpr ColorRoles d = ColorRoles::dark();
    AURORA_TEST_CHECK(relative_luminance(d.surface) < relative_luminance(l.surface));
    AURORA_TEST_CHECK(relative_luminance(d.surface_container) < relative_luminance(l.surface_container));
    AURORA_TEST_CHECK(relative_luminance(d.on_surface) > relative_luminance(l.on_surface));
}

AURORA_TEST_CASE(ten_style_columns_judged_by_shared_registry) {
    // 判据主体：10 列 × 登记表每一对，按各自档位裁决。公式取库内实现、配对与档位取登记表——
    // 本用例与 check_theme_contrast 读同一张表，两侧不再各写一份 WCAG。
    using aurora::detail::ContrastTier;
    for (const ThemeStyle style :
         {ThemeStyle::Material, ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome, ThemeStyle::Kde}) {
        for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
            const ColorRoles roles = Theme::from_style(style, scheme).roles;
            for (const detail::ContrastPairAudit &row : detail::AURORA_CONTRAST_AUDITS) {
                // 逐条登记理由：不适用档靠理由说明「为什么跳过」，空理由即判据失效。
                AURORA_TEST_CHECK(!row.reason.empty());
                const double min_ratio = tier_floor(row.tier);
                if (row.tier == ContrastTier::NotApplicable) {
                    continue;
                }
                const double ratio = contrast_ratio(roles.*(row.fg), roles.*(row.bg));
                const detail::ContrastPresetFloor *floor = find_preset_floor(style, scheme, row.fg, row.bg);
                if (floor == nullptr) {
                    AURORA_TEST_CHECK(ratio >= min_ratio);
                } else {
                    AURORA_TEST_CHECK(ratio >= floor->floor_ratio);
                    AURORA_TEST_CHECK(ratio < min_ratio);  // 已达档位线 => 该豁免行必须显式删除
                }
            }
        }
    }
}

AURORA_TEST_CASE(preset_floors_point_at_registered_pairs) {
    // 豁免表不得指向未登记的色对，也不得在档位线之上占位——这是「豁免失效要显式撤销」的另一半。
    for (const detail::ContrastPresetFloor &floor : detail::AURORA_CONTRAST_PRESET_FLOORS) {
        AURORA_TEST_CHECK(!floor.reason.empty());
        double min_ratio = -1.0;
        for (const detail::ContrastPairAudit &row : detail::AURORA_CONTRAST_AUDITS) {
            if (row.fg == floor.fg && row.bg == floor.bg) {
                min_ratio = tier_floor(row.tier);
            }
        }
        AURORA_TEST_CHECK(min_ratio > 0.0);
        AURORA_TEST_CHECK(floor.floor_ratio < min_ratio);
    }
}

}  // namespace aurora::test_cases::utest_theme_roles

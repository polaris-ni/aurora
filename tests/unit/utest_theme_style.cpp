/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_style.h
/// 测试说明: 覆盖两轴正交模型的枚举面——ThemeStyle 取值域恒五档且不含明暗位、DesktopEnvironment
/// 四态齐备、两者与 Theme 的挂载关系（默认档 = Material）。色角色与令牌取值由
/// utest_theme_tokens / utest_theme_roles 覆盖；风格预设与工厂由 utest_theme_presets 覆盖。

#include <cstddef>

#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_style {

namespace {
/// @brief 风格轴的全部档位（顺序即枚举声明序）。
constexpr ThemeStyle AURORA_ALL_STYLES[] = {
    ThemeStyle::Material, ThemeStyle::Windows, ThemeStyle::MacOs, ThemeStyle::Gnome, ThemeStyle::Kde,
};
}  // namespace

AURORA_TEST_CASE(theme_style_domain_is_five_orthogonal_to_scheme) {
    // 五档且取值连续，供代码生成与枚举登记表逐字对齐。
    AURORA_TEST_CHECK(static_cast<int>(ThemeStyle::Material) == 0);
    AURORA_TEST_CHECK(static_cast<int>(ThemeStyle::Kde) == 4);
    AURORA_TEST_CHECK(std::size(AURORA_ALL_STYLES) == 5U);
    // 「逐档枚举值 == 其声明序号」一条断言同时钉住两件事：序号连续（无第六枚取值）与五档互异
    // （严格递增蕴含两两不等）。故不再另写两两比对循环——那只是同一命题的重复表述。
    // 由此也不存在「某风格的暗色档」这类第六枚取值：明暗位由 ColorScheme 独立承担。
    int ordinal = 0;
    for (const ThemeStyle style : AURORA_ALL_STYLES) {
        AURORA_TEST_CHECK(static_cast<int>(style) == ordinal++);
    }
}

AURORA_TEST_CASE(desktop_environment_four_states) {
    AURORA_TEST_CHECK(static_cast<int>(DesktopEnvironment::Unknown) == 0);
    AURORA_TEST_CHECK(static_cast<int>(DesktopEnvironment::Gnome) == 1);
    AURORA_TEST_CHECK(static_cast<int>(DesktopEnvironment::Kde) == 2);
    AURORA_TEST_CHECK(static_cast<int>(DesktopEnvironment::Other) == 3);
    // 「有证据但不认识」与「什么也没说」必须可区分：Unknown 与 Other 不得同值。
    AURORA_TEST_CHECK(DesktopEnvironment::Unknown != DesktopEnvironment::Other);
}

AURORA_TEST_CASE(theme_default_style_is_material_baseline) {
    // 未显式给风格时，值语义聚合落在 Material 基线（无证据环境的回落档）。
    const Theme theme;
    AURORA_TEST_CHECK(theme.style == ThemeStyle::Material);
    AURORA_TEST_CHECK(Theme::light().style == ThemeStyle::Material);
    AURORA_TEST_CHECK(Theme::dark().style == ThemeStyle::Material);
    AURORA_TEST_CHECK(Theme::with_defaults().style == ThemeStyle::Material);
    // 风格与明暗各持一枚字段，互不折平：同一风格可同时是明或暗。
    AURORA_TEST_CHECK(Theme::light().style == Theme::dark().style);
    AURORA_TEST_CHECK(Theme::light().scheme != Theme::dark().scheme);
}

}  // namespace aurora::test_cases::utest_theme_style

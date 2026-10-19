/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme.h
/// 测试说明: 覆盖强类型主题聚合——默认构造即 light 基准列（scheme 不表态）、light()/dark() 工厂置列与档、
/// with_defaults 与默认构造一致、随 ThemeExtension 的类型级扩展转发、以及作为值类型经扩展注册表深拷贝的
/// 独立性。（色角色取值与 WCAG 由 utest_theme_roles 覆盖；CSD 派生由 utest_theme_csd_bridge 覆盖。）

#include "aurora/core/font.h"
#include "aurora/theming/theme.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme {

namespace {
/// @brief 测试用扩展（模拟应用自定义令牌面）。
struct Brand {
    int level = 0;  ///< 强度档位。
};
}  // namespace

AURORA_TEST_CASE(default_theme_is_light_baseline) {
    // 裸主题不再是「无色空壳」：字段声明默认值即 light 基准列，scheme 保持不表态。
    const Theme theme;
    AURORA_TEST_CHECK(theme.roles.surface == ColorRoles::light().surface);
    AURORA_TEST_CHECK(theme.csd == CsdRoles::light());
    AURORA_TEST_CHECK(theme.scheme == ColorScheme::Unspecified);
    AURORA_TEST_CHECK(theme.type_scale.body_medium == Font{});
}

AURORA_TEST_CASE(light_and_dark_factories_set_column_and_scheme) {
    const Theme light = Theme::light();
    AURORA_TEST_CHECK(light.scheme == ColorScheme::Light);
    AURORA_TEST_CHECK(light.roles.primary == Color{0x00, 0x00, 0xFF});
    AURORA_TEST_CHECK(light.roles.focus_ring == Color::black());

    const Theme dark = Theme::dark();
    AURORA_TEST_CHECK(dark.scheme == ColorScheme::Dark);
    // 暗色 surface 取旧 background 暗值（唯一消费者观感不变）；primary 为 WCAG 修正对。
    AURORA_TEST_CHECK(dark.roles.surface == Color{0x20, 0x21, 0x24});
    AURORA_TEST_CHECK(dark.roles.primary == Color{0xC0, 0xC1, 0xFF});
    AURORA_TEST_CHECK(dark.roles.focus_ring == Color::white());
    // 焦点环必须与主色拉开，否则环会被读成控件自带的一圈边框（见 Widget::paint_content）。
    AURORA_TEST_CHECK(dark.roles.focus_ring != dark.roles.primary);
    AURORA_TEST_CHECK(light.roles.focus_ring != light.roles.primary);
}

AURORA_TEST_CASE(with_defaults_matches_default_construct) {
    const Theme root = Theme::with_defaults();
    const Theme base;
    AURORA_TEST_CHECK(root.roles.surface == base.roles.surface);
    AURORA_TEST_CHECK(root.csd == base.csd);
    AURORA_TEST_CHECK(root.scheme == base.scheme);
}

AURORA_TEST_CASE(theme_extension_forwarding_set_get_remove) {
    Theme theme;
    AURORA_TEST_CHECK(theme.extension<Brand>() == nullptr);
    theme.set_extension(Brand{7});
    const Brand *got = theme.extension<Brand>();
    AURORA_TEST_REQUIRE(got != nullptr);
    AURORA_TEST_CHECK_EQ(got->level, 7);
    AURORA_TEST_CHECK(theme.remove_extension<Brand>());
    AURORA_TEST_CHECK(theme.extension<Brand>() == nullptr);
}

AURORA_TEST_CASE(theme_copy_is_deep_value_including_extensions) {
    // 主题经 Provider 注入子树，拷贝语义必须是深值（扩展注册表独立）。
    Theme original;
    original.set_extension(Brand{1});

    Theme copy = original;
    copy.set_extension(Brand{99});  // 改副本不影响源

    const Brand *src = original.extension<Brand>();
    const Brand *dst = copy.extension<Brand>();
    AURORA_TEST_REQUIRE(src != nullptr);
    AURORA_TEST_REQUIRE(dst != nullptr);
    AURORA_TEST_CHECK_EQ(src->level, 1);
    AURORA_TEST_CHECK_EQ(dst->level, 99);
}

}  // namespace aurora::test_cases::utest_theme

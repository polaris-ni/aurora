/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme.h
/// 测试说明: 覆盖强类型主题聚合——默认构造即 light 基准列（scheme 不表态）、light()/dark() 工厂置列与档、
/// with_defaults 与默认构造一致、随 ThemeExtension 的类型级扩展转发、作为值类型经扩展注册表深拷贝的
/// 独立性，以及 `operator==` 的逐字段语义（度量令牌各组与 style / scheme 同参与；扩展按类型键
/// 逐条判等，自身无 `operator==` 的扩展恒判不等——保守方向，宁可多标一次脏也不放过真实变更）。
/// （色角色取值与 WCAG 由 utest_theme_roles 覆盖；度量档位与令牌对照表由 utest_theme_scale_tokens 覆盖；
/// CSD 派生由 utest_theme_csd_bridge 覆盖。）

#include "aurora/core/font.h"
#include "aurora/theming/theme.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme {

namespace {
/// @brief 测试用扩展（模拟应用自定义令牌面）。
struct Brand {
    int level = 0;  ///< 强度档位。
};

/// @brief 可判等的测试用扩展（验证扩展注册表逐条比对的是值而非「有没有这一项」）。
struct Seal {
    int code = 0;  ///< 印章编号。

    /// @brief 逐字段相等比较。
    /// @param other 比较对象。
    /// @return 编号相等时为 true。
    [[nodiscard]] auto operator==(const Seal &other) const -> bool = default;
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

AURORA_TEST_CASE(theme_equality_compares_every_field_including_measure_scales) {
    // 换肤 / 覆写物化的「同值守卫」全靠这一枚 ==：任何一字段单独变化都必须判为「变了」。
    const Theme base;
    AURORA_TEST_CHECK(base == Theme{});  // 同值即相等（空扩展表两项恒等）
    AURORA_TEST_CHECK(base == Theme::with_defaults());
    AURORA_TEST_CHECK(base != Theme::light());  // 唯一差异是 scheme——不表态与明色档不同值

    Theme roles = base;
    roles.roles.surface = Color{1, 2, 3, 4};
    AURORA_TEST_CHECK(base != roles);

    Theme type_scale = base;
    type_scale.type_scale.body_medium.size_pt = 15.0F;
    AURORA_TEST_CHECK(base != type_scale);

    Theme shapes = base;
    shapes.shapes.small += 1.0F;
    AURORA_TEST_CHECK(base != shapes);  // 度量令牌各组参与比较：只改圆角也判为变

    Theme spacing = base;
    spacing.spacing.small += 1.0F;
    AURORA_TEST_CHECK(base != spacing);

    Theme controls = base;
    controls.controls.indicator_size += 1.0F;
    AURORA_TEST_CHECK(base != controls);

    Theme motion = base;
    motion.motion.medium = Duration::from_ms(1.0);
    AURORA_TEST_CHECK(base != motion);

    Theme conventions = base;
    conventions.conventions.focus_ring_style = FocusRingStyle::OuterGlow;
    AURORA_TEST_CHECK(base != conventions);  // 惯例组的枚举位同样参与比较

    Theme style = base;
    style.style = ThemeStyle::Windows;
    AURORA_TEST_CHECK(base != style);  // 身份位单独变化也判为变

    Theme csd = base;
    csd.csd.title_bar_bg_active = Color{9, 9, 9, 9};
    AURORA_TEST_CHECK(base != csd);

    Theme scheme = base;
    scheme.scheme = ColorScheme::Dark;
    AURORA_TEST_CHECK(base != scheme);

    AURORA_TEST_CHECK(Theme::light() != Theme::dark());  // 明暗两档恒不等（roles/csd/scheme 三处皆变）
}

AURORA_TEST_CASE(theme_equality_checks_extensions_by_value_and_converts_to_conservative) {
    Theme plain;
    Theme seal_three_a = plain;
    seal_three_a.set_extension(Seal{3});
    Theme seal_three_b = plain;
    seal_three_b.set_extension(Seal{3});
    Theme seal_four = plain;
    seal_four.set_extension(Seal{4});

    AURORA_TEST_CHECK(plain != seal_three_a);  // 多一项即不等
    AURORA_TEST_CHECK(seal_three_a == seal_three_b);  // 同类型同值 → 相等（比的是值，不是「有没有注册」）
    AURORA_TEST_CHECK(seal_three_a != seal_four);  // 同类型不同值 → 不等

    // 无 operator== 的扩展类型：该项恒判不等，故比较结果只可能偏「变了」——同值守卫最坏退化成多标一次脏。
    Theme brand_a = plain;
    brand_a.set_extension(Brand{5});
    Theme brand_b = plain;
    brand_b.set_extension(Brand{5});
    AURORA_TEST_CHECK(brand_a != brand_b);
    AURORA_TEST_CHECK(brand_a != plain);
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

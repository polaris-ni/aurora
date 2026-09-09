/// 测试类型: unit
/// 目标单元: include/aurora/theming/style_props.h
/// 测试说明: 覆盖 TokenOr<T> 两态构造（具体值/令牌名）与 is_token/token_name/concrete 查询、
/// resolve 在「令牌命中 / 令牌缺失 / 类型不匹配 / 具体值直通」四路语义，
/// 以及 StyleProps 经 Theme 解析为 ResolvedStyle 的逐字段落值与默认回退

#include <optional>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/theming/style_props.h"
#include "aurora/theming/theme.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_style_props {

AURORA_TEST_CASE(token_or_defaults_to_concrete_value) {
    // 未设置样式 → 具体默认值，而非空令牌名（避免「未设置」被当成「查名为空的令牌」）。
    const TokenOr<Color> field;
    AURORA_TEST_CHECK_FALSE(field.is_token());
    AURORA_TEST_CHECK_FALSE(field.token_name().has_value());
    AURORA_TEST_REQUIRE_TRUE(field.concrete().has_value());
    AURORA_TEST_CHECK_EQ(field.concrete().value_or(Color{}), Color{});
}

AURORA_TEST_CASE(token_or_concrete_construction) {
    const TokenOr<Color> color{Color::red()};
    const TokenOr<double> radius{8.0};
    const TokenOr<Font> font{Font{.family = "Noto Sans", .size_pt = 16.0F}};

    AURORA_TEST_CHECK_FALSE(color.is_token());
    AURORA_TEST_CHECK_FALSE(radius.is_token());
    AURORA_TEST_CHECK_FALSE(font.is_token());

    AURORA_TEST_CHECK_EQ(color.concrete().value_or(Color{}), Color::red());
    AURORA_TEST_CHECK_NEAR(radius.concrete().value_or(0.0), 8.0, 1e-9);
    AURORA_TEST_CHECK_EQ(font.concrete().value_or(Font{}).family, std::string{"Noto Sans"});
}

AURORA_TEST_CASE(token_or_token_name_construction) {
    const TokenOr<Color> from_literal{"color.primary"};
    const TokenOr<Color> from_string{std::string{"color.secondary"}};

    AURORA_TEST_CHECK_TRUE(from_literal.is_token());
    AURORA_TEST_CHECK_TRUE(from_string.is_token());
    AURORA_TEST_CHECK_EQ(from_literal.token_name().value_or(""), std::string{"color.primary"});
    AURORA_TEST_CHECK_EQ(from_string.token_name().value_or(""), std::string{"color.secondary"});
    // 令牌态下 concrete() 必为空：两态互斥。
    AURORA_TEST_CHECK_FALSE(from_literal.concrete().has_value());
}

AURORA_TEST_CASE(resolve_returns_token_value_when_present_and_typed) {
    Theme theme;
    theme.set_token("color.primary", TokenValue{Color::red()});
    theme.set_token("space.md", TokenValue{12.0});

    const TokenOr<Color> color{"color.primary"};
    const TokenOr<double> space{"space.md"};

    AURORA_TEST_CHECK_EQ(color.resolve(theme, Color::blue()), Color::red());
    AURORA_TEST_CHECK_NEAR(space.resolve(theme, 0.0), 12.0, 1e-9);
}

AURORA_TEST_CASE(resolve_falls_back_when_token_missing) {
    const Theme theme;  // 空令牌表
    const TokenOr<Color> color{"color.missing"};
    AURORA_TEST_CHECK_EQ(color.resolve(theme, Color::blue()), Color::blue());
}

AURORA_TEST_CASE(resolve_falls_back_on_token_type_mismatch) {
    // 令牌存在但类型不匹配 → 回退，不抛异常、不做隐式转换。
    Theme theme;
    theme.set_token("space.md", TokenValue{12.0});

    const TokenOr<Color> mismatched{"space.md"};
    AURORA_TEST_CHECK_EQ(mismatched.resolve(theme, Color::green()), Color::green());
}

AURORA_TEST_CASE(resolve_passthrough_for_concrete_value) {
    // 具体值态下 resolve 不查主题：即使主题里有同名令牌也以具体值为准。
    Theme theme;
    theme.set_token("color.primary", TokenValue{Color::blue()});

    const TokenOr<Color> concrete{Color::red()};
    AURORA_TEST_CHECK_EQ(concrete.resolve(theme, Color::green()), Color::red());
}

AURORA_TEST_CASE(style_props_default_resolve_yields_zeroed_style) {
    // 全默认 StyleProps 解析：颜色为 Color{}、字体为 Font{}、尺寸为 0。
    const StyleProps props;
    const ResolvedStyle resolved = props.resolve(Theme::with_defaults());

    AURORA_TEST_CHECK_EQ(resolved.background, Color{});
    AURORA_TEST_CHECK_EQ(resolved.foreground, Color{});
    AURORA_TEST_CHECK_EQ(resolved.font, Font{});
    AURORA_TEST_CHECK_NEAR(resolved.corner_radius, 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(resolved.padding, 0.0, 1e-9);
}

AURORA_TEST_CASE(style_props_resolves_mixed_token_and_concrete_fields) {
    Theme theme;
    theme.set_token("color.bg", TokenValue{Color::from_rgba(10, 20, 30)});
    theme.set_token("space.lg", TokenValue{24.0});
    theme.set_token("font.body", TokenValue{Font{.family = "Noto Sans", .size_pt = 15.0F}});

    StyleProps props;
    props.background = TokenOr<Color>{"color.bg"};
    props.foreground = TokenOr<Color>{Color::white()};  // 具体值
    props.font = TokenOr<Font>{"font.body"};
    props.corner_radius = TokenOr<double>{"space.lg"};
    props.padding = TokenOr<double>{"space.missing"};  // 缺失 → 0

    const ResolvedStyle resolved = props.resolve(theme);
    AURORA_TEST_CHECK_EQ(resolved.background, Color::from_rgba(10, 20, 30));
    AURORA_TEST_CHECK_EQ(resolved.foreground, Color::white());
    AURORA_TEST_CHECK_EQ(resolved.font.family, std::string{"Noto Sans"});
    AURORA_TEST_CHECK_NEAR(resolved.corner_radius, 24.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(resolved.padding, 0.0, 1e-9);
}

AURORA_TEST_CASE(style_props_tracks_theme_token_change) {
    // 同一 StyleProps 在不同主题下解析出不同结果（换肤正确性的最小契约）。
    Theme light;
    light.set_token("color.bg", TokenValue{Color::white()});
    Theme dark;
    dark.set_token("color.bg", TokenValue{Color::black()});

    const StyleProps props{.background = TokenOr<Color>{"color.bg"}};
    AURORA_TEST_CHECK_EQ(props.resolve(light).background, Color::white());
    AURORA_TEST_CHECK_EQ(props.resolve(dark).background, Color::black());
}

}  // namespace aurora::test_cases::utest_style_props

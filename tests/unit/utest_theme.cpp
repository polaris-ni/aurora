/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme.h
/// 测试说明: 覆盖 TokenValue 三态承载与 is/as 判定、Theme 扁平字段默认契约、light/dark/with_defaults 工厂、
/// 命名令牌的登记/查询/覆盖、token_or 的类型匹配与类型不匹配回退

#include <optional>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/theming/theme.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme {

AURORA_TEST_CASE(token_value_defaults_to_color_black) {
    // 默认构造落在 variant 首选项（Color），其值为 Color{}。
    const TokenValue token;
    AURORA_TEST_CHECK_TRUE(token.is<Color>());
    AURORA_TEST_CHECK_FALSE(token.is<Font>());
    AURORA_TEST_CHECK_FALSE(token.is<double>());
    AURORA_TEST_CHECK_EQ(token.as<Color>().value_or(Color::red()), Color{});
}

AURORA_TEST_CASE(token_value_carries_each_alternative) {
    const TokenValue as_color{Color::red()};
    const TokenValue as_font{Font{.family = "Noto Sans", .size_pt = 18.0F}};
    const TokenValue as_dimension{12.5};

    AURORA_TEST_CHECK_TRUE(as_color.is<Color>());
    AURORA_TEST_CHECK_TRUE(as_font.is<Font>());
    AURORA_TEST_CHECK_TRUE(as_dimension.is<double>());

    AURORA_TEST_CHECK_EQ(as_color.as<Color>().value_or(Color{}), Color::red());
    AURORA_TEST_CHECK_EQ(as_font.as<Font>().value_or(Font{}).family, std::string{"Noto Sans"});
    AURORA_TEST_CHECK_NEAR(as_dimension.as<double>().value_or(0.0), 12.5, 1e-9);
}

AURORA_TEST_CASE(token_value_as_returns_nullopt_on_type_mismatch) {
    // 类型不匹配返回 nullopt 而非抛异常：解析层据此回退 fallback。
    const TokenValue token{Color::blue()};
    AURORA_TEST_CHECK_FALSE(token.as<Font>().has_value());
    AURORA_TEST_CHECK_FALSE(token.as<double>().has_value());
    AURORA_TEST_CHECK_TRUE(token.as<Color>().has_value());
}

AURORA_TEST_CASE(theme_default_flat_fields) {
    const Theme theme;
    AURORA_TEST_CHECK_EQ(theme.background, Color::white());
    AURORA_TEST_CHECK_EQ(theme.primary, Color::blue());
    AURORA_TEST_CHECK_EQ(theme.on_primary, Color::white());
    AURORA_TEST_CHECK_EQ(theme.text, Color::black());
    AURORA_TEST_CHECK_EQ(theme.font, Font{});
    AURORA_TEST_CHECK_TRUE(theme.tokens.empty());
}

AURORA_TEST_CASE(theme_light_is_default_and_dark_overrides) {
    // light() 等价于默认构造；dark() 只改背景/主色/文本三色。
    AURORA_TEST_CHECK_EQ(Theme::light().background, Theme{}.background);
    AURORA_TEST_CHECK_EQ(Theme::light().primary, Theme{}.primary);

    const Theme dark = Theme::dark();
    AURORA_TEST_CHECK_EQ(dark.background, Color::from_rgba(32, 33, 36));
    AURORA_TEST_CHECK_EQ(dark.primary, Color::from_rgba(90, 120, 240));
    AURORA_TEST_CHECK_EQ(dark.text, Color::white());
    AURORA_TEST_CHECK_NE(dark.background, Theme::light().background);
}

AURORA_TEST_CASE(theme_with_defaults_is_merge_root) {
    // with_defaults 与默认构造逐字段一致，作为 resolve_theme 的兜底根主题。
    const Theme root = Theme::with_defaults();
    const Theme base;
    AURORA_TEST_CHECK_EQ(root.background, base.background);
    AURORA_TEST_CHECK_EQ(root.primary, base.primary);
    AURORA_TEST_CHECK_EQ(root.on_primary, base.on_primary);
    AURORA_TEST_CHECK_EQ(root.text, base.text);
    AURORA_TEST_CHECK_EQ(root.font, base.font);
}

AURORA_TEST_CASE(theme_token_roundtrip_and_overwrite) {
    Theme theme;
    AURORA_TEST_CHECK_FALSE(theme.token("color.brand").has_value());

    theme.set_token("color.brand", TokenValue{Color::red()});
    const auto first = theme.token("color.brand");
    AURORA_TEST_REQUIRE_TRUE(first.has_value());
    AURORA_TEST_CHECK_EQ(first.value_or(TokenValue{}).as<Color>().value_or(Color{}), Color::red());

    // 同名覆盖：后写胜出，令牌数不增长。
    theme.set_token("color.brand", TokenValue{Color::blue()});
    AURORA_TEST_CHECK_EQ(theme.tokens.size(), 1U);
    AURORA_TEST_CHECK_EQ(theme.token_or<Color>("color.brand", Color{}), Color::blue());
}

AURORA_TEST_CASE(theme_token_or_matches_type_or_falls_back) {
    Theme theme;
    theme.set_token("space.md", TokenValue{8.0});
    theme.set_token("color.brand", TokenValue{Color::red()});

    AURORA_TEST_CHECK_NEAR(theme.token_or<double>("space.md", 0.0), 8.0, 1e-9);
    AURORA_TEST_CHECK_EQ(theme.token_or<Color>("color.brand", Color{}), Color::red());

    // 缺失令牌 → fallback。
    AURORA_TEST_CHECK_NEAR(theme.token_or<double>("space.missing", 4.0), 4.0, 1e-9);
    // 类型不匹配（space.md 是 double，按 Color 取）→ fallback。
    AURORA_TEST_CHECK_EQ(theme.token_or<Color>("space.md", Color::green()), Color::green());
}

AURORA_TEST_CASE(theme_is_copyable_value_type) {
    // 主题经 Provider 注入子树，拷贝语义必须是深值（令牌表独立）。
    Theme original;
    original.set_token("color.brand", TokenValue{Color::red()});

    Theme copy = original;
    copy.set_token("color.brand", TokenValue{Color::blue()});
    copy.set_token("color.extra", TokenValue{Color::green()});

    AURORA_TEST_CHECK_EQ(original.token_or<Color>("color.brand", Color{}), Color::red());
    AURORA_TEST_CHECK_FALSE(original.token("color.extra").has_value());
    AURORA_TEST_CHECK_EQ(copy.tokens.size(), 2U);
}

}  // namespace aurora::test_cases::utest_theme

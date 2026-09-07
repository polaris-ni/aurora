/// 测试类型: unit
/// 目标单元: include/aurora/theming/style_props.h
/// 测试说明: 两态样式字段 TokenOr（令牌名/具体值判定、resolve 命中/回退/类型不匹配）与 StyleProps.resolve 单元测试

#include "aurora/theming/style_props.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_style_props {

AURORA_TEST() {
    // ---- 1. 默认构造为「具体默认值」（T 的默认），而非令牌名 ----
    {
        const TokenOr<double> d;
        AURORA_TEST_CHECK_FALSE(d.is_token());
        AURORA_TEST_CHECK(d.concrete().has_value());
        AURORA_TEST_CHECK_EQ(d.concrete().value_or(-1.0), 0.0);
        AURORA_TEST_CHECK_FALSE(d.token_name().has_value());

        const TokenOr<Color> c;
        AURORA_TEST_CHECK_FALSE(c.is_token());
        AURORA_TEST_CHECK(c.concrete() == Color{});
    }

    // ---- 2. 具体值构造 ----
    {
        const TokenOr<Color> c{Color::red()};
        AURORA_TEST_CHECK_FALSE(c.is_token());
        AURORA_TEST_CHECK(c.concrete().value() == Color::red());
    }

    // ---- 3. 令牌名构造（const char* 与 std::string 两条便捷路径） ----
    {
        const TokenOr<Color> from_lit{"color.primary"};
        AURORA_TEST_CHECK(from_lit.is_token());
        AURORA_TEST_CHECK(from_lit.concrete() == std::nullopt);
        AURORA_TEST_CHECK(from_lit.token_name().value_or("?") == std::string_view("color.primary"));

        const TokenOr<double> from_str{std::string("spacing.md")};
        AURORA_TEST_CHECK(from_str.is_token());
        AURORA_TEST_CHECK(from_str.token_name().value_or("?") == std::string_view("spacing.md"));
    }

    // ---- 4. resolve：具体值直接返回，忽略主题 ----
    {
        const Theme theme = Theme::dark();
        const TokenOr<Color> c{Color::red()};
        AURORA_TEST_CHECK(c.resolve(theme, Color::blue()) == Color::red());
    }

    // ---- 5. resolve：令牌命中且类型匹配返回令牌值；类型不匹配或缺失回退 fallback ----
    {
        Theme theme;
        theme.set_token("c", TokenValue{Color::blue()});
        theme.set_token("r", TokenValue{8.0});

        const TokenOr<Color> tok_c{std::string("c")};
        AURORA_TEST_CHECK(tok_c.resolve(theme, Color::red()) == Color::blue());  // 命中

        const TokenOr<double> tok_r{std::string("r")};
        AURORA_TEST_CHECK_EQ(tok_r.resolve(theme, 0.0), 8.0);

        // 类型不匹配：把 double 令牌当 Color 解析 → 回退 fallback
        const Color fb_mismatch{1, 2, 3};
        const TokenOr<Color> wrong{std::string("r")};
        AURORA_TEST_CHECK(wrong.resolve(theme, fb_mismatch) == fb_mismatch);

        // 令牌缺失 → 回退 fallback
        const Color fb_missing{9, 9, 9};
        const TokenOr<Color> missing{std::string("nope")};
        AURORA_TEST_CHECK(missing.resolve(theme, fb_missing) == fb_missing);
    }

    // ---- 6. StyleProps.resolve：逐字段解析为 ResolvedStyle ----
    {
        Theme theme;
        theme.set_token("bg", TokenValue{Color::from_rgba(10, 20, 30)});

        StyleProps sp;
        sp.background = TokenOr<Color>{std::string("bg")};  // 令牌命中
        sp.foreground = TokenOr<Color>{Color::white()};  // 具体值
        sp.corner_radius = TokenOr<double>{4.0};  // 具体值
        sp.padding = TokenOr<double>{std::string("absent")};  // 令牌缺失 → fallback 0

        const ResolvedStyle r = sp.resolve(theme);
        AURORA_TEST_CHECK(r.background == Color::from_rgba(10, 20, 30));
        AURORA_TEST_CHECK(r.foreground == Color::white());
        AURORA_TEST_CHECK_EQ(r.corner_radius, 4.0);
        AURORA_TEST_CHECK_EQ(r.padding, 0.0);
    }
}

}  // namespace aurora::test_cases::utest_style_props

// T2 — 主题令牌（Design Tokens） + StyleProps 解析测试
//
// 覆盖：
//   - Theme.set_token / token() 命中与兜底
//   - token_or<T> 类型匹配 / 不匹配 / 缺失回退
//   - ThemeScope 注入后后代 token() 命中最近生效值（经 resolve_theme，无需挂载）
//   - StyleProps::resolve：具体值直接返回、令牌名解析、令牌缺失/类型不匹配回退

// ── API 覆盖映射 ─────────────────────────────
// theming/style_props.h(StyleProps 解析)、theming/theme.h(Theme.set_token/token_or)。

#include <memory>

#include "aurora/aurora.h"
#include "aurora/theming/theme_query.h"
#include "aurora/widget/text.h"
#include "test_harness.h"

using aurora::Color;
using aurora::Font;
using aurora::Node;
using aurora::StyleProps;
using aurora::Text;
using aurora::Theme;
using aurora::ThemeScope;
using aurora::TokenOr;
using aurora::Widget;

AURORA_TEST() {
    // ---- 1. Theme 令牌登记与查询 ----
    {
        Theme theme;
        theme.set_token("color.brand", Color::red());
        theme.set_token("font.body", Font{.family = "serif", .size_pt = 20});
        theme.set_token("space.md", 16.0);

        auto c = theme.token("color.brand");
        AURORA_TEST_REQUIRE_MSG(c.has_value(), "token hit should exist");
        AURORA_TEST_REQUIRE_MSG(c->as<Color>().has_value(), "color.brand should be Color type");
        AURORA_TEST_CHECK_EQ(c->as<Color>().value(), Color::red());

        auto miss = theme.token("does.not.exist");
        AURORA_TEST_CHECK_MSG(!miss.has_value(), "missing token should return nullopt");

        // token_or 类型匹配
        AURORA_TEST_CHECK_EQ(theme.token_or<Color>("color.brand", Color::white()), Color::red());
        AURORA_TEST_CHECK_EQ(theme.token_or<Font>("font.body", Font{}).family, std::string("serif"));
        AURORA_TEST_CHECK_NEAR(theme.token_or<double>("space.md", 0.0), 16.0, 1e-9);

        // token_or 类型不匹配 → 回退
        AURORA_TEST_CHECK_NEAR(theme.token_or<double>("color.brand", 9.0), 9.0, 1e-9);
        // token_or 缺失 → 回退
        AURORA_TEST_CHECK_EQ(theme.token_or<Color>("missing", Color::blue()), Color::blue());
    }

    // ---- 2. ThemeScope 注入后后代 token() 命中最近生效值 ----
    {
        Theme outer;
        outer.set_token("brand", Color::red());
        auto target = std::make_shared<Text>();

        Theme inner;
        inner.set_token("brand", Color::blue());  // 同 token 名，内层覆盖
        inner.set_token("accent", Color::green());
        auto scope_inner = std::make_shared<ThemeScope>(inner, Node{std::static_pointer_cast<Widget>(target)});
        auto scope_outer = std::make_shared<ThemeScope>(outer, Node{std::static_pointer_cast<Widget>(scope_inner)});

        Theme merged = resolve_theme(Node{std::static_pointer_cast<Widget>(scope_outer)}, *target);

        // 最近生效值：内层 brand 覆盖外层 brand
        auto brand = merged.token("brand");
        AURORA_TEST_REQUIRE_MSG(brand.has_value(), "descendants should inherit brand token");
        const auto brand_color = brand->as<Color>();
        AURORA_TEST_REQUIRE_MSG(brand_color.has_value(), "brand token should be Color");
        AURORA_TEST_CHECK_EQ(brand_color.value(), Color::blue());

        // 内层独有的令牌同样可见
        auto accent = merged.token("accent");
        AURORA_TEST_REQUIRE_MSG(accent.has_value(), "descendants should inherit inner accent token");
        const auto accent_color = accent->as<Color>();
        AURORA_TEST_REQUIRE_MSG(accent_color.has_value(), "accent token should be Color");
        AURORA_TEST_CHECK_EQ(accent_color.value(), Color::green());
    }

    // ---- 3. StyleProps 解析：具体值 / 令牌名 / 缺失 / 类型不匹配 ----
    {
        Theme theme;
        theme.set_token("color.brand", Color::red());
        theme.set_token("space.md", 16.0);
        theme.set_token("font.body", Font{.family = "serif", .size_pt = 20});

        // 3a. 字段填具体值 → 直接返回，不走令牌
        {
            StyleProps sp;
            sp.background = Color::green();
            sp.corner_radius = 8.0;
            sp.font = Font{.family = "mono"};
            auto r = sp.resolve(theme);
            AURORA_TEST_CHECK_EQ(r.background, Color::green());
            AURORA_TEST_CHECK_NEAR(r.corner_radius, 8.0, 1e-9);
            AURORA_TEST_CHECK_EQ(r.font.family, std::string("mono"));
            AURORA_TEST_CHECK_NEAR(r.padding, 0.0, 1e-9);  // 未设 → 默认
        }

        // 3b. 字段填令牌名 → 从 Theme 解析
        {
            StyleProps sp;
            sp.background = "color.brand";
            sp.padding = "space.md";
            sp.font = "font.body";
            auto r = sp.resolve(theme);
            AURORA_TEST_CHECK_EQ(r.background, Color::red());
            AURORA_TEST_CHECK_NEAR(r.padding, 16.0, 1e-9);
            AURORA_TEST_CHECK_EQ(r.font.family, std::string("serif"));
            AURORA_TEST_CHECK_NEAR(r.font.size_pt, 20.0F, 1e-3F);
        }

        // 3c. 令牌缺失 → 回退 fallback
        {
            StyleProps sp;
            sp.corner_radius = "missing.token";
            auto r = sp.resolve(theme);
            AURORA_TEST_CHECK_NEAR(r.corner_radius, 0.0, 1e-9);
        }

        // 3d. 令牌类型不匹配 → 回退 fallback（令牌为 Color，但字段期望 double）
        {
            StyleProps sp;
            sp.corner_radius = "color.brand";
            auto r = sp.resolve(theme);
            AURORA_TEST_CHECK_NEAR(r.corner_radius, 0.0, 1e-9);
        }
    }

    // ---- 4. TokenOr 两态字段自省 ----
    {
        TokenOr concrete = Color::red();
        AURORA_TEST_CHECK_FALSE(concrete.is_token());
        AURORA_TEST_CHECK(concrete.concrete().has_value());
        AURORA_TEST_CHECK_FALSE(concrete.token_name().has_value());

        TokenOr<Color> named("color.brand");
        AURORA_TEST_CHECK_TRUE(named.is_token());
        // 前置 CHECK 已记录失败，此处仅取值比对（is_token 为真即令牌名有值）。
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        AURORA_TEST_CHECK_EQ(named.token_name().value(), std::string("color.brand"));
        AURORA_TEST_CHECK_FALSE(named.concrete().has_value());

        // 默认构造为具体默认值（非空令牌名）
        TokenOr<Color> def;
        AURORA_TEST_CHECK_FALSE(def.is_token());
    }
}

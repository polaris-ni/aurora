/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_query.h
/// 测试说明: 覆盖 resolve_theme 在无注入时回退明色基线（与 inherit_theme 的兜底合一）、根处 ThemeScope 生效、
/// 嵌套 ThemeScope 的最近祖先优先、深层子树继承祖先主题，以及目标不在树中时的兜底行为；并单独钉住兜底档
/// 恒 ≡ Material 基线（不随风格档探测漂移）

#include <memory>
#include <string>

#include "aurora/core/color.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_query.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/node.h"
#include "aurora/widget/placeholder.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_query {

AURORA_TEST_CASE(resolve_theme_without_injection_returns_defaults) {
    // 树中无 ThemeProvider → 兜底明色基线 Theme::light()，与 inherit_theme 的无注入兜底合一
    // （旧偏差：本函数曾兜 Theme::with_defaults()，两枚兜底差在 scheme 位）。
    Node const root{Placeholder{}};
    const Theme resolved = resolve_theme(root);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::light().roles.surface);
    AURORA_TEST_CHECK_EQ(resolved.roles.primary, Theme::light().roles.primary);
    AURORA_TEST_CHECK_EQ(resolved.roles.on_surface, Theme::light().roles.on_surface);
    AURORA_TEST_CHECK_EQ(resolved.scheme, ColorScheme::Light);
}

AURORA_TEST_CASE(resolve_theme_fallback_is_material_baseline_not_platform_style) {
    // 三处兜底同源的查询侧：无 Provider 时逐组令牌全等于裸主题（Material 基线），身份位也不是探测档。
    // 若兜底被改成 `Theme::default_style()`，在非 Material 风格档的构建上本用例即红。
    Node const root{Placeholder{}};
    const Theme resolved = resolve_theme(root);
    AURORA_TEST_CHECK(resolved.style == ThemeStyle::Material);
    AURORA_TEST_CHECK(resolved.shapes == ShapeScale{});
    AURORA_TEST_CHECK(resolved.spacing == SpacingScale{});
    AURORA_TEST_CHECK(resolved.controls == ControlMetrics{});
    AURORA_TEST_CHECK(resolved.conventions == LayoutConventions{});
}

AURORA_TEST_CASE(resolve_theme_at_root_scope) {
    Node const root{ThemeScope{Theme::dark(), Placeholder{}}};
    const Theme resolved = resolve_theme(root);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::dark().roles.surface);
    AURORA_TEST_CHECK_EQ(resolved.roles.on_surface, Theme::dark().roles.on_surface);
}

AURORA_TEST_CASE(resolve_theme_prefers_nearest_ancestor_scope) {
    // 嵌套覆盖：内层 ThemeScope（dark）对子树生效，外层（light）被遮蔽。
    auto inner = std::make_shared<ThemeScope>(Theme::dark(), Placeholder{});
    Node const root{ThemeScope{Theme::light(), Node{inner}}};

    const Theme root_theme = resolve_theme(root);
    AURORA_TEST_CHECK_EQ(root_theme.roles.surface, Theme::light().roles.surface);

    const Theme inner_theme = resolve_theme(root, *inner);
    AURORA_TEST_CHECK_EQ(inner_theme.roles.surface, Theme::dark().roles.surface);
}

AURORA_TEST_CASE(resolve_theme_descendant_inherits_ancestor) {
    // 目标本身不是 Provider：沿 DFS 继承最近祖先注入的主题。
    auto leaf = std::make_shared<Placeholder>();
    Node const root{ThemeScope{Theme::dark(), Node{leaf}}};

    const Theme resolved = resolve_theme(root, *leaf);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::dark().roles.surface);
    AURORA_TEST_CHECK_EQ(resolved.roles.primary, Theme::dark().roles.primary);
}

AURORA_TEST_CASE(resolve_theme_target_outside_tree_falls_back) {
    // 目标不在树中：DFS 走完全树仍未命中，结果保持兜底明色基线（不崩溃、不返回脏值）。
    Node const root{ThemeScope{Theme::dark(), Placeholder{}}};
    const Placeholder outsider;
    const Theme resolved = resolve_theme(root, outsider);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::light().roles.surface);
    AURORA_TEST_CHECK(resolved == Theme::light());
}

}  // namespace aurora::test_cases::utest_theme_query

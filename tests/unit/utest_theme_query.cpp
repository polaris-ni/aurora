/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_query.h
/// 测试说明: 覆盖 resolve_theme 在无注入时回退默认主题、根处 ThemeScope 生效、嵌套 ThemeScope 的最近祖先优先、
/// 深层子树继承祖先主题，以及目标不在树中时的兜底行为

#include <memory>
#include <string>

#include "aurora/core/color.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_query.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/node.h"
#include "aurora/widget/placeholder.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_theme_query {

AURORA_TEST_CASE(resolve_theme_without_injection_returns_defaults) {
    // 树中无 ThemeProvider → 兜底根主题 Theme::with_defaults()。
    Node root{Placeholder{}};
    const Theme resolved = resolve_theme(root);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::with_defaults().roles.surface);
    AURORA_TEST_CHECK_EQ(resolved.roles.primary, Theme::with_defaults().roles.primary);
    AURORA_TEST_CHECK_EQ(resolved.roles.on_surface, Theme::with_defaults().roles.on_surface);
}

AURORA_TEST_CASE(resolve_theme_at_root_scope) {
    Node root{ThemeScope{Theme::dark(), Placeholder{}}};
    const Theme resolved = resolve_theme(root);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::dark().roles.surface);
    AURORA_TEST_CHECK_EQ(resolved.roles.on_surface, Theme::dark().roles.on_surface);
}

AURORA_TEST_CASE(resolve_theme_prefers_nearest_ancestor_scope) {
    // 嵌套覆盖：内层 ThemeScope（dark）对子树生效，外层（light）被遮蔽。
    auto inner = std::make_shared<ThemeScope>(Theme::dark(), Placeholder{});
    Node root{ThemeScope{Theme::light(), Node{inner}}};

    const Theme root_theme = resolve_theme(root);
    AURORA_TEST_CHECK_EQ(root_theme.roles.surface, Theme::light().roles.surface);

    const Theme inner_theme = resolve_theme(root, *inner);
    AURORA_TEST_CHECK_EQ(inner_theme.roles.surface, Theme::dark().roles.surface);
}

AURORA_TEST_CASE(resolve_theme_descendant_inherits_ancestor) {
    // 目标本身不是 Provider：沿 DFS 继承最近祖先注入的主题。
    auto leaf = std::make_shared<Placeholder>();
    Node root{ThemeScope{Theme::dark(), Node{leaf}}};

    const Theme resolved = resolve_theme(root, *leaf);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::dark().roles.surface);
    AURORA_TEST_CHECK_EQ(resolved.roles.primary, Theme::dark().roles.primary);
}

AURORA_TEST_CASE(resolve_theme_target_outside_tree_falls_back) {
    // 目标不在树中：DFS 走完全树仍未命中，结果保持兜底默认主题（不崩溃、不返回脏值）。
    Node root{ThemeScope{Theme::dark(), Placeholder{}}};
    const Placeholder outsider;
    const Theme resolved = resolve_theme(root, outsider);
    AURORA_TEST_CHECK_EQ(resolved.roles.surface, Theme::with_defaults().roles.surface);
}

}  // namespace aurora::test_cases::utest_theme_query

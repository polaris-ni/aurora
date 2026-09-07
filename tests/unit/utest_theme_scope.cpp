/// 测试类型: unit
/// 目标单元: include/aurora/theming/theme_scope.h
/// 测试说明: 主题作用域（ThemeScope 类型名/Provider 关系、inherit_theme 无注入回退浅色主题）单元测试

#include "aurora/environment/environment.h"
#include "aurora/theming/theme_scope.h"
#include "aurora/widget/node.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_theme_scope {

AURORA_TEST() {
    // ---- 1. inherit_theme：无注入环境（env 为空）回退浅色主题，永不崩溃 ----
    {
        const BuildContext ctx;  // env == nullptr
        const Theme t = inherit_theme(ctx);
        AURORA_TEST_CHECK(t.background == Color::white());
        AURORA_TEST_CHECK(t.primary == Color::blue());
        AURORA_TEST_CHECK(t.text == Color::black());  // 与 Theme::light() 各扁平字段一致
    }

    // ---- 2. 浅色与深色主题在背景上确有区分（回退判定有意义） ----
    {
        AURORA_TEST_CHECK(!(Theme::light().background == Theme::dark().background));
        AURORA_TEST_CHECK(Theme::dark().background == Color::from_rgba(32, 33, 36));
        AURORA_TEST_CHECK(Theme::light().background == Color::white());
    }

    // ---- 3. ThemeScope：以固定主题包装子树，类型名为 "ThemeScope" ----
    {
        ThemeScope scope{Theme::dark(), Node{}};
        AURORA_TEST_CHECK_EQ(std::string_view(scope.type_name()), std::string_view("ThemeScope"));
        // 本质是一个 Provider<Theme>：经基类引用虚派发仍得 "ThemeScope"
        Provider<Theme> &base = scope;
        AURORA_TEST_CHECK_EQ(std::string_view(base.type_name()), std::string_view("ThemeScope"));
    }

    // ---- 4. ThemeScope：运行时换肤（共享 State<Theme> 注入）仍为 "ThemeScope" ----
    {
        auto shared = std::make_shared<State<Theme>>(Theme::light());
        ThemeScope scope{shared, Node{}};
        AURORA_TEST_CHECK_EQ(std::string_view(scope.type_name()), std::string_view("ThemeScope"));
        shared->set(Theme::dark());  // 换肤不改变类型名，仅驱动子树重渲染
        AURORA_TEST_CHECK_EQ(std::string_view(scope.type_name()), std::string_view("ThemeScope"));
    }
}

}  // namespace aurora::test_cases::utest_theme_scope

/// 测试类型: integration
/// 目标单元: include/aurora/ui/factories.h
/// 测试说明: 验证测试辅助层本身（tests/support/test_helpers.h 的 init_headless/pump/tap/
///           type_text/expect_text/expect_tree_contains/expect_count/expect_bounds/expect_visible/
///           absolute_bounds）在真实控件树上行为正确
/// 覆盖说明: 辅助是全部 itest 的地基；此处用工厂构建 Column/Text/Button/TextInput 树自证

#include <string>

#include "aurora/aurora.h"
#include "aurora/ui/factories.h"
#include "framework/aurora_test.h"
#include "test_helpers.h"

namespace aurora::test_cases::itest_aurora_test_api {

using au::test::absolute_bounds;
using au::test::expect_bounds;
using au::test::expect_count;
using au::test::expect_text;
using au::test::expect_tree_contains;
using au::test::expect_visible;
using au::test::init_headless;
using au::test::pump;
using au::test::tap;
using au::test::TestEnv;
using au::test::type_text;
using au::ui::button;
using au::ui::input;
using au::ui::label;

AURORA_TEST_CASE(helpers_assert_text_count_tree_and_visibility) {
    TestEnv env = init_headless(300, 200);
    label(*env.root_widget, "Hello");
    label(*env.root_widget, "World");
    button(*env.root_widget, "Go");

    pump(env);
    expect_text(env.root, "Hello");
    expect_text(env.root, "World");
    expect_count(env.root, "Text", 2);
    expect_tree_contains(env.root, "Button");
    expect_visible(env.root, true);

    const auto kids = env.root_widget->child_nodes();
    AURORA_TEST_CHECK_MSG(!kids.empty(), "root has children");
    expect_visible(kids[0]);
}

AURORA_TEST_CASE(helpers_tap_triggers_button_on_click) {
    TestEnv env = init_headless(200, 100);
    bool clicked = false;
    const Button* b = button(*env.root_widget, "Hit", {}, [&clicked]() -> void { clicked = true; });
    pump(env);
    tap(env, *b);
    AURORA_TEST_CHECK_MSG(clicked, "tap synthesized press+release triggered on_click");
}

AURORA_TEST_CASE(helpers_type_text_updates_text_input_value) {
    TestEnv env = init_headless(200, 100);
    TextInput* in = input(*env.root_widget, "");
    pump(env);
    type_text(env, *in, "abc");
    AURORA_TEST_CHECK_MSG(in->value() == "abc", "type_text updated TextInput value");
}

AURORA_TEST_CASE(helpers_expect_bounds_matches_laid_out_node) {
    TestEnv env = init_headless(200, 100);
    label(*env.root_widget, "Hi");
    pump(env);
    const auto kids = env.root_widget->child_nodes();
    AURORA_TEST_CHECK_MSG(!kids.empty(), "root has a child");
    const Rect b = kids[0].bounds();
    AURORA_TEST_CHECK_MSG(b.size.width > 0.0F, "text node laid out with nonzero width");
    expect_bounds(kids[0], b);  // 自洽：期望等于实际应通过
}

AURORA_TEST_CASE(helpers_absolute_bounds_locates_widget_in_tree) {
    TestEnv env = init_headless(200, 100);
    const Button* b = button(*env.root_widget, "Find me");
    pump(env);

    const auto box = absolute_bounds(env.root, *b);
    AURORA_TEST_CHECK_MSG(box.has_value(), "absolute_bounds finds the target widget after pump");
    if (box.has_value()) {
        AURORA_TEST_CHECK_MSG(box->size.width > 0.0F && box->size.height > 0.0F,
                              "absolute_bounds returns a non-degenerate rect");
        // 布局一帧后根 Column 占满视口，按钮应落在视口矩形内。
        AURORA_TEST_CHECK_MSG(box->origin.x >= 0.0F && box->origin.y >= 0.0F, "absolute_bounds origin is non-negative");
    }
}

}  // namespace aurora::test_cases::itest_aurora_test_api

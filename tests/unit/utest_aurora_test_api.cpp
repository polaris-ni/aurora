/// 测试类型: unit
/// 目标单元: include/aurora/aurora.h
/// 测试说明: aurora_test_api 单元测试
///

// 覆盖 aurora::test 原语：expect_* / pump / tap / type_text 确定性断言。
#include <string>

#include "aurora/aurora.h"
#include "aurora/test_helpers.h"
#include "aurora/ui/factories.h"
#include "aurora/widget/button.h"
#include "aurora/widget/text_input.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_aurora_test_api {

using au::test::init_headless;


using au::test::TestEnv;
using au::test::expect_bounds;
using au::test::expect_count;
using au::test::expect_text;
using au::test::expect_tree_contains;
using au::test::expect_visible;
using au::test::pump;
using au::test::tap;
using au::test::type_text;
using au::ui::button;
using au::ui::input;
using au::ui::label;




static void test_expect_text_count_tree() {
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
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    expect_visible(kids[0]);
}

static void test_tap_triggers_on_click() {
    TestEnv env = init_headless(200, 100);
    bool clicked = false;
    Button const *b = button(*env.root_widget, "Hit", {}, [&]() -> void { clicked = true; });
    pump(env);
    tap(env, *b);
    AURORA_TEST_CHECK_MSG(clicked, "tap synthesized press+release triggered on_click");
}

static void test_type_text_updates_input() {
    TestEnv env = init_headless(200, 100);
    TextInput *in = input(*env.root_widget, "");
    pump(env);
    type_text(env, *in, "abc");
    AURORA_TEST_CHECK_MSG(in->value() == "abc", "type_text updated TextInput value");
}

static void test_expect_bounds_smoke() {
    TestEnv env = init_headless(200, 100);
    label(*env.root_widget, "Hi");
    pump(env);
    const auto kids = env.root_widget->child_nodes();
    AURORA_TEST_CHECK_MSG(!kids.empty(), "root has a child");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const Rect b = kids[0].bounds();
    AURORA_TEST_CHECK_MSG(b.size.width > 0.0F, "text node laid out with nonzero width");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    expect_bounds(kids[0], b);  // 自洽：期望等于实际应通过
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== aurora_test_api_test ===\n");
    test_expect_text_count_tree();
    test_tap_triggers_on_click();
    test_type_text_updates_input();
    test_expect_bounds_smoke();
}

}  // namespace aurora::test_cases::utest_aurora_test_api
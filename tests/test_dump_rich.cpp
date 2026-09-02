// test_dump_rich.cpp — 覆盖 dump_tree_rich 富格式：#id、bounds、visible、text、style、listeners、树形符。
#include <string>

#include "aurora/aurora.h"
#include "aurora/test_helpers.h"
#include "aurora/ui/factories.h"
#include "aurora/widget/widget.h"

#include "test_harness.h"

using aurora::Text;
using aurora::test::init_headless;
using aurora::test::TestEnv;
using aurora::ui::button;
using aurora::ui::label;
using aurora::ui::vbox;

static void test_rich_fields_and_id() {
    TestEnv env = init_headless(200, 100);
    Text const *t = label(*env.root_widget, "Hi");
    (void)t;
    // 给该文本节点设置 #id
    AURORA_TEST_CHECK_MSG(env.root_widget->child_count() >= 1, "root has one child");
    env.root_widget->child(0).set_id("title");

    pump(env);
    const std::string s = dump_tree_rich(env.root);

    AURORA_TEST_CHECK_MSG(s.find("Column") != std::string::npos, "rich dump contains root type");
    AURORA_TEST_CHECK_MSG(s.find("#title") != std::string::npos, "rich dump contains #id");
    AURORA_TEST_CHECK_MSG(s.find("text: \"Hi\"") != std::string::npos, "rich dump contains text");
    AURORA_TEST_CHECK_MSG(s.find("visible: true") != std::string::npos, "rich dump contains visible");
    AURORA_TEST_CHECK_MSG(s.find("listeners: []") != std::string::npos, "rich dump shows empty listeners for Text");
    AURORA_TEST_CHECK_MSG(s.find("bounds: [") != std::string::npos, "rich dump contains bounds");
}

static void test_rich_tree_chars() {
    TestEnv env = init_headless(200, 100);
    vbox(*env.root_widget); // 第一个子：空容器
    label(*env.root_widget, "A");
    button(*env.root_widget, "B");
    pump(env);
    const std::string s = dump_tree_rich(env.root);

    AURORA_TEST_CHECK_MSG(s.find("├─") != std::string::npos, "rich dump uses ├─ branch");
    AURORA_TEST_CHECK_MSG(s.find("└─") != std::string::npos, "rich dump uses └─ branch");
    // 最后一行应为 └─（末级子），且整体含 Button 的 on_click 监听
    AURORA_TEST_CHECK_MSG(s.find("on_click") != std::string::npos, "rich dump lists Button listeners");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== dump_rich_test ===\n");
    test_rich_fields_and_id();
    test_rich_tree_chars();
}

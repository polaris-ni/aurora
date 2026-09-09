/// 测试类型: integration
/// 目标单元: include/aurora/widget/inspect.h
/// 测试说明: 覆盖 dump_tree_rich 富格式——#id、bounds、visible、text、style、listeners
///           与 ├─ └─ 树形连接符，供 AI 文本断言消费

#include <string>

#include "aurora/aurora.h"
#include "aurora/ui/factories.h"
#include "aurora/widget/inspect.h"
#include "test_helpers.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_dump_rich {

using au::dump_tree_rich;
using au::test::init_headless;
using au::test::pump;
using au::test::TestEnv;
using au::ui::button;
using au::ui::label;
using au::ui::vbox;

AURORA_TEST_CASE(dump_rich_shows_id_text_visible_and_bounds) {
    TestEnv env = init_headless(200, 100);
    const Text *t = label(*env.root_widget, "Hi");
    (void)t;
    AURORA_TEST_CHECK_MSG(env.root_widget->child_count() >= 1, "root has one child");
    env.root_widget->child(0).set_id("title");

    pump(env);
    const std::string s = dump_tree_rich(env.root);

    AURORA_TEST_CHECK_MSG(s.find("Column") != std::string::npos, "rich dump contains root type");
    AURORA_TEST_CHECK_MSG(s.find("#title") != std::string::npos, "rich dump contains #id");
    AURORA_TEST_CHECK_MSG(s.find("text: \"Hi\"") != std::string::npos, "rich dump contains text");
    AURORA_TEST_CHECK_MSG(s.find("visible: true") != std::string::npos, "rich dump contains visible");
    AURORA_TEST_CHECK_MSG(s.find("listeners: []") != std::string::npos,
                          "rich dump shows empty listeners for Text");
    AURORA_TEST_CHECK_MSG(s.find("bounds: [") != std::string::npos, "rich dump contains bounds");
}

AURORA_TEST_CASE(dump_rich_uses_tree_chars_and_lists_button_listener) {
    TestEnv env = init_headless(200, 100);
    vbox(*env.root_widget);  // 第一个子：空容器
    label(*env.root_widget, "A");
    button(*env.root_widget, "B");
    pump(env);
    const std::string s = dump_tree_rich(env.root);

    AURORA_TEST_CHECK_MSG(s.find("├─") != std::string::npos, "rich dump uses ├─ branch");
    AURORA_TEST_CHECK_MSG(s.find("└─") != std::string::npos, "rich dump uses └─ branch");
    // 最后一行应为 └─（末级子），且整体含 Button 的 on_click 监听。
    AURORA_TEST_CHECK_MSG(s.find("on_click") != std::string::npos, "rich dump lists Button listeners");
}

}  // namespace aurora::test_cases::itest_dump_rich

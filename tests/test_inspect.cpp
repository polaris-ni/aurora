// test_inspect.cpp — 运行时可观测（dump_tree / dump_tree_json / query / get_state）1:1 测试。
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Column;
using aurora::Constraints;
using aurora::Node;
using aurora::Size;
using aurora::Text;

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_inspect ===\n");

    auto root = Node{ Column{ Node{ Button{ "Hi" } }, Node{ Text{ "x" } } } };

    BuildContext ctx;
    root.widget().mount(ctx);
    root.widget().layout(
        Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 200, .height = 200 } }, ctx);

    // 1) dump_tree 返回非空字符串。
    auto tree = dump_tree(root);
    AURORA_TEST_CHECK(!tree.empty());

    // 2) dump_tree 列出 Button 类型。
    AURORA_TEST_CHECK(tree.find("Button") != std::string::npos);

    // 3) query 按类型名精确匹配找到 Button。
    auto found = query("Button", root);
    AURORA_TEST_CHECK(!found.empty());

    // 4) get_state 沿路径读取子节点 type。
    auto st = get_state("children/0/type", root);
    AURORA_TEST_CHECK(st.is_string() && st.get<std::string>() == "Button");

    // 5) dump_tree_json 返回对象且含 "type" 键。
    auto j = dump_tree_json(root);
    AURORA_TEST_CHECK(j.is_object() && j.contains("type"));
    AURORA_TEST_CHECK(j["type"].get<std::string>() == "Column");

    // 6) dump_tree_json 含 "children" 数组，且含两个子节点。
    AURORA_TEST_CHECK(j.contains("children") && j["children"].is_array());
    AURORA_TEST_CHECK(j["children"].size() == 2);

    // 7) query 找到 Text 子节点。
    auto texts = query("Text", root);
    AURORA_TEST_CHECK(!texts.empty());

    // 8) query 未知类型返回空。
    auto none = query("DefinitelyNotAWidget", root);
    AURORA_TEST_CHECK(none.empty());

    // 9) get_state 读取第二个子节点 type。
    auto st2 = get_state("children/1/type", root);
    AURORA_TEST_CHECK(st2.is_string() && st2.get<std::string>() == "Text");

    // 10) get_state 读取根节点自身 type。
    auto st_root = get_state("type", root);
    AURORA_TEST_CHECK(st_root.is_string() && st_root.get<std::string>() == "Column");

    // 11) get_state 数组越界返回空 Json。
    auto st_oob = get_state("children/99/type", root);
    AURORA_TEST_CHECK(st_oob.is_null());

    // 12) get_state 未知键路径返回空 Json。
    auto st_missing = get_state("nope/key", root);
    AURORA_TEST_CHECK(st_missing.is_null());

    // 13) query 大小写敏感：小写不匹配。
    auto lower = query("button", root);
    AURORA_TEST_CHECK(lower.empty());

    // 14) dump_tree 支持 depth 参数（缩进不崩溃）。
    auto tree_deep = dump_tree(root, 2);
    AURORA_TEST_CHECK(!tree_deep.empty());
}

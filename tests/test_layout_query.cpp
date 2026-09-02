// test_layout_query.cpp — 布局查询（describe_layout / layout_of）1:1 测试：
// 顶层尺寸 / 子节点坐标 / 嵌套结构 (dump_tree_json) / get_state 路径查询 / 未挂载降级。
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Column;
using aurora::Constraints;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Row;
using aurora::Size;
using aurora::Text;

static void test_layout_query() {
    auto root = Node{ Column{ Node{ Text{ "a" } }, Node{ Text{ "b" } } } };
    BuildContext ctx;
    root.widget().mount(ctx);
    const Size sz = root.widget().layout(
        Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 320, .height = 240 } }, ctx);
    root.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = sz });

    auto desc = describe_layout(root);
    AURORA_TEST_CHECK(desc.contains("width") && desc["width"].get<float>() > 0.0f);
    AURORA_TEST_CHECK(desc.contains("height") && desc["height"].get<float>() > 0.0f);
    AURORA_TEST_CHECK(desc.contains("type") && desc["type"].get<std::string>() == "Column");
    AURORA_TEST_CHECK(desc.contains("x") && desc["x"].get<float>() == 0.0f);
    AURORA_TEST_CHECK(desc.contains("y") && desc["y"].get<float>() == 0.0f);

    auto snap = layout_of(root);
    AURORA_TEST_CHECK(snap.size.width > 0.0f);
    AURORA_TEST_CHECK(snap.size.height > 0.0f);
    AURORA_TEST_CHECK(near_f(snap.size.width, desc["width"].get<float>()));

    // 子节点查询：Column 含 2 个 Text。
    auto kids = root.widget().child_nodes();
    AURORA_TEST_CHECK(kids.size() == 2);
    AURORA_TEST_CHECK(std::string(kids[0].widget().type_name()) == "Text");

    // 为子节点设置坐标后查询。
    kids[0].set_bounds(
        Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = Size{ .width = 50.0f, .height = 20.0f } });
    kids[1].set_bounds(
        Rect{ .origin = Point{ .x = 0.0f, .y = 20.0f }, .size = Size{ .width = 50.0f, .height = 20.0f } });
    auto d0 = describe_layout(kids[0]);
    AURORA_TEST_CHECK(near_f(d0["x"].get<float>(), 0.0f));
    AURORA_TEST_CHECK(near_f(d0["y"].get<float>(), 0.0f));
    AURORA_TEST_CHECK(near_f(d0["width"].get<float>(), 50.0f));
    auto d1 = describe_layout(kids[1]);
    AURORA_TEST_CHECK(near_f(d1["y"].get<float>(), 20.0f));

    auto s1 = layout_of(kids[1]);
    AURORA_TEST_CHECK(near_f(s1.origin.y, 20.0f));

    // 深层嵌套 Column > Row > Text：结构查询。
    auto deep = Node{ Column{ Node{ Row{ Node{ Text{ "x" } } } } } };
    BuildContext dctx;
    deep.widget().mount(dctx);
    const Size dsz = deep.widget().layout(
        Constraints{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 200, .height = 200 } }, dctx);
    deep.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = dsz });
    auto deep_desc = describe_layout(deep);
    AURORA_TEST_CHECK(std::string(deep_desc["type"].get<std::string>()) == "Column");

    auto deep_json = dump_tree_json(deep);
    AURORA_TEST_CHECK(deep_json["children"].size() == 1);
    AURORA_TEST_CHECK(std::string(deep_json["children"][0]["type"].get<std::string>()) == "Row");
    AURORA_TEST_CHECK(std::string(deep_json["children"][0]["children"][0]["type"].get<std::string>()) == "Text");

    // get_state 沿路径查询嵌套 type。
    auto st = get_state("children/0/children/0/type", deep);
    AURORA_TEST_CHECK(st.is_string() && st.get<std::string>() == "Text");

    // 未挂载 / 无 bounds 的节点查询不崩溃，width 为 0。
    Node fresh{ Text{ "z" } };
    auto fd = describe_layout(fresh);
    AURORA_TEST_CHECK(fd["width"].get<float>() == 0.0f);
    AURORA_TEST_CHECK(std::string(fd["type"].get<std::string>()) == "Text");
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_layout_query ===\n");
    test_layout_query();
}

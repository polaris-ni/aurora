// test_repeater.cpp — Repeater 控件 1:1 测试：子节点展开 / 空列表 / 动态增删 /
// max_depth / describe / 序列化。

#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "test_harness.h"

namespace serialization = aurora::serialization;
using aurora::AURORA_DEFAULT_MAX_WIDGET_DEPTH;
using aurora::BuildContext;
using aurora::Constraints;
using aurora::Node;
using aurora::Repeater;
using aurora::Size;
using aurora::State;
using aurora::Text;

static void test_repeater() {
    const auto items = std::make_shared<State<std::vector<std::string>>>(std::vector<std::string>{ "a", "b", "c" });
    Repeater<std::string> rep{ items, [](const std::string &s, int) -> Node { return Node{ Text{ s } }; } };
    constexpr BuildContext ctx;
    rep.mount(ctx);
    constexpr Constraints c{ .min = Size{ .width = 0, .height = 0 }, .max = Size{ .width = 100, .height = 100 } };
    rep.layout(c, ctx);

    AURORA_TEST_CHECK(rep.child_nodes().size() == 3);
    AURORA_TEST_CHECK(std::string(rep.type_name()) == "Repeater");

    const auto kids = rep.child_nodes();
    AURORA_TEST_CHECK(std::string(kids[0].widget().type_name()) == "Text");
    AURORA_TEST_CHECK(std::string(kids[2].widget().type_name()) == "Text");

    // 空列表 → 0 子节点。
    const auto empty = std::make_shared<State<std::vector<std::string>>>(std::vector<std::string>{});
    Repeater<std::string> rep2{ empty, [](const std::string &s, int) -> Node { return Node{ Text{ s } }; } };
    constexpr BuildContext ctx2;
    rep2.mount(ctx2);
    rep2.layout(c, ctx2);
    AURORA_TEST_CHECK(rep2.child_nodes().empty());

    // 动态增加：set 新向量后重新布局 → 子节点变为 4。
    {
        auto v = items->get();
        v.emplace_back("d");
        items->set(std::move(v));
    }
    rep.layout(c, ctx);
    AURORA_TEST_CHECK(rep.child_nodes().size() == 4);

    // 动态删除首项：重新布局 → 子节点变为 3。
    {
        auto v = items->get();
        v.erase(v.begin());
        items->set(std::move(v));
    }
    rep.layout(c, ctx);
    AURORA_TEST_CHECK(rep.child_nodes().size() == 3);
    AURORA_TEST_CHECK(std::string(rep.child_nodes()[0].widget().type_name()) == "Text");

    // 布局尺寸非负。
    const Size sz = rep.size();
    AURORA_TEST_CHECK(sz.width >= 0.0f && sz.height >= 0.0f);

    // max_depth 访问与设置。
    AURORA_TEST_CHECK(rep.max_depth() == AURORA_DEFAULT_MAX_WIDGET_DEPTH);
    rep.set_max_depth(8);
    AURORA_TEST_CHECK(rep.max_depth() == 8);

    // describe_static / describe 提供类型名。
    AURORA_TEST_CHECK(std::string(Repeater<std::string>::describe_static().name) == "Repeater");
    AURORA_TEST_CHECK(std::string(rep.describe().name) == "Repeater");

    // 序列化：type 与 items 不序列化（note 字段）。
    auto js = serialization::to_json(rep);
    AURORA_TEST_CHECK(js.contains("type") && js["type"].get<std::string>() == "Repeater");
    AURORA_TEST_CHECK(js["props"].contains("note"));
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== test_repeater ===\n");
    test_repeater();
}

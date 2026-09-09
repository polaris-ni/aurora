/// 测试类型: unit
/// 目标单元: include/aurora/widget/layout_query.h
/// 测试说明: 覆盖 layout_of / describe_layout——已布局根节点查询、子节点 bounds 快照查询、
/// 输出 JSON 键与类型完整性、未布局节点的零值降级

#include <memory>
#include <utility>

#include "aurora/environment/build_context.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/layout_query.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_layout_query {

namespace {

[[nodiscard]] auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 构造并布局一棵 Column → [Text("a"), Text("b")] 测试树，返回根 Node 与布局尺寸。
[[nodiscard]] auto make_laid_out_column(float w, float h, BuildContext &ctx) -> std::pair<Node, Size> {
    auto col = std::make_shared<Column>();
    col->add(Node{Text{"a"}});
    col->add(Node{Text{"b"}});
    Node root{std::move(col)};
    root.widget().mount(ctx);
    const Size sz = root.widget().layout(bounded(w, h), ctx);
    root.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = sz});
    return {std::move(root), sz};
}

}  // namespace

AURORA_TEST_CASE(layout_query_describe_layout_reports_laid_out_root) {
    BuildContext ctx;
    auto [root, sz] = make_laid_out_column(320.0F, 240.0F, ctx);

    const Json desc = describe_layout(root);
    AURORA_TEST_CHECK_EQ(desc["type"], "Column");
    AURORA_TEST_CHECK_NEAR(desc["x"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(desc["y"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_TRUE(desc["width"].get<float>() > 0.0F);
    AURORA_TEST_CHECK_TRUE(desc["height"].get<float>() > 0.0F);

    // layout_of 与 describe_layout 读同一份 bounds，两者一致且等于布局返回尺寸。
    const Rect snap = layout_of(root);
    AURORA_TEST_CHECK_NEAR(snap.size.width, sz.width, 1e-4F);
    AURORA_TEST_CHECK_NEAR(snap.size.height, sz.height, 1e-4F);
    AURORA_TEST_CHECK_NEAR(snap.size.width, desc["width"].get<float>(), 1e-4F);
}

AURORA_TEST_CASE(layout_query_child_bounds_snapshot_query) {
    BuildContext ctx;
    auto root = make_laid_out_column(320.0F, 240.0F, ctx).first;

    // 拷贝子 Node（共享 widget、快照 bounds）后写入确定性坐标，再查询。
    auto kids = root.widget().child_nodes();
    AURORA_TEST_REQUIRE_EQ(kids.size(), 2U);
    kids[0].set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 50.0F, .height = 20.0F}});
    kids[1].set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 20.0F}, .size = Size{.width = 50.0F, .height = 20.0F}});

    const Json d0 = describe_layout(kids[0]);
    AURORA_TEST_CHECK_EQ(d0["type"], "Text");
    AURORA_TEST_CHECK_NEAR(d0["x"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d0["width"].get<float>(), 50.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(d0["height"].get<float>(), 20.0F, 1e-4F);

    const Json d1 = describe_layout(kids[1]);
    AURORA_TEST_CHECK_NEAR(d1["y"].get<float>(), 20.0F, 1e-4F);

    const Rect s1 = layout_of(kids[1]);
    AURORA_TEST_CHECK_NEAR(s1.origin.y, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s1.size.width, 50.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_query_unmounted_node_degrades_to_zero_rect) {
    Node fresh{Text{"z"}};  // 未挂载 / 未布局

    const Json fd = describe_layout(fresh);
    AURORA_TEST_CHECK_EQ(fd["type"], "Text");
    AURORA_TEST_CHECK_NEAR(fd["width"].get<float>(), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fd["height"].get<float>(), 0.0F, 1e-4F);

    const Rect fr = layout_of(fresh);
    AURORA_TEST_CHECK_NEAR(fr.origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fr.origin.y, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fr.size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(fr.size.height, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(layout_query_describe_layout_json_schema_complete) {
    BuildContext ctx;
    auto root = make_laid_out_column(200.0F, 200.0F, ctx).first;

    const Json desc = describe_layout(root);
    AURORA_TEST_CHECK_TRUE(desc.is_object());
    AURORA_TEST_CHECK_TRUE(desc.contains("type"));
    AURORA_TEST_CHECK_TRUE(desc.contains("x"));
    AURORA_TEST_CHECK_TRUE(desc.contains("y"));
    AURORA_TEST_CHECK_TRUE(desc.contains("width"));
    AURORA_TEST_CHECK_TRUE(desc.contains("height"));
    AURORA_TEST_CHECK_TRUE(desc["type"].is_string());
    AURORA_TEST_CHECK_TRUE(desc["x"].is_number());
    AURORA_TEST_CHECK_TRUE(desc["y"].is_number());
    AURORA_TEST_CHECK_TRUE(desc["width"].is_number());
    AURORA_TEST_CHECK_TRUE(desc["height"].is_number());
}

}  // namespace aurora::test_cases::utest_layout_query

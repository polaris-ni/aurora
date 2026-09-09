/// 测试类型: integration
/// 目标单元: include/aurora/core/dimension.h + include/aurora/render/offscreen.h
/// 测试说明: 百分比尺寸意图集成——percent 宽/高按视口比例落盘、100% 填满、
/// fill() 等价撑满、嵌套百分比按父级解析、小比例分数

#include <utility>

#include "aurora/core/dimension.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_percent_layout {

AURORA_TEST_CASE(percent_width_half_viewport) {
    // 50% 宽度子项在 800px 视口中布局为 400px。
    Text txt{"Hello"};
    txt.width(percent(0.5F));

    Node root{std::move(txt)};
    const Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 400.0F, 1e-3F);
}

AURORA_TEST_CASE(percent_height_quarter_viewport) {
    // 25% 高度在 600px 视口中 = 150px。
    Text txt{"Hi"};
    txt.height(percent(0.25F));

    Node root{std::move(txt)};
    const Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK_NEAR(snap["box"]["h"].get<float>(), 150.0F, 1e-3F);
}

AURORA_TEST_CASE(percent_both_dimensions) {
    // 50% x 50% 在 800x600 = 400x300。
    Text txt{"Box"};
    txt.width(percent(0.5F));
    txt.height(percent(0.5F));

    Node root{std::move(txt)};
    const Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 400.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(snap["box"]["h"].get<float>(), 300.0F, 1e-3F);
}

AURORA_TEST_CASE(percent_full_fills_viewport) {
    Text txt{"Full"};
    txt.width(percent(1.0F));
    txt.height(percent(1.0F));

    Node root{std::move(txt)};
    const Json snap = render_to_logical_snapshot(root, 640, 480);
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 640.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(snap["box"]["h"].get<float>(), 480.0F, 1e-3F);
}

AURORA_TEST_CASE(fill_expands_to_viewport_width) {
    Text txt{"Fill"};
    txt.width(fill());

    Node root{std::move(txt)};
    const Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 800.0F, 1e-3F);
}

AURORA_TEST_CASE(nested_percent_resolves_against_parent) {
    // Column 占 100%，内部 Text 占 50%（按父级实际宽度解析）。
    Text txt{"Inner"};
    txt.width(percent(0.5F));

    Column col{Node{std::move(txt)}};
    col.width(percent(1.0F));

    Node root{std::move(col)};
    const Json snap = render_to_logical_snapshot(root, 800, 600);
    // Column 应为 800px 宽。
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 800.0F, 1e-3F);
    // 内部 Text 应为 400px（50% of 800）。
    AURORA_TEST_CHECK_NEAR(snap["children"][0]["box"]["w"].get<float>(), 400.0F, 1e-3F);
}

AURORA_TEST_CASE(small_percent_fraction) {
    Text txt{"Tiny"};
    txt.width(percent(0.1F));  // 10% of 800 = 80

    Node root{std::move(txt)};
    const Json snap = render_to_logical_snapshot(root, 800, 600);
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 80.0F, 1e-3F);
}

}  // namespace aurora::test_cases::itest_percent_layout

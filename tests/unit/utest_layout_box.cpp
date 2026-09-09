/// 测试类型: unit
/// 目标单元: include/aurora/layout/layout_box.h
/// 测试说明: 覆盖 LayoutBox 纯值类型的默认零值、children 嵌套树与插入顺序一致、拷贝深拷贝后互不影响

#include "aurora/layout/layout_box.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_layout_box {

AURORA_TEST_CASE(default_constructs_zeroed_geometry) {
    const LayoutBox box;
    AURORA_TEST_CHECK_NEAR(box.rect.origin.x, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(box.rect.origin.y, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(box.rect.size.width, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(box.rect.size.height, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(box.constraints.min.width, 0.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(box.constraints.max.width, Size::infinity().width);
    AURORA_TEST_CHECK_TRUE(box.children.empty());
}

AURORA_TEST_CASE(nests_children_and_preserves_order) {
    LayoutBox root;
    root.rect.size = Size{.width = 100.0F, .height = 50.0F};

    LayoutBox first;
    first.rect = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 30.0F, .height = 10.0F}};
    LayoutBox second;
    second.rect = Rect{.origin = Point{.x = 30.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = 10.0F}};
    root.children.push_back(first);
    root.children.push_back(second);

    AURORA_TEST_REQUIRE_EQ(root.children.size(), 2U);
    AURORA_TEST_CHECK_NEAR(root.children[0].rect.origin.x, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(root.children[0].rect.size.width, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(root.children[1].rect.origin.x, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(root.children[1].rect.size.width, 40.0F, 0.0F);

    // 两层嵌套：子盒再挂孙盒，树形结构保持。
    root.children[0].children.push_back(LayoutBox{});
    AURORA_TEST_CHECK_EQ(root.children[0].children.size(), 1U);
    AURORA_TEST_CHECK_TRUE(root.children[1].children.empty());
}

AURORA_TEST_CASE(copy_is_deep_on_children) {
    LayoutBox source;
    LayoutBox child;
    child.rect.size.width = 20.0F;
    source.children.push_back(child);

    LayoutBox copy = source;  // 值语义：拷贝后各自独立
    copy.children[0].rect.size.width = 99.0F;
    AURORA_TEST_CHECK_NEAR(source.children[0].rect.size.width, 20.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(copy.children[0].rect.size.width, 99.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_layout_box

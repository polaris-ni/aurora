/// 测试类型: unit
/// 目标单元: include/aurora/layout/layout_box.h
/// 测试说明: 布局盒值类型（默认几何/约束、聚合初始化、递归子盒树、命中/相交随 Rect 语义）单元测试

#include <vector>

#include "aurora/core/types.h"
#include "aurora/layout/layout_box.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_layout_box {

AURORA_TEST() {
    // ---- 1. 默认构造：零矩形 + 默认约束（min 零、max 无限）+ 无子盒 ----
    {
        const LayoutBox box;
        AURORA_TEST_CHECK(box.rect == Rect{});
        AURORA_TEST_CHECK(box.rect.size.width == 0.0F);
        AURORA_TEST_CHECK(box.constraints == Constraints{});
        AURORA_TEST_CHECK(box.constraints.max.width == std::numeric_limits<float>::infinity());
        AURORA_TEST_CHECK(box.children.empty());
    }

    // ---- 2. 聚合初始化：按声明序 rect / constraints / children ----
    {
        const Rect r{.origin = Point{.x = 10.0F, .y = 20.0F}, .size = Size{.width = 100.0F, .height = 50.0F}};
        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 200.0F}};
        const LayoutBox box{.rect = r, .constraints = c, .children = {}};
        AURORA_TEST_CHECK_EQ(box.rect.right(), 110.0F);
        AURORA_TEST_CHECK_EQ(box.rect.bottom(), 70.0F);
        AURORA_TEST_CHECK(box.constraints == c);
    }

    // ---- 3. 递归子盒树：children 顺序即子节点顺序，可任意嵌套 ----
    {
        LayoutBox leaf{};
        leaf.rect = Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 20.0F, .height = 20.0F}};

        LayoutBox mid{};
        mid.rect = Rect{.origin = Point{}, .size = Size{.width = 40.0F, .height = 40.0F}};
        mid.children.push_back(leaf);

        LayoutBox root{};
        root.rect = Rect{.origin = Point{}, .size = Size{.width = 80.0F, .height = 80.0F}};
        root.children.push_back(mid);
        root.children.push_back(leaf);

        AURORA_TEST_CHECK_EQ(root.children.size(), std::size_t{2});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK_EQ(root.children[0].children.size(), std::size_t{1});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK(root.children[0].children[0].rect == leaf.rect);
    }

    // ---- 4. 命中判定复用 Rect::contains（子盒坐标相对父盒） ----
    {
        LayoutBox box{
            .rect = Rect{.origin = Point{.x = 5.0F, .y = 5.0F}, .size = Size{.width = 10.0F, .height = 10.0F}},
            .constraints = Constraints{},
            .children = {}};
        AURORA_TEST_CHECK(box.rect.contains(Point{.x = 10.0F, .y = 10.0F}));
        AURORA_TEST_CHECK_FALSE(box.rect.contains(Point{.x = 1.0F, .y = 1.0F}));
    }

    // ---- 5. 拷贝即值语义深拷贝子树 ----
    {
        LayoutBox src{};
        src.children.push_back(LayoutBox{});
        src.children.push_back(LayoutBox{});
        const LayoutBox copy = src;
        AURORA_TEST_CHECK_EQ(copy.children.size(), std::size_t{2});
    }
}

}  // namespace aurora::test_cases::utest_layout_box

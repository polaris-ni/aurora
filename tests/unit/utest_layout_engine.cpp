/// 测试类型: unit
/// 目标单元: include/aurora/layout/layout_engine.h
/// 测试说明: 布局引擎入口（layout 写入尺寸、build_box 递归收集子盒、layout_to_box 组合测量+收集）单元测试

#include "aurora/core/types.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/divider.h"
#include "aurora/widget/node.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_layout_engine {

AURORA_TEST() {
    constexpr BuildContext ctx;
    constexpr Constraints c{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 200.0F}};

    // ---- 1. layout()：两阶段布局把尺寸写回各 widget，受约束夹取 ----
    {
        Node root = Column{Divider{}, Divider{}};
        LayoutEngine::layout(root.widget(), c, ctx);
        const Size sz = root.widget().size();
        AURORA_TEST_CHECK(sz.width <= 200.0F);
        AURORA_TEST_CHECK(sz.height <= 200.0F);
    }

    // ---- 2. build_box()：按 Node 树几何递归收集，子盒数与 child_nodes 一致 ----
    {
        Node root = Column{Divider{}, Divider{}, Divider{}};
        LayoutEngine::layout(root.widget(), c, ctx);
        root.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = root.widget().size()});

        const LayoutBox box = LayoutEngine::build_box(root);
        AURORA_TEST_CHECK(box.rect == root.bounds());
        AURORA_TEST_CHECK_EQ(box.children.size(), root.widget().child_nodes().size());
        AURORA_TEST_CHECK_EQ(box.children.size(), std::size_t{3});
    }

    // ---- 3. layout_to_box()：一次性测量 + 收集，产出与 build_box 一致的结构 ----
    {
        Node root = Column{Divider{}, Divider{}};
        const LayoutBox box = LayoutEngine::layout_to_box(root, c, ctx);
        AURORA_TEST_CHECK_EQ(box.children.size(), std::size_t{2});
    }

    // ---- 4. 叶子节点：无子盒，build_box 返回空的 children ----
    {
        Node leaf = Divider{};
        leaf.set_bounds(Rect{.origin = Point{.x = 4.0F, .y = 4.0F}, .size = Size{.width = 16.0F, .height = 16.0F}});
        const LayoutBox box = LayoutEngine::build_box(leaf);
        AURORA_TEST_CHECK(box.children.empty());
        AURORA_TEST_CHECK_EQ(box.rect.origin.x, 4.0F);
        AURORA_TEST_CHECK_EQ(box.rect.size.height, 16.0F);
    }
}

}  // namespace aurora::test_cases::utest_layout_engine

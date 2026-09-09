/// 测试类型: unit
/// 目标单元: include/aurora/widget/stack.h
/// 测试说明: 覆盖 Stack 层叠布局——同原点堆叠取最大包围盒、对齐原点落定子盒、
/// fit Loose 放宽最小约束、fit Expand 二次布局撑满包围盒、Overlay 别名、
/// 序列化往返与自描述

#include <memory>
#include <type_traits>

#include "aurora/widget/stack.h"
#include "aurora/widget/text.h"
#include "aurora/layout/layout_engine.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_stack {

namespace {

auto box(float w, float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::fixed(w));
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto box_expand_w(float h) -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::Length::expand());
    t->height(aurora::Length::fixed(h));
    return Node{t};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

}  // namespace

AURORA_TEST_CASE(stack_stacks_children_at_origin_takes_max_bbox) {
    Stack st{box(100.0F, 20.0F), box(60.0F, 40.0F)};
    AURORA_TEST_CHECK_EQ(std::string{st.type_name()}, "Stack");

    const Size s = [&] {
        LayoutEngine::layout(st, bounded(300.0F, 300.0F));
        return st.size();
    }();
    AURORA_TEST_CHECK_NEAR(s.width, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 40.0F, 1e-4F);
    // 默认 TopLeft：两个子节点都落在原点。
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().origin.y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(stack_alignment_positions_children_within_bbox) {
    Stack st({box(100.0F, 20.0F), box(60.0F, 40.0F)}, Alignment::Center);
    LayoutEngine::layout(st, bounded(300.0F, 300.0F));
    // 包围盒 100x40：A(100x20)→(0,10)；B(60x40)→(20,0)。
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().origin.x, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().origin.y, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().origin.x, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().origin.y, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(stack_fit_loose_relaxes_min_constraint) {
    // Loose：子约束 min 归零——Expand 子项在松约束下仍取 max（200）。
    Stack st;
    st.add(box_expand_w(20.0F));
    st.set_fit(StackFit::Loose);
    LayoutEngine::layout(st, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(st.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().size.width, 200.0F, 1e-4F);
}

AURORA_TEST_CASE(stack_fit_expand_reruns_layout_with_tight_bbox) {
    // Expand：首轮与 Passthrough 相同按父约束测量，栈尺寸取子节点最大包围盒再 clamp
    // （非 Flutter 的「撑满父约束 max」）；随后以 tight 包围盒（min==max==栈尺寸）二次
    // 布局——无显式尺寸的轴被强制成栈尺寸，显式 Fixed 盒按 CSS box 语义优先于 tight
    // 约束、保持设定值。本例：首轮宽 Expand 子项 200x10、固定子项 60x20 → 包围盒
    // 200x20；二轮宽 Expand 子项宽被 tight 拉满 200（高保持显式 10），固定子项保持 60x20。
    Stack st;
    st.add(box_expand_w(10.0F));
    st.add(box(60.0F, 20.0F));
    st.set_fit(StackFit::Expand);
    LayoutEngine::layout(st, bounded(200.0F, 100.0F));
    AURORA_TEST_CHECK_NEAR(st.size().width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.size().height, 20.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().size.width, 200.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().size.height, 10.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().size.width, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().size.height, 20.0F, 1e-4F);
}

AURORA_TEST_CASE(stack_respects_min_constraint_floor) {
    // 子项都很小但 min 抬高：包围盒被 min 撑起。
    Stack st{box(10.0F, 10.0F)};
    Constraints c{.min = Size{.width = 80.0F, .height = 50.0F}, .max = Size{.width = 200.0F, .height = 100.0F}};
    LayoutEngine::layout(st, c);
    const Size s = st.size();
    AURORA_TEST_CHECK_NEAR(s.width, 80.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(s.height, 50.0F, 1e-4F);
}

AURORA_TEST_CASE(overlay_alias_is_stack) {
    static_assert(std::is_same_v<Overlay, Stack>, "Overlay 必须是 Stack 的便捷别名");
    Overlay st{box(10.0F, 10.0F)};
    AURORA_TEST_CHECK_EQ(std::string{st.type_name()}, "Stack");
}

AURORA_TEST_CASE(props_serialize_deserialize_roundtrip) {
    Stack src({box(10.0F, 10.0F)}, Alignment::BottomRight);
    src.set_fit(StackFit::Expand);

    Json props;
    src.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["alignment"].get<int>(), static_cast<int>(Alignment::BottomRight));
    AURORA_TEST_CHECK_EQ(props["fit"].get<int>(), static_cast<int>(StackFit::Expand));

    Stack dst;
    dst.deserialize_props(props);
    AURORA_TEST_CHECK_TRUE(dst.child_nodes().empty());  // props 不携带子节点

    // 重新构造验证反序列化后的对齐行为：BottomRight（大兄弟撑起 100x50 包围盒）。
    Stack st({box(40.0F, 10.0F), box(100.0F, 50.0F)}, Alignment::TopLeft);
    st.deserialize_props(props);
    LayoutEngine::layout(st, bounded(300.0F, 300.0F));
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().origin.x, 60.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[0].bounds().origin.y, 40.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(st.child_nodes()[1].bounds().origin.x, 0.0F, 1e-4F);
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = Stack::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "Stack");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "multiple");
}

}  // namespace aurora::test_cases::utest_stack

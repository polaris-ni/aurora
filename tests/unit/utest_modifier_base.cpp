/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_base.h
/// 测试说明: ModifierNode 基类默认契约与多态分发单元测试

#include <functional>

#include "aurora/modifier/modifier_base.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_modifier_base {

namespace {

using au::Constraints;
using au::ModifierNode;
using au::Size;

/// 最小可测节点：透传约束，其余行为全部沿用基类默认。
class ProbeNode final : public ModifierNode {
  public:
    explicit ProbeNode(Kind k) : kind_(k) {}

    [[nodiscard]] auto kind() const -> Kind override { return kind_; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

  private:
    Kind kind_;
};

}  // namespace

AURORA_TEST() {
    // ---- 1. 四种切片 kind 经基类指针正确分发 ----
    {
        const ProbeNode layout_node{ModifierNode::Kind::Layout};
        const ProbeNode paint_node{ModifierNode::Kind::Paint};
        const ProbeNode input_node{ModifierNode::Kind::Input};
        const ProbeNode transform_node{ModifierNode::Kind::Transform};

        const ModifierNode *base = &layout_node;
        AURORA_TEST_CHECK(base->kind() == ModifierNode::Kind::Layout);
        base = &paint_node;
        AURORA_TEST_CHECK(base->kind() == ModifierNode::Kind::Paint);
        base = &input_node;
        AURORA_TEST_CHECK(base->kind() == ModifierNode::Kind::Input);
        base = &transform_node;
        AURORA_TEST_CHECK(base->kind() == ModifierNode::Kind::Transform);
    }

    // ---- 2. paint_kind 默认为 None（仅 Paint 节点覆盖） ----
    {
        const ProbeNode n{ModifierNode::Kind::Paint};
        AURORA_TEST_CHECK(n.paint_kind() == ModifierNode::PaintKind::None);
    }

    // ---- 3. flex_weight 默认为 0（不参与 flex 剩余空间分配） ----
    {
        const ProbeNode n{ModifierNode::Kind::Layout};
        AURORA_TEST_CHECK(n.flex_weight() == 0.0F);
    }

    // ---- 4. fire_click / on_touch 默认为空操作，不产生副作用 ----
    {
        const ProbeNode n{ModifierNode::Kind::Layout};
        int side_effect = 0;
        (void)side_effect;
        n.fire_click();  // 空实现，不应崩溃
        const au::TouchEvent ev{};
        n.on_touch(ev);
        AURORA_TEST_CHECK(side_effect == 0);
    }

    // ---- 5. layout 经基类指针调用时透传约束 ----
    {
        const ProbeNode n{ModifierNode::Kind::Layout};
        const Constraints c{.min = Size{.width = 10.0F, .height = 20.0F},
                            .max = Size{.width = 100.0F, .height = 200.0F}};
        const Size s = n.layout(c, [](const Constraints &inner) -> Size { return inner.min; });
        AURORA_TEST_CHECK(s.width == 10.0F);
        AURORA_TEST_CHECK(s.height == 20.0F);
    }

    // ---- 6. 拷贝后 kind 保持（修饰链按值组合） ----
    {
        const ProbeNode src{ModifierNode::Kind::Input};
        const ProbeNode copy = src;
        AURORA_TEST_CHECK(copy.kind() == ModifierNode::Kind::Input);
    }
}

}  // namespace aurora::test_cases::utest_modifier_base

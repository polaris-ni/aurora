/// 测试类型: unit
/// 目标单元: include/aurora/modifier/modifier_base.h
/// 测试说明: 覆盖 ModifierNode 基类默认契约——paint_kind()=None、flex_weight()=0、
/// fire_click()/on_touch() 空操作；Kind/PaintKind 枚举值序（widget 层 switch 分发依赖）

#include "aurora/modifier/modifier_base.h"

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_modifier_base {

namespace {

/// 最小具体节点：仅上报 Kind，其余走基类默认实现。
class StubNode final : public ModifierNode {
  public:
    explicit StubNode(ModifierNode::Kind k) : kind_(k) {}
    [[nodiscard]] auto kind() const -> Kind override { return kind_; }
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

  private:
    ModifierNode::Kind kind_;
};

}  // namespace

AURORA_TEST_CASE(base_defaults_paint_kind_none) {
    // 非 Paint 节点 paint_kind 默认 None（供 PaintKind switch 分发兜底）。
    const StubNode n(ModifierNode::Kind::Layout);
    AURORA_TEST_CHECK_EQ(n.paint_kind(), ModifierNode::PaintKind::None);
    AURORA_TEST_CHECK_EQ(n.paint_kind(), static_cast<ModifierNode::PaintKind>(0));
}

AURORA_TEST_CASE(base_defaults_flex_weight_zero_and_noop_hooks) {
    const StubNode n(ModifierNode::Kind::Input);
    // 非 FlexWeight 节点权重 0（不扩展）；fire_click / on_touch 基类默认空操作（可安全调用）。
    AURORA_TEST_CHECK_NEAR(n.flex_weight(), 0.0F, 0.0F);
    AURORA_TEST_CHECK_NO_THROW(n.fire_click());
    const TouchEvent e;
    AURORA_TEST_CHECK_NO_THROW(n.on_touch(e));
}

AURORA_TEST_CASE(base_kind_enum_values_stable) {
    // 枚举值序被渲染层 switch 依赖，锁定为显式声明的 0..3。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::Kind::Layout), 0);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::Kind::Paint), 1);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::Kind::Input), 2);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::Kind::Transform), 3);
}

AURORA_TEST_CASE(base_paint_kind_enum_values_stable) {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::None), 0);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::Background), 1);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::GradientBackground), 2);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::Shadow), 3);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::Blend), 4);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::ShaderMask), 5);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::CacheLayer), 6);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::Border), 7);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::Clip), 8);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::ClipRounded), 9);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ModifierNode::PaintKind::Blur), 10);
}

AURORA_TEST_CASE(base_stub_passthrough_layout) {
    // 透明节点的 layout 契约：把约束原样交给子测量，返回其结果。
    const StubNode n(ModifierNode::Kind::Layout);
    const Constraints c{.min = Size{.width = 10.0F, .height = 20.0F}, .max = Size{.width = 30.0F, .height = 40.0F}};
    const Size s = n.layout(c, [](const Constraints &inner) {
        AURORA_TEST_CHECK_EQ(inner.min.width, 10.0F);
        AURORA_TEST_CHECK_EQ(inner.max.height, 40.0F);
        return Size{.width = 15.0F, .height = 25.0F};
    });
    AURORA_TEST_CHECK_NEAR(s.width, 15.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(s.height, 25.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_modifier_base

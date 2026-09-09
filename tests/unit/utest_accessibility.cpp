/// 测试类型: unit
/// 目标单元: include/aurora/core/accessibility.h
/// 测试说明: 覆盖角色推断映射、默认动作集、位掩码判定与无障碍树构建计数（以最小控件桩驱动）

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_accessibility {

namespace {

/// @brief 最小叶控件桩：仅补齐抽象纯虚函数，用于纯逻辑的无障碍树构建（不触发布局/绘制）。
class ProbeLeaf final : public aurora::LeafWidget {
  public:
    explicit ProbeLeaf(const char* type) : type_{type} {}

    [[nodiscard]] auto type_name() const -> const char* override { return type_; }

  protected:
    auto on_layout(const aurora::Constraints& /*c*/, const aurora::BuildContext& /*ctx*/) -> aurora::Size override {
        return {};
    }

    auto on_paint(aurora::Painter& /*p*/, const aurora::Rect& /*bounds*/, const aurora::BuildContext& /*ctx*/)
        -> void override {}

  private:
    const char* type_;
};

/// @brief 最小容器桩：type_name 固定为真实控件名 "Column"（角色推断应落到 Generic）。
class ProbeContainer final : public aurora::Container {
  public:
    [[nodiscard]] auto type_name() const -> const char* override { return "Column"; }

  protected:
    auto on_layout(const aurora::Constraints& /*c*/, const aurora::BuildContext& /*ctx*/) -> aurora::Size override {
        return {};
    }
};

}  // namespace

AURORA_TEST_CASE(infer_role_maps_known_type_names) {
    using aurora::AccessibilityRole;
    using aurora::infer_accessibility_role;

    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Button"), AccessibilityRole::Button);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Text"), AccessibilityRole::Text);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("RichText"), AccessibilityRole::Text);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Label"), AccessibilityRole::Text);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("TextInput"), AccessibilityRole::TextInput);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("RichTextEdit"), AccessibilityRole::TextInput);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Checkbox"), AccessibilityRole::Checkbox);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Switch"), AccessibilityRole::Switch);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Slider"), AccessibilityRole::Slider);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("ImageView"), AccessibilityRole::Image);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("SvgImage"), AccessibilityRole::Image);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("LazyList"), AccessibilityRole::List);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("GridView"), AccessibilityRole::List);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("ProgressIndicator"), AccessibilityRole::Progress);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Dialog"), AccessibilityRole::Dialog);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Popup"), AccessibilityRole::Dialog);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Drawer"), AccessibilityRole::Dialog);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Header"), AccessibilityRole::Header);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("AppBar"), AccessibilityRole::Header);
}

AURORA_TEST_CASE(infer_role_unknown_falls_back_to_generic) {
    using aurora::AccessibilityRole;
    using aurora::infer_accessibility_role;

    AURORA_TEST_CHECK_EQ(infer_accessibility_role("Column"), AccessibilityRole::Generic);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role("AbsolutelyUnknownWidget"), AccessibilityRole::Generic);
    AURORA_TEST_CHECK_EQ(infer_accessibility_role(""), AccessibilityRole::Generic);
}

AURORA_TEST_CASE(default_actions_per_role) {
    using aurora::AccessibilityAction;
    using aurora::AccessibilityRole;
    using aurora::default_actions;

    // 交互控件。
    const auto button = default_actions(AccessibilityRole::Button);
    AURORA_TEST_CHECK((static_cast<std::uint16_t>(button) & static_cast<std::uint16_t>(AccessibilityAction::Click)) !=
                      0);
    AURORA_TEST_CHECK((static_cast<std::uint16_t>(button) & static_cast<std::uint16_t>(AccessibilityAction::Invoke)) !=
                      0);
    AURORA_TEST_CHECK((static_cast<std::uint16_t>(button) & static_cast<std::uint16_t>(AccessibilityAction::Focus)) !=
                      0);

    // 可设值控件。
    const auto slider = default_actions(AccessibilityRole::Slider);
    AURORA_TEST_CHECK((static_cast<std::uint16_t>(slider) & static_cast<std::uint16_t>(AccessibilityAction::Value)) !=
                      0);
    // 切换控件。
    const auto checkbox = default_actions(AccessibilityRole::Checkbox);
    AURORA_TEST_CHECK(
        (static_cast<std::uint16_t>(checkbox) & static_cast<std::uint16_t>(AccessibilityAction::Toggle)) != 0);

    // 非交互角色：图片/标题/进度默认无动作。
    AURORA_TEST_CHECK_EQ(default_actions(AccessibilityRole::Image), AccessibilityAction::None);
    AURORA_TEST_CHECK_EQ(default_actions(AccessibilityRole::Header), AccessibilityAction::None);
    AURORA_TEST_CHECK_EQ(default_actions(AccessibilityRole::Progress), AccessibilityAction::None);

    // 通用容器至少可聚焦。
    AURORA_TEST_CHECK_EQ(default_actions(AccessibilityRole::Generic), AccessibilityAction::Focus);

    // 兜底契约：表外新增角色默认无动作（不再静默继承 Focus）。
    // 越界取值正是本用例被测目标（验证 default_actions 对未知角色的兜底），不可改为合法枚举值。
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto future_role = static_cast<AccessibilityRole>(200);
    AURORA_TEST_CHECK_EQ(default_actions(future_role), AccessibilityAction::None);
}

AURORA_TEST_CASE(node_defaults_and_has_action_bitmask) {
    using aurora::AccessibilityAction;
    using aurora::AccessibilityNode;

    // 默认节点：Generic 角色、无动作、空边界盒、无子节点。
    const AccessibilityNode node;
    AURORA_TEST_CHECK_EQ(node.role, aurora::AccessibilityRole::Generic);
    AURORA_TEST_CHECK_EQ(node.actions, AccessibilityAction::None);
    AURORA_TEST_CHECK_FALSE(node.has_action(AccessibilityAction::Focus));
    AURORA_TEST_CHECK(node.name.empty());
    AURORA_TEST_CHECK(node.value.empty());
    AURORA_TEST_CHECK(node.children.empty());
    AURORA_TEST_CHECK_EQ(node.bounds.size.width, 0.0F);

    // 组合位掩码：has_action 按位测试单个位。
    AccessibilityNode combined;
    combined.actions = AccessibilityAction::Focus | AccessibilityAction::Click;
    AURORA_TEST_CHECK(combined.has_action(AccessibilityAction::Focus));
    AURORA_TEST_CHECK(combined.has_action(AccessibilityAction::Click));
    AURORA_TEST_CHECK_FALSE(combined.has_action(AccessibilityAction::Value));
}

AURORA_TEST_CASE(build_tree_maps_roles_and_default_actions) {
    ProbeContainer root;
    root.add(aurora::Node{ProbeLeaf{"Button"}});
    root.add(aurora::Node{ProbeLeaf{"Text"}});

    const auto tree = aurora::build_accessibility_tree(root);
    // 根为 Column（表外类型）→ Generic + 默认 Focus。
    AURORA_TEST_CHECK_EQ(tree.role, aurora::AccessibilityRole::Generic);
    AURORA_TEST_CHECK_EQ(tree.actions, aurora::AccessibilityAction::Focus);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 2U);

    const auto& button = tree.children[0];
    AURORA_TEST_CHECK_EQ(button.role, aurora::AccessibilityRole::Button);
    AURORA_TEST_CHECK(button.has_action(aurora::AccessibilityAction::Click));
    AURORA_TEST_CHECK(button.has_action(aurora::AccessibilityAction::Invoke));
    AURORA_TEST_CHECK(button.children.empty());

    const auto& text = tree.children[1];
    AURORA_TEST_CHECK_EQ(text.role, aurora::AccessibilityRole::Text);
    AURORA_TEST_CHECK(text.has_action(aurora::AccessibilityAction::Focus));
    AURORA_TEST_CHECK_FALSE(text.has_action(aurora::AccessibilityAction::Click));
}

AURORA_TEST_CASE(build_tree_recurses_and_counts_nodes) {
    ProbeContainer inner;
    inner.add(aurora::Node{ProbeLeaf{"Header"}});

    ProbeContainer root;
    // Widget 拷贝已删除：Node 以移动接管栈上容器（make_shared 移动构造出堆上副本）。
    root.add(aurora::Node{std::move(inner)});
    root.add(aurora::Node{ProbeLeaf{"Slider"}});

    const auto tree = aurora::build_accessibility_tree(root);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 2U);

    // 嵌套容器递归展开：内层 Column → Generic，其子 Header。
    const auto& nested = tree.children[0];
    AURORA_TEST_CHECK_EQ(nested.role, aurora::AccessibilityRole::Generic);
    AURORA_TEST_REQUIRE_EQ(nested.children.size(), 1U);
    AURORA_TEST_CHECK_EQ(nested.children[0].role, aurora::AccessibilityRole::Header);

    AURORA_TEST_CHECK_EQ(tree.children[1].role, aurora::AccessibilityRole::Slider);
    // 计数含根：root + inner + Header + Slider = 4。
    AURORA_TEST_CHECK_EQ(aurora::accessibility_node_count(tree), 4U);
}

AURORA_TEST_CASE(node_count_of_leaf_is_one) {
    const aurora::AccessibilityNode leaf;
    AURORA_TEST_CHECK_EQ(aurora::accessibility_node_count(leaf), 1U);
}

}  // namespace aurora::test_cases::utest_accessibility

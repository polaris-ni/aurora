/// 测试类型: unit
/// 目标单元: include/aurora/core/accessibility.h
/// 测试说明: 覆盖角色推断映射、默认动作集、位掩码判定、无障碍树构建计数、语义几何（布局累加 /
///           绘制优先）、name/value 自填与 hook 覆写优先级（最小控件桩 + 真实控件驱动）

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/core/accessibility.h"
#include "aurora/environment/build_context.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/switch.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_accessibility {

namespace {

/// @brief 探测子项固定尺寸（SemanticTree 几何断言的基准值）。
constexpr aurora::Size kProbeItem{.width = 100.0F, .height = 20.0F};

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

/// @brief 纵向排布的容器桩：子节点按固定 `kProbeItem` 纵向落位并写回 Node 局部盒。
///        用于验证语义树几何沿子节点局部原点的逐级累加（不经任何绘制）。
class ProbeColumn final : public aurora::Container {
  public:
    [[nodiscard]] auto type_name() const -> const char* override { return "Column"; }

  protected:
    auto on_layout(const aurora::Constraints& /*c*/, const aurora::BuildContext& /*ctx*/) -> aurora::Size override {
        float y = 0.0F;
        for (auto& child : children_) {
            child.set_bounds(aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = y}, .size = kProbeItem});
            y += kProbeItem.height;
        }
        return aurora::Size{.width = kProbeItem.width, .height = y};
    }
};

/// @brief 无 `Node` 几何的虚拟化容器桩：子节点存在私有表中、只经 `for_each_child` 暴露
///        （LazyList / NavigatorHost 等真实容器的同构简化），用于锁定语义树的兜底展开路径。
class ProbeVirtualList final : public aurora::Widget {
  public:
    ProbeVirtualList() : item_{std::make_shared<ProbeLeaf>("Text")} {}

    [[nodiscard]] auto type_name() const -> const char* override { return "LazyList"; }

    auto for_each_child(const std::function<void(const aurora::Widget&)>& fn) const -> void override { fn(*item_); }

  protected:
    auto on_layout(const aurora::Constraints& /*c*/, const aurora::BuildContext& /*ctx*/) -> aurora::Size override {
        return aurora::Size{.width = 200.0F, .height = 300.0F};
    }

    auto on_paint(aurora::Painter& /*p*/, const aurora::Rect& /*bounds*/, const aurora::BuildContext& /*ctx*/)
        -> void override {}

  private:
    std::shared_ptr<aurora::Widget> item_;
};

class ProbeAnnotatedLeaf final : public aurora::LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char* override { return "Button"; }
    [[nodiscard]] auto accessibility_label() const -> std::string override { return "提交订单"; }
    [[nodiscard]] auto accessibility_value() const -> std::string override { return "已就绪"; }
    [[nodiscard]] auto accessibility_hint() const -> std::string override { return "回车提交"; }

  protected:
    auto on_layout(const aurora::Constraints& /*c*/, const aurora::BuildContext& /*ctx*/) -> aurora::Size override {
        return {};
    }

    auto on_paint(aurora::Painter& /*p*/, const aurora::Rect& /*bounds*/, const aurora::BuildContext& /*ctx*/)
        -> void override {}
};

/// @brief 进程级无障碍设置的 RAII 复原守卫：同一进程串跑多个用例，改动必须还原。
class ScopedSettings final {
  public:
    ScopedSettings() : saved_{aurora::current_accessibility_settings()} {}
    ~ScopedSettings() { aurora::set_accessibility_settings(saved_); }

    ScopedSettings(const ScopedSettings&) = delete;
    auto operator=(const ScopedSettings&) -> ScopedSettings& = delete;
    ScopedSettings(ScopedSettings&&) = delete;
    auto operator=(ScopedSettings&&) -> ScopedSettings& = delete;

  private:
    aurora::AccessibilitySettings saved_;
};

/// @brief 无障碍事件处理器的 RAII 安装/卸载守卫。
///
/// lambda 捕获的是用例栈上的收集向量指针：析构先卸载处理器再让向量自然销毁，
/// 故回调期间指针必然有效（事件来源控件也活在同一作用域内）。
class ScopedEventHandler final {
  public:
    explicit ScopedEventHandler(std::vector<aurora::AccessibilityEvent>* out) {
        aurora::set_accessibility_event_handler(
            [out](const aurora::AccessibilityEvent& e) -> void { out->push_back(e); });
    }
    ~ScopedEventHandler() { aurora::set_accessibility_event_handler(nullptr); }

    ScopedEventHandler(const ScopedEventHandler&) = delete;
    auto operator=(const ScopedEventHandler&) -> ScopedEventHandler& = delete;
    ScopedEventHandler(ScopedEventHandler&&) = delete;
    auto operator=(ScopedEventHandler&&) -> ScopedEventHandler& = delete;
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

AURORA_TEST_CASE(build_tree_fills_bounds_from_child_local_boxes) {
    ProbeColumn column;
    column.add(aurora::Node{ProbeLeaf{"Button"}});
    column.add(aurora::Node{ProbeLeaf{"Text"}});
    aurora::LayoutEngine::layout(column, aurora::Constraints{.min = aurora::Size{},
                                                             .max = aurora::Size{.width = 400.0F, .height = 400.0F}});

    const auto tree = aurora::build_accessibility_tree(column);
    // 根：未绘制 → 取布局结果置于原点（100 × 40）。
    AURORA_TEST_CHECK_NEAR(tree.bounds.size.width, kProbeItem.width, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.bounds.size.height, 2.0F * kProbeItem.height, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.bounds.origin.x, 0.0F, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.bounds.origin.y, 0.0F, 1e-5F);

    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 2U);
    // 子节点几何 = 根原点 + Node 局部原点累加（首项 y=0、次项 y=20）。
    AURORA_TEST_CHECK_NEAR(tree.children[0].bounds.origin.y, 0.0F, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.children[1].bounds.origin.y, kProbeItem.height, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.children[0].bounds.size.width, kProbeItem.width, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.children[1].bounds.size.height, kProbeItem.height, 1e-5F);
}

AURORA_TEST_CASE(build_tree_prefers_painted_bounds_over_layout_box) {
    ProbeColumn column;
    column.add(aurora::Node{ProbeLeaf{"Button"}});
    aurora::LayoutEngine::layout(column, aurora::Constraints{.min = aurora::Size{},
                                                             .max = aurora::Size{.width = 400.0F, .height = 400.0F}});

    // 绘制盒（含祖先偏移）优先于布局累加盒：present 过的树几何即真实屏幕坐标。
    aurora::Painter painter;
    painter.begin(256, 256);
    constexpr aurora::Point root_origin{.x = 12.0F, .y = 34.0F};
    column.paint(painter,
                 aurora::Rect{.origin = root_origin, .size = aurora::Size{.width = 100.0F, .height = 20.0F}},
                 aurora::BuildContext{});

    const auto tree = aurora::build_accessibility_tree(column);
    AURORA_TEST_CHECK_NEAR(tree.bounds.origin.x, root_origin.x, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.bounds.origin.y, root_origin.y, 1e-5F);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 1U);
    AURORA_TEST_CHECK_NEAR(tree.children[0].bounds.origin.x, root_origin.x, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.children[0].bounds.origin.y, root_origin.y, 1e-5F);
}

AURORA_TEST_CASE(build_tree_honours_explicit_root_box) {
    ProbeLeaf leaf{"Button"};
    const aurora::Rect given{.origin = aurora::Point{.x = 5.0F, .y = 7.0F},
                             .size = aurora::Size{.width = 42.0F, .height = 21.0F}};
    const auto tree = aurora::build_accessibility_tree(leaf, given);
    AURORA_TEST_CHECK_NEAR(tree.bounds.origin.x, given.origin.x, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.bounds.size.width, given.size.width, 1e-5F);
}

AURORA_TEST_CASE(build_tree_self_fills_name_and_value_from_widgets) {
    // 控件自带语义：Button 取 label、Text 取文本、Progress 取数值（无需布局/绘制即可自填）。
    aurora::Button button{std::string{"确定"}};
    AURORA_TEST_CHECK_EQ(aurora::build_accessibility_tree(button).name, std::string{"确定"});

    aurora::Text text{std::string{"订单总额"}};
    AURORA_TEST_CHECK_EQ(aurora::build_accessibility_tree(text).name, std::string{"订单总额"});

    aurora::Checkbox checked{aurora::Reactive<bool>{true}};
    AURORA_TEST_CHECK_EQ(aurora::build_accessibility_tree(checked).value, std::string{"true"});

    aurora::Switch off{aurora::Reactive<bool>{false}};
    AURORA_TEST_CHECK_EQ(aurora::build_accessibility_tree(off).value, std::string{"false"});

    aurora::Slider slider{aurora::Reactive<double>{0.5}};
    AURORA_TEST_CHECK_EQ(aurora::build_accessibility_tree(slider).value, std::to_string(0.5));

    aurora::ProgressIndicator progress{aurora::Reactive<double>{0.25}};
    AURORA_TEST_CHECK_EQ(aurora::build_accessibility_tree(progress).value, std::to_string(0.25));

    // 编辑框：name 取占位提示、value 取当前内容，两者互不相干。
    aurora::TextInput input;
    input.set_placeholder("收货人");
    input.set_value("张三");
    const auto input_tree = aurora::build_accessibility_tree(input);
    AURORA_TEST_CHECK_EQ(input_tree.name, std::string{"收货人"});
    AURORA_TEST_CHECK_EQ(input_tree.value, std::string{"张三"});
}

AURORA_TEST_CASE(build_tree_prefers_widget_hooks_and_exposes_hint) {
    ProbeAnnotatedLeaf annotated;
    const auto tree = aurora::build_accessibility_tree(annotated);
    AURORA_TEST_CHECK_EQ(tree.role, aurora::AccessibilityRole::Button);
    AURORA_TEST_CHECK_EQ(tree.name, std::string{"提交订单"});
    AURORA_TEST_CHECK_EQ(tree.value, std::string{"已就绪"});
    AURORA_TEST_CHECK_EQ(tree.hint, std::string{"回车提交"});

    // 未覆写的控件：三个钩子均为空串（保留给宿主自行覆写）。
    ProbeLeaf plain{"Slider"};
    const auto plain_tree = aurora::build_accessibility_tree(plain);
    AURORA_TEST_CHECK(plain_tree.name.empty());
    AURORA_TEST_CHECK(plain_tree.value.empty());
    AURORA_TEST_CHECK(plain_tree.hint.empty());
}

AURORA_TEST_CASE(build_tree_skips_invisible_children) {
    ProbeColumn column;
    column.add(aurora::Node{ProbeLeaf{"Button"}});
    column.add(aurora::Node{ProbeLeaf{"Text"}});
    column.child(1).widget().show.set(false);

    // 与绘制 / 命中同口径：show == false 的子树不进语义树，避免朗读视觉上不存在的控件。
    const auto tree = aurora::build_accessibility_tree(column);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 1U);
    AURORA_TEST_CHECK_EQ(tree.children[0].role, aurora::AccessibilityRole::Button);
}

AURORA_TEST_CASE(build_tree_expands_children_without_node_geometry) {
    ProbeVirtualList list;
    aurora::LayoutEngine::layout(list, aurora::Constraints{.min = aurora::Size{},
                                                           .max = aurora::Size{.width = 200.0F, .height = 300.0F}});

    const auto tree = aurora::build_accessibility_tree(list);
    AURORA_TEST_CHECK_EQ(tree.role, aurora::AccessibilityRole::List);
    // 兜底展开：子树不被丢掉；几何不可得 ⇒ 后代继承容器盒。
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 1U);
    AURORA_TEST_CHECK_EQ(tree.children[0].role, aurora::AccessibilityRole::Text);
    AURORA_TEST_CHECK_NEAR(tree.children[0].bounds.origin.x, tree.bounds.origin.x, 1e-5F);
    AURORA_TEST_CHECK_NEAR(tree.children[0].bounds.size.width, tree.bounds.size.width, 1e-5F);
}

AURORA_TEST_CASE(settings_default_is_neutral_and_font_scale_clamps_illegal) {
    const ScopedSettings guard;
    aurora::set_accessibility_settings(aurora::AccessibilitySettings{});

    const auto& s = aurora::current_accessibility_settings();
    AURORA_TEST_CHECK_FALSE(s.reduce_motion);
    AURORA_TEST_CHECK_FALSE(s.high_contrast);
    AURORA_TEST_CHECK_FALSE(s.screen_reader_active);
    AURORA_TEST_CHECK_NEAR(s.resolved_font_scale(), 1.0F, 1e-6F);

    // 非法倍率 → 回落 1.0（不污染度量）；合法倍率原样保留。
    AURORA_TEST_CHECK_NEAR(aurora::AccessibilitySettings{.font_scale = 0.0F}.resolved_font_scale(), 1.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(aurora::AccessibilitySettings{.font_scale = -2.0F}.resolved_font_scale(), 1.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(aurora::AccessibilitySettings{.font_scale = 1.5F}.resolved_font_scale(), 1.5F, 1e-6F);
}

AURORA_TEST_CASE(environment_injection_overrides_process_default) {
    const ScopedSettings guard;
    aurora::set_accessibility_settings(aurora::AccessibilitySettings{.font_scale = 1.5F});

    aurora::Environment env;
    env.set_local(aurora::AccessibilitySettings{.reduce_motion = true, .font_scale = 2.5F});
    aurora::BuildContext ctx;
    ctx.env = &env;

    // Environment 注入优先于进程级默认值。
    const auto injected = aurora::resolved_accessibility_settings(ctx);
    AURORA_TEST_CHECK(injected.reduce_motion);
    AURORA_TEST_CHECK_NEAR(injected.resolved_font_scale(), 2.5F, 1e-6F);

    // 未注入 ⇒ 回落进程级默认。
    const aurora::BuildContext bare{};
    AURORA_TEST_CHECK_NEAR(aurora::resolved_accessibility_settings(bare).resolved_font_scale(), 1.5F, 1e-6F);
}

AURORA_TEST_CASE(reduce_motion_snaps_controller_to_endpoint) {
    const ScopedSettings guard;

    // 常态：按时间推进，产生中间值。
    aurora::set_accessibility_settings(aurora::AccessibilitySettings{});
    aurora::AnimationController gradual{1.0};
    gradual.forward();
    gradual.tick(0.016);
    AURORA_TEST_CHECK(gradual.is_animating());
    AURORA_TEST_CHECK_NEAR(gradual.value(), 0.016, 1e-6);

    // 减弱动态效果：不再渐变，单帧直落本次播放的目标端点（状态机与「播完」一致）。
    aurora::set_accessibility_settings(aurora::AccessibilitySettings{.reduce_motion = true});

    aurora::AnimationController forward_ctl{1.0};
    forward_ctl.forward();
    forward_ctl.tick(0.0);
    AURORA_TEST_CHECK_NEAR(forward_ctl.value(), 1.0, 1e-6);
    AURORA_TEST_CHECK(forward_ctl.is_completed());
    AURORA_TEST_CHECK(forward_ctl.dirty());

    aurora::AnimationController backward_ctl{1.0};
    backward_ctl.forward();
    backward_ctl.tick(1.0);
    backward_ctl.reverse();
    backward_ctl.tick(0.0);
    AURORA_TEST_CHECK_NEAR(backward_ctl.value(), 0.0, 1e-6);
    AURORA_TEST_CHECK(backward_ctl.is_dismissed());

    // 静止的控制器不受影响：reduce_motion 不应凭空驱动动画。
    aurora::AnimationController idle{1.0};
    idle.tick(0.016);
    AURORA_TEST_CHECK_FALSE(idle.dirty());
    AURORA_TEST_CHECK_NEAR(idle.value(), 0.0, 1e-6);
}

AURORA_TEST_CASE(font_scale_scales_text_layout_geometry) {
    const ScopedSettings guard;
    const aurora::Constraints bounded{.min = aurora::Size{}, .max = aurora::Size{.width = 200.0F, .height = 400.0F}};

    aurora::set_accessibility_settings(aurora::AccessibilitySettings{});
    aurora::Text base{std::string{"Aurora"}};
    base.font.size_pt = 14.0F;
    aurora::LayoutEngine::layout(base, bounded);

    aurora::set_accessibility_settings(aurora::AccessibilitySettings{.font_scale = 2.0F});
    aurora::Text scaled{std::string{"Aurora"}};
    scaled.font.size_pt = 14.0F;
    aurora::LayoutEngine::layout(scaled, bounded);

    // 字号倍率生效 ⇒ 度量结果变大（相对比较，不依赖具体字体/度量后端）。
    AURORA_TEST_CHECK(base.size().height > 0.0F);
    AURORA_TEST_CHECK(scaled.size().height > base.size().height);
    AURORA_TEST_CHECK(scaled.size().width > base.size().width);
}

AURORA_TEST_CASE(focus_value_and_structure_changes_raise_events) {
    std::vector<aurora::AccessibilityEvent> events;
    const ScopedEventHandler listen{&events};

    aurora::Checkbox box{aurora::Reactive<bool>{false}};
    box.set_value(true);

    ProbeColumn column;
    column.on_focus_change(true);

    column.add(aurora::Node{ProbeLeaf{"Button"}});

    AURORA_TEST_REQUIRE_EQ(events.size(), 3U);
    AURORA_TEST_CHECK_EQ(events[0].kind, aurora::AccessibilityEventKind::ValueChanged);
    AURORA_TEST_CHECK_EQ(events[0].target, &box);
    AURORA_TEST_CHECK_EQ(events[1].kind, aurora::AccessibilityEventKind::FocusChanged);
    AURORA_TEST_CHECK_EQ(events[1].target, &column);
    AURORA_TEST_CHECK_EQ(events[2].kind, aurora::AccessibilityEventKind::StructureChanged);
    AURORA_TEST_CHECK_EQ(events[2].target, &column);

    // 语义树照常可用：事件通道与树构建互不干扰。
    const auto tree = aurora::build_accessibility_tree(column);
    AURORA_TEST_REQUIRE_EQ(tree.children.size(), 1U);
}

AURORA_TEST_CASE(text_input_edits_raise_value_changed) {
    std::vector<aurora::AccessibilityEvent> events;
    const ScopedEventHandler listen{&events};

    aurora::TextInput input;
    input.on_focus_change(true);  // 文本必经 FocusManager 落到焦点控件（顺带触发焦点事件）
    aurora::TextInputEvent commit;
    commit.text = "A";
    input.on_text_input(commit);

    // 焦点转移与内容变化各一条：FocusChanged → ValueChanged，顺序即发生顺序。
    AURORA_TEST_CHECK_EQ(input.value(), std::string{"A"});
    AURORA_TEST_REQUIRE_EQ(events.size(), 2U);
    AURORA_TEST_CHECK_EQ(events[0].kind, aurora::AccessibilityEventKind::FocusChanged);
    AURORA_TEST_CHECK_EQ(events[1].kind, aurora::AccessibilityEventKind::ValueChanged);
    AURORA_TEST_CHECK_EQ(events[1].target, &input);
}

}  // namespace aurora::test_cases::utest_accessibility

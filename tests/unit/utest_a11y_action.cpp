/// 测试类型: unit
/// 目标单元: include/aurora/widget/widget.h（perform_accessibility_action）、include/aurora/event/focus.h（resolve_focus_manager）
/// 测试说明: 读屏动作通道的默认路由：Focus 经 resolve_focus_manager 落焦（G1，不依赖派发栈）、
///           Click/Invoke 走同口径命中测试 + press/release 两段派发、Scroll* 转滚轮增量（G32）、
///           语义强相关动作（Toggle/Value/Select）基类不支持；以及无焦点管理器时的降级

#include <cstdint>
#include <memory>
#include <utility>

#include "aurora/core/accessibility.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/focus.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_a11y_action {

namespace {

/// @brief 可聚焦叶控件桩：固定尺寸、可命中，记录收到的指针/滚动事件。
class ActionProbe final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char *override { return "ActionProbe"; }

    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override {
        size_ = Size{.width = 100.0F, .height = 40.0F};
        return size_;
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            ++press_count;
        }
        Widget::on_pointer_event(e);
    }
    auto on_scroll(ScrollEvent &e) -> void override {
        last_delta_y = e.delta_y;
        last_delta_x = e.delta_x;
        ++scroll_count;
        e.is_handled = true;
    }
    auto on_focus_change(bool focused) -> void override {
        if (focused) {
            ++gained;
        }
        Widget::on_focus_change(focused);
    }

    /// @brief 声明为滚动目标（滚轮派发的落点判据）：否则 `EventDispatcher` 的滚轮路由不会落到这里。
    [[nodiscard]] auto wants_scroll() const -> bool override { return true; }

    int press_count = 0;
    int scroll_count = 0;
    int gained = 0;
    float last_delta_y = 0.0F;
    float last_delta_x = 0.0F;
};

/// @brief 零尺寸控件桩：几何退化 → 命中测试恒空、滚动量恒零（动作应被拒）。
class ZeroProbe final : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char *override { return "ZeroProbe"; }

  protected:
    auto on_layout(const Constraints & /*c*/, const BuildContext & /*ctx*/) -> Size override { return {}; }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

/// @brief 派发期焦点管理器槽位的 RAII 复原（全局状态，必须还原）。
class ScopedFocusManager final {
  public:
    explicit ScopedFocusManager(FocusManager *fm) : saved_{current_focus_manager()} {
        set_current_focus_manager(fm);
    }
    ~ScopedFocusManager() { set_current_focus_manager(saved_); }

    ScopedFocusManager(const ScopedFocusManager &) = delete;
    auto operator=(const ScopedFocusManager &) -> ScopedFocusManager & = delete;
    ScopedFocusManager(ScopedFocusManager &&) = delete;
    auto operator=(ScopedFocusManager &&) -> ScopedFocusManager & = delete;

  private:
    FocusManager *saved_;
};

/// @brief 走一次真实布局：动作通道读的是布局产物 `size_`（未布局即零尺寸 → 动作拒收）。
[[nodiscard]] auto laid_out(const std::shared_ptr<Widget> &w) -> std::shared_ptr<Widget> {
    (void)w->layout(Constraints{}, BuildContext{});
    return w;
}

}  // namespace

AURORA_TEST_CASE(focus_action_reaches_widget_outside_dispatch_stack) {
    // G1：读屏回调不在派发栈内，`current_focus_manager()` 恒空 —— 必须能自行解析焦点管理器。
    set_current_focus_manager(nullptr);
    const auto probe = std::make_shared<ActionProbe>();
    probe->set_tab_index(0);

    const bool ok = probe->perform_accessibility_action({.action = AccessibilityAction::Focus});
    AURORA_TEST_CHECK_TRUE(ok);
    AURORA_TEST_CHECK_GE(probe->gained, 1);
}

AURORA_TEST_CASE(resolve_focus_manager_prefers_dispatch_stack_manager) {
    FocusManager explicit_manager;
    FocusManager *saved = current_focus_manager();
    {
        const ScopedFocusManager scope{&explicit_manager};
        const auto probe = std::make_shared<ActionProbe>();
        AURORA_TEST_CHECK_EQ(resolve_focus_manager(*probe), &explicit_manager);
    }
    AURORA_TEST_CHECK_EQ(current_focus_manager(), saved);  // 作用域退出即复原
}

AURORA_TEST_CASE(resolve_focus_manager_is_stable_per_root) {
    set_current_focus_manager(nullptr);
    const auto root = std::make_shared<ActionProbe>();
    FocusManager *first = resolve_focus_manager(*root);
    FocusManager *second = resolve_focus_manager(*root);
    AURORA_TEST_CHECK_NOT_NULL(first);
    AURORA_TEST_CHECK_EQ(first, second);  // 同根复用同一实例（焦点状态不分裂）
}

AURORA_TEST_CASE(click_action_dispatches_press_and_release) {
    set_current_focus_manager(nullptr);
    const auto probe = std::make_shared<ActionProbe>();
    (void)laid_out(probe);
    const bool ok = probe->perform_accessibility_action({.action = AccessibilityAction::Click});
    AURORA_TEST_CHECK_TRUE(ok);
    AURORA_TEST_CHECK_GE(probe->press_count, 1);
}

AURORA_TEST_CASE(invoke_action_shares_click_route) {
    set_current_focus_manager(nullptr);
    const auto probe = std::make_shared<ActionProbe>();
    (void)laid_out(probe);
    AURORA_TEST_CHECK_TRUE(probe->perform_accessibility_action({.action = AccessibilityAction::Invoke}));
    AURORA_TEST_CHECK_GE(probe->press_count, 1);
}

AURORA_TEST_CASE(zero_size_widget_rejects_click_and_scroll) {
    set_current_focus_manager(nullptr);
    const auto probe = std::make_shared<ZeroProbe>();
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::Click}));
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::ScrollDown}));
}

AURORA_TEST_CASE(scroll_actions_dispatch_wheel_equivalent_delta) {
    set_current_focus_manager(nullptr);
    const auto probe = std::make_shared<ActionProbe>();
    (void)laid_out(probe);
    AURORA_TEST_CHECK_TRUE(probe->perform_accessibility_action({.action = AccessibilityAction::ScrollDown}));
    AURORA_TEST_CHECK_GE(probe->scroll_count, 1);
    AURORA_TEST_CHECK_LT(probe->last_delta_y, 0.0F);  // 下滚：delta 为负（与滚轮同约定）

    probe->last_delta_y = 0.0F;
    AURORA_TEST_CHECK_TRUE(probe->perform_accessibility_action({.action = AccessibilityAction::ScrollUp}));
    AURORA_TEST_CHECK_GT(probe->last_delta_y, 0.0F);

    AURORA_TEST_CHECK_TRUE(probe->perform_accessibility_action({.action = AccessibilityAction::ScrollRight}));
    AURORA_TEST_CHECK_LT(probe->last_delta_x, 0.0F);
}

AURORA_TEST_CASE(semantic_actions_are_not_supported_by_base) {
    const auto probe = std::make_shared<ActionProbe>();
    // 语义强相关：必须由控件显式覆写（Checkbox/Switch → Toggle、Slider → Value）。
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::Toggle}));
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::Value}));
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::Select}));
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::ScrollIntoView}));
    AURORA_TEST_CHECK_FALSE(probe->perform_accessibility_action({.action = AccessibilityAction::None}));
}

}  // namespace aurora::test_cases::utest_a11y_action

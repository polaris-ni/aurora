/// 测试类型: integration
/// 目标单元: include/aurora/widget/widget.h + include/aurora/event/focus.h + include/aurora/navigation/navigator_host.h
/// 测试说明: widget 生命周期 / UAF 回归集成——自杀式点击（on_click 释放最后一个
/// shared_ptr 后派发链不悬垂）、焦点控件先于 FocusManager 被回收（live_focused 存活
/// 视图，按键/文本派发安全返回 false 且不虚调用）、栈/成员控件 weak_from_this 为空
/// 时回退裸指针路径、NavigatorHost 先于 Animator 析构时控制器自动注销、
/// Animator::remove 幂等与未登记者无操作

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/animation/timeline.h"
#include "aurora/core/color.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_widget_lifetime {

namespace {

/// 可聚焦叶控件：记录获焦/失焦次数，用于焦点悬垂断言。
class FocusLeaf final : public LeafWidget {
  public:
    int gained = 0;
    int lost = 0;
    /// activate() 计数出口：指向调用方栈变量，故本控件析构后仍可安全读取，
    /// 用于断言「已回收的焦点控件不再被虚调用」而无需解引用已释放对象。
    int *activate_sink = nullptr;

    auto on_focus_change(bool focused) -> void override {
        if (focused) {
            ++gained;
        } else {
            ++lost;
        }
    }
    auto activate() -> void override {
        if (activate_sink != nullptr) {
            ++*activate_sink;
        }
    }
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FocusLeaf"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FocusLeaf", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 40.0F, .height = 20.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
};

/// 整屏纯色页（NavigatorHost 转场用）。
class SolidPage final : public Widget {
  public:
    Color bg;
    explicit SolidPage(Color c) : bg(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, bg);
    }
};

/// 在 root 上跑一次完整的 Press+Release（触发 Button::activate → on_click）。
auto click_at(Widget &root, const Point &p, FocusManager *fm) -> void {
    MouseEvent press;
    press.position = p;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    EventDispatcher::dispatch(root, press, fm);

    MouseEvent release;
    release.position = p;
    release.action = MouseAction::Release;
    release.button = MouseButton::Left;
    EventDispatcher::dispatch(root, release, fm);
}

/// 布局 + 绘制一遍，让命中链的 bounds 生效（ctx 生命周期覆盖整次派发）。
auto realize(Widget &root, Painter &p, int w, int h) -> void {
    const BuildContext ctx;
    root.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F},
                         .max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}};
    root.layout(cc, ctx);
    root.paint(p,
               Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                    .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
               ctx);
}

}  // namespace

AURORA_TEST_CASE(suicidal_click_clears_subtree_safely) {
    // on_click 丢掉持有自己的最后一个 shared_ptr：派发链裸指针在回调返回后
    // 不得再写入（keepalive 语义）。
    auto col = std::make_shared<Column>();
    auto btn = std::make_shared<Button>("kill me");
    std::shared_ptr<Button> holder = btn;  // btn 在树外的唯一额外强引用
    int clicks = 0;
    btn->set_on_click([&col, &holder, &clicks]() {
        ++clicks;
        // 清空子树：丢掉树内对 btn 的强引用（模拟 push_replacement 重建页面）。
        col->adopt_children(std::vector<Node>{});
        holder.reset();  // 丢掉最后一个外部强引用 → 若无 keepalive，btn 此刻即释放
    });
    col->add(Node{btn});
    btn.reset();  // 之后 btn 只由 col 子树 + holder 持有

    Painter painter;
    painter.begin(200, 200);
    realize(*col, painter, 200, 200);
    const Rect bb = col->child_nodes()[0].bounds();
    const Point center{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};

    click_at(*col, center, nullptr);
    AURORA_TEST_CHECK_EQ(clicks, 1);
    AURORA_TEST_CHECK_EQ(col->child_count(), 0U);
    AURORA_TEST_CHECK_TRUE(holder == nullptr);  // 外部强引用已释放（控件确在回调中销毁）
}

AURORA_TEST_CASE(reclaimed_focus_widget_is_detected_not_dereferenced) {
    // 焦点控件被回收后：focused() 返回 nullptr、has_focus 为 false、按键/文本派发
    // 安全返回 false 且不再虚调用 activate()。
    FocusManager fm;
    Column dummy_root;
    int activations = 0;
    auto leaf = std::make_shared<FocusLeaf>();
    leaf->activate_sink = &activations;
    fm.set_focus(leaf.get());
    AURORA_TEST_CHECK_TRUE(fm.focused() == leaf.get());
    AURORA_TEST_CHECK_TRUE(fm.has_focus(leaf.get()));
    AURORA_TEST_CHECK_EQ(leaf->gained, 1);

    // 前置证明：控件存活时 Enter 确实走到 focused->activate() 的虚调用路径。
    KeyEvent live_enter;
    live_enter.action = KeyAction::Down;
    live_enter.key = static_cast<int>(KeyCode::Enter);
    AURORA_TEST_CHECK_TRUE(EventDispatcher::dispatch(dummy_root, live_enter, fm));
    AURORA_TEST_CHECK_EQ(activations, 1);

    const Widget *raw = leaf.get();
    leaf.reset();  // 焦点控件被回收，FocusManager 仍留有记录

    AURORA_TEST_CHECK_NULL(fm.focused());
    AURORA_TEST_CHECK_FALSE(fm.has_focus(raw));

    KeyEvent dead_enter;
    dead_enter.action = KeyAction::Down;
    dead_enter.key = static_cast<int>(KeyCode::Enter);
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(dummy_root, dead_enter, fm));
    KeyEvent dead_space;
    dead_space.action = KeyAction::Down;
    dead_space.key = static_cast<int>(KeyCode::Space);
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(dummy_root, dead_space, fm));
    AURORA_TEST_CHECK_EQ(activations, 1);  // 仍是存活期的那一次

    // 按键 / 文本派发遇到已回收焦点：安全返回 false，绝不虚调用。
    KeyEvent ke;
    ke.action = KeyAction::Down;
    ke.key = static_cast<int>(KeyCode::Backspace);
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(dummy_root, ke, fm));

    TextInputEvent te;
    te.text = "x";
    AURORA_TEST_CHECK_FALSE(EventDispatcher::dispatch(dummy_root, te, fm));

    // 文本派发的早退分支必须复原 thread-local。
    AURORA_TEST_CHECK_NULL(current_focus_manager());

    // 清焦点不应对已释放控件发 on_focus_change(false)。
    fm.clear();
    AURORA_TEST_CHECK_NULL(fm.focused());
}

AURORA_TEST_CASE(stack_widget_fallback_still_dispatches) {
    // 栈/成员控件未被 shared_ptr 持有（weak_from_this 为空弱引用）：
    // 不得因 guard 为空而丢弃事件。
    FocusManager fm;
    FocusLeaf stack_leaf;  // 栈对象
    fm.set_focus(&stack_leaf);
    AURORA_TEST_CHECK_TRUE(fm.focused() == &stack_leaf);
    AURORA_TEST_CHECK_TRUE(fm.has_focus(&stack_leaf));
    AURORA_TEST_CHECK_EQ(stack_leaf.gained, 1);

    // 栈上根 + shared 子控件的点击派发不被静默吞掉。
    Column col;
    auto btn = std::make_shared<Button>("stack tree");
    int clicks = 0;
    btn->set_on_click([&clicks]() { ++clicks; });
    col.add(Node{btn});

    Painter painter;
    painter.begin(200, 200);
    realize(col, painter, 200, 200);
    const Rect bb = col.child_nodes()[0].bounds();
    const Point center{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};
    click_at(col, center, &fm);
    AURORA_TEST_CHECK_EQ(clicks, 1);
}

AURORA_TEST_CASE(navigator_host_destruction_unregisters_from_animator) {
    // host 析构 → ~NavigatorHost 调 anim.remove(ctrl_)；若未注销，
    // 析构后的 tick 会写已释放内存（ASan 下 heap-use-after-free）。
    Animator anim;
    RouteTransition fade;
    fade.animated = true;
    fade.kind = TransitionKind::Fade;
    fade.duration_seconds = 0.4;

    {
        NavigatorHost host{anim};
        const BuildContext ctx;
        host.mount(ctx);
        host.push(Route{Node{SolidPage{Color::red()}}, "a", fade});
        host.push(Route{Node{SolidPage{Color::blue()}}, "b", fade});  // 触发 begin_transition
        anim.tick(0.1);
        AURORA_TEST_CHECK_TRUE(anim.has_active());  // 转场进行中
    }

    anim.tick(0.1);
    anim.tick(0.1);
    AURORA_TEST_CHECK_FALSE(anim.has_active());
}

AURORA_TEST_CASE(animator_remove_is_idempotent_and_stops_writes) {
    Animator anim;
    AnimationController c{1.0};
    State target{0.0};
    anim.bind(c, Tween{0.0, 1.0}, target);
    c.forward(0.0);
    anim.tick(0.5);
    AURORA_TEST_CHECK_TRUE(target.get() > 0.0);  // tick 写入绑定 State

    anim.remove(c);
    const double frozen = target.get();
    anim.tick(0.5);
    AURORA_TEST_CHECK_EQ(target.get(), frozen);  // 注销后 tick 不再写

    anim.remove(c);  // 重复注销幂等
    AnimationController never{1.0};  // 从未登记
    anim.remove(never);
    AURORA_TEST_CHECK_FALSE(anim.has_active());
}

}  // namespace aurora::test_cases::itest_widget_lifetime

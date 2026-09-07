/// 测试类型: unit
/// 目标单元: include/aurora/widget/widget.h
/// 测试说明: utest_widget_lifetime 单元测试
///
// 目标源单元（历史映射，保留供审计）: Widget + Widget
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// Containers(Column/Row/Stack 等，经 popup/splitter/tab_bar 等容器用例与本文件行使)、
// ExpansionPanel、ImageWidget、Timer(Timer 控件，经本文件 hooks/components 段及
// test_timers 行使)、 NavigatorHost(NavigatorHost 宿主操作)、TransitionLayer(转场图层)。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/aurora.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/flex.h"
#include "aurora/layout/flex_layouter.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/switch.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora_test_harness.h"

using au::AnimationController;
using au::Animator;
using au::BuildContext;
using au::Button;
using au::Checkbox;
using au::Color;
using au::Column;
using au::Constraints;
using au::CrossAxisAlignment;
using au::current_focus_manager;
using au::EventDispatcher;
using au::Flex;
using au::FlexDirection;
using au::FlexItem;
using au::FlexLayouter;
using au::FocusManager;
using au::Json;
using au::KeyAction;
using au::KeyCode;
using au::KeyEvent;
using au::LayoutCtxBase;
using au::MainAxisAlignment;
using au::MainAxisSize;
using au::Modifier;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::NavigatorHost;
using au::Node;
using au::Painter;
using au::Point;
using au::ProgressIndicator;
using au::Reactive;
using au::Rect;
using au::Route;
using au::RouteTransition;
using au::Row;
using au::RowProps;
using au::SignalViewBase;
using au::Size;
using au::Slider;
using au::Stack;
using au::State;
using au::Switch;
using au::Text;
using au::TextInputEvent;
using au::TextProps;
using au::TransitionKind;
using au::Tween;
using au::Widget;
using au::WidgetDescriptor;

// （自 utest_widget.cpp 拆分：生命周期/UAF 回归段）

namespace aurora::test_cases::utest_widget_lifetime {

namespace colors = aurora::colors;
using au::Alignment;
using au::ColumnProps;
using au::Divider;
using au::DividerProps;
using au::LeafWidget;
using au::LocalizedString;
using au::Orientation;

namespace {

/// @brief 可聚焦叶控件：记录获焦/失焦次数，用于焦点悬垂断言。
class FocusLeaf : public LeafWidget {
  public:
    int gained = 0;
    int lost = 0;
    /// @brief activate() 计数出口：指向调用方栈变量，故本控件析构后仍可安全读取，
    ///        用于断言「已回收的焦点控件不再被虚调用」而无需解引用已释放对象。
    int *activate_sink = nullptr;
    void on_focus_change(bool focused) override {
        if (focused) {
            ++gained;
        } else {
            ++lost;
        }
    }
    void activate() override {
        if (activate_sink != nullptr) {
            ++*activate_sink;
        }
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FocusLeaf"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FocusLeaf", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 40.0F, .height = 20.0F});
    }
    void on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}
};

/// @brief 整屏纯色页（NavigatorHost 转场用）。
struct SolidPage : Widget {
    Color bg;
    explicit SolidPage(Color c) : bg(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    void on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) override { p.fill_rect(bounds, bg); }
};

/// @brief 在 root 上跑一次完整的 Press+Release（触发 Button::activate → on_click）。
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

/// @brief 布局 + 绘制一遍，让命中链的 bounds 生效。ctx 由调用方持有（须活过整次派发）。
auto realize(Widget &root, Painter &p, BuildContext const &ctx, const int w, const int h) -> void {
    root.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    root.layout(cc, ctx);
    root.paint(p,
               Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                    .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
               ctx);
}

}  // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_lifetime_uaf ===\n");

    Painter painter;
    painter.begin(200, 200);

    // ---- 1) 自杀式点击：on_click 丢掉持有自己的最后一个 shared_ptr ----
    // 修复前：deliver_chain 的裸指针在回调返回后被 Widget::on_pointer_event 写入 → UAF。
    {
        auto col = std::make_shared<Column>();
        auto btn = std::make_shared<Button>("kill me");
        // 该 shared_ptr 拷贝是 btn 在树外的唯一额外强引用；on_click 里连同树内的一起丢掉。
        std::shared_ptr<Button> holder = btn;
        int clicks = 0;
        btn->set_on_click([&col, &holder, &clicks]() -> void {
            ++clicks;
            // 清空子树：丢掉树内对 btn 的强引用（模拟 push_replacement 重建页面）。
            col->adopt_children(std::vector<Node>{});
            holder.reset();  // 丢掉最后一个外部强引用 → 若无 keepalive，btn 此刻即释放
        });
        col->add(Node{btn});
        btn.reset();  // 之后 btn 只由 col 子树 + holder 持有

        BuildContext ctx;
        realize(*col, painter, ctx, 200, 200);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Rect bb = col->child_nodes()[0].bounds();
        const Point center{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};

        click_at(*col, center, nullptr);
        AURORA_TEST_CHECK_MSG(clicks == 1, "suicidal on_click fired and dispatch did not crash (keepalive effective)");
        AURORA_TEST_CHECK_MSG(col->child_count() == 0, "subtree cleared in callback");
        AURORA_TEST_CHECK_MSG(holder == nullptr,
                              "external strong reference released (widget actually destroyed in callback)");
    }

    // ---- 2) 焦点控件先于 FocusManager 被回收 ----
    // 修复前：focused_ 裸指针悬垂，focused() 返回野指针，按键派发即虚调用已释放内存。
    {
        FocusManager fm;
        Column dummy_root;
        int activations = 0;
        auto leaf = std::make_shared<FocusLeaf>();
        leaf->activate_sink = &activations;
        fm.set_focus(leaf.get());
        AURORA_TEST_CHECK_MSG(fm.focused() == leaf.get(), "shared_ptr widget can gain focus normally");
        AURORA_TEST_CHECK_MSG(fm.has_focus(leaf.get()), "has_focus is true for live widget");
        AURORA_TEST_CHECK_EQ(leaf->gained, 1);

        // 前置证明：控件存活时 Enter 确实走到 focused->activate() 这条虚调用路径，
        // 否则下方「回收后不再触发」会变成永远成立的假阴性。
        KeyEvent live_enter;
        live_enter.action = KeyAction::Down;
        live_enter.key = static_cast<int>(KeyCode::Enter);
        AURORA_TEST_CHECK_MSG(EventDispatcher::dispatch(dummy_root, live_enter, fm),
                              "live focused widget activated by Enter");
        AURORA_TEST_CHECK_EQ(activations, 1);

        Widget *raw = leaf.get();
        leaf.reset();  // 焦点控件被回收，FocusManager 仍留有记录

        AURORA_TEST_CHECK_MSG(fm.focused() == nullptr, "reclaimed focused widget returns nullptr via live_focused()");
        AURORA_TEST_CHECK_MSG(!fm.has_focus(raw), "has_focus is false for reclaimed widget (must not dereference)");

        // Enter / Space 是唯一直达虚函数 activate() 的按键路径：修复前这里会对
        // 已释放内存读 vtable。回收后必须安全返回 false 且不再计数。
        KeyEvent dead_enter;
        dead_enter.action = KeyAction::Down;
        dead_enter.key = static_cast<int>(KeyCode::Enter);
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, dead_enter, fm),
                              "reclaimed focus: Enter no longer virtual-calls activate()");
        KeyEvent dead_space;
        dead_space.action = KeyAction::Down;
        dead_space.key = static_cast<int>(KeyCode::Space);
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, dead_space, fm),
                              "reclaimed focus: Space no longer virtual-calls activate()");
        AURORA_TEST_CHECK_EQ(activations, 1);  // 仍是存活期的那一次

        // 按键 / 文本派发遇到已回收焦点：安全返回 false，绝不虚调用。
        KeyEvent ke;
        ke.action = KeyAction::Down;
        ke.key = static_cast<int>(KeyCode::Backspace);
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, ke, fm),
                              "key dispatch safely returns false when focus reclaimed");

        TextInputEvent te;
        te.text = "x";
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, te, fm),
                              "text dispatch safely returns false when focus reclaimed");

        // 文本派发的早退分支必须复原 thread-local，否则 &fm 泄漏到调用者作用域外。
        AURORA_TEST_CHECK_MSG(current_focus_manager() == nullptr,
                              "TextInput early-return branch restored current_focus_manager");

        // 清焦点不应对已释放控件发 on_focus_change(false)。
        fm.clear();
        AURORA_TEST_CHECK_MSG(fm.focused() == nullptr, "no focus after clear()");
    }

    // ---- 3) 栈/成员控件回退路径：weak_from_this() 为空弱引用时仍须正常工作 ----
    // 防「只用 weak_ptr」的过度修复：栈控件恒 lock 失败会让全部事件被静默丢弃。
    {
        FocusManager fm;
        FocusLeaf stack_leaf;  // 栈对象，未被 shared_ptr 持有
        fm.set_focus(&stack_leaf);
        AURORA_TEST_CHECK_MSG(fm.focused() == &stack_leaf, "stack widget (empty weak ref) still returned as focus");
        AURORA_TEST_CHECK_MSG(fm.has_focus(&stack_leaf), "stack widget has_focus is true");
        AURORA_TEST_CHECK_EQ(stack_leaf.gained, 1);

        // 栈上 Button 的点击派发不得因 guard 为空而被丢弃。
        Column col;
        auto btn = std::make_shared<Button>("stack tree");
        int clicks = 0;
        btn->set_on_click([&clicks]() -> void { ++clicks; });
        col.add(Node{btn});
        BuildContext ctx;
        realize(col, painter, ctx, 200, 200);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Rect bb = col.child_nodes()[0].bounds();
        const Point center{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};
        click_at(col, center, &fm);
        AURORA_TEST_CHECK_MSG(clicks == 1,
                              "stack root + shared child click still dispatched normally (not swallowed by lock)");
    }

    // ---- 4) NavigatorHost 先于 Animator 析构 ----
    // 修复前：begin_transition 把成员 ctrl_ / progress_ 注册进 Animator 且无注销，
    // host 析构后 anim.tick() 即 tick 已释放的控制器并写已释放的 State。
    {
        Animator anim;
        RouteTransition fade;
        fade.animated = true;
        fade.kind = TransitionKind::Fade;
        fade.duration_seconds = 0.4;

        {
            NavigatorHost host{anim};
            BuildContext ctx;
            host.mount(ctx);
            host.push(Route{Node{SolidPage{Color::red()}}, "a", fade});
            host.push(Route{Node{SolidPage{Color::blue()}}, "b", fade});  // 触发 begin_transition
            anim.tick(0.1);
            AURORA_TEST_CHECK_MSG(anim.has_active(), "transition in progress: Animator has active controller");
        }  // host 析构 → ~NavigatorHost 调 anim.remove(ctrl_)

        // 若未注销，下面这两次 tick 会写已释放内存（ASan 下 heap-use-after-free）。
        anim.tick(0.1);
        anim.tick(0.1);
        AURORA_TEST_CHECK_MSG(!anim.has_active(),
                              "controllers unregistered from Animator after NavigatorHost destruction");
    }

    // ---- 5) Animator::remove 语义：重复注销幂等、未登记者无操作 ----
    {
        Animator anim;
        AnimationController c{1.0};
        State target{0.0};
        anim.bind(c, Tween{0.0, 1.0}, target);
        c.forward(0.0);
        anim.tick(0.5);
        AURORA_TEST_CHECK_MSG(target.get() > 0.0, "tick writes target State after bind");

        anim.remove(c);
        const double frozen = target.get();
        anim.tick(0.5);
        AURORA_TEST_CHECK_MSG(target.get() == frozen, "tick no longer writes target State after remove");

        anim.remove(c);  // 重复注销幂等
        AnimationController never{1.0};  // 从未登记
        anim.remove(never);
        AURORA_TEST_CHECK_MSG(!anim.has_active(), "repeated/invalid remove is safe");
    }
}

AURORA_TEST() { aurora::test_cases::utest_widget_lifetime::run(); }

}  // namespace aurora::test_cases::utest_widget_lifetime

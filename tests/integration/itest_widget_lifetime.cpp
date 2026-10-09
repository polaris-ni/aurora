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

/// @brief 挂载观测控件：记录挂载 / 卸载次数与订阅条数，并留下「挂载时读到的宿主身份」足迹。
///
/// 订阅条数是「净订阅不增长」唯一可判的量：`mount` 的既有幂等保护**本来就**让重复挂载不重复订阅，
/// 宿主自报的挂载次数区分不出「旧宿主已退订」与「只是没重复挂」。
class MountProbe final : public Widget {
  public:
    int mounts = 0;  ///< `on_mount` 触发次数
    int unmounts = 0;  ///< `on_unmount` 触发次数
    std::uint64_t host_of_mount = 0;  ///< 最近一次挂载时 ctx 的宿主身份
    std::uint64_t host_of_unmount = 0;  ///< 最近一次卸载时回传 ctx 的宿主身份

    [[nodiscard]] auto type_name() const -> const char * override { return "MountProbe"; }
    /// @brief 额外登记一个自有信号，使订阅条数 = 自有 1 + `modifier` + `show` = 3（固定值可硬断言）。
    /// @param out 信号视图累加表（本控件追加自身信号）。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&tick_state_); }
    [[nodiscard]] auto effect_count() const -> std::size_t { return effects_.size(); }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 10.0F, .height = 10.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        ++mounts;
        host_of_mount = ctx.host_id;
    }
    auto on_unmount(const BuildContext &ctx) -> void override {
        ++unmounts;
        host_of_unmount = ctx.host_id;
    }

  private:
    Reactive<int> tick_state_;
};

/// @brief 铺满给定的布局约束（内容自然尺寸小于它，故不做夹取计算）。
/// @param w 约束最大宽。
/// @param h 约束最大高。
/// @return 下限 0、上限为给定尺寸的约束。
auto bounded_for(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

/// @brief 造一份宿主 ctx：`scale` / `size` 刻意与另一宿主**完全相同**，只有 `host_id` 不同。
///
/// 「值相等」判据的变异体（拿 `scale` / 主题等值比同一性）必须在这份构造下转红，所以这里不做任何区分。
/// @param host_id 宿主身份（0 表示未声明宿主）。
/// @param scale 设备像素密度（两宿主刻意相同）。
/// @return 该宿主的构建上下文。
auto host_ctx(std::uint64_t host_id, float scale) -> BuildContext {
    BuildContext ctx;
    ctx.host_id = host_id;
    ctx.scale_factor = scale;
    ctx.size = Size{.width = 200.0F, .height = 120.0F};
    return ctx;
}

}  // namespace

AURORA_TEST_CASE(suicidal_click_clears_subtree_safely) {
    // on_click 丢掉持有自己的最后一个 shared_ptr：派发链裸指针在回调返回后
    // 不得再写入（keepalive 语义）。
    auto col = std::make_shared<Column>();
    auto btn = std::make_shared<Button>("kill me");
    std::shared_ptr<Button> holder = btn;  // btn 在树外的唯一额外强引用
    int clicks = 0;
    btn->set_on_click([&col, &holder, &clicks]() -> void {
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
    btn->set_on_click([&clicks]() -> void { ++clicks; });
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

AURORA_TEST_CASE(same_widget_moves_to_second_host_is_remounted_cleanly) {
    // 换宿主重挂：同一实例先挂进宿主 A，再摘下挂进宿主 B。
    // 修复前 `mounted_` 恒真 ⇒ B 侧永远拿不到 on_mount，而 A 侧的订阅仍活着。
    // 两宿主的 scale / 尺寸 / 环境**完全相同**，只有 host_id 不同：值相等判据的变异体在此转红。
    const BuildContext ctx_a = host_ctx(101U, 2.0F);
    const BuildContext ctx_b = host_ctx(202U, 2.0F);
    AURORA_TEST_REQUIRE(ctx_a.host_id != ctx_b.host_id);

    auto probe = std::make_shared<MountProbe>();
    Column host_a;
    host_a.set_children({Node{probe}});
    host_a.mount(ctx_a);
    host_a.layout(bounded_for(200.0F, 120.0F), ctx_a);
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);

    // 摘下（A 侧不代调 unmount，见 OverlayHost 用例的负向契约），挂进宿主 B。
    host_a.remove_child(probe.get());
    Column host_b;
    host_b.set_children({Node{probe}});
    host_b.mount(ctx_b);
    host_b.layout(bounded_for(200.0F, 120.0F), ctx_b);

    AURORA_TEST_CHECK_EQ(probe->mounts, 2);  // B 侧真的重新挂载了
    AURORA_TEST_CHECK_EQ(probe->unmounts, 1);  // A 侧那次真的退掉了
    AURORA_TEST_CHECK_EQ(probe->host_of_unmount, 101U);  // 卸载拿到的是**旧宿主**那份 ctx
    AURORA_TEST_CHECK_EQ(probe->host_of_mount, 202U);
    // 净订阅不增长：退订后再订阅，仍是挂载一次的条数（3 = 自有 1 + modifier + show）。
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
}

AURORA_TEST_CASE(repeated_mount_in_same_host_still_subscribes_once) {
    // D4：同宿主重复 mount 只订阅一次——转场复用同一实例这一既有保护不得被重挂语义破坏。
    const BuildContext ctx = host_ctx(303U, 1.0F);
    auto probe = std::make_shared<MountProbe>();
    probe->mount(ctx);
    probe->mount(ctx);
    probe->mount(ctx);
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
}

AURORA_TEST_CASE(unmount_without_mount_is_idempotent) {
    // 判据 6：未挂载即调用 unmount 幂等——不崩、不留「已清订阅却仍标着已挂载」的半个状态；
    // 随后的正常挂载仍应恰好一次。
    auto probe = std::make_shared<MountProbe>();
    AURORA_TEST_CHECK_NO_THROW(probe->unmount());
    AURORA_TEST_CHECK_NO_THROW(probe->unmount());
    AURORA_TEST_CHECK_EQ(probe->mounts, 0);
    AURORA_TEST_CHECK_EQ(probe->unmounts, 0);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 0U);

    const BuildContext ctx = host_ctx(404U, 1.0F);
    probe->mount(ctx);
    AURORA_TEST_CHECK_EQ(probe->mounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 3U);
    probe->unmount();
    AURORA_TEST_CHECK_EQ(probe->unmounts, 1);
    AURORA_TEST_CHECK_EQ(probe->effect_count(), 0U);
    probe->unmount();  // 重复卸载幂等
    AURORA_TEST_CHECK_EQ(probe->unmounts, 1);
}

}  // namespace aurora::test_cases::itest_widget_lifetime

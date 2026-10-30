/// 测试类型: unit
/// 目标单元: include/aurora/widget/dismissible.h + include/aurora/event/gesture.h（DragToDismiss）
/// 测试说明: DragToDismiss reduce_motion 短路（飞出/回位两方向单帧落端点，回调语义不变）；
/// Dismissible 控件事件喂入→跟手 progress 1:1 联动（布局后按主轴向尺寸校准行程）→拖动中
/// 消费事件；松手裁决两分支（低于阈值 spring 回位保留在树 / 高于阈值飞出触发默认摘除——
/// 容器 children 收缩、持有计数归位）；自定义 on_dismissed 回调覆盖默认摘除；
/// 自描述元数据（type_name / describe / 默认行程）。
/// 另含 DragToDismiss 收口到公共弹簧后的控件侧回归：全局时间缩放 1.0 → 2.0 时 spring 段到位
/// 帧数减半（弹簧为指数衰减，故取上下界）而终值不变；0.0 档冻结时时间轴不推进、
/// 按帧重绘请求数为 0、在途标志保留，解冻后自驱续跑到同一终值。
/// 主轴贯通回归：构造轴决定行程标定的尺寸（竖向取高、横向取宽）、跟手取哪个坐标分量、绘制平移落在
/// 哪条轴（由记录绘制盒的子控件断言），另一轴向的分量一律不参与。

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/core/accessibility.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/widget/dismissible.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "support/animation_fixture.h"

namespace aurora::test_cases::utest_dismissible {

namespace {

using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Dismissible;
using aurora::DragAxis;
using aurora::DragToDismiss;
using aurora::EventDispatcher;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::Point;
using aurora::Rect;
using aurora::Size;
using aurora::SpringDescription;
using aurora::Text;
using aurora::Widget;

auto make_mouse(MouseAction action, Point pos) -> MouseEvent {
    MouseEvent e;
    e.action = action;
    e.position = pos;
    return e;
}

/// 100x50 固定文本盒（px 意图决定尺寸，与字体度量无关）。
auto fixed_box() -> Node {
    auto t = std::make_shared<Text>(".");
    t->width(aurora::px(100.0F));
    t->height(aurora::px(50.0F));
    return Node{t};
}

/// 记录自身被绘制时收到的盒：位移落在 x 还是 y 只能由子侧的绘制盒判定（父侧 paint 平移不动布局盒）。
class ProbeBox final : public Text {
  public:
    /// @brief 构造 100x50 的探针盒（尺寸与 `fixed_box()` 同，保证行程标定可精确预期）。
    ProbeBox() : Text(".") {
        width(aurora::px(100.0F));
        height(aurora::px(50.0F));
    }

    Rect seen{};  ///< 最近一次 on_paint 收到的绘制盒（初始为原点零盒 = 从未绘制）

  protected:
    /// @brief 记下盒后照常绘制（不改绘制内容，只取几何）。
    /// @param p 绘制器。
    /// @param bounds 本次绘制盒。
    /// @param ctx 构建上下文。
    auto on_paint(aurora::Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        seen = bounds;
        Text::on_paint(p, bounds, ctx);
    }
};

/// 双列定宽测试容器：每个子项约束固定 100x50，横排累加（布局完全确定，便于命中与行程校准）。
class TestRow final : public aurora::Container {
  public:
    auto type_name() const -> const char * override { return "TestRow"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        float x = 0.0F;
        for (Node &ch : children_) {
            const Size cs =
                ch.widget().layout(Constraints{.min = Size{}, .max = Size{.width = 100.0F, .height = 50.0F}}, ctx);
            ch.set_bounds(Rect{.origin = Point{.x = x, .y = 0.0F}, .size = cs});
            x += cs.width;
        }
        size_ = c.constrain(Size{.width = x, .height = 50.0F});
        return size_;
    }

    auto on_paint(aurora::Painter & /*painter*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/)
        -> void override {}
};

/// reduce_motion 进程级全局设置守卫：构造开启、析构还原默认，防跨用例污染。
class ReduceMotionGuard {
  public:
    ReduceMotionGuard() { aurora::set_accessibility_settings(aurora::AccessibilitySettings{.reduce_motion = true}); }
    ~ReduceMotionGuard() { aurora::set_accessibility_settings(aurora::AccessibilitySettings{}); }
    ReduceMotionGuard(const ReduceMotionGuard &) = delete;
    auto operator=(const ReduceMotionGuard &) -> ReduceMotionGuard & = delete;
    ReduceMotionGuard(ReduceMotionGuard &&) = delete;
    auto operator=(ReduceMotionGuard &&) -> ReduceMotionGuard & = delete;
};

auto t_ms(long long ms) -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
}

/// 暴露 protected 的 TouchEvent 派发入口（on_pointer_event(TouchEvent&) 覆写）。
class TouchableDismiss final : public Dismissible {
  public:
    using Dismissible::Dismissible;
    auto feed_touch(TouchEvent &e) -> void { on_pointer_event(e); }
};

/// 鼠标路径跟手到 `target` 后松手（须已布局：行程取布局校准后的 travel_distance）。
/// 末帧刻意反向 0.5dp，使交接速度与裁决方向相反而被 `on_release` 丢弃 —— 两侧（不同时间
/// 缩放、冻结前后）从此走**同一条零速弹簧轨迹**，帧数差才只归因于 dt 漏斗而非速度交接。
auto drag_and_release(Dismissible &dis, EventDispatcher &dispatcher, double target) -> void {
    const double travel = dis.travel_distance;
    auto mouse_at = [&](MouseAction action, double along) -> MouseEvent {
        return make_mouse(action, Point{.x = static_cast<float>(10.0 + (travel * along)), .y = 10.0F});
    };
    MouseEvent press = mouse_at(MouseAction::Press, 0.0);
    (void)dispatcher.dispatch_mouse(dis, press);
    MouseEvent overshoot = mouse_at(MouseAction::Move, target + 0.005);
    (void)dispatcher.dispatch_mouse(dis, overshoot);
    MouseEvent settle = mouse_at(MouseAction::Move, target);
    (void)dispatcher.dispatch_mouse(dis, settle);
    MouseEvent release = mouse_at(MouseAction::Release, target);
    (void)dispatcher.dispatch_mouse(dis, release);
}

/// 根位 Dismissible（无 Container 祖先）+ 定长约束：行程恒为 100，飞出终态原地保留便于读终值。
auto make_laid_out_root(Dismissible &dis) -> void {
    dis.layout(Constraints{.min = Size{}, .max = Size{.width = 100.0F, .height = 50.0F}}, BuildContext{});
    AURORA_TEST_REQUIRE_NEAR(dis.travel_distance, 100.0, 1e-9);
}

}  // namespace

// ---- 自描述元数据 ----

AURORA_TEST_CASE(descriptor_and_default_travel) {
    Dismissible d(fixed_box());
    AURORA_TEST_CHECK_EQ(std::string{d.type_name()}, "Dismissible");
    const auto desc = Dismissible::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{desc.name}, "Dismissible");
    AURORA_TEST_CHECK_EQ(std::string{desc.children_policy}, "single");
    AURORA_TEST_CHECK_EQ(desc.properties.size(), 1U);
    AURORA_TEST_CHECK_EQ(std::string{desc.properties[0].name}, "axis");
    AURORA_TEST_CHECK_EQ(desc.events.size(), 1U);
    AURORA_TEST_CHECK_EQ(desc.events[0], "on_dismissed");
    AURORA_TEST_CHECK_NEAR(d.travel_distance, 200.0, 1e-9);
}

// ---- DragToDismiss：reduce_motion 短路 ----

AURORA_TEST_CASE(dtd_reduce_motion_short_circuits_to_endpoint) {
    ReduceMotionGuard guard;  // 进程级开启，析构还原

    // 飞出方向：拖 0.7 松手 → 单帧 tick 直接落 1 并触发 dismissed（不经过 spring 多帧）。
    DragToDismiss dtd(DragAxis::Horizontal, 100.0, SpringDescription{});
    dtd.recognizer_slop_for_test(5.0);
    int dismissed = 0;
    dtd.on_dismissed([&dismissed] { ++dismissed; });
    dtd.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    dtd.on_mouse(make_mouse(MouseAction::Move, Point{.x = 70.0F, .y = 0.0F}));
    dtd.on_release();
    AURORA_TEST_CHECK_TRUE(dtd.is_animating());
    dtd.tick(1.0 / 60.0);  // 单帧短路
    AURORA_TEST_CHECK_FALSE(dtd.is_animating());
    AURORA_TEST_CHECK_NEAR(dtd.progress().get(), 1.0, 1e-9);
    AURORA_TEST_CHECK_EQ(dismissed, 1);

    // 回位方向：拖 0.3 松手 → 单帧落 0，不触发 dismissed。
    DragToDismiss back(DragAxis::Horizontal, 100.0, SpringDescription{});
    back.recognizer_slop_for_test(5.0);
    int back_dismissed = 0;
    back.on_dismissed([&back_dismissed] { ++back_dismissed; });
    back.on_mouse(make_mouse(MouseAction::Press, Point{.x = 0.0F, .y = 0.0F}));
    back.on_mouse(make_mouse(MouseAction::Move, Point{.x = 30.0F, .y = 0.0F}));
    back.on_release();
    back.tick(1.0 / 60.0);
    AURORA_TEST_CHECK_NEAR(back.progress().get(), 0.0, 1e-9);
    AURORA_TEST_CHECK_EQ(back_dismissed, 0);
}

// ---- Dismissible 控件：事件喂入 → progress 联动 ----

AURORA_TEST_CASE(dismissible_drag_progresses_one_to_one_and_consumes_events) {
    auto dis = std::make_shared<Dismissible>(fixed_box());
    TestRow row;
    row.add(Node{dis});
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, BuildContext{});
    // 首次布局后行程按主轴向尺寸校准：100（TestRow 固定约束宽）。
    AURORA_TEST_CHECK_NEAR(dis->travel_distance, 100.0, 1e-6);

    EventDispatcher dispatcher;
    // 按下在第一个子项区域 (10,10)，右拖 50dp → progress 0.5（1:1，非动画）。
    MouseEvent press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press);
    MouseEvent move = make_mouse(MouseAction::Move, Point{.x = 60.0F, .y = 10.0F});
    AURORA_TEST_CHECK_TRUE(dispatcher.dispatch_mouse(row, move));
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.5, 1e-9);
    AURORA_TEST_CHECK_TRUE(move.is_handled);  // 拖动中消费事件，父级不响应

    // 主轴锁定：y 分量大幅移动不改变 progress。
    MouseEvent vertical = make_mouse(MouseAction::Move, Point{.x = 60.0F, .y = 40.0F});
    (void)dispatcher.dispatch_mouse(row, vertical);
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.5, 1e-9);

    // 拖回 0 后松手：spring 回位收敛，恢复非动画态。
    MouseEvent rewind = make_mouse(MouseAction::Move, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, rewind);
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.0, 1e-9);
    MouseEvent release = make_mouse(MouseAction::Release, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, release);
    for (int i = 0; i < 100 && dis->is_animating(); ++i) {
        row.tick(t_ms(static_cast<long long>(i + 1) * 16));
    }
    AURORA_TEST_CHECK_FALSE(dis->is_animating());

    // 新一次按下：超 slop 前的小位移不激活拖动，事件不被 Dismissible 接管。
    MouseEvent press2 = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press2);
    MouseEvent nudge = make_mouse(MouseAction::Move, Point{.x = 12.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, nudge);
    AURORA_TEST_CHECK_FALSE(nudge.is_handled);  // 位移 2dp < slop 8dp
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.0, 1e-9);
}

// ---- Dismissible 控件：松手裁决两分支 ----

AURORA_TEST_CASE(dismissible_release_below_threshold_springs_back_and_stays) {
    auto dis = std::make_shared<Dismissible>(fixed_box());
    TestRow row;
    row.add(Node{dis});
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, BuildContext{});

    EventDispatcher dispatcher;
    MouseEvent press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press);
    MouseEvent move = make_mouse(MouseAction::Move, Point{.x = 40.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, move);
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.3, 1e-9);
    MouseEvent release = make_mouse(MouseAction::Release, Point{.x = 40.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, release);

    // 帧驱动 spring 回位：收敛到 0，节点保留在容器中。
    for (int i = 0; i < 400 && dis->is_animating(); ++i) {
        row.tick(t_ms(static_cast<long long>(i + 1) * 16));
    }
    AURORA_TEST_CHECK_FALSE(dis->is_animating());
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.0, 1e-3);
    AURORA_TEST_CHECK_EQ(row.child_nodes().size(), 1U);  // 未摘除
}

AURORA_TEST_CASE(dismissible_release_above_threshold_removes_self_from_container) {
    auto first = std::make_shared<Dismissible>(fixed_box());
    auto second = std::make_shared<Dismissible>(fixed_box());
    TestRow row;
    row.add(Node{first});
    row.add(Node{second});
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, BuildContext{});
    AURORA_TEST_CHECK_EQ(row.child_nodes().size(), 2U);

    // 拖第一项到 0.7（≥ 阈值）松手：spring 飞出 → 默认从容器摘除自身。
    EventDispatcher dispatcher;
    MouseEvent press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press);
    MouseEvent move = make_mouse(MouseAction::Move, Point{.x = 80.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, move);
    AURORA_TEST_CHECK_NEAR(first->progress(), 0.7, 1e-9);
    MouseEvent release = make_mouse(MouseAction::Release, Point{.x = 80.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, release);

    // 帧驱动直到 spring 收敛（fire_dismissed 在收敛帧摘除）。
    for (int i = 0; i < 400 && first->is_animating(); ++i) {
        row.tick(t_ms(static_cast<long long>(i + 1) * 16));
    }
    AURORA_TEST_CHECK_FALSE(first->is_animating());
    AURORA_TEST_CHECK_NEAR(first->progress(), 1.0, 1e-3);
    AURORA_TEST_CHECK_EQ(row.child_nodes().size(), 1U);  // 已摘除
    AURORA_TEST_CHECK_EQ(first.use_count(), 1);  // 仅测试自身持有
    AURORA_TEST_CHECK_EQ(second.use_count(), 2);  // 测试 + 容器

    // 摘除后重排（remove_child 标脏；真实帧循环派发事件前会重排），幸存项顶到首位。
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, BuildContext{});
    // 幸存子项仍可正常拖动（容器迭代器安全：摘除后同一帧 tick 不失效）。
    MouseEvent press2 = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press2);
    MouseEvent move2 = make_mouse(MouseAction::Move, Point{.x = 60.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, move2);
    AURORA_TEST_CHECK_NEAR(second->progress(), 0.5, 1e-9);
}

AURORA_TEST_CASE(dismissible_custom_callback_overrides_default_removal) {
    const auto dis = std::make_shared<Dismissible>(fixed_box());
    TestRow row;
    row.add(Node{dis});
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, BuildContext{});

    int dismissed = 0;
    dis->on_dismissed([&dismissed] { ++dismissed; });  // 覆盖默认摘除

    EventDispatcher dispatcher;
    MouseEvent press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press);
    MouseEvent move = make_mouse(MouseAction::Move, Point{.x = 70.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, move);
    MouseEvent release = make_mouse(MouseAction::Release, Point{.x = 70.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, release);
    for (int i = 0; i < 400 && dis->is_animating(); ++i) {
        row.tick(t_ms(static_cast<long long>(i + 1) * 16));
    }
    AURORA_TEST_CHECK_EQ(dismissed, 1);  // 回调触发
    AURORA_TEST_CHECK_EQ(row.child_nodes().size(), 1U);  // 不自动摘除
    AURORA_TEST_CHECK_EQ(dis.use_count(), 2);  // 测试 + 容器仍持有
}

// ---- on_paint：progress 0 / progress>0 两分支 ----

AURORA_TEST_CASE(dismissible_paint_progress_zero_and_in_flight) {
    Painter painter;
    constexpr BuildContext ctx;

    // progress = 0：直接绘制子节点（无位移/渐隐）。
    Dismissible rest{fixed_box()};
    rest.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, ctx);
    rest.paint(painter, Rect{.origin = Point{}, .size = rest.size()}, ctx);

    // progress > 0：paint 期平移 + 渐隐（须先驱动拖动再绘制；动画不改布局盒）。
    const auto dis = std::make_shared<Dismissible>(fixed_box());
    TestRow row;
    row.add(Node{dis});
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, ctx);
    EventDispatcher dispatcher;
    MouseEvent press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, press);
    MouseEvent move = make_mouse(MouseAction::Move, Point{.x = 60.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(row, move);
    AURORA_TEST_CHECK_NEAR(dis->progress(), 0.5, 1e-9);
    dis->paint(painter, Rect{.origin = Point{}, .size = dis->size()}, ctx);

    // 垂直轴构造的静止态绘制：仅覆盖「progress 为 0 时走无位移分支」；轴方向的断言见下一条用例。
    Dismissible vertical{fixed_box(), DragAxis::Vertical};
    vertical.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, ctx);
    vertical.paint(painter, Rect{.origin = Point{}, .size = vertical.size()}, ctx);
}

// @brief 主轴贯通：竖向构造的行程标定、跟手取分量、绘制平移三条路径都按构造轴走（缺陷回归）。
//        此前 `Dismissible` 复制了一份轴字段却从未赋值，竖向请求表现为「拖得动、裁决对，但位移画在
//        水平轴、行程按宽度折算」，故这里逐条钉住轴向：行程取高、横向拖不给进度、位移只落 y。
AURORA_TEST_CASE(dismissible_axis_routes_travel_progress_and_paint_offset) {
    Painter painter;
    constexpr BuildContext ctx;

    // ---- 竖向：行程按高度标定，位移只落 y ----
    const auto v_probe = std::make_shared<ProbeBox>();
    Dismissible vertical{Node{v_probe}, DragAxis::Vertical};
    const Rect v_box{.origin = Point{.x = 4.0F, .y = 6.0F}, .size = Size{.width = 100.0F, .height = 50.0F}};
    vertical.layout(Constraints{.min = Size{}, .max = Size{.width = 100.0F, .height = 50.0F}}, ctx);
    AURORA_TEST_CHECK_NEAR(vertical.travel_distance, 50.0, 1e-9);  // 主轴向 = 高，不是宽 100

    EventDispatcher dispatcher;
    // 纯横向拖动：识别器已起拖（50dp > slop），但主轴取 y 分量 → 进度恒 0。
    MouseEvent vh_press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(vertical, vh_press);
    MouseEvent vh_move = make_mouse(MouseAction::Move, Point{.x = 60.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(vertical, vh_move);
    AURORA_TEST_CHECK_NEAR(vertical.progress(), 0.0, 1e-9);

    // 纵向拖 25dp → progress = 25 / 50 = 0.5，绘制盒下移 25、x 保持不变。
    MouseEvent v_press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(vertical, v_press);
    MouseEvent v_move = make_mouse(MouseAction::Move, Point{.x = 10.0F, .y = 35.0F});
    (void)dispatcher.dispatch_mouse(vertical, v_move);
    AURORA_TEST_CHECK_NEAR(vertical.progress(), 0.5, 1e-9);
    vertical.paint(painter, v_box, ctx);
    AURORA_TEST_CHECK_NEAR(v_probe->seen.origin.x, v_box.origin.x, 1e-6);
    AURORA_TEST_CHECK_NEAR(v_probe->seen.origin.y, v_box.origin.y + 25.0F, 1e-6);

    // ---- 横向：行程按宽度标定，位移只落 x ----
    const auto h_probe = std::make_shared<ProbeBox>();
    Dismissible horizontal{Node{h_probe}, DragAxis::Horizontal};
    const Rect h_box{.origin = Point{.x = 4.0F, .y = 6.0F}, .size = Size{.width = 100.0F, .height = 50.0F}};
    horizontal.layout(Constraints{.min = Size{}, .max = Size{.width = 100.0F, .height = 50.0F}}, ctx);
    AURORA_TEST_CHECK_NEAR(horizontal.travel_distance, 100.0, 1e-9);  // 主轴向 = 宽

    MouseEvent h_press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(horizontal, h_press);
    MouseEvent h_move = make_mouse(MouseAction::Move, Point{.x = 60.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(horizontal, h_move);
    AURORA_TEST_CHECK_NEAR(horizontal.progress(), 0.5, 1e-9);  // 50 / 100
    horizontal.paint(painter, h_box, ctx);
    AURORA_TEST_CHECK_NEAR(h_probe->seen.origin.x, h_box.origin.x + 50.0F, 1e-6);
    AURORA_TEST_CHECK_NEAR(h_probe->seen.origin.y, h_box.origin.y, 1e-6);
}

// ---- 触摸路径 + 无 Container 祖先的静默摘除 ----

AURORA_TEST_CASE(dismissible_touch_drag_drives_progress) {
    TouchableDismiss dis{fixed_box()};
    // 约束 100x50 → 行程校准为 100（跟手 1:1 可精确预期）。
    dis.layout(Constraints{.min = Size{}, .max = Size{.width = 100.0F, .height = 50.0F}}, BuildContext{});

    TouchEvent press;
    press.points.push_back(TouchPoint{
        .position = Point{.x = 10.0F, .y = 10.0F}, .prev_position = Point{.x = 10.0F, .y = 10.0F}, .is_active = true});
    dis.feed_touch(press);
    AURORA_TEST_CHECK_NEAR(dis.progress(), 0.0, 1e-9);

    TouchEvent move;
    move.points.push_back(TouchPoint{
        .position = Point{.x = 50.0F, .y = 10.0F}, .prev_position = Point{.x = 10.0F, .y = 10.0F}, .is_active = true});
    dis.feed_touch(move);
    AURORA_TEST_CHECK_NEAR(dis.progress(), 0.4, 1e-9);  // 触摸取首活跃点，1:1 同鼠标

    // 松手裁决入口在鼠标路径：经 MouseEvent Release 触发 spring 回位（0.4 < 阈值）。
    EventDispatcher dispatcher;
    MouseEvent mrel = make_mouse(MouseAction::Release, Point{.x = 50.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(dis, mrel);
    AURORA_TEST_CHECK_TRUE(dis.is_animating());
    for (int i = 0; i < 400 && dis.is_animating(); ++i) {
        dis.tick(t_ms(static_cast<long long>(i + 1) * 16));
    }
    AURORA_TEST_CHECK_FALSE(dis.is_animating());
    AURORA_TEST_CHECK_NEAR(dis.progress(), 0.0, 1e-3);
}

AURORA_TEST_CASE(dismissible_without_container_ancestor_keeps_terminal_state) {
    // 直接作为根（无 Container 祖先）：飞出后默认摘除静默保持终态，不崩溃。
    // 约束 100x50 → 行程校准为 100；拖到 0.9（≥ 阈值）松手。
    Dismissible root{fixed_box()};
    root.layout(Constraints{.min = Size{}, .max = Size{.width = 100.0F, .height = 50.0F}}, BuildContext{});

    EventDispatcher dispatcher;
    MouseEvent press = make_mouse(MouseAction::Press, Point{.x = 10.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(root, press);
    MouseEvent move = make_mouse(MouseAction::Move, Point{.x = 90.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(root, move);
    MouseEvent release = make_mouse(MouseAction::Release, Point{.x = 90.0F, .y = 10.0F});
    (void)dispatcher.dispatch_mouse(root, release);
    AURORA_TEST_CHECK_TRUE(root.is_animating());
    for (int i = 0; i < 400 && root.is_animating(); ++i) {
        root.tick(t_ms(static_cast<long long>(i + 1) * 16));  // Widget::tick 公共入口 → tick_gestures
    }
    AURORA_TEST_CHECK_FALSE(root.is_animating());
    AURORA_TEST_CHECK_NEAR(root.progress(), 1.0, 1e-3);  // 终态保持，无祖先可摘除
}

// ---- 判据 10 / 判据 11：DragToDismiss 收口到公共弹簧后的控件侧回归 ----

// @brief 判据 10（Dismissible 行）：全局时间缩放 1.0 → 2.0 时 spring 段到位帧数减半，终值不变。
AURORA_TEST_CASE(dismissible_spring_frames_halve_when_time_scale_doubles) {
    // 事件序列与时间戳序列两侧完全同形，只换 Animator 的倍率：帧数差只能来自单一 dt 漏斗。
    auto settle_frames = [](double time_scale) -> std::pair<int, double> {
        aurora::Animator animator;
        animator.set_time_scale(time_scale);
        const aurora::testing::ScopedCurrentAnimator scope{animator};

        Dismissible root{fixed_box()};
        make_laid_out_root(root);

        EventDispatcher dispatcher;
        drag_and_release(root, dispatcher, 0.7);  // ≥ 阈值 → 飞出方向
        AURORA_TEST_REQUIRE_TRUE(root.is_animating());

        int frames = 0;
        while (root.is_animating() && frames < 600) {
            root.tick(t_ms(static_cast<long long>(++frames) * 16));  // 首帧 dt 取 1/60，其后每帧 16ms
        }
        AURORA_TEST_REQUIRE_LT(frames, 600);  // 触顶即没自驱到定格，减半比较无从谈起
        AURORA_TEST_REQUIRE_FALSE(root.is_animating());
        return {frames, root.progress()};
    };

    const auto slow = settle_frames(1.0);
    const auto fast = settle_frames(2.0);

    // 终值与倍率无关：定格写的是 animate_to 的 target 本身，不是某一帧的采样值。
    AURORA_TEST_CHECK_NEAR(slow.second, 1.0, 1e-12);
    AURORA_TEST_CHECK_NEAR(fast.second, 1.0, 1e-12);
    AURORA_TEST_CHECK_GT(slow.first, 1);  // 确为多帧收敛（单帧落点是 reduce_motion 的档）
    AURORA_TEST_CHECK_LT(fast.first, slow.first);  // 倍率加倍 → 到位更快

    // 弹簧是指数衰减，「动画时间轴加倍 ⇒ 帧数精确减半」不成立：到位帧数是收敛时刻 T 的阶梯函数
    // n(s) = min{n : s·t(n) ≥ T}，故 2·n(2) 与 n(1) 只差一个取整误差（≤2 帧）——断言上下界而非等式。
    const int doubled = 2 * fast.first;
    AURORA_TEST_CHECK_LE(doubled, slow.first + 3);
    AURORA_TEST_CHECK_GE(doubled, slow.first - 3);
}

// @brief 判据 11（控件自驱侧）：缩放 0.0 冻结段不推进、按帧重绘请求数为 0，解冻后续跑到同一终值。
AURORA_TEST_CASE(dismissible_freeze_holds_spring_and_thaw_resumes_to_same_endpoint) {
    aurora::Animator animator;
    animator.set_time_scale(1.0);
    const aurora::testing::ScopedCurrentAnimator scope{animator};

    Dismissible root{fixed_box()};
    make_laid_out_root(root);

    // 重绘请求计数：mark_needs_paint → request_frame 落在本控件私有回调上（不经渲染器接线）。
    int repaints = 0;
    root.on_dirty = [&repaints](bool /*layout*/) -> void { ++repaints; };

    EventDispatcher dispatcher;
    drag_and_release(root, dispatcher, 0.7);
    AURORA_TEST_REQUIRE_TRUE(root.is_animating());

    int frame = 0;
    root.tick(t_ms(static_cast<long long>(++frame) * 16));  // 未冻结的一帧
    AURORA_TEST_CHECK_GT(repaints, 0);  // 计数器自检：非冻结帧必须标脏
    AURORA_TEST_CHECK_GT(root.progress(), 0.7);  // 且确实在推进

    // 冻结段：12 帧既不动时间轴，也不「停着不动却满帧唤醒」。
    const double frozen_progress = root.progress();
    animator.set_time_scale(0.0);
    repaints = 0;
    for (int i = 0; i < 12; ++i) {
        root.tick(t_ms(static_cast<long long>(++frame) * 16));
    }
    AURORA_TEST_CHECK_EQ(repaints, 0);
    AURORA_TEST_CHECK_NEAR(root.progress(), frozen_progress, 0.0);  // 逐比特不动：冻结段零写入
    AURORA_TEST_CHECK_TRUE(root.is_animating());  // 在途标志保留，冻结不等于中止
    AURORA_TEST_CHECK_FALSE(animator.take_rearm_request());  // 冻结方向不置重挂位（只解冻才置）

    // 解冻续跑：由冻结前的进度自行走到同一终值，不丢进度也不重头再拖。
    animator.set_time_scale(1.0);
    AURORA_TEST_CHECK_TRUE(animator.take_rearm_request());  // 帧循环不深睡的凭据（应用侧在此送一帧）
    repaints = 0;
    while (root.is_animating() && frame < 600) {
        root.tick(t_ms(static_cast<long long>(++frame) * 16));
    }
    AURORA_TEST_REQUIRE_LT(frame, 600);
    AURORA_TEST_CHECK_FALSE(root.is_animating());
    AURORA_TEST_CHECK_NEAR(root.progress(), 1.0, 1e-12);
    AURORA_TEST_CHECK_GT(repaints, 0);  // 解冻后恢复正常标脏
    root.on_dirty = nullptr;
}

}  // namespace aurora::test_cases::utest_dismissible

/// 测试类型: unit
/// 目标单元: include/aurora/widget/dismissible.h + include/aurora/event/gesture.h（DragToDismiss）
/// 测试说明: DragToDismiss reduce_motion 短路（飞出/回位两方向单帧落端点，回调语义不变）；
/// Dismissible 控件事件喂入→跟手 progress 1:1 联动（布局后按主轴向尺寸校准行程）→拖动中
/// 消费事件；松手裁决两分支（低于阈值 spring 回位保留在树 / 高于阈值飞出触发默认摘除——
/// 容器 children 收缩、持有计数归位）；自定义 on_dismissed 回调覆盖默认摘除；
/// 自描述元数据（type_name / describe / 默认行程）。

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include "aurora/core/accessibility.h"
#include "aurora/event/dispatcher.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/dismissible.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

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

/// 双列定宽测试容器：每个子项约束固定 100x50，横排累加（布局完全确定，便于命中与行程校准）。
class TestRow final : public aurora::Container {
  public:
    auto type_name() const -> const char * override { return "TestRow"; }

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
    dtd.on_dismissed([&dismissed]() { ++dismissed; });
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
    back.on_dismissed([&back_dismissed]() { ++back_dismissed; });
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
    auto dis = std::make_shared<Dismissible>(fixed_box());
    TestRow row;
    row.add(Node{dis});
    row.layout(Constraints{.min = Size{}, .max = Size{.width = 400.0F, .height = 100.0F}}, BuildContext{});

    int dismissed = 0;
    dis->on_dismissed([&dismissed]() { ++dismissed; });  // 覆盖默认摘除

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

}  // namespace aurora::test_cases::utest_dismissible

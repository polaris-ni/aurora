/// 测试类型: unit
/// 目标单元: include/aurora/widget/timer.h
/// 测试说明: 组件级定时控件。覆盖默认/周期构造、ticks 信号、静态/运行期自描述、信号收集、on_layout 转发、
/// 有运行中 Scheduler 时挂载注册周期任务（set_interval，tick 触发自增并回调 on_tick）、无 Scheduler 时降级
/// （Diagnostics::warn + 仍以 tick=0 挂载子 UI 不崩溃）、卸载 cancel 句柄 + 卸载子节点、析构取消句柄。
/// 判据：挂载到有 Scheduler 的 Timer 经一次 tick 后 ticks()==1 且 on_tick 被回调；无 Scheduler 时子节点仍挂载、
/// 不抛异常。

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

#include "aurora/app/scheduler.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/timer.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_timer {

namespace {

class LeafProbe final : public Widget {
  public:
    int mounts = 0;
    int unmounts = 0;
    [[nodiscard]] auto type_name() const -> const char * override { return "LeafProbe"; }
    auto collect_signals(StateSink &out) -> void override { (void)out; }
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 20.0F, .height = 20.0F});
    }
    auto on_paint(Painter & /*p*/, const Rect & /*b*/, const BuildContext & /*ctx*/) -> void override {}
    auto on_mount(const BuildContext &ctx) -> void override {
        Widget::on_mount(ctx);
        ++mounts;
    }
    auto on_unmount(const BuildContext &ctx) -> void override {
        Widget::on_unmount(ctx);
        ++unmounts;
    }
};

}  // namespace

AURORA_TEST_CASE(api_surface) {
    Timer t;
    AURORA_TEST_CHECK_EQ(std::string(t.type_name()), "Timer");
    AURORA_TEST_CHECK_EQ(t.ticks().get(), 0);
    auto d = Timer::describe_static();
    AURORA_TEST_CHECK_EQ(d.name, "Timer");
    auto d2 = t.describe();
    AURORA_TEST_CHECK_EQ(d2.name, "Timer");
    StateSink sigs;
    t.collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.sources().size(), 1U);
}

AURORA_TEST_CASE(mount_with_running_scheduler_registers_interval) {
    Scheduler sched;
    Scheduler::set_current(&sched);

    int fired = 0;
    auto child = std::make_shared<LeafProbe>();
    Timer t2(
        std::chrono::seconds(1), [&](const ReadOnlyState<int> &) -> Node { return Node{child}; },
        [&](int n) { fired = n; });
    BuildContext ctx;
    t2.mount(ctx);  // 有 Scheduler：set_interval 注册周期任务
    AURORA_TEST_CHECK_EQ(child->mounts, 1);

    // 子节点布局 / 绘制（on_layout / on_paint 转发）。
    t2.layout(Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 100.0F}},
              ctx);
    Painter painter;
    t2.paint(painter, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 100.0F}},
             ctx);

    // 推进超过一个周期：tick 自增为 1 且 on_tick 携带计数 1。
    sched.tick(1.1);
    AURORA_TEST_CHECK_EQ(t2.ticks().get(), 1);
    AURORA_TEST_CHECK_EQ(fired, 1);

    t2.unmount();  // cancel 句柄 + 卸载子节点
    AURORA_TEST_CHECK_EQ(child->unmounts, 1);
    Scheduler::set_current(nullptr);
}

AURORA_TEST_CASE(mount_without_scheduler_degrades_gracefully) {
    auto child2 = std::make_shared<LeafProbe>();
    Timer t3(std::chrono::seconds(1), [&](const ReadOnlyState<int> &) -> Node { return Node{child2}; });
    BuildContext ctx;
    t3.mount(ctx);  // 无 Scheduler：降级告警 + 仍以 tick=0 挂载子 UI
    AURORA_TEST_CHECK_EQ(child2->mounts, 1);
    AURORA_TEST_CHECK_EQ(t3.ticks().get(), 0);
    t3.unmount();
    AURORA_TEST_CHECK_EQ(child2->unmounts, 1);
}

}  // namespace aurora::test_cases::utest_timer

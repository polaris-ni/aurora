/// 测试类型: unit
/// 目标单元: include/aurora/widget/timer.h
/// 测试说明: 定时控件（tick 信号初值 0、类型名/自描述、collect_signals 暴露 tick、builder 构造子 UI）单元测试

#include <chrono>
#include <string>
#include <vector>

#include "aurora/widget/timer.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_timer {

AURORA_TEST() {
    using namespace std::chrono_literals;  // NOLINT(build/namespaces) —— 测试点即周期字面量

    // ---- 1. 默认 Timer：tick 信号从 0 起，类型名 "Timer" ----
    {
        Timer t;
        AURORA_TEST_CHECK_EQ(t.ticks().get(), 0);
        AURORA_TEST_CHECK_EQ(std::string(t.type_name()), std::string("Timer"));
    }

    // ---- 2. collect_signals：把内部 tick 状态登记为一个可订阅信号 ----
    {
        Timer t;
        std::vector<SignalViewBase *> out;
        t.collect_signals(out);
        AURORA_TEST_CHECK_EQ(out.size(), std::size_t{1});
        AURORA_TEST_CHECK(out[0] != nullptr);
    }

    // ---- 3. 带 builder 的 Timer：构建子 UI 一次，tick 仍为初值 ----
    {
        int built = 0;
        Timer t(16ms, [&built](const SignalView<int> &tick) -> Node {
            ++built;
            (void)tick.get();
            return Node{};
        });
        AURORA_TEST_CHECK_EQ(built, 1);  // builder 在构造时被调用一次
        AURORA_TEST_CHECK_EQ(t.ticks().get(), 0);
    }

    // ---- 4. 自描述：period 必填、on_tick 可选，声明示例非空 ----
    {
        const WidgetDescriptor d = Timer::describe_static();
        AURORA_TEST_CHECK_EQ(std::string(d.name), std::string("Timer"));
        AURORA_TEST_CHECK_EQ(d.properties.size(), std::size_t{2});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK_EQ(std::string(d.properties[0].name), std::string("period"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK(d.properties[0].required);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK_EQ(std::string(d.properties[1].name), std::string("on_tick"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        AURORA_TEST_CHECK(!d.properties[1].required);
        AURORA_TEST_CHECK(!d.examples.empty());
    }
}

}  // namespace aurora::test_cases::utest_timer

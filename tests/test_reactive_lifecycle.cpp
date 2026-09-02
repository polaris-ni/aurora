// test_reactive_lifecycle.cpp — 覆盖响应式内核（State ↔ Effect）观察者图的
// 生命周期安全。验证「任一侧先析构」都不会再解引用失效对象（此前为双向裸指针悬垂隐患）。
// ── API 覆盖映射 ─────────────────────────────
// state/effect.h(Effect 析构安全/重跑)。

#include <memory>

#include "aurora/aurora.h"
#include "aurora/state/computed.h"
#include "aurora/state/effect.h"
#include "aurora/state/state.h"
#include "aurora/state/state_graph.h"
#include "aurora/state/subscription.h"

#include "test_harness.h"

using aurora::Computed;
using aurora::Effect;
using aurora::State;
using aurora::StateGraph;
using aurora::Subscription;

// T1：State 活得比 Effect 久。Effect 作用域结束后，State.set 不得崩溃，
//     也不得再运行已释放的 Effect（连接应在 notify 时被惰性摘除）。
static void test_state_outlives_effect() {
    State s{ 0 };
    int runs = 0;
    {
        Effect e([&]() -> void {
            (void)s.get();
            ++runs;
        });
        e.run();                       // 订阅并运行一次
        AURORA_TEST_CHECK_EQ(runs, 1); // 作用域内运行一次
    }
    // e 已析构 → 锚点释放 → s 的观察者连接失效
    s.set(5); // 必须不崩溃，且不再运行已死的 Effect
    AURORA_TEST_CHECK_EQ(runs, 1);
    s.set(9);
    AURORA_TEST_CHECK_EQ(runs, 1);
}

// T2：Effect 活得比 State 久（复现原 SEGFAULT 根因：旧 dispose 遍历 m_deps 触碰已死的 State）。
//     现在 dispose 仅清自身，不触碰任何 SignalView，故 State 先析构后再 dispose Effect 必须稳定。
static void test_effect_outlives_state() {
    std::shared_ptr<Effect> e;
    {
        const State s{ 0 };
        e = std::make_shared<Effect>([&]() -> void { (void)s.get(); });
        e->run();
    }
    // s 已析构；e 仍存活，其 m_deps 持有对死 State 的（失效）引用
    e->dispose(); // 旧实现在此崩溃；新实现仅置位 + 清 deps，安全
    AURORA_TEST_CHECK(e->is_disposed());
    // e 在此作用域末尾析构 → 再次 dispose（幂等），亦不得崩溃
}

// T3：Computed 依赖 State 的两种析构顺序。
static void test_computed_lifecycle() {
    // 3a：State 活得比 Computed（内部 Effect）久 —— State 后续 set 不应运行已死的内部 Effect。
    State s{ 1 };
    {
        const Computed<int> c{ [&]() -> int { return s.get() * 2; } };
        AURORA_TEST_CHECK_EQ(c.get(), 2);
    }
    // c 析构 → 内部 Effect 锚点释放 → s 的观察者连接失效
    s.set(5); // 必须不崩溃
    AURORA_TEST_CHECK_EQ(s.get(), 5);

    // 3b：Computed 活得比其依赖 State 久 —— 仅析构，不读取值，必须不崩溃。
    std::shared_ptr<Computed<int>> c2;
    {
        const State s2{ 3 };
        c2 = std::make_shared<Computed<int>>([&]() -> int { return s2.get() + 1; });
        AURORA_TEST_CHECK_EQ(c2->get(), 4);
    }
    // s2 已析构；c2 仍存活，其内部 Effect 的 m_deps 持有失效引用
    // c2 在此作用域末尾析构 → 内部 Effect dispose，不得崩溃
}

// T4：行为回归 —— 响应式核心行为在改造后仍正确。
static void test_reactive_behavior_regression() {
    // State → Effect 响应
    State s{ 0 };
    int seen = -1;
    Effect e([&]() -> void { seen = s.get(); });
    e.run();
    AURORA_TEST_CHECK_EQ(seen, 0);
    s.set(3);
    AURORA_TEST_CHECK_EQ(seen, 3);

    // Computed 重算
    State a{ 2 };
    Computed<int> c{ [&]() -> int { return a.get() * 10; } };
    AURORA_TEST_CHECK_EQ(c.get(), 20);
    a.set(5);
    AURORA_TEST_CHECK_EQ(c.get(), 50);

    // bind RAII：订阅作用域结束后自动取消
    State s2{ 0 };
    int bound = 0;
    {
        Subscription sub = aurora::bind(s2, [&](int v) -> void { bound = v; });
        s2.set(1);
        AURORA_TEST_CHECK_EQ(bound, 1);
    }
    s2.set(2);
    AURORA_TEST_CHECK_EQ(bound, 1); // 已取消

    // StateGraph 仍能在改造后正确产出 "observes" 边
    State sg{ 0 };
    Effect eg([&]() -> void { (void)sg.get(); });
    eg.run();
    auto edges = StateGraph::edges();
    bool has_observes = false;
    for (const auto &ed : edges) {
        if (ed.kind == "observes") {
            has_observes = true;
            break;
        }
    }
    AURORA_TEST_CHECK(has_observes);
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== reactive_lifecycle_test ===\n");
    test_state_outlives_effect();
    test_effect_outlives_state();
    test_computed_lifecycle();
    test_reactive_behavior_regression();
}

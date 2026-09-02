// test_subscription.cpp — 覆盖 T1：RAII Subscription + bind(SignalView&) / bind(Store&) 生命周期。
// ── API 覆盖映射 ─────────────────────────────
// state/computed.h(Computed 派生)、state/reactive.h(State/Effect 内核)、state/binding.h(bind 双向绑定)、
// core/event_stream.h(EventStream 订阅流) → 本文件既有用例行使。

#include <string>

#include "aurora/aurora.h"
#include "aurora/state/computed.h"
#include "aurora/state/reactive.h"
#include "aurora/state/state.h"
#include "aurora/state/store.h"
#include "aurora/state/subscription.h"

#include "test_harness.h"

using aurora::Action;
using aurora::Computed;
using aurora::make_store;
using aurora::Reactive;
using aurora::State;
using aurora::Subscription;

// 1) Subscription 包裹 Store::subscribe：离开作用域自动取消，监听器不再触发。
static void test_subscription_store_raii() {
    int calls = 0;
    const auto store = make_store<int>(0, [](int s, const Action &) -> int { return s; });
    {
        const Subscription sub = aurora::bind(*store, [&](int v) -> void { calls += v + 1; });
        AURORA_TEST_CHECK(sub.active());
        store->dispatch(Action{ "a" }); // s 不变（reducer 恒等），listener 仍被调用一次
        AURORA_TEST_CHECK_EQ(calls, 1);
    }
    // sub 已析构 → 自动取消
    AURORA_TEST_CHECK_EQ(calls, 1);
    store->dispatch(Action{ "b" });
    AURORA_TEST_CHECK_EQ(calls, 1); // 取消后不再触发
}

// 2) bind(State&)：初始应用 + 变化同步；RAII 作用域结束自动取消。
//    注意：订阅结束后不再 set 同一信号源（触及内核既有观察者清理约定，见 T1 说明）。
static void test_bind_state_sync_and_cleanup() {
    State count{ 3 };
    int seen = -1;
    Subscription sub = aurora::bind(count, [&](int v) -> void { seen = v; });
    AURORA_TEST_CHECK(sub.active());
    AURORA_TEST_CHECK_EQ(seen, 3); // 首次立即应用当前值
    count.set(7);
    AURORA_TEST_CHECK_EQ(seen, 7);
    count.set(9);
    AURORA_TEST_CHECK_EQ(seen, 9);

    // 移交所有权后原句柄失效、新句柄有效；reset 后彻底取消（active 翻转）。
    Subscription moved = std::move(sub);
    // 测试意图即验证移动后原句柄失效（active 翻转为 false），非误用。
    // NOLINTNEXTLINE(bugprone-use-after-move)
    AURORA_TEST_CHECK_FALSE(sub.active());
    AURORA_TEST_CHECK(moved.active());
    moved.reset();
    AURORA_TEST_CHECK_FALSE(moved.active());
}

// 3) bind(Reactive&) 与 bind(Computed&)：同样经 SignalView 基类工作；验证依赖链 r→c→bind。
//    所有 set 均在订阅作用域内执行，避免订阅结束后的悬垂（见 T1 说明）。
static void test_bind_reactive_and_computed() {
    Reactive r{ 10 };
    int seen_r = 0;
    {
        Subscription sub = aurora::bind(r, [&](int v) -> void { seen_r = v; });
        AURORA_TEST_CHECK_EQ(seen_r, 10);
        r.set(20);
        AURORA_TEST_CHECK_EQ(seen_r, 20);
    }
    // 订阅结束后不再 set r。

    Computed<int> c{ [&]() -> int { return r.get() * 2; } }; // r 当前为 20 → c 初始 40
    int seen_c = 0;
    {
        Subscription sub = aurora::bind(c, [&](int v) -> void { seen_c = v; });
        AURORA_TEST_CHECK_EQ(seen_c, 40); // 2 * 20
        r.set(5);
        AURORA_TEST_CHECK_EQ(seen_c, 10); // 2 * 5（依赖链 r→c→bind 生效）
    }
    // 订阅结束后不再 set r。
}

// 4) Subscription 移动语义 + release()：取消权转移后仍有效，release 后弃管。
static void test_subscription_move_and_release() {
    int calls = 0;
    const auto store = make_store<int>(0, [](int s, const Action &) -> int { return s; });
    Subscription a = aurora::bind(*store, [&](int) -> void { ++calls; });
    Subscription b = std::move(a); // 移动
    // 测试意图即验证移动后原句柄失效（active 为 false），非误用。
    // NOLINTNEXTLINE(bugprone-use-after-move)
    AURORA_TEST_CHECK_FALSE(a.active());
    AURORA_TEST_CHECK(b.active());
    store->dispatch(Action{ "x" });
    AURORA_TEST_CHECK_EQ(calls, 1);

    const std::function<void()> handle = b.release(); // 弃管，b 不再持有
    AURORA_TEST_CHECK_FALSE(b.active());
    AURORA_TEST_CHECK_EQ(calls, 1);
    handle(); // 手动取消
    store->dispatch(Action{ "y" });
    AURORA_TEST_CHECK_EQ(calls, 1); // 已取消
}

AURORA_TEST() {
    AURORA_TEST_PRINTF("=== subscription_test ===\n");
    test_subscription_store_raii();
    test_bind_state_sync_and_cleanup();
    test_bind_reactive_and_computed();
    test_subscription_move_and_release();
}

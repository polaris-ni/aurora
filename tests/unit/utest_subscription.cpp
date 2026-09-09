/// 测试类型: unit
/// 目标单元: include/aurora/state/subscription.h
/// 测试说明: Subscription 句柄的活跃状态、幂等 reset、移动语义与 release 交接，以及 connect(State)/connect(Store)
/// 的立即应用、增量回调与析构自动退订

#include <vector>

#include "aurora/state/subscription.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_subscription {

AURORA_TEST_CASE(default_subscription_is_inactive_and_safe) {
    // 空句柄：未订阅态，reset/release 均安全。
    Subscription sub;
    AURORA_TEST_CHECK_FALSE(sub.active());
    AURORA_TEST_CHECK_NO_THROW(sub.reset());

    const auto released = sub.release();
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(released));  // 释放空句柄仍是空

    Subscription from_empty{std::function<void()>()};
    AURORA_TEST_CHECK_FALSE(from_empty.active());
}

AURORA_TEST_CASE(reset_invokes_cancel_once_and_is_idempotent) {
    int cancelled = 0;
    Subscription sub{[&cancelled]() -> void { ++cancelled; }};
    AURORA_TEST_CHECK_TRUE(sub.active());

    sub.reset();
    AURORA_TEST_CHECK_EQ(cancelled, 1);
    AURORA_TEST_CHECK_FALSE(sub.active());

    sub.reset();  // 幂等：已空的句柄重复 reset 不再触发
    AURORA_TEST_CHECK_EQ(cancelled, 1);
}

AURORA_TEST_CASE(move_semantics_transfer_ownership_and_flush_pending_cancel) {
    // 移动构造：所有权转移，源句柄置空。
    int first = 0;
    Subscription s1{[&first]() -> void { ++first; }};
    Subscription s2 = std::move(s1);
    AURORA_TEST_CHECK_TRUE(s2.active());
    // 本用例正是验证「移动后源句柄被置空」的契约：对 moved-from 的 s1 调用 active()
    // 是刻意断言其已进入非活跃态，并非误用移动后对象，故抑制 use-after-move。
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_FALSE(s1.active());
    s2.reset();
    AURORA_TEST_CHECK_EQ(first, 1);

    // 移动赋值：先 reset 自身旧句柄（清算未执行的取消），再接管新句柄。
    int old_cancelled = 0;
    Subscription s3{[&old_cancelled]() -> void { ++old_cancelled; }};
    int new_cancelled = 0;
    Subscription s4{[&new_cancelled]() -> void { ++new_cancelled; }};
    s3 = std::move(s4);
    AURORA_TEST_CHECK_EQ(old_cancelled, 1);
    AURORA_TEST_CHECK_TRUE(s3.active());
    // 本用例验证「移动赋值后源句柄 s4 被置空」的契约：对 moved-from 的 s4 调用
    // active() 是刻意断言其已进入非活跃态，并非误用，故抑制 use-after-move。
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_FALSE(s4.active());
    s3.reset();
    AURORA_TEST_CHECK_EQ(new_cancelled, 1);
    AURORA_TEST_CHECK_EQ(old_cancelled, 1);  // 旧句柄不会被再次触发
}

AURORA_TEST_CASE(release_returns_handle_and_disables_auto_cancel) {
    // release：放弃所有权，析构不再自动取消；句柄由调用方手动执行。
    int cancelled = 0;
    Subscription sub{[&cancelled]() -> void { ++cancelled; }};
    const auto handle = sub.release();
    AURORA_TEST_CHECK_FALSE(sub.active());
    AURORA_TEST_CHECK_EQ(cancelled, 0);  // release 本身不触发取消
    handle();
    AURORA_TEST_CHECK_EQ(cancelled, 1);
}

AURORA_TEST_CASE(connect_state_applies_current_value_then_tracks_updates) {
    // connect(State)：立即用当前值回调一次，此后每次 set 增量回调最新值。
    State<int> s{1};
    std::vector<int> seen;
    const auto sub = connect(s, [&seen](int v) -> void { seen.push_back(v); });
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
    AURORA_TEST_CHECK_EQ(seen.front(), 1);

    s.set(2);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    AURORA_TEST_CHECK_EQ(seen.back(), 2);

    s.set(3);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 3U);
    AURORA_TEST_CHECK_EQ(seen.back(), 3);
}

AURORA_TEST_CASE(connect_state_subscription_scopes_to_destruction) {
    // RAII 契约：离开作用域自动退订；底层 Effect 被安全 dispose，后续 set 不再触发且无悬垂。
    // 计数含立即应用 1 次：connect 当下回调 calls=1，set(1) 后 calls=2。
    State<int> s{0};
    int calls = 0;
    {
        const auto sub = connect(s, [&calls](int) -> void { ++calls; });
        s.set(1);
        AURORA_TEST_REQUIRE_EQ(calls, 2);
    }
    s.set(2);  // 订阅已随作用域结束自动取消
    AURORA_TEST_CHECK_EQ(calls, 2);
    AURORA_TEST_CHECK_NO_THROW(s.set(3));
    AURORA_TEST_CHECK_EQ(calls, 2);
}

AURORA_TEST_CASE(connect_store_receives_new_state_on_dispatch) {
    // connect(Store)：不做立即应用；每次 dispatch 产生新状态即回调一次；析构自动退订。
    const auto reducer = [](const int& current, const Action& action) -> int {
        const int* v = action.payload_as<int>();
        return v != nullptr ? *v : current;
    };
    Store<int> store{0, reducer};
    std::vector<int> seen;
    {
        const auto sub = connect(store, [&seen](const int& v) -> void { seen.push_back(v); });
        AURORA_TEST_CHECK_TRUE(seen.empty());  // 与 State 重载不同：不立即应用

        store.dispatch(Action{"set", 5});
        AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);
        AURORA_TEST_CHECK_EQ(seen.back(), 5);

        store.dispatch(Action{"set", 9});
        AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
        AURORA_TEST_CHECK_EQ(seen.back(), 9);
    }
    store.dispatch(Action{"set", 100});  // 已退订，不再回调
    AURORA_TEST_CHECK_EQ(seen.size(), 2U);
}

}  // namespace aurora::test_cases::utest_subscription

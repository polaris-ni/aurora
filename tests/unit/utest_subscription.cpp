/// 测试类型: unit
/// 目标单元: include/aurora/state/subscription.h
/// 测试说明: Subscription 的活跃状态、幂等 reset、移动构造与移动赋值（接管前先清算旧句柄）、
/// 析构即取消；与 State / ReadOnlyState / effect 的实际退订联动

#include <functional>
#include <utility>
#include <vector>

#include "aurora/state/effect.h"
#include "aurora/state/read_only_state.h"
#include "aurora/state/state.h"
#include "aurora/state/subscription.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_subscription {

AURORA_TEST_CASE(default_subscription_is_inactive_and_reset_is_safe) {
    // 空句柄：未订阅态，reset 安全且幂等。
    Subscription sub;
    AURORA_TEST_CHECK_FALSE(sub.active());
    AURORA_TEST_CHECK_NO_THROW(sub.reset());
    AURORA_TEST_CHECK_FALSE(sub.active());

    // 空取消回调构造的句柄同样是非活跃态。
    Subscription from_empty{std::function<void()>{}};
    AURORA_TEST_CHECK_FALSE(from_empty.active());
}

AURORA_TEST_CASE(reset_invokes_cancel_once_and_is_idempotent) {
    int cancelled = 0;
    Subscription sub{[&cancelled]() -> void { ++cancelled; }};
    AURORA_TEST_CHECK_TRUE(sub.active());

    sub.reset();
    AURORA_TEST_CHECK_EQ(cancelled, 1);
    AURORA_TEST_CHECK_FALSE(sub.active());

    sub.reset();  // 已空的句柄重复 reset 不再触发
    AURORA_TEST_CHECK_EQ(cancelled, 1);
}

AURORA_TEST_CASE(move_semantics_transfer_ownership_and_flush_pending_cancel) {
    // 移动构造：所有权转移，源句柄置空。
    int first = 0;
    Subscription s1{[&first]() -> void { ++first; }};
    Subscription s2 = std::move(s1);
    AURORA_TEST_CHECK_TRUE(s2.active());
    // 契约就是「移动后源句柄进入非活跃态」：此断言刻意读取 moved-from 对象。
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_FALSE(s1.active());
    s2.reset();
    AURORA_TEST_CHECK_EQ(first, 1);

    // 移动赋值：先清算自身旧句柄（即时取消），再接管新句柄。
    int old_cancelled = 0;
    Subscription s3{[&old_cancelled]() -> void { ++old_cancelled; }};
    int new_cancelled = 0;
    Subscription s4{[&new_cancelled]() -> void { ++new_cancelled; }};
    s3 = std::move(s4);
    AURORA_TEST_CHECK_EQ(old_cancelled, 1);
    AURORA_TEST_CHECK_TRUE(s3.active());
    // 同上：刻意断言 moved-from 的 s4 已非活跃。
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_FALSE(s4.active());
    s3.reset();
    AURORA_TEST_CHECK_EQ(new_cancelled, 1);
    AURORA_TEST_CHECK_EQ(old_cancelled, 1);  // 旧句柄不会被二次触发
}

AURORA_TEST_CASE(subscription_from_state_subscribe_stops_after_reset) {
    // State::subscribe 返回的凭证：reset 之后写入不再回调。
    State<int> s{0};
    std::vector<int> seen;
    auto sub = s.subscribe([&](const int &v) { seen.push_back(v); });
    AURORA_TEST_REQUIRE_EQ(seen.size(), 1U);  // 立即交付首值
    s.set(1);
    AURORA_TEST_REQUIRE_EQ(seen.size(), 2U);
    sub.reset();
    s.set(2);
    AURORA_TEST_CHECK_EQ(seen.size(), 2U);
}

AURORA_TEST_CASE(subscription_scopes_to_destruction) {
    // RAII：凭证随作用域结束自动退订；退出后再写不再回调。
    State<int> s{0};
    int calls = 0;
    {
        auto sub = s.subscribe([&](const int &) { ++calls; });
        s.set(1);
        AURORA_TEST_REQUIRE_EQ(calls, 2);  // 首值 + 变化
    }
    s.set(2);
    AURORA_TEST_CHECK_EQ(calls, 2);
    AURORA_TEST_CHECK_NO_THROW(s.set(3));
    AURORA_TEST_CHECK_EQ(calls, 2);
}

AURORA_TEST_CASE(subscription_move_keeps_underlying_observation_alive) {
    // 移动交接到新凭证后，订阅本体不变、继续生效；旧凭证析构不取消。
    State<int> s{0};
    int calls = 0;
    Subscription outer;
    {
        auto sub = s.subscribe([&](const int &) { ++calls; });
        outer = std::move(sub);
        AURORA_TEST_CHECK_EQ(calls, 1);
    }  // sub 已移动：析构不取消
    s.set(1);
    AURORA_TEST_CHECK_EQ(calls, 2);
    outer.reset();
    s.set(2);
    AURORA_TEST_CHECK_EQ(calls, 2);
}

AURORA_TEST_CASE(read_only_state_subscription_tracks_shared_source) {
    // 只读句柄的订阅跟随其来源：可写句柄写入照常回调。
    State<int> s{0};
    ReadOnlyState<int> ro = s;
    int calls = 0;
    auto sub = ro.subscribe([&](const int &) { ++calls; });
    AURORA_TEST_CHECK_EQ(calls, 1);
    s.set(1);
    AURORA_TEST_CHECK_EQ(calls, 2);
}

}  // namespace aurora::test_cases::utest_subscription

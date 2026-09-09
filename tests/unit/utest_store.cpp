/// 测试类型: unit
/// 目标单元: include/aurora/state/store.h
/// 测试说明: Action 的类型擦除载荷存取、Store 经 reducer 的单向数据流（状态更新/新值旧值通知/订阅与退订/as_signal 信号视图）与 make_store 共享所有权

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/state/store.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_store {

AURORA_TEST_CASE(action_carries_typed_payload) {
    const Action with_payload{"set_count", 42};
    AURORA_TEST_CHECK_STREQ(with_payload.type, "set_count");
    const int* value = with_payload.payload_as<int>();
    AURORA_TEST_REQUIRE_NOT_NULL(value);
    AURORA_TEST_CHECK_EQ(*value, 42);

    // 无载荷动作：payload 为空，payload_as 恒返回 nullptr（任意 T）。
    const Action no_payload{"increment"};
    AURORA_TEST_CHECK_TRUE(no_payload.payload == nullptr);
    AURORA_TEST_CHECK_NULL(no_payload.payload_as<int>());
    AURORA_TEST_CHECK_NULL(no_payload.payload_as<std::string>());
}

AURORA_TEST_CASE(store_dispatch_runs_reducer_and_updates_state) {
    auto store = make_store<int>(0, [](const int& state, const Action& action) -> int {
        if (action.type == "add") {
            const int* delta = action.payload_as<int>();
            return delta != nullptr ? state + *delta : state;
        }
        if (action.type == "reset") {
            return 0;
        }
        return state;
    });
    AURORA_TEST_REQUIRE_NOT_NULL(store);
    AURORA_TEST_CHECK_EQ(store->get_state(), 0);

    // reducer 第一参收到当前状态：增量在既有状态上累积。
    store->dispatch(Action{"add", 5});
    AURORA_TEST_CHECK_EQ(store->get_state(), 5);
    store->dispatch(Action{"add", 7});
    AURORA_TEST_CHECK_EQ(store->get_state(), 12);
    store->dispatch(Action{"reset"});
    AURORA_TEST_CHECK_EQ(store->get_state(), 0);
}

AURORA_TEST_CASE(store_listener_receives_new_and_prev_state) {
    // Listener 签名为 (newState, prevState)：两次派发的序对逐一核对。
    std::vector<std::pair<int, int>> observed;
    auto store = make_store<int>(0, [](const int& s, const Action&) { return s + 1; });
    store->subscribe([&observed](const int& next, const int& prev) { observed.push_back({next, prev}); });

    store->dispatch(Action{"increment"});
    store->dispatch(Action{"increment"});

    AURORA_TEST_REQUIRE_EQ(observed.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(observed[0].first, 1);
    AURORA_TEST_CHECK_EQ(observed[0].second, 0);
    AURORA_TEST_CHECK_EQ(observed[1].first, 2);
    AURORA_TEST_CHECK_EQ(observed[1].second, 1);
}

AURORA_TEST_CASE(store_unsubscribe_stops_listener) {
    int first_calls = 0;
    int second_calls = 0;
    auto store = make_store<int>(0, [](const int& s, const Action&) { return s + 1; });
    auto unsubscribe_first = store->subscribe([&first_calls](const int&, const int&) { ++first_calls; });
    store->subscribe([&second_calls](const int&, const int&) { ++second_calls; });

    store->dispatch(Action{"increment"});
    AURORA_TEST_CHECK_EQ(first_calls, 1);
    AURORA_TEST_CHECK_EQ(second_calls, 1);

    // 退订仅移除目标监听，其余监听照常；重复调用句柄安全（惰性置空）。
    unsubscribe_first();
    store->dispatch(Action{"increment"});
    AURORA_TEST_CHECK_EQ(first_calls, 1);
    AURORA_TEST_CHECK_EQ(second_calls, 2);

    unsubscribe_first();
    store->dispatch(Action{"increment"});
    AURORA_TEST_CHECK_EQ(first_calls, 1);
    AURORA_TEST_CHECK_EQ(second_calls, 3);
}

AURORA_TEST_CASE(store_as_signal_reflects_dispatched_state) {
    auto store = make_store<std::string>(std::string{"init"}, [](const std::string&, const Action& action) {
        const std::string* next = action.payload_as<std::string>();
        return next != nullptr ? *next : std::string{};
    });
    const std::shared_ptr<State<std::string>> signal = store->as_signal();
    AURORA_TEST_CHECK_STREQ(signal->get(), "init");

    // 派发写穿内部 State<S>：信号视图随 dispatch 同步更新。
    store->dispatch(Action{"rename", std::string{"next"}});
    AURORA_TEST_CHECK_STREQ(signal->get(), "next");
    AURORA_TEST_CHECK_STREQ(store->get_state(), "next");

    // as_signal 稳定返回同一信号实例。
    AURORA_TEST_CHECK(store->as_signal() == signal);
}

AURORA_TEST_CASE(make_store_shares_ownership_and_reduces_from_initial) {
    auto store = make_store<int>(3, [](const int& s, const Action&) { return s * 2; });
    AURORA_TEST_CHECK_EQ(store->get_state(), 3);

    // 首次派发：reducer 收到初始状态，通知携带 prev = 初始值。
    int prev_seen = -1;
    store->subscribe([&prev_seen](const int&, const int& prev) { prev_seen = prev; });
    store->dispatch(Action{"double"});
    AURORA_TEST_CHECK_EQ(store->get_state(), 6);
    AURORA_TEST_CHECK_EQ(prev_seen, 3);

    // 工厂返回共享所有权：最后一个引用释放即销毁。
    std::weak_ptr<Store<int>> weak = store;
    AURORA_TEST_CHECK_FALSE(weak.expired());
    store.reset();
    AURORA_TEST_CHECK_TRUE(weak.expired());
}

}  // namespace aurora::test_cases::utest_store

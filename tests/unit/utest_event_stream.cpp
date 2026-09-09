/// 测试类型: unit
/// 目标单元: include/aurora/core/event_stream.h
/// 测试说明: EventStream 的订阅与发射、多订阅者按订阅顺序收到、Subscription RAII 退订、reset
/// 幂等、移动语义所有权转移与自退订、未知 id 退订 no-op、空流发射安全与多流独立

#include <string>
#include <utility>
#include <vector>

#include "aurora/core/event_stream.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_event_stream {

namespace m = aurora::testing::matchers;

AURORA_TEST_CASE(subscribe_and_emit_delivers_value) {
    aurora::EventStream<int> stream;
    int received = 0;
    const auto sub = stream.subscribe([&received](const int& v) -> void { received = v; });
    AURORA_TEST_CHECK(static_cast<bool>(sub));
    stream.emit(5);
    AURORA_TEST_CHECK_EQ(received, 5);
}

AURORA_TEST_CASE(emit_reaches_all_subscribers_in_subscription_order) {
    aurora::EventStream<int> stream;
    std::vector<int> order;
    const auto first = stream.subscribe([&order](const int& v) -> void { order.push_back(100 + v); });
    const auto second = stream.subscribe([&order](const int& v) -> void { order.push_back(200 + v); });
    AURORA_TEST_CHECK(static_cast<bool>(first));
    AURORA_TEST_CHECK(static_cast<bool>(second));

    stream.emit(1);
    AURORA_TEST_REQUIRE_THAT(order, m::size_is(2));
    // 内部为按 id 升序的 std::map：先订阅者先收到。
    AURORA_TEST_CHECK_EQ(order[0], 101);
    AURORA_TEST_CHECK_EQ(order[1], 201);
}

AURORA_TEST_CASE(subscription_destructor_unsubscribes) {
    aurora::EventStream<int> stream;
    int calls = 0;
    {
        auto sub = stream.subscribe([&calls](const int&) -> void { ++calls; });
        stream.emit(1);
        AURORA_TEST_CHECK_EQ(calls, 1);
    }
    // 句柄析构已自动退订：后续发射不再回调。
    stream.emit(2);
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(reset_unsubscribes_and_double_reset_safe) {
    aurora::EventStream<int> stream;
    int calls = 0;
    auto sub = stream.subscribe([&calls](const int&) -> void { ++calls; });
    AURORA_TEST_CHECK(static_cast<bool>(sub));
    sub.reset();
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(sub));
    stream.emit(1);
    AURORA_TEST_CHECK_EQ(calls, 0);

    sub.reset();  // 已退订句柄重复 reset 为 no-op
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(sub));
}

AURORA_TEST_CASE(subscription_move_semantics_transfer_ownership) {
    aurora::EventStream<int> stream;
    int calls = 0;
    auto moved_from = stream.subscribe([&calls](const int&) -> void { ++calls; });
    auto moved_to = std::move(moved_from);
    // 本用例的断言目标就是「移动后源句柄失效」：对 moved-from 做只读的 bool 转换是刻意检查，
    // 非误用；改写将破坏被测语义（Subscription 移动后置空属实现契约）。
    // NOLINTNEXTLINE(bugprone-use-after-move)
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(moved_from));
    AURORA_TEST_CHECK(static_cast<bool>(moved_to));
    stream.emit(1);
    AURORA_TEST_CHECK_EQ(calls, 1);

    // 移动赋值：目标句柄先释放自己的原订阅，再接管新订阅。
    auto other = stream.subscribe([&calls](const int&) -> void { calls += 10; });
    other = std::move(moved_to);
    stream.emit(2);
    AURORA_TEST_CHECK_EQ(calls, 2);  // +10 的订阅已随赋值释放，仅原订阅收到

    other.reset();
    stream.emit(3);
    AURORA_TEST_CHECK_EQ(calls, 2);  // 全部退订后不再回调
}

AURORA_TEST_CASE(unsubscribe_unknown_id_is_noop) {
    aurora::EventStream<int> stream;
    // 公开退订入口对未知 id 安全。
    AURORA_TEST_CHECK_NO_THROW(stream.unsubscribe(9999));

    int calls = 0;
    auto sub = stream.subscribe([&calls](const int&) -> void { ++calls; });
    AURORA_TEST_CHECK(static_cast<bool>(sub));
    stream.emit(1);
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(independent_streams_and_empty_emit) {
    aurora::EventStream<std::string> a;
    aurora::EventStream<std::string> b;
    std::vector<std::string> seen;
    const auto sub_a = a.subscribe([&seen](const std::string& v) -> void { seen.push_back("a:" + v); });
    const auto sub_b = b.subscribe([&seen](const std::string& v) -> void { seen.push_back("b:" + v); });
    AURORA_TEST_CHECK(static_cast<bool>(sub_a));
    AURORA_TEST_CHECK(static_cast<bool>(sub_b));

    a.emit("x");
    AURORA_TEST_REQUIRE_EQ(seen.size(), std::size_t{1});
    AURORA_TEST_CHECK_THAT(seen[0], m::str_eq("a:x"));  // 流之间互不串扰

    // 无订阅者时发射是安全 no-op。
    aurora::EventStream<int> empty;
    AURORA_TEST_CHECK_NO_THROW(empty.emit(1));
}

}  // namespace aurora::test_cases::utest_event_stream

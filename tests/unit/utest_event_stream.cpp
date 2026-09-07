/// 测试类型: unit
/// 目标单元: include/aurora/core/event_stream.h
/// 测试说明: 值类型事件流 EventStream（订阅/发射/RAII 退订/句柄移动）单元测试

#include <string>
#include <utility>
#include <vector>

#include "aurora/core/event_stream.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_event_stream {

AURORA_TEST() {
    // ---- 1. 订阅后 emit 能收到值 ----
    {
        EventStream<int> stream;
        int got = 0;
        const auto sub = stream.subscribe([&got](const int &v) { got = v; });
        stream.emit(42);
        AURORA_TEST_CHECK(got == 42);
    }

    // ---- 2. 未订阅时 emit 为空操作 ----
    {
        EventStream<int> stream;
        stream.emit(1);  // 不得崩溃
        AURORA_TEST_CHECK(true);
    }

    // ---- 3. 多订阅者按 id 升序收到同一事件 ----
    {
        EventStream<int> stream;
        std::vector<int> order;
        const auto a = stream.subscribe([&order](const int &v) { order.push_back(v * 10); });
        const auto b = stream.subscribe([&order](const int &v) { order.push_back(v * 100); });
        stream.emit(1);
        AURORA_TEST_CHECK_EQ(order.size(), 2U);
        AURORA_TEST_CHECK(order[0] == 10);
        AURORA_TEST_CHECK(order[1] == 100);
    }

    // ---- 4. 多次 emit 累计 ----
    {
        EventStream<int> stream;
        int sum = 0;
        const auto sub = stream.subscribe([&sum](const int &v) { sum += v; });
        stream.emit(1);
        stream.emit(2);
        stream.emit(3);
        AURORA_TEST_CHECK(sum == 6);
    }

    // ---- 5. Subscription 析构自动退订 ----
    {
        EventStream<int> stream;
        int hits = 0;
        {
            const auto sub = stream.subscribe([&hits](const int &) { ++hits; });
            stream.emit(1);
            AURORA_TEST_CHECK(hits == 1);
        }
        stream.emit(2);
        AURORA_TEST_CHECK(hits == 1);  // 退订后不再收到
    }

    // ---- 6. 显式 reset 退订，且可重复调用（幂等） ----
    {
        EventStream<int> stream;
        int hits = 0;
        auto sub = stream.subscribe([&hits](const int &) { ++hits; });
        stream.emit(1);
        AURORA_TEST_CHECK(hits == 1);

        sub.reset();
        AURORA_TEST_CHECK(!static_cast<bool>(sub));
        sub.reset();  // 幂等
        stream.emit(2);
        AURORA_TEST_CHECK(hits == 1);
    }

    // ---- 7. 活跃订阅句柄为真 ----
    {
        EventStream<int> stream;
        const auto sub = stream.subscribe([](const int &) {});
        AURORA_TEST_CHECK(static_cast<bool>(sub));
    }

    // ---- 8. 移动构造：新句柄接管，原句柄失效（不重复退订） ----
    {
        EventStream<int> stream;
        int hits = 0;
        {
            auto a = stream.subscribe([&hits](const int &) { ++hits; });
            {
                auto b = std::move(a);
                AURORA_TEST_CHECK(static_cast<bool>(b));
                AURORA_TEST_CHECK(!static_cast<bool>(a));  // 已转移
                stream.emit(1);
                AURORA_TEST_CHECK(hits == 1);
            }
            // b 析构退订；a 已无宿主，析构不应二次退订
            stream.emit(2);
            AURORA_TEST_CHECK(hits == 1);
        }
    }

    // ---- 9. 移动赋值：先退订自身再接管 ----
    {
        EventStream<int> stream;
        int x = 0;
        int y = 0;
        auto a = stream.subscribe([&x](const int &) { ++x; });
        auto b = stream.subscribe([&y](const int &) { ++y; });
        a = std::move(b);  // a 原订阅被退订，接管 b 的
        stream.emit(1);
        AURORA_TEST_CHECK(x == 0);
        AURORA_TEST_CHECK(y == 1);
    }

    // ---- 10. 非平凡负载（字符串）按引用传递 ----
    {
        EventStream<std::string> stream;
        std::string got;
        const auto sub = stream.subscribe([&got](const std::string &s) { got = s; });
        stream.emit(std::string("payload"));
        AURORA_TEST_CHECK(got == "payload");
    }

    // ---- 11. 默认构造的 Subscription 为空句柄 ----
    {
        EventStream<int>::Subscription empty{};
        AURORA_TEST_CHECK(!static_cast<bool>(empty));
        empty.reset();  // 空句柄 reset 安全
        AURORA_TEST_CHECK(true);
    }
}

}  // namespace aurora::test_cases::utest_event_stream

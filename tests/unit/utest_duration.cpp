/// 测试类型: unit
/// 目标单元: include/aurora/core/duration.h
/// 测试说明: 时长强类型 Duration（秒存储 / 工厂 / chrono 互转 / 相等语义）与毫秒字面量单元测试

#include <chrono>

#include "aurora/core/duration.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_duration {

static_assert(Duration::from_ms(1000.0).seconds == 1.0);
static_assert(Duration::from_seconds(2.0).seconds == 2.0);

AURORA_TEST() {
    // ---- 1. 默认构造为零时长 ----
    {
        const Duration d{};
        AURORA_TEST_CHECK(d.seconds == 0.0);
    }

    // ---- 2. 显式双精度构造与 from_seconds 等价 ----
    {
        const Duration a{1.5};
        const auto b = Duration::from_seconds(1.5);
        AURORA_TEST_CHECK(a.seconds == 1.5);
        AURORA_TEST_CHECK(a == b);
    }

    // ---- 3. from_ms 做毫秒→秒换算 ----
    {
        const auto d = Duration::from_ms(250.0);
        AURORA_TEST_CHECK(d.seconds == 0.25);

        const auto e = Duration::from_ms(1000.0);
        AURORA_TEST_CHECK(e == Duration::from_seconds(1.0));
    }

    // ---- 4. 负时长可表达（用于反向/回退动画） ----
    {
        const auto d = Duration::from_seconds(-0.5);
        AURORA_TEST_CHECK(d.seconds == -0.5);
        AURORA_TEST_CHECK(d != Duration::from_seconds(0.5));
    }

    // ---- 5. to_chrono 与 std::chrono 互操作 ----
    {
        const auto d = Duration::from_seconds(2.0);
        const auto c = d.to_chrono();
        AURORA_TEST_CHECK(c == std::chrono::duration<double>(2.0));
        AURORA_TEST_CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(c).count() == 2000);
    }

    // ---- 6. 相等/不等语义（按秒值比较） ----
    {
        AURORA_TEST_CHECK(Duration::from_seconds(1.0) == Duration::from_ms(1000.0));
        AURORA_TEST_CHECK(Duration::from_seconds(1.0) != Duration::from_seconds(1.001));
    }

    // ---- 7. 拷贝后仍相等（值语义） ----
    {
        const auto a = Duration::from_seconds(3.0);
        const Duration b = a;
        AURORA_TEST_CHECK(a == b);
        AURORA_TEST_CHECK(b.seconds == 3.0);
    }

    // ---- 8. 毫秒字面量与 from_ms 等价 ----
    {
        using namespace aurora::literals;  // NOLINT(build/namespaces_literals) —— 测试点即字面量
        const auto d = 250_ms;
        AURORA_TEST_CHECK(d.seconds == 0.25);

        const auto e = 1500_ms;
        AURORA_TEST_CHECK(e == Duration::from_seconds(1.5));
    }

    // ---- 9. constexpr 场景：编译期可求值的时长换算 ----
    {
        constexpr auto half = Duration::from_ms(500.0);
        static_assert(half.seconds == 0.5);
        AURORA_TEST_CHECK(half.seconds == 0.5);
    }
}

}  // namespace aurora::test_cases::utest_duration

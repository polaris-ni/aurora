/// 测试类型: unit
/// 目标单元: include/aurora/core/duration.h
/// 测试说明: 覆盖 Duration 构造与显式性、from_seconds/from_ms 换算、to_chrono、相等比较与 _ms 字面量

#include <chrono>
#include <type_traits>

#include "aurora/core/duration.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_duration {

namespace au = aurora;

/// @brief 默认构造为零时长。
AURORA_TEST_CASE(default_ctor_is_zero) {
    constexpr aurora::Duration d{};
    static_assert(d.seconds == 0.0);
    AURORA_TEST_CHECK_EQ(d.seconds, 0.0);
}

/// @brief 单参构造为 explicit（秒），禁止 double 隐式收窄成时长。
AURORA_TEST_CASE(explicit_ctor_takes_seconds) {
    constexpr aurora::Duration d{1.5};
    static_assert(d.seconds == 1.5);
    static_assert(std::is_constructible_v<aurora::Duration, double>);
    static_assert(!std::is_convertible_v<double, aurora::Duration>);  // explicit 的编译期契约
    static_assert(!std::is_convertible_v<int, aurora::Duration>);
    AURORA_TEST_CHECK_EQ(d.seconds, 1.5);
}

/// @brief from_seconds 原样保存；from_ms 除以 1000（250ms == 0.25s 精确可表示）。
AURORA_TEST_CASE(from_seconds_and_from_ms_conversions) {
    constexpr auto s = aurora::Duration::from_seconds(1.5);
    static_assert(s.seconds == 1.5);
    constexpr auto ms = aurora::Duration::from_ms(250.0);
    static_assert(ms.seconds == 0.25);  // 250/1000.0 在二进制浮点下精确
    AURORA_TEST_CHECK_EQ(s.seconds, 1.5);
    AURORA_TEST_CHECK_EQ(ms.seconds, 0.25);
    AURORA_TEST_CHECK_NEAR(aurora::Duration::from_ms(100.0).seconds, 0.1, 1e-12);  // 0.1 非精确二进制数
    AURORA_TEST_CHECK_EQ(aurora::Duration::from_ms(1500.0).seconds, 1.5);
    AURORA_TEST_CHECK_EQ(aurora::Duration::from_ms(0.0).seconds, 0.0);
}

/// @brief to_chrono 返回 std::chrono::duration<double>，秒数保持一致。
AURORA_TEST_CASE(to_chrono_preserves_seconds) {
    static_assert(std::is_same_v<decltype(aurora::Duration{}.to_chrono()), std::chrono::duration<double>>);
    constexpr auto d = aurora::Duration::from_seconds(2.5);
    static_assert(d.to_chrono().count() == 2.5);
    AURORA_TEST_CHECK_NEAR(d.to_chrono().count(), 2.5, 1e-12);
    AURORA_TEST_CHECK_NEAR(aurora::Duration::from_ms(250.0).to_chrono().count(), 0.25, 1e-12);
}

/// @brief 相等比较按秒值逐位判断（== / != 互为否定）。
AURORA_TEST_CASE(equality_compares_seconds) {
    constexpr auto a = aurora::Duration::from_seconds(1.0);
    constexpr auto b = aurora::Duration{1.0};
    constexpr auto c = aurora::Duration::from_ms(1001.0);
    static_assert(a == b);
    static_assert(a != c);
    AURORA_TEST_CHECK(a == b);
    AURORA_TEST_CHECK(a != c);
    AURORA_TEST_CHECK_FALSE(a == c);
    AURORA_TEST_CHECK_FALSE(a != b);
}

/// @brief _ms 字面量映射为秒（250_ms == 0.25s），constexpr 可用于编译期断言。
AURORA_TEST_CASE(ms_literal_maps_to_seconds) {
    using aurora::literals::operator""_ms;  // using-declaration：仅引入具名字面量（库约定：TU 内显式引入）

    static_assert(250_ms == aurora::Duration{0.25});
    AURORA_TEST_CHECK((250_ms) == aurora::Duration::from_ms(250.0));
    AURORA_TEST_CHECK_EQ((0_ms).seconds, 0.0);
    AURORA_TEST_CHECK_NEAR((12.5_ms).seconds, aurora::Duration::from_ms(12.5).seconds, 1e-12);  // long double 重载
}

/// @brief 边界：负时长与大值原样保存（当前契约对取值范围无前置条件校验）。
AURORA_TEST_CASE(negative_and_large_values_are_preserved) {
    constexpr auto neg = aurora::Duration{-1.5};
    static_assert(neg.seconds == -1.5);
    AURORA_TEST_CHECK_EQ(aurora::Duration::from_seconds(-0.25).seconds, -0.25);
    AURORA_TEST_CHECK_NEAR(aurora::Duration::from_ms(-250.0).seconds, -0.25, 1e-12);
    AURORA_TEST_CHECK_EQ(aurora::Duration::from_seconds(1.0e9).seconds, 1.0e9);
    AURORA_TEST_CHECK(neg != aurora::Duration{1.5});
}

}  // namespace aurora::test_cases::utest_duration

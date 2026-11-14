/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/frame_clock.h
/// 测试说明: 动画帧时钟的**纯时序状态机**——帧延迟表登记与钳正、按帧 delay 推进、
/// 环形回绕、追帧单轮上限（burst 64）与停摆重起算、单帧 / 未登记不启动、
/// `ms_until_due` 的等待收敛读数（未到期 / 恰到期 / 过期均非负）。
///
/// 变异自证（改坏实现应转红的条目）：
///  * 把换帧判定改成「到期就跳两帧」（advance 内连加两次）→ `..._advances_one_frame_per_due`
///    转红（帧序错位、回绕点错帧）；
///  * 摘掉追帧上限（BURST_MAX 改 `INT_MAX`）→ `..._burst_is_capped` 转红（停摆 100 帧的一
///    次调用连跳 100 帧而非 64 帧）；
///  * 把单帧延迟钳正删掉（0 延迟照用）→ `..._clamps_zero_delay` 转红（deadline 不前进，
///    同一时刻无限换帧）。

#include <chrono>
#include <cstdint>
#include <vector>

#include "aurora/window/detail/frame_clock.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_frame_clock {

using aurora::detail::FrameClock;
using Clock = FrameClock::Clock;
using TimePoint = FrameClock::TimePoint;

[[nodiscard]] auto at(TimePoint base, long long ms) -> TimePoint { return base + std::chrono::milliseconds{ms}; }

AURORA_TEST_CASE(frame_clock_advances_one_frame_per_due) {
    FrameClock clock;
    clock.load({33U, 16U, 50U});
    const TimePoint t0 = Clock::now();
    clock.start(t0);
    AURORA_TEST_CHECK_TRUE(clock.running());
    AURORA_TEST_CHECK_EQ(clock.frame(), 0);
    AURORA_TEST_CHECK_EQ(clock.frames(), 3);

    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 32)), -1);  // 第 0 帧（33ms）未到期
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 33)), 1);  // → 第 1 帧（16ms）
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 40)), -1);
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 49)), 2);  // → 第 2 帧（50ms），截止 49+50=99
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 99)), 0);  // 环形回绕 → 第 0 帧
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 100)), -1);  // 新截止 99+33=132，未到期
}

AURORA_TEST_CASE(frame_clock_burst_is_capped) {
    FrameClock clock;
    std::vector<std::uint32_t> delays(100U, 10U);  // 100 帧 × 10ms
    clock.load(delays);
    const TimePoint t0 = Clock::now();
    clock.start(t0);
    // 停摆 1000ms = 100 个换帧点：单轮至多连跳 64 帧，其余由后续帧继续。
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 1000)), 64);
    // 上限触发后从当前时刻按当前帧延迟重新起算，不是无限期顶格连跳。
    const auto next = clock.ms_until_due(at(t0, 1000));
    AURORA_TEST_REQUIRE_TRUE(next.has_value());
    AURORA_TEST_CHECK_NEAR(*next, 10.0, 1.0);
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 1009)), -1);
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 1010)), 65);
}

AURORA_TEST_CASE(frame_clock_single_frame_and_empty_do_not_run) {
    FrameClock static_cursor;
    static_cursor.load({200U});
    const TimePoint t0 = Clock::now();
    static_cursor.start(t0);
    AURORA_TEST_CHECK_TRUE(static_cursor.running());  // 单帧也允许「运行」态，但不参与换帧
    AURORA_TEST_CHECK_EQ(static_cursor.advance_due(at(t0, 10'000)), -1);
    AURORA_TEST_CHECK_FALSE(static_cursor.ms_until_due(at(t0, 10'000)).has_value());

    FrameClock empty;
    empty.load({});
    empty.start(t0);
    AURORA_TEST_CHECK_FALSE(empty.running());
    AURORA_TEST_CHECK_EQ(empty.advance_due(at(t0, 100)), -1);
}

AURORA_TEST_CASE(frame_clock_clamps_zero_delay) {
    FrameClock clock;
    clock.load({0U, 0U});  // 非法 0 延迟按 1ms 钳正（0 延迟 = deadline 不前进 = 同一时刻无限换帧）
    const TimePoint t0 = Clock::now();
    clock.start(t0);
    AURORA_TEST_CHECK_EQ(clock.advance_due(at(t0, 5)), 1);
    // 1ms 一换：5ms 时钟已转过 5 次（1/2/3/4/5ms 各一次），帧序停在 1。
    AURORA_TEST_CHECK_EQ(clock.frame(), 1);
}

AURORA_TEST_CASE(frame_clock_ms_until_due_shapes_wait) {
    FrameClock clock;
    clock.load({33U, 16U, 50U});
    const TimePoint t0 = Clock::now();
    clock.start(t0);
    const auto before = clock.ms_until_due(at(t0, 10));
    AURORA_TEST_REQUIRE_TRUE(before.has_value());
    AURORA_TEST_CHECK_NEAR(*before, 23.0, 1.0);
    const auto due = clock.ms_until_due(at(t0, 33));
    AURORA_TEST_REQUIRE_TRUE(due.has_value());
    AURORA_TEST_CHECK_NEAR(*due, 0.0, 0.5);
    // 未 start 时不参与等待收敛。
    FrameClock idle;
    idle.load({33U, 16U, 50U});
    AURORA_TEST_CHECK_FALSE(idle.ms_until_due(t0).has_value());
    // stop 之后同理。
    idle.start(t0);
    idle.stop();
    AURORA_TEST_CHECK_FALSE(idle.ms_until_due(t0).has_value());
}

}  // namespace aurora::test_cases::utest_frame_clock

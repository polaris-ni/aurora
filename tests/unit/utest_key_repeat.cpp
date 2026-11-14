/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/key_repeat.h
/// 测试说明: 键盘自动重复的**纯时序状态机**——delay/rate 折算与到期判定、
/// 间隔推进（40ms 档）、追帧单轮上限（burst 16）、非法 rate 如实不重复、
/// release 按键码停表（他键 Up 不误伤）与再按重启、cancel 无条件停表、
/// `ms_until_due` 的等待收敛读数、0 间隔钳制与亚毫秒间隔防护。
///
/// 变异自证（改坏实现应转红的条目）：
///  * 把间隔折算改成沿用 delay（`interval()` 返回 `delay_ms_`）→ `..._obeys_rate_and_delay`
///    与 `..._burst_is_capped` 转红（到期点与追帧条数全错位）；
///  * 摘掉追帧上限（BURST_MAX 改 `INT_MAX`）→ `..._burst_is_capped` 转红（停摆 100 间隔的
///    一帧被合成 100 条而非 16 条）；
///  * 把 `rate_hz_ <= 0` 的拒绝改成「按住也算重复」→ `..._without_rate_does_not_repeat` 与
///    `..._clamps_degenerate_intervals` 转红（合成器未报就猜默认值，违背「如实不为」）；
///  * 把 `release` 改成无条件停表（不比对键码）→ `..._release_stops...` 的负守卫条目转红。

#include <chrono>
#include <cstdint>

#include "aurora/window/detail/key_repeat.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_key_repeat {

using aurora::detail::KeyRepeatState;
using Clock = KeyRepeatState::Clock;
using TimePoint = Clock::time_point;

/// @brief 以基准时刻 + 毫秒偏移构造时间点（用例内统一口径，避免各处手拼 duration）。
[[nodiscard]] auto at(TimePoint base, long long ms) -> TimePoint { return base + std::chrono::milliseconds{ms}; }

AURORA_TEST_CASE(key_repeat_obeys_rate_and_delay) {
    KeyRepeatState st;
    st.configure(25, 400);  // 25Hz → 40ms 间隔；按住 400ms 后首条
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    AURORA_TEST_CHECK_TRUE(st.active());
    AURORA_TEST_CHECK_EQ(st.keycode(), 30U);

    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 399)), 0);  // 首条未到期
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 400)), 1);  // 恰好到期：一条
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 439)), 0);  // 下一条在 440
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 440)), 1);  // 下一条截止 480（上一条调用已消费 440）
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 480)), 1);
}

AURORA_TEST_CASE(key_repeat_burst_is_capped) {
    KeyRepeatState st;
    st.configure(25, 400);
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    // 帧循环停摆 100 个间隔：单轮至多补 16 条，其余由后续帧继续（不灌爆事件队列）。
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 400 + (40 * 100))), 16);
    // 上限触发后从当前时刻按整间隔重新计时，不是无限期顶格补。
    const auto next = st.ms_until_due(at(t0, 400 + (40 * 100)));
    AURORA_TEST_REQUIRE_TRUE(next.has_value());
    AURORA_TEST_CHECK_NEAR(*next, 40.0, 1.0);
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 400 + (40 * 100) + 39)), 0);
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 400 + (40 * 100) + 40)), 1);
}

AURORA_TEST_CASE(key_repeat_without_rate_does_not_repeat) {
    KeyRepeatState st;
    st.configure(0, 400);  // 合成器未广播 / 报了非法值：如实不合成，不猜默认值
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    AURORA_TEST_CHECK_FALSE(st.active());
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 10'000)), 0);
    AURORA_TEST_CHECK_FALSE(st.ms_until_due(t0).has_value());
}

AURORA_TEST_CASE(key_repeat_release_stops_and_other_keycodes_are_ignored) {
    KeyRepeatState st;
    st.configure(25, 50);
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    // 他键的 Up 不误伤当前重复（多键乱序抬起是常态）。
    st.release(31U);
    AURORA_TEST_CHECK_TRUE(st.active());
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 50)), 1);
    // 本键抬起即停表；再按同一键重新起算。
    st.release(30U);
    AURORA_TEST_CHECK_FALSE(st.active());
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 500)), 0);
    st.press(at(t0, 500), 30U);
    AURORA_TEST_CHECK_TRUE(st.active());
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 549)), 0);
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 550)), 1);
    // 换键按住：后到的 Down 直接覆盖（同一时刻只有一个逻辑按住键参与重复）。
    st.press(at(t0, 600), 31U);
    AURORA_TEST_CHECK_EQ(st.keycode(), 31U);
}

AURORA_TEST_CASE(key_repeat_cancel_stops_everything) {
    KeyRepeatState st;
    st.configure(25, 50);
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    st.cancel();  // 键盘焦点离开：按住键可能永远等不到 Up
    AURORA_TEST_CHECK_FALSE(st.active());
    AURORA_TEST_CHECK_EQ(st.emit_due(at(t0, 10'000)), 0);
    AURORA_TEST_CHECK_FALSE(st.ms_until_due(t0).has_value());
}

AURORA_TEST_CASE(key_repeat_ms_until_due_shapes_wait) {
    KeyRepeatState st;
    st.configure(25, 400);
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    // 首条前读数 = 剩余 delay；到期时刻读数归零（poll 超时取 0，立即回帧循环合成）。
    const auto before = st.ms_until_due(at(t0, 100));
    AURORA_TEST_REQUIRE_TRUE(before.has_value());
    AURORA_TEST_CHECK_NEAR(*before, 300.0, 1.0);
    const auto due = st.ms_until_due(at(t0, 400));
    AURORA_TEST_REQUIRE_TRUE(due.has_value());
    AURORA_TEST_CHECK_NEAR(*due, 0.0, 0.5);
    // 过期后读数也是 0（不给出负数超时）。
    const auto overdue = st.ms_until_due(at(t0, 500));
    AURORA_TEST_REQUIRE_TRUE(overdue.has_value());
    AURORA_TEST_CHECK_NEAR(*overdue, 0.0, 0.5);
    // 未 press 时不参与等待收敛。
    KeyRepeatState idle;
    idle.configure(25, 400);
    AURORA_TEST_CHECK_FALSE(idle.ms_until_due(t0).has_value());
}

AURORA_TEST_CASE(key_repeat_clamps_degenerate_intervals) {
    KeyRepeatState st;
    st.configure(0, 0);
    // delay <= 0 钳到 1ms、rate <= 0 不重复：两者都不产生 0 间隔风暴。
    const TimePoint t0 = Clock::now();
    st.press(t0, 30U);
    AURORA_TEST_CHECK_FALSE(st.active());

    // 高速率（>1000Hz）的亚毫秒间隔钳到 1ms。
    KeyRepeatState fast;
    fast.configure(2000, 0);
    fast.press(t0, 30U);
    AURORA_TEST_CHECK_TRUE(fast.active());
    AURORA_TEST_CHECK_EQ(fast.emit_due(at(t0, 5)), 5);  // 1ms 一条，5ms 补 5 条
}

}  // namespace aurora::test_cases::utest_key_repeat

#pragma once

// 键盘自动重复的时序策略（**纯状态机**，零平台依赖）。
//
// 为何单列且零平台依赖：Wayland 没有服务端按键重复（`wl_keyboard.repeat_info` 只广播
// rate / delay，合成由客户端负责），重复合成挂在帧循环上（`poll_platform_events` 每帧驱动、
// `wait_events` 按最近截止收敛 poll 超时），而帧循环的真实节奏不可控（掉帧、断点、后台节流）
// ——「到期几条」的判定一旦写错就是事件风暴或长按丢字，这类时序错位只有纯函数测试才兜得住。
// 平台桥只做「原始值 → 填状态机 → 按返回值合成事件」，语义判定全在本头，可无头单测。
//
// 与 X11 的口径差异：X11 经 `XkbSetDetectableAutoRepeat` 由服务端合成重复（真实键序 Down/Up
// 交替）；Wayland 客户端合成的是**纯 Down 序列**（无 Up）——与 Win32 的 `WM_KEYDOWN` 自动重复
// 位行为等价，文本编辑器按「连续 Down」消费，两条路径语义一致。
//
// 门控：无。本头在所有平台编译（含 wasm），以便单测在任何 CI 作业上都能跑。

#include <chrono>
#include <cstdint>
#include <optional>

namespace aurora::detail {

/// @brief 单轮追帧的重复合成上限：帧循环停摆后每帧至多补 16 条，其余由后续帧继续。
inline constexpr int AURORA_KEY_REPEAT_BURST_MAX = 16;

/// @brief 按键自动重复的时序状态机（Wayland 后端驱动；`utest_key_repeat` 无头全证）。
///
/// 生命周期：`repeat_info` 到达时 `configure`（一条键盘一个配置）；`Down` 事件 `press`、
/// `Up` / 键盘焦点离开 `release`；帧循环每帧 `emit_due(now)` 取「本轮应合成的重复条数」，
/// 空闲等待用 `ms_until_due(now)` 收敛 poll 超时。内部只记一个按住键（键盘同一时刻只有一个
/// 逻辑按住键参与自动重复，后到的 Down 直接覆盖——与 Win32 的重复键位语义一致）。
///
/// 防御口径：`rate_hz <= 0`（合成器未报或报了非法值）⇒ 不合成重复（如实不为，不猜默认值）；
/// `delay_ms <= 0` ⇒ 视为立即开始（钳到 1ms，避免 0 间隔风暴）；帧循环停摆后追帧时单轮合成
/// 有硬上限（`AURORA_KEY_REPEAT_BURST_MAX`）——补齐靠后续帧继续推进，不一口气灌进事件队列。
class KeyRepeatState {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// @brief 登记合成器广播的重复参数（`wl_keyboard.repeat_info`）。
    /// @param rate_hz 每秒重复次数（Hz）；<= 0 视为「不合成重复」。
    /// @param delay_ms 首次重复前的按住时长（毫秒）；<= 0 钳到 1。
    auto configure(int rate_hz, int delay_ms) -> void {
        rate_hz_ = rate_hz;
        delay_ms_ = delay_ms > 0 ? delay_ms : 1;
        // 按住中的键不重置计时：interval() 每次现算，下一次到期起按新间隔推进。
    }

    /// @brief 键按下（Wayland `PRESSED`）：进入重复待发状态，首个重复在 `delay_ms` 后到期。
    /// @param now 当前时刻。
    /// @param keycode evdev 键码（原样透传，状态机不解释键值）。
    auto press(TimePoint now, std::uint32_t keycode) -> void {
        active_ = rate_hz_ > 0;
        keycode_ = keycode;
        if (active_) {
            const auto delay = std::chrono::milliseconds{delay_ms_};
            deadline_ = now + delay;
        }
    }

    /// @brief 键抬起 / 键盘焦点离开（`RELEASED` / `kb_leave`）：停止重复。
    /// @param keycode 抬起的键码；与按住键不同时不动当前状态（允许多键乱序抬起）。
    auto release(std::uint32_t keycode) -> void {
        if (active_ && keycode == keycode_) {
            active_ = false;
        }
    }

    /// @brief 无条件停止重复（键盘焦点离开本表面时调用——按住键可能永远等不到 `Up`）。
    auto cancel() -> void { active_ = false; }

    /// @brief 是否处于「按住待重复」状态（且合成器给出了合法 rate）。
    [[nodiscard]] auto active() const -> bool { return active_; }

    /// @brief 当前参与重复的键码（不活跃时值无意义）。
    [[nodiscard]] auto keycode() const -> std::uint32_t { return keycode_; }

    /// @brief 取出本轮到期的重复条数并推进内部时钟（每帧调用一次）。
    ///
    /// 单轮上限 `AURORA_KEY_REPEAT_BURST_MAX`：帧循环停摆（断点、后台节流）后追帧时不会把
    /// 积压的几十条一次性灌进事件队列——文本框只需「按住时持续出字」，补齐由后续帧继续。
    /// @param now 当前时刻。
    /// @return 本轮应合成的重复条数（0 = 无到期）。
    auto emit_due(TimePoint now) -> int {
        if (!active_) {
            return 0;
        }
        const auto interval_ms = interval();
        if (interval_ms.count() <= 0) {
            return 0;
        }
        int emitted = 0;
        while (now >= deadline_ && emitted < AURORA_KEY_REPEAT_BURST_MAX) {
            deadline_ += interval_ms;
            ++emitted;
        }
        // 停摆过久时 deadline 可能仍落后于 now：不再追赶，从 now 起重新按整间隔计时
        // （连续追帧只会每帧顶格补条，观感上比「暂停一拍再匀速」更差）。
        if (emitted == AURORA_KEY_REPEAT_BURST_MAX && now >= deadline_) {
            deadline_ = now + interval_ms;
        }
        return emitted;
    }

    /// @brief 距下一次重复到期的毫秒数（`wait_events` 收敛 poll 超时用）。
    /// @param now 当前时刻。
    /// @return 毫秒数；不活跃或间隔非法时为 `std::nullopt`（调用方按原超时等待）。
    [[nodiscard]] auto ms_until_due(TimePoint now) const -> std::optional<double> {
        if (!active_) {
            return std::nullopt;
        }
        const auto interval_ms = interval();
        if (interval_ms.count() <= 0) {
            return std::nullopt;
        }
        if (now >= deadline_) {
            return 0.0;  // 已到期：poll 超时取 0，立即回到帧循环合成
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(deadline_ - now);
        return remaining.count();
    }

  private:
    /// @brief 当前重复间隔（毫秒）；`rate_hz` 非法时为 0（不合成）。
    [[nodiscard]] auto interval() const -> std::chrono::milliseconds {
        if (rate_hz_ <= 0) {
            return std::chrono::milliseconds{0};
        }
        // 1000/rate 向下取整到毫秒；亚毫秒间隔（rate > 1000）钳到 1ms，防 0 间隔风暴。
        const auto ms = 1000 / rate_hz_;
        return std::chrono::milliseconds{ms > 0 ? ms : 1};
    }

    int rate_hz_ = 0;  ///< 合成器广播的重复速率（Hz；0 = 未收到或非法）
    int delay_ms_ = 1;  ///< 首次重复前的按住时长（毫秒）
    bool active_ = false;  ///< 是否有按住键在参与重复
    std::uint32_t keycode_ = 0U;  ///< 参与重复的 evdev 键码
    TimePoint deadline_;  ///< 下一次重复的到期时刻（仅在 active_ 为真时有意义，press 时赋值）
};

}  // namespace aurora::detail

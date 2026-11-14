#pragma once

// 动画帧时钟（**纯状态机**，零平台依赖）。
//
// 为何单列且零平台依赖：动画光标（wl_cursor 的 image_count > 1，如 wait / watch）由客户端
// 按 `wl_cursor_image::delay` 逐帧重提交，而重提交挂在帧循环上（`poll_platform_events` 每帧
// 驱动、`wait_events` 按最近截止收敛 poll 超时）——帧循环的真实节奏不可控（掉帧、断点、后台
// 节流），「该不该换帧 / 换到第几帧 / 停摆后怎么补」的判定写错就是频闪或画面凝滞，这类时序
// 错位只有纯函数测试才兜得住。平台桥只做「读主题帧延迟表 → 填时钟 → 按返回的帧号重提交」。
//
// 门控：无。本头在所有平台编译（含 wasm），以便单测在任何 CI 作业上都能跑。

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace aurora::detail {

/// @brief 停摆后的单轮换帧上限：帧循环停摆后每帧至多连跳 64 帧，其余由后续帧继续。
inline constexpr int AURORA_FRAME_CLOCK_BURST_MAX = 64;

/// @brief 帧序推进时钟（动画光标驱动；`utest_frame_clock` 无头全证）。
///
/// 用法：平台桥在形状 / 主题变化时 `load(delays)` + `start(now)`；帧循环每帧
/// `advance_due(now)` 取「本轮应切到的帧号」（未到期返回 -1，调用方不提交）；
/// 空闲等待用 `ms_until_due(now)` 收敛 poll 超时。防御口径：单帧延迟 <= 0 钳到 1ms
/// （防 0 延迟频闪风暴）；停摆追帧单轮有硬上限（`AURORA_FRAME_CLOCK_BURST_MAX`），
/// 补齐靠后续帧继续，不一口气连跳到尾；帧表为空或单帧（静态光标）不启动。
class FrameClock {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// @brief 登记帧延迟表（毫秒 / 帧，与帧序一一对应；<= 0 钳到 1）。
    /// 登记即停表：旧形状的播放位置不作数，须随后 `start` 重新起播。
    auto load(const std::vector<std::uint32_t> &delays_ms) -> void {
        delays_.clear();
        delays_.reserve(delays_ms.size());
        for (const std::uint32_t d : delays_ms) {
            delays_.push_back(d > 0U ? d : 1U);
        }
        running_ = false;
        frame_ = 0;
    }

    /// @brief 从第 0 帧起播（形状变化 / enter 重放整段动画时调用）。
    auto start(TimePoint now) -> void {
        frame_ = 0;
        if (!delays_.empty()) {
            deadline_ = now + std::chrono::milliseconds{delays_[0]};
            running_ = true;
        }
    }

    /// @brief 停止驱动（主题失效 / 提交失败时调用）。
    auto stop() -> void { running_ = false; }

    /// @brief 是否在驱动（帧表非空且已 start、未 stop）。
    [[nodiscard]] auto running() const -> bool { return running_; }

    /// @brief 当前帧序号（0 起；未加载时 0）。
    [[nodiscard]] auto frame() const -> int { return frame_; }

    /// @brief 帧数（0 = 未登记；1 = 静态光标）。
    [[nodiscard]] auto frames() const -> int { return static_cast<int>(delays_.size()); }

    /// @brief 取出本轮应切到的帧号并推进内部时钟（每帧调用一次）。
    /// @param now 当前时刻。
    /// @return 切换后的帧号；未到期（或未启动 / 单帧）返回 -1，调用方不提交。
    auto advance_due(TimePoint now) -> int {
        if (!running_ || delays_.size() < 2) {
            return -1;
        }
        int jumps = 0;
        bool advanced = false;
        while (now >= deadline_ && jumps < AURORA_FRAME_CLOCK_BURST_MAX) {
            frame_ = (frame_ + 1) % static_cast<int>(delays_.size());
            deadline_ += std::chrono::milliseconds{delays_[static_cast<std::size_t>(frame_)]};
            ++jumps;
            advanced = true;
        }
        // 停摆过久：不再追赶，按当前帧延迟重新起算（连续顶格补帧的观感比「暂停一拍再匀速」差）。
        if (jumps == AURORA_FRAME_CLOCK_BURST_MAX && now >= deadline_) {
            deadline_ = now + std::chrono::milliseconds{delays_[static_cast<std::size_t>(frame_)]};
        }
        return advanced ? frame_ : -1;
    }

    /// @brief 距下一次换帧的毫秒数（`wait_events` 收敛 poll 超时用）。
    /// @param now 当前时刻。
    /// @return 毫秒数；不运行 / 单帧时为 `std::nullopt`（调用方按原超时等待）。
    [[nodiscard]] auto ms_until_due(TimePoint now) const -> std::optional<double> {
        if (!running_ || delays_.size() < 2) {
            return std::nullopt;
        }
        if (now >= deadline_) {
            return 0.0;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(deadline_ - now);
        return remaining.count();
    }

  private:
    std::vector<std::uint32_t> delays_;  ///< 各帧展示时长（毫秒，已钳正）
    int frame_ = 0;  ///< 当前帧序号
    bool running_ = false;  ///< 是否在驱动
    TimePoint deadline_;  ///< 当前帧的换帧时刻（running 时有效，start 时赋值）
};

}  // namespace aurora::detail

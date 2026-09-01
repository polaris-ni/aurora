#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace aurora {

namespace detail {
/// @brief 定时器内部条目：由 Scheduler 持有，TimerHandle 经 shared_ptr 引用以安全取消。
struct TimerEntry {
    std::chrono::steady_clock::duration deadline{}; ///< 相对调度器内部时钟的截止时刻。
    std::chrono::steady_clock::duration period{};   ///< 周期（一次性任务为 0）。
    std::function<void()> callback{};               ///< 到期回调（主线程触发）。
    bool recurring = false;                         ///< true=周期任务，false=一次性。
    bool cancelled = false;                         ///< cancel() 置位；触发前校验避免悬空。
};
} // namespace detail

/**
 * @brief 可取消的定时器句柄。
 *
 * 由 `Scheduler::set_timeout` / `set_interval` 返回；轻量、可拷贝、可值语义传递。
 * 持内部 `TimerEntry` 的 `shared_ptr`，`cancel()` 仅置 `cancelled` 标志，
 * 不访问 Scheduler 实例，故句柄可安全地跨作用域持有（含 Scheduler 已析构后取消）。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class TimerHandle {
  public:
    TimerHandle() = default;

    /// @brief 取消已注册的定时任务（幂等；已触发的一次性任务取消无效）。
    auto cancel() const -> void {
        if (m_entry != nullptr) {
            m_entry->cancelled = true;
        }
    }

    /// @brief 任务是否仍活跃（未取消且 Scheduler 尚持有该条目）。
    [[nodiscard]] auto active() const -> bool { return m_entry != nullptr && !m_entry->cancelled; }

  private:
    friend class Scheduler;
    explicit TimerHandle(std::shared_ptr<detail::TimerEntry> e) : m_entry(std::move(e)) {}

    std::shared_ptr<detail::TimerEntry> m_entry;
};

/**
 * @brief 应用级定时任务调度器（命令式）。
 *
 * 由 `Application::run()` 的帧循环每帧按 `std::chrono::steady_clock` 累加的 `dt` 驱动
 * （`tick(double dt_seconds)`），所有回调在主线程触发。组件级 `Timer` 控件在 `on_mount`
 * 时经线程局部 `Scheduler::current()` 取得运行中的实例并注册周期任务。
 *
 * 典型规模下定时任务数量极少，内部用 `std::vector` + 线性扫描，`tick` 每帧 O(n)，无堆抖动。
 * 先在扫描阶段收集到期项、再统一触发，避免回调内重注册导致迭代器失效。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class Scheduler {
  public:
    using Duration = std::chrono::steady_clock::duration;
    using Clock = std::chrono::steady_clock;

    /// @brief 注册一次性延时任务：经 `d` 后触发 `cb` 一次并自动移除。
    auto set_timeout(Duration d, std::function<void()> cb) -> TimerHandle {
        auto e = std::make_shared<detail::TimerEntry>();
        e->deadline = m_elapsed + d;
        e->period = Duration::zero();
        e->callback = std::move(cb);
        e->recurring = false;
        e->cancelled = false;
        m_entries.push_back(e);
        return TimerHandle(e);
    }

    /// @brief 注册周期任务：每经 `period` 触发一次 `cb`，直至 `TimerHandle::cancel()`。
    auto set_interval(Duration period, std::function<void()> cb) -> TimerHandle {
        auto e = std::make_shared<detail::TimerEntry>();
        e->deadline = m_elapsed + period;
        e->period = period;
        e->callback = std::move(cb);
        e->recurring = true;
        e->cancelled = false;
        m_entries.push_back(e);
        return TimerHandle(e);
    }

    /// @brief 每帧推进并触发到期任务（由帧循环调用，`dt_seconds` 为上一帧间隔秒）。
    auto tick(double dt_seconds) -> void {
        m_elapsed += std::chrono::duration_cast<Duration>(std::chrono::duration<double>(dt_seconds));

        std::vector<std::shared_ptr<detail::TimerEntry>> due;
        for (auto &e : m_entries) {
            if (!e->cancelled && e->deadline <= m_elapsed) {
                due.push_back(e);
            }
        }
        for (auto &e : due) {
            if (e->cancelled) {
                continue;
            }
            if (e->recurring) {
                e->deadline += e->period; // 相对重排（帧间隔通常 << period，罕见追帧仅触发一次）
            } else {
                e->cancelled = true; // 一次性：标记待剪除
            }
            if (e->callback) {
                e->callback();
            }
        }
        // 剪除已取消的一次性条目；周期条目保留至 cancel() 才移除。
        std::erase_if(m_entries,
                      [](const std::shared_ptr<detail::TimerEntry> &e) { return e->cancelled && !e->recurring; });
    }

    /// @brief 取消全部任务（含周期任务）并清空。
    auto clear() -> void {
        for (auto &e : m_entries) {
            e->cancelled = true;
        }
        m_entries.clear();
    }

    /// @brief 最近一个待触发任务的剩余毫秒（已到期钳为 0；无任务返回 -1），
    /// 供帧调度决策取值（CPU 性能专项阶段 A3）：idle 帧睡到最近到期时刻即可。
    [[nodiscard]] auto next_deadline_ms() const -> double {
        double best = -1.0;
        for (const auto &e : m_entries) {
            if (e->cancelled) {
                continue;
            }
            const double ms = std::chrono::duration<double, std::milli>(e->deadline - m_elapsed).count();
            const double clamped = ms < 0.0 ? 0.0 : ms;
            if (best < 0.0 || clamped < best) {
                best = clamped;
            }
        }
        return best;
    }

    /// @brief 当前运行中的应用级调度器（由 `Application::run()` 起止设置）。
    ///        组件级 `Timer` 在 `on_mount` 时取用；无运行中 App 时返回 nullptr。
    [[nodiscard]] static auto current() -> Scheduler * { return g_current; }

    /// @brief 设置/清除当前运行实例（线程局部；`Application::run()` 内部调用）。
    static auto set_current(Scheduler *s) -> void { g_current = s; }

  private:
    std::vector<std::shared_ptr<detail::TimerEntry>> m_entries;
    Duration m_elapsed{ Duration::zero() };
    static inline thread_local Scheduler *g_current = nullptr;
};

} // namespace aurora

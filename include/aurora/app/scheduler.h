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
    std::chrono::steady_clock::duration deadline{};  ///< 相对调度器内部时钟的截止时刻。
    std::chrono::steady_clock::duration period{};  ///< 周期（一次性任务为 0）。
    std::function<void()> callback;  ///< 到期回调（主线程触发）。
    bool recurring = false;  ///< true=周期任务，false=一次性。
    bool cancelled = false;  ///< cancel() 置位；触发前校验避免悬空。
};
}  // namespace detail

/// @brief 可取消的定时器句柄。
///
/// 由 `Scheduler::set_timeout` / `set_interval` 返回；轻量、可拷贝、可值语义传递。
/// 持内部 `TimerEntry` 的 `shared_ptr`，`cancel()` 仅置 `cancelled` 标志，
/// 不访问 Scheduler 实例，故句柄可安全地跨作用域持有（含 Scheduler 已析构后取消）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class TimerHandle {
  public:
    /// @brief 默认构造出空句柄：entry_ 为空，active() 恒为 false，cancel() 为 no-op。
    TimerHandle() = default;

    /// @brief 取消已注册的定时任务（幂等；已触发的一次性任务取消无效）。
    auto cancel() const -> void {
        if (entry_ != nullptr) {
            entry_->cancelled = true;
        }
    }

    /// @brief 任务是否仍活跃（未取消且句柄非空）。
    /// @return 句柄持有条目且条目未标记取消时为 `true`。
    [[nodiscard]] auto active() const -> bool { return entry_ != nullptr && !entry_->cancelled; }

  private:
    friend class Scheduler;
    /// @brief 由 Scheduler 在注册任务时构造：接管对应 TimerEntry 的共享所有权。
    /// @param e 已登记的定时条目（cancel() 经它置位取消标志）。
    explicit TimerHandle(std::shared_ptr<detail::TimerEntry> e) : entry_(std::move(e)) {}

    std::shared_ptr<detail::TimerEntry> entry_;
};

/// @brief 应用级定时任务调度器（命令式）。
///
/// 由 `Application::run()` 的帧循环每帧按 `std::chrono::steady_clock` 累加的 `dt` 驱动
/// （`tick(double dt_seconds)`），所有回调在主线程触发。组件级 `Timer` 控件在 `on_mount`
/// 时经线程局部 `Scheduler::current()` 取得运行中的实例并注册周期任务。
///
/// 典型规模下定时任务数量极少，内部用 `std::vector` + 线性扫描，`tick` 每帧 O(n)，无堆抖动。
/// 先在扫描阶段收集到期项、再统一触发，避免回调内重注册导致迭代器失效。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class Scheduler {
  public:
    /// @brief 时长类型别名：以 `steady_clock` 的 tick 计，单调不回拨。
    using Duration = std::chrono::steady_clock::duration;
    /// @brief 时钟类型别名：`Scheduler` 内部与对外暴露的计时基准（单调时钟）。
    using Clock = std::chrono::steady_clock;

    /// @brief 注册一次性延时任务：经 `d` 后触发 `cb` 一次并自动移除。
    /// @param d 自当前内部时钟累加时刻起的延时时长。
    /// @param cb 到期回调（主线程、`tick` 内触发；可为空函数）。
    /// @return 可取消句柄；任务触发后自动剪除，句柄随即失效。
    auto set_timeout(Duration d, std::function<void()> cb) -> TimerHandle {
        auto e = std::make_shared<detail::TimerEntry>();
        e->deadline = elapsed_ + d;
        e->period = Duration::zero();
        e->callback = std::move(cb);
        e->recurring = false;
        e->cancelled = false;
        entries_.push_back(e);
        return TimerHandle(e);
    }

    /// @brief 注册周期任务：每经 `period` 触发一次 `cb`，直至 `TimerHandle::cancel()`。
    /// @param period 触发周期（截止时刻按此相对重排）。
    /// @param cb 到期回调（主线程、`tick` 内触发；可为空函数）。
    /// @return 可取消句柄；`cancel()` 后条目在下一次 `tick` 的剪除阶段移除。
    auto set_interval(Duration period, std::function<void()> cb) -> TimerHandle {
        auto e = std::make_shared<detail::TimerEntry>();
        e->deadline = elapsed_ + period;
        e->period = period;
        e->callback = std::move(cb);
        e->recurring = true;
        e->cancelled = false;
        entries_.push_back(e);
        return TimerHandle(e);
    }

    /// @brief 每帧推进并触发到期任务（由帧循环调用，`dt_seconds` 为上一帧间隔秒）。
    /// @param dt_seconds 上一帧经过的秒数（累加进内部时钟；先收集到期项再统一触发，允许回调内重注册）。
    auto tick(double dt_seconds) -> void {
        elapsed_ += std::chrono::duration_cast<Duration>(std::chrono::duration<double>(dt_seconds));

        std::vector<std::shared_ptr<detail::TimerEntry>> due;
        for (auto &e : entries_) {
            if (!e->cancelled && e->deadline <= elapsed_) {
                due.push_back(e);
            }
        }
        for (auto &e : due) {
            if (e->cancelled) {
                continue;
            }
            if (e->recurring) {
                e->deadline += e->period;  // 相对重排（帧间隔通常 << period，罕见追帧仅触发一次）
            } else {
                e->cancelled = true;  // 一次性：标记待剪除
            }
            if (e->callback) {
                e->callback();
            }
        }
        // 剪除已取消的一次性条目；周期条目保留至 cancel() 才移除。
        std::erase_if(entries_, [](const std::shared_ptr<detail::TimerEntry> &e) -> bool {
            return e->cancelled && !e->recurring;
        });
    }

    /// @brief 取消全部任务（含周期任务）并清空。
    auto clear() -> void {
        for (auto &e : entries_) {
            e->cancelled = true;
        }
        entries_.clear();
    }

    /// @brief 最近一个待触发任务的剩余毫秒（已到期钳为 0；无任务返回 -1），
    /// 供帧调度决策取值：idle 帧睡到最近到期时刻即可。
    /// @return 最小剩余毫秒（>= 0）；没有任何未取消任务时为 -1.0。
    [[nodiscard]] auto next_deadline_ms() const -> double {
        double best = -1.0;
        for (const auto &e : entries_) {
            if (e->cancelled) {
                continue;
            }
            const double ms = std::chrono::duration<double, std::milli>(e->deadline - elapsed_).count();
            const double clamped = ms < 0.0 ? 0.0 : ms;
            if (best < 0.0 || clamped < best) {
                best = clamped;
            }
        }
        return best;
    }

    /// @brief 当前运行中的应用级调度器（由 `Application::run()` 起止设置）。
    ///        组件级 `Timer` 在 `on_mount` 时取用；无运行中 App 时返回 nullptr。
    /// @return 运行中实例的裸指针（非拥有）；本线程无 App 运行时为 nullptr。
    [[nodiscard]] static auto current() -> Scheduler * { return current_; }

    /// @brief 设置/清除当前运行实例（线程局部；`Application::run()` 内部调用）。
    /// @param s 登记为当前实例的调度器（非拥有）；传 nullptr 即清除登记。
    static auto set_current(Scheduler *s) -> void { current_ = s; }

  private:
    std::vector<std::shared_ptr<detail::TimerEntry>> entries_;
    Duration elapsed_{Duration::zero()};  ///< 内部单调时钟：各次 tick 的 dt 累加值，任务截止时刻以此为基准
    static inline thread_local Scheduler *current_ = nullptr;  ///< 运行中的实例（线程局部；无 App 运行时为 nullptr）
};

}  // namespace aurora

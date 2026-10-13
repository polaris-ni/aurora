#pragma once

// ============================================================
// 条件等待辅助（tests/framework/async_wait.h）
// ------------------------------------------------------------
// 网络用例的结论落在「另一个线程什么时候把这件事办了」上，而这类等待此前每个文件各自手写
// （thread + condition_variable + 预算常量），于是同一台机器上同一个用例会有三种超时口径与三种
// 失败形态：有的静默返回布尔（超时与失败分不开）、有的用 REQUIRE（超时之后同用例的其余断言
// 全部不执行）、有的直接忙等自旋（把 CI 的一核吃满）。本头把这三件事收成一处，判据口径：
//   - **无忙等**：`Watch<T>::wait_for` 由条件变量唤醒，`polls` 恒为 0；`wait_until` 是有限轮询，
//     每轮之间睡一个量化步长（缺省 1 ms），轮数受预算约束并可自报。
//   - **超时自报**：`WaitOutcome::report()` 给出主题、实测耗时、预算与轮数；红的时候第一行日志
//     就能分辨「等早了」还是「根本没发生」。
//   - **不吞断言**：`AURORA_TEST_WAIT_UNTIL` 走非致命通道（`CHECK` 级别），超时只记这一条失败，
//     用例继续往下跑——等待失败往往意味着后面的现场观测更有信息量。
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。
// ============================================================

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "assertions.h"

namespace aurora::test {

/// @brief 缺省等待预算：覆盖回环往返与跨线程投递，又不至于把挂死伪装成慢。
inline constexpr std::chrono::milliseconds AURORA_WAIT_BUDGET{3000};

/// @brief 有限轮询的量化步长：这是「不忙等」与「响应快」之间的取值，不是自旋。
inline constexpr std::chrono::milliseconds AURORA_WAIT_QUANTUM{1};

/// @brief 一次条件等待的结果；失败时靠 `report()` 自报现场。
struct WaitOutcome {
    /// @brief 谓词是否在预算内成立。
    bool satisfied = false;
    /// @brief 实测耗时。
    std::chrono::milliseconds elapsed{0};
    /// @brief 本次预算。
    std::chrono::milliseconds budget{0};
    /// @brief 谓词被求值的次数；通知驱动形态恒为 0。
    int polls = 0;
    /// @brief 等待主题（进 `report()`，供红时定位是哪一步没发生）。
    std::string_view subject;

    /// @brief 组装自报文本（一律 ASCII，失败信息要能进任何终端代码页）。
    /// @return 通过时为 `wait[<主题>] ok: elapsed=…ms polls=…`；超时附带预算以示「等过了头」。
    [[nodiscard]] auto report() const -> std::string {
        std::string text{"wait["};
        text += subject;
        text += satisfied ? "] ok: elapsed=" : "] UNSATISFIED within budget: elapsed=";
        text += std::to_string(elapsed.count());
        text += "ms budget=";
        text += std::to_string(budget.count());
        text += "ms polls=";
        text += std::to_string(polls);
        return text;
    }
};

/// @brief 有限轮询直到谓词成立或预算耗尽。
///
/// 先判一次再睡，故「谓词早已成立」的形态零睡眠；每轮之间睡 `AURORA_WAIT_QUANTUM`，不是忙等。
/// 谓词读的是**非本头保护的状态**（裸 `std::atomic`、局部布尔等）时才用它；读跨线程普通变量请改用
/// `Watch<T>`，那条路既不轮询也不会数据竞争。
/// @tparam Pred 可调用体，返回可转 `bool` 的值；须在调用线程可求值。
/// @param subject 等待主题，进超时文本。
/// @param pred 目标条件。
/// @param budget 预算；缺省 `AURORA_WAIT_BUDGET`。
/// @return 结果与现场（`satisfied` / `elapsed` / `polls`）。
template <typename Pred>
[[nodiscard]] auto wait_until(std::string_view subject, const Pred &pred,
                              std::chrono::milliseconds budget = AURORA_WAIT_BUDGET) -> WaitOutcome {
    const auto started = std::chrono::steady_clock::now();
    WaitOutcome outcome{.budget = budget, .subject = subject};
    while (true) {
        ++outcome.polls;
        if (static_cast<bool>(pred())) {
            outcome.satisfied = true;
            break;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        if (elapsed >= budget) {
            outcome.elapsed = elapsed;
            return outcome;
        }
        std::this_thread::sleep_for(AURORA_WAIT_QUANTUM);
    }
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return outcome;
}

/// @brief 有锁有通知的观察位：把「别的线程改了某个值」变成可等待的事件而不是轮询对象。
///
/// 写入方 `set()` / `update()`，等待方 `wait_for(pred)`；等待由条件变量唤醒，故 `WaitOutcome::polls`
/// 恒为 0——这也是「无忙等」那条判据的可断言形态。值本身在锁内拷贝出来，等待方读到的永远是完整快照。
/// @tparam T 观察值类型；须可拷贝、可比较赋值。
/// @note Thread: 全部成员线程安全
/// @note Side-effects: none
template <typename T>
class Watch {
  public:
    /// @brief 以初值构造。
    /// @param initial 初始观察值。
    explicit Watch(T initial = T{}) : value_(std::move(initial)) {}

    /// @brief 析构：成员（值 + 锁 + 条件变量）全部自管，无需额外收敛。
    ~Watch() = default;

    Watch(const Watch &) = delete;
    auto operator=(const Watch &) -> Watch & = delete;
    Watch(Watch &&) = delete;
    auto operator=(Watch &&) -> Watch & = delete;

    /// @brief 写入新值并唤醒全部等待者。
    /// @param next 新值。
    auto set(T next) -> void {
        {
            std::scoped_lock lock(mutex_);
            value_ = std::move(next);
            ++writes_;
        }
        cv_.notify_all();
    }

    /// @brief 就地改写观察值并唤醒全部等待者（累加、置位一类的写入方惯用形态）。
    /// @param fn 接收 `T &` 的可调用体；在锁内执行，故不得回调回本对象的等待接口。
    template <typename Fn>
    auto update(const Fn &fn) -> void {
        {
            std::scoped_lock lock(mutex_);
            fn(value_);
            ++writes_;
        }
        cv_.notify_all();
    }

    /// @brief 取当前值快照。
    /// @return 值的副本。
    [[nodiscard]] auto get() const -> T {
        std::scoped_lock lock(mutex_);
        return value_;
    }

    /// @brief 迄今被写入的次数（写入方「只该发生一次」的判据据此数出来）。
    /// @return 写入次数。
    [[nodiscard]] auto writes() const -> int {
        std::scoped_lock lock(mutex_);
        return writes_;
    }

    /// @brief 等待谓词在锁内成立；由通知唤醒，不轮询。
    /// @tparam Pred 可调用体，收到当前值快照、返回可转 `bool` 的值。
    /// @param subject 等待主题，进超时文本。
    /// @param pred 目标条件。
    /// @param budget 预算；缺省 `AURORA_WAIT_BUDGET`。
    /// @return 结果与现场；`polls` 恒为 0（通知驱动）。
    template <typename Pred>
    [[nodiscard]] auto wait_for(std::string_view subject, const Pred &pred,
                                std::chrono::milliseconds budget = AURORA_WAIT_BUDGET) const -> WaitOutcome {
        const auto started = std::chrono::steady_clock::now();
        WaitOutcome outcome{.budget = budget, .subject = subject};
        std::unique_lock<std::mutex> lock(mutex_);
        outcome.satisfied =
            cv_.wait_for(lock, budget, [&pred, this]() -> bool { return static_cast<bool>(pred(value_)); });
        outcome.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        return outcome;
    }

    /// @brief 等待观察值等于给定值（「落定到某个终态」的常见写法）。
    /// @tparam U 可与 `T` 比较的类型。
    /// @param subject 等待主题。
    /// @param expected 目标值。
    /// @param budget 预算。
    /// @return 结果与现场。
    template <typename U>
    [[nodiscard]] auto wait_for_value(std::string_view subject, const U &expected,
                                      std::chrono::milliseconds budget = AURORA_WAIT_BUDGET) const -> WaitOutcome {
        return wait_for(subject, [&expected](const T &current) -> bool { return current == expected; }, budget);
    }

  private:
    mutable std::mutex mutex_;  ///< 保护 value_ 与 writes_
    mutable std::condition_variable cv_;  ///< 写入即通知：等待方不轮询
    T value_;  ///< 观察值
    int writes_ = 0;  ///< 写入次数
};

/// @brief 计数观察位：写入方每次 `bump()`，等待方等「到第 N 次」。
using CountWatch = Watch<std::size_t>;

}  // namespace aurora::test

// ---------------------------------------------------------------------------
// 宏层
// ---------------------------------------------------------------------------

/// @brief 非致命地断言一次条件等待：超时只记这一条失败（文本自带耗时与轮数），用例继续执行。
///
/// 刻意用 `CHECK` 而非 `REQUIRE`——等待超时之后，同用例的后续观测（回调计数、状态快照）往往才是
/// 定位线索，致命断言会把它们一起丢掉。
// 宏形态是必需而非偷懒：宏内要就地声明局部量并转给检查宏，而检查宏自身要取 `__FILE__` / `__LINE__`，
// 函数形态拿不到调用点位置。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) 调用点位置只能由预处理器就地展开取得
#define AURORA_TEST_WAIT_UNTIL(outcome)                                                               \
    do {                                                                                              \
        const ::aurora::test::WaitOutcome aurora_test_wait_outcome = (outcome);                       \
        AURORA_TEST_CHECK_MSG(aurora_test_wait_outcome.satisfied, aurora_test_wait_outcome.report()); \
    } while (false)

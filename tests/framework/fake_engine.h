#pragma once

// ============================================================
// 脚本化引擎替身（tests/framework/fake_engine.h）
// ------------------------------------------------------------
// `Engine` 已经把「什么时候执行、什么时候到期」藏在接口之后，但自带引擎的到期时刻由真实单调时钟
// 决定：要测「连接时限到点后恰一次失败」就只能真睡到点，慢、抖，而且把时限本身与竞态窗口混在一起
// 测——红的时候分不清是逻辑错了还是机器慢了。本替身把**时间**从**就绪**里拆出来：
//   - 任务与定时器都只登记，不自己走；用例调 `advance()` 才推进时钟并触发到期项。
//   - fd 的就绪仍走真实的宿主驱动 pump（`framework/net_wait.h`），所以「连上了没收应答」这类场景
//     依然由内核给出事实，只有时限是用例说了算。
// 于是超时、取消、交错竞速、代理无应答这四类时序判据变成逐值断言，且全程零出站（回环与本机 IPC）。
//
// 安装经 `net::detail::set_engine_override()`（库内接缝，见 `src/aurora/net/detail/engine_override.h`），
// 用 `FakeEngineScope` 的 RAII 成对拆装。替身期间常驻 `PollEngine` 不会被本层路径拉起。
//
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。
// ============================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/capabilities.h"
#include "aurora/net/detail/engine_override.h"
#include "aurora/net/engine.h"
#include "net_wait.h"

namespace aurora::test {

/// @brief 脚本化引擎：手动时钟 + 可拒绝投递 + 可改写能力位的 `net::Engine` 实现。
///
/// 三条与真实引擎对齐的口径（替身不能自创语义，否则测的是替身）：
/// - **停机丢弃未到期定时器**：与 `PollEngine` 一致，定时器只承载「放弃等待」，静默执行一个过期续体
///   会造出假的完成时序。
/// - **停机后再次投递即重新工作**：真实引擎是惰性启动，替身同样不在停机后拒绝新任务。
/// - **到期次序按登记序稳定**：同一到期时刻内先登记先执行，用例可据此断言「谁先落结局」。
///
/// 与真实引擎**不同**的一处：替身不自持线程，`advance()` 在执行调用者的线程上跑任务体。这是刻意取舍
/// ——把「回调落在哪个线程」这件事交给用例显式决定，而不是由后台线程随时插入。宿主驱动形态下就绪投递
/// 本来也在用例线程，两条路径同线程 ⇒ 断言不需要任何锁。
/// @note Thread: 全部成员线程安全（内部一把互斥量；任务体在 `advance()` / `run_pending()` 的调用线程执行）
/// @note Side-effects: `post` / `post_after` 只登记，执行发生在推进时钟的那一次调用里
class FakeEngine final : public aurora::net::Engine {
  public:
    /// @brief 构造：时钟从 0 起算，能力位为「有事件源、可流式」的最小可用形态（其余位由用例按需打开）。
    FakeEngine() {
        caps_.event_source = true;
        caps_.streaming = true;
    }

    /// @brief 析构：替身不持线程与句柄，登记中的任务随对象一并丢弃（不留悬垂执行体）。
    ~FakeEngine() override {
        std::scoped_lock lock(mutex_);
        queue_.clear();
    }

    /// @brief 禁复制与移动：`FakeEngineScope` 装的是**这一枚对象的地址**，搬动它会让接缝指向旧地址。
    FakeEngine(const FakeEngine &) = delete;
    auto operator=(const FakeEngine &) -> FakeEngine & = delete;
    FakeEngine(FakeEngine &&) = delete;
    auto operator=(FakeEngine &&) -> FakeEngine & = delete;

    // ---- net::Engine 接口 ----

    /// @brief 引擎名：固定回报 `"fake"`，供 Inspector / 日志与用例区分替身。
    /// @return `"fake"`。
    [[nodiscard]] auto name() const -> std::string_view override { return "fake"; }

    /// @brief 能力矩阵：回报 `set_capabilities()` 设定的值（未设则为构造时的最小可用形态）。
    /// @return 能力位集合副本。
    [[nodiscard]] auto capabilities() const -> aurora::net::Capabilities override {
        std::scoped_lock lock(mutex_);
        return caps_;
    }

    /// @brief 登记一个「立即到期」的任务（不执行，直到 `run_pending()` 或 `advance()`）。
    /// @param task 待执行体。
    /// @return 受理为 OK；`refuse_posts(true)` 期间为 `net-platform-unavailable`（与真实引擎的无引擎腿同码）。
    [[nodiscard]] auto post(std::function<void()> task) -> aurora::Result<void> override {
        return schedule(std::chrono::milliseconds{0}, std::move(task));
    }

    /// @brief 登记一个「`delay` 之后到期」的任务；到期时刻 = 当前手动时钟 + `delay`。
    /// @param delay 相对到期时长；非正值等价于立即到期（与真实引擎口径一致）。
    /// @param task 到期执行体。
    /// @return 登记成功为 OK；拒绝期同 `post()`。
    [[nodiscard]] auto post_after(std::chrono::milliseconds delay, std::function<void()> task)
        -> aurora::Result<void> override {
        return schedule(delay, std::move(task));
    }

    /// @brief 是否处于工作状态。
    /// @return 受理过尚未停机的任务为 true；`shutdown()` 后为 false，再次受理重新为 true。
    [[nodiscard]] auto running() const -> bool override {
        std::scoped_lock lock(mutex_);
        return running_;
    }

    /// @brief 停机：丢弃未到期的登记项（不执行、不报错），计数进 `dropped_on_shutdown()`。
    ///
    /// 立即到期的任务同样丢弃——本替身里没有「已入队但未跑」与「已到期」的差别，两者都是未执行的登记。
    auto shutdown() -> void override {
        std::scoped_lock lock(mutex_);
        dropped_on_shutdown_ += static_cast<int>(queue_.size());
        queue_.clear();
        running_ = false;
    }

    // ---- 脚本面：用例据此推进时间与注入故障 ----

    /// @brief 当前手动时钟。
    /// @return 自构造起算的毫秒数。
    [[nodiscard]] auto now() const -> std::chrono::milliseconds {
        std::scoped_lock lock(mutex_);
        return now_;
    }

    /// @brief 推进时钟并执行到期项（含执行期间新登记且同样到期的项）。
    ///
    /// 到期项按「到期时刻 → 登记次序」稳定执行；时钟随每一项的到期时刻前进，故续体里再登记的相对
    /// 时限以该项到期时刻为基准，与真实引擎的时间语义一致。
    /// @param delta 推进量；非正值只把已到期项跑干净。
    /// @return 本次实际执行的任务数。
    auto advance(std::chrono::milliseconds delta) -> std::size_t {
        const std::int64_t target = [&]() -> std::int64_t {
            std::scoped_lock lock(mutex_);
            return (now_ + std::max(delta, std::chrono::milliseconds{0})).count();
        }();
        std::size_t executed = 0;
        while (true) {
            std::function<void()> task;
            {
                std::scoped_lock lock(mutex_);
                const auto due = std::ranges::min_element(queue_, [](const Item &a, const Item &b) -> bool {
                    return a.due < b.due || (a.due == b.due && a.seq < b.seq);
                });
                if (due == queue_.end() || due->due > target) {
                    now_ = std::chrono::milliseconds{target};
                    break;
                }
                now_ = std::max(now_, std::chrono::milliseconds{due->due});
                task = std::move(due->task);
                queue_.erase(due);
            }
            ++executed;
            ++tasks_run_;
            task();
        }
        return executed;
    }

    /// @brief 只执行「已到期」的登记项，不推进时钟。
    /// @return 本次执行的任务数。
    auto run_pending() -> std::size_t {
        std::size_t executed = 0;
        while (true) {
            std::function<void()> task;
            {
                std::scoped_lock lock(mutex_);
                const auto due =
                    std::ranges::find_if(queue_, [this](const Item &item) -> bool { return item.due <= now_.count(); });
                if (due == queue_.end()) {
                    break;
                }
                task = std::move(due->task);
                queue_.erase(due);
            }
            ++executed;
            ++tasks_run_;
            task();
        }
        return executed;
    }

    /// @brief 反复推进时钟直到没有登记项或触到上限。
    ///
    /// 用于「不关心具体时限、只关心链路最终收敛」的场景；上限用来把「登记永远排空不了」这种缺陷
    /// 变成一次可见的失败而不是一次挂死。
    /// @param ceiling 最多推进的总时长。
    /// @param step 每次推进的步长。
    /// @return 执行的任务数。
    auto drain(std::chrono::milliseconds ceiling = std::chrono::milliseconds{60000},
               std::chrono::milliseconds step = std::chrono::milliseconds{1}) -> std::size_t {
        std::size_t executed = 0;
        for (std::int64_t spent = 0; spent <= ceiling.count(); spent += std::max(step.count(), std::int64_t{1})) {
            executed += advance(step);
            if (pending() == 0) {
                break;
            }
        }
        return executed;
    }

    /// @brief 尚未执行的登记项数（立即到期与未到期定时器一并计入）。
    /// @return 登记项数。
    [[nodiscard]] auto pending() const -> std::size_t {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

    /// @brief 打开 / 关闭「拒绝投递」：让 `post` / `post_after` 一律回 `net-platform-unavailable`。
    ///
    /// 真实引擎的无引擎腿就是这个答复，替身必须能给同一条码，否则「排队失败要把原因带进回调」这类
    /// 判据只能靠构造一个 wasm 腿来测。
    /// @param refusing 为真时后续投递一律被拒。
    auto refuse_posts(bool refusing) -> void {
        std::scoped_lock lock(mutex_);
        refusing_ = refusing;
    }

    /// @brief 改写能力位（供「能力缺失即降级」的判据脚本化，例如把 `tls` 置真 / 置假）。
    /// @param caps 新的能力集合。
    auto set_capabilities(aurora::net::Capabilities caps) -> void {
        std::scoped_lock lock(mutex_);
        caps_ = caps;
    }

    /// @brief 迄今执行过的任务总数（跨 `advance()` 累计）。
    /// @return 执行数。
    [[nodiscard]] auto tasks_run() const -> int { return tasks_run_.load(); }

    /// @brief 迄今被拒的投递次数。
    /// @return 次数。
    [[nodiscard]] auto posts_refused() const -> int {
        std::scoped_lock lock(mutex_);
        return posts_refused_;
    }

    /// @brief 迄今因停机被丢弃的登记项数。
    /// @return 次数。
    [[nodiscard]] auto dropped_on_shutdown() const -> int {
        std::scoped_lock lock(mutex_);
        return dropped_on_shutdown_;
    }

  private:
    /// @brief 一条登记项：到期时刻 + 登记序号（同刻保序）+ 执行体。
    struct Item {
        std::int64_t due = 0;  ///< 到期时刻（相对构造起算的毫秒）
        std::uint64_t seq = 0;  ///< 登记序号
        std::function<void()> task;  ///< 执行体
    };

    /// @brief 登记一条任务（`post` 与 `post_after` 的共同腿）。
    /// @param delay 相对到期时长；非正值等价于立即到期。
    /// @param task 执行体。
    /// @return 受理为 OK；拒绝期为 `net-platform-unavailable`。
    [[nodiscard]] auto schedule(std::chrono::milliseconds delay, std::function<void()> task) -> aurora::Result<void> {
        std::scoped_lock lock(mutex_);
        if (refusing_) {
            ++posts_refused_;
            return aurora::make_error(aurora::ErrorCode::NetPlatformUnavailable, "post: fake engine is refusing work");
        }
        if (!task) {
            return aurora::make_error(aurora::ErrorCode::NetPlatformUnavailable, "post: empty task");
        }
        const auto due = now_ + std::max(delay, std::chrono::milliseconds{0});
        queue_.push_back(Item{.due = due.count(), .seq = ++sequence_, .task = std::move(task)});
        running_ = true;
        return {};
    }

    mutable std::mutex mutex_;  ///< 保护下方全部字段
    std::vector<Item> queue_;  ///< 登记项集合（执行时按到期时刻与登记序取最小）
    aurora::net::Capabilities caps_{};  ///< 能力位
    std::chrono::milliseconds now_{0};  ///< 手动时钟
    std::uint64_t sequence_ = 0;  ///< 登记序号
    bool running_ = false;  ///< 工作状态
    bool refusing_ = false;  ///< 拒绝投递开关
    int posts_refused_ = 0;  ///< 被拒次数
    int dropped_on_shutdown_ = 0;  ///< 停机丢弃数
    std::atomic<int> tasks_run_{0};  ///< 执行总数（执行在锁外，故用原子）
};

/// @brief 替身安装守卫：作用域内 `net::Engine::instance()` 返回该替身，退出即恢复本腿真实引擎。
///
/// 与 `HostDriven` 配对使用：时钟归替身、就绪归宿主 pump，两者都在用例线程，时序才是逐值可断言的。
/// @note Thread: 安装与拆除限同一线程
/// @note Side-effects: 改写进程级引擎入口的指向
class FakeEngineScope {
  public:
    /// @brief 装入替身。
    /// @param engine 替身对象；生命周期必须长于本守卫。
    explicit FakeEngineScope(FakeEngine &engine) {
        aurora::net::detail::set_engine_override(&engine);
        aurora::test::net::pump_tail_slot() = [&engine]() -> void { engine.run_pending(); };
    }

    /// @brief 拆除：先摘泵尾巴再交回引擎入口，次序与安装相反。
    ~FakeEngineScope() {
        aurora::test::net::pump_tail_slot() = {};
        aurora::net::detail::clear_engine_override();
    }

    /// @brief 守卫不可拷贝：同一进程只有一个引擎入口指向。
    FakeEngineScope(const FakeEngineScope &) = delete;
    /// @brief 守卫不可拷贝赋值。
    auto operator=(const FakeEngineScope &) -> FakeEngineScope & = delete;
    /// @brief 守卫不可移动：移动会让拆装次序脱离与作用域的对应关系。
    FakeEngineScope(FakeEngineScope &&) = delete;
    /// @brief 守卫不可移动赋值。
    auto operator=(FakeEngineScope &&) -> FakeEngineScope & = delete;
};

}  // namespace aurora::test

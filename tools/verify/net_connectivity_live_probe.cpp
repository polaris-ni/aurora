// tools/verify/net_connectivity_live_probe.cpp — 链路态真机验收探针（非 CTest）。
//
// 覆盖：`ConnectivityMonitor` 在真实路由 / 接口表上的三档读数、编译期采集腿令牌、订阅首拍、
// 插队采样受理与快照只读性。无头单测（`utest_net_connectivity` / `utest_net_connectivity_state`）
// 用注入替身把状态机逐值钉死，但证不了「这条腿在本机真的读到了东西」——本探针补的正是这一段。
//
// 为什么必须有本探针（无头 CI 证明不了的部分）：
//   * 采集腿读的是 OS 表 / 伪文件 / 动态存储，替身读数与真实读数之间没有等价性可推；
//   * 「拔线 / 关适配器 → 恢复」三段各有可观察读数，只有真机能给出；
//   * 无采集腿的构建（wasm）应恒报 `Unknown` + `None`，这条降级判据也要真跑一次。
//
// 三段证据（`--expect` 选段，缺省 `auto`）：
//   auto   ：首快照（`Unknown` / 腿令牌 / `stale` / `generation=0`）→ 订阅首拍 → `refresh()`
//            受理 → 快照只读（重复取不快照不推进代际）→ 句柄 `token()` / `is_registered()`。
//   offline：在**拔线 / 删除缺省路由之后**跑，等状态落到 `Offline` 并打印到达时的成因。
//   online ：在**恢复之后**跑，等状态落到 `Online` 并打印到达时的成因。
//
// 退出码：0 = 判据全通过；1 = 有 FAIL；2 = 环境不可用（本构建没有采集腿，`source` 为 `none`）。
//
// 构建与运行（方式 ① 推荐）：
//   cmake -S . -B build-verify -DAURORA_BUILD_VERIFY_TOOLS=ON
//   cmake --build build-verify --target aurora_verify_net_connectivity
//   build-verify\aurora_verify_net_connectivity.exe --expect auto
//
// 三段式的人工动作（Windows）：在「网络连接」里禁用当前适配器 → 跑 `--expect offline` →
// 重新启用 → 跑 `--expect online`。Linux 桌面等价动作是 `ip route del default` 与恢复。
// 探针本身**零出站**：只调 `ConnectivityMonitor` 的查询与订阅入口，不发起任何连接。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/core/log.h"
#include "verify_args.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

// @brief 自动段累计的失败数。
//
// 文件作用域：探针是单文件、单线程、无状态的小工具，计数器本身就是它的全部可变状态；
// 为了绕过静态检查把它包进单例类只是形式主义，反而多一层不必要的间接。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int failures = 0;

/// @brief 记录一条断言结论：打印 PASS / FAIL 行并累计失败数。
auto check(bool passed, const std::string &label) -> void {
    emit(std::string{passed ? "[PASS] " : "[FAIL] "} + label);
    if (!passed) {
        ++failures;
    }
}

/// @brief 记录一条「本轮未得到覆盖」的判据：打印 SKIP 行，不计失败。
auto skip(const std::string &label) -> void { emit("[SKIP] " + label); }

/// @brief 订阅回调的跨线程落点：引擎线程投递、探针主线程按预算等。
///
/// 以 `shared_ptr` 持有、由回调按值捕获：注销时可能仍有一枚在途投递正执行回调，若落点先析构
/// 就是一次 use-after-free。捕获共享所有权让落点活到最后一个回调返回为止。
class Watcher {
  public:
    /// @brief 收下一枚变更；回调在引擎线程执行。
    /// @param change 本次变更。
    auto record(const aurora::net::ConnectivityChange &change) -> void {
        {
            const std::scoped_lock lock(mutex_);
            changes_.push_back(change);
        }
        cv_.notify_all();
    }

    /// @brief 等首枚变更抵达。
    /// @param timeout 等待预算。
    /// @return 抵达为 true；超时为 false。
    [[nodiscard]] auto wait_first(std::chrono::milliseconds timeout) -> bool {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this]() -> bool { return !changes_.empty(); });
    }

    /// @brief 等到第一枚落在目标态的变更。
    /// @param state 目标态。
    /// @param timeout 等待预算。
    /// @return 命中的变更；超时为空值。
    [[nodiscard]] auto wait_state(aurora::net::NetworkState state, std::chrono::milliseconds timeout)
        -> std::optional<aurora::net::ConnectivityChange> {
        std::unique_lock lock(mutex_);
        const bool arrived = cv_.wait_for(lock, timeout, [this, state]() -> bool {
            return std::ranges::any_of(changes_, [state](const aurora::net::ConnectivityChange &change) -> bool {
                return change.current.state == state;
            });
        });
        if (!arrived) {
            return std::nullopt;
        }
        for (const auto &change : changes_) {
            if (change.current.state == state) {
                return change;
            }
        }
        return std::nullopt;
    }

    /// @brief 已收下的变更数。
    /// @return 条数。
    [[nodiscard]] auto count() const -> std::size_t {
        const std::scoped_lock lock(mutex_);
        return changes_.size();
    }

    /// @brief 第一枚变更。
    /// @return 有变更时为其值拷贝；否则空值。
    [[nodiscard]] auto first() const -> std::optional<aurora::net::ConnectivityChange> {
        const std::scoped_lock lock(mutex_);
        if (changes_.empty()) {
            return std::nullopt;
        }
        return changes_.front();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_{};
    std::vector<aurora::net::ConnectivityChange> changes_;
};

/// @brief 一枚事实的打印形态：三档 / 腿令牌 / 代际 / 陈旧位 / 起始时刻 / 展示用失败码。
/// @param fact 待打印事实。
/// @param start 探针起点（用于把 `since` 折算成相对毫秒）。
/// @return 单行文本。
auto describe(const aurora::net::ConnectivityFact &fact, std::chrono::steady_clock::time_point start) -> std::string {
    const auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fact.since - start).count();
    std::string out = "state=";
    out += std::string{aurora::net::network_state_text(fact.state)};
    out += " source=";
    out += std::string{aurora::net::connectivity_source_text(fact.source)};
    out += " generation=" + std::to_string(fact.generation);
    out += std::string{" stale="} + (fact.stale ? "true" : "false");
    out += " since_ms=" + std::to_string(since_ms);
    out += " last_failure=";
    out += fact.last_failure.has_value() ? *fact.last_failure : std::string{"null"};
    return out;
}

/// @brief 一枚变更加上成因的打印形态。
/// @param change 待打印变更。
/// @param start 探针起点。
/// @return 单行文本。
auto describe_change(const aurora::net::ConnectivityChange &change, std::chrono::steady_clock::time_point start)
    -> std::string {
    return std::string{"cause="} + std::string{aurora::net::connectivity_change_cause_text(change.cause)} + " " +
           describe(change.current, start);
}

/// @brief 与 `Engine::shutdown()` 的收敛点：少了它，常驻线程会跨过 `main` 的返回。
/// @note Thread: 构造 / 拆除均在主线程
/// @note Side-effects: 拆除时停机常驻引擎线程
class EngineScope {
  public:
    EngineScope() = default;
    ~EngineScope() { aurora::net::Engine::instance().shutdown(); }
    EngineScope(const EngineScope &) = delete;
    auto operator=(const EngineScope &) -> EngineScope & = delete;
    EngineScope(EngineScope &&) = delete;
    auto operator=(EngineScope &&) -> EngineScope & = delete;
};

/// @brief 秒 → 毫秒预算。
/// @param seconds 命令行给定的秒数。
/// @return 毫秒预算。
auto budget(int seconds) -> std::chrono::milliseconds { return std::chrono::milliseconds{seconds} * 1000; }

/// @brief `auto` 段：首快照纪律、订阅首拍、插队受理与快照只读性。
/// @param timeout 等待预算。
/// @return 退出码：0 全通过（无采集腿时同样为 0，由调用方折算为环境不可用）；1 有 FAIL。
auto run_auto(std::chrono::milliseconds timeout) -> int {
    const auto start = std::chrono::steady_clock::now();
    auto &monitor = aurora::net::ConnectivityMonitor::instance();

    // 1. 从未采样时的首快照：Unknown + 编译期腿令牌 + stale + generation=0，且**不触发采样**。
    const auto initial = monitor.snapshot();
    emit("first snapshot : " + describe(initial, start));
    check(initial.state == aurora::net::NetworkState::Unknown, "first snapshot is Unknown before any sample");
    check(initial.generation == 0, "first snapshot carries generation 0");
    check(initial.stale, "first snapshot is marked stale");
    check(!initial.last_failure.has_value(), "last_failure is empty before any connection outcome");
    if (initial.source == aurora::net::ConnectivitySource::None) {
        skip("no probe leg in this build: the fact can never leave Unknown (source=none)");
        return 0;
    }
    check(true, "this build carries a compiled probe leg (source != none)");

    // 2. 订阅：登记成功即安排一次 SubscriptionStart 采样。
    const auto before_first = monitor.snapshot();
    auto watcher = std::make_shared<Watcher>();
    auto subscribed =
        monitor.subscribe([watcher](const aurora::net::ConnectivityChange &change) { watcher->record(change); });
    if (!subscribed) {
        check(false, "subscribe() is accepted on a build with a compiled leg (" + subscribed.error().code + ")");
        return 1;
    }
    auto handle = std::move(subscribed.value());
    check(handle.token() != 0, "the registration token is non-zero");
    check(handle.is_registered(), "the registration reports itself registered");

    // 3. 首拍抵达：cause 为 SubscriptionStart，且把状态从 Unknown 带出。
    const bool arrived = watcher->wait_first(timeout);
    check(arrived, "the subscription-start sample delivers a change");
    if (!arrived) {
        return 1;
    }
    const auto first = watcher->first();
    if (first.has_value()) {
        emit("first change   : " + describe_change(*first, start));
        check(first->cause == aurora::net::ConnectivityChangeCause::SubscriptionStart,
              "the first change is caused by SubscriptionStart");
        check(first->previous.state == before_first.state && first->previous.generation == before_first.generation,
              "the first change anchors its previous value at the subscribe-time snapshot");
        check(first->current.generation >= 1, "the first delivered change carries generation >= 1");
        check(first->current.state != aurora::net::NetworkState::Unknown,
              "the compiled leg leaves Unknown on the first sample (online or offline)");
        check(!first->current.stale, "a successful sample clears the stale bit");
        check(!first->current.last_failure.has_value(), "no failure is recorded while no exchange failed");
    }

    // 4. `refresh()` 受理：返回 OK 即「请求已受理」，结果经快照或通知抵达。
    const auto refreshed = monitor.refresh();
    check(refreshed.ok(), std::string{"refresh() accepts the request"} +
                              (refreshed.ok() ? std::string{} : " (" + refreshed.error().code + ")"));
    if (!refreshed.ok()) {
        return 1;
    }
    const auto token = handle.token();
    check(token != 0 && handle.is_registered(), "the registration stays registered after deliveries");

    // 5. 快照只读：重复取不快照不推进代际、不发布通知。
    const auto settled = monitor.snapshot();
    const auto count_after_sample = watcher->count();
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const auto again = monitor.snapshot();
    check(again.generation == settled.generation, "snapshot() is a read: repeated reads do not advance the generation");
    check(again.state == settled.state, "snapshot() is a read: repeated reads do not change the state");
    check(watcher->count() == count_after_sample, "snapshot() is a read: repeated reads publish no notification");

    // 6. 句柄移动契约：源句柄转空态，目标接管订阅。
    aurora::net::ConnectivityRegistration moved = std::move(handle);
    // 故意读 moved-from 源句柄：契约要求它可读且为空态，不属于使用已移动对象。
    // NOLINTNEXTLINE(bugprone-use-after-move)
    check(handle.token() == 0 && !handle.is_registered(), "a moved-from registration becomes an empty handle");
    check(moved.token() == token && moved.is_registered(), "the moved-to handle keeps the subscription");
    return failures == 0 ? 0 : 1;
}

/// @brief `offline` / `online` 段：等在目标态上落定一次，打印到达时的成因与事实。
/// @param expected 目标态（Offline 或 Online）。
/// @param timeout 等待预算。
/// @return 退出码：0 全通过；1 有 FAIL。
auto run_expect(aurora::net::NetworkState expected, std::chrono::milliseconds timeout) -> int {
    const auto start = std::chrono::steady_clock::now();
    auto &monitor = aurora::net::ConnectivityMonitor::instance();
    const auto initial = monitor.snapshot();
    emit("first snapshot : " + describe(initial, start));
    const auto target = std::string{aurora::net::network_state_text(expected)};

    auto watcher = std::make_shared<Watcher>();
    auto subscribed =
        monitor.subscribe([watcher](const aurora::net::ConnectivityChange &change) { watcher->record(change); });
    if (!subscribed) {
        if (initial.source == aurora::net::ConnectivitySource::None) {
            skip("no probe leg in this build: subscribe() reports net-platform-unavailable");
            return 0;
        }
        check(false, "subscribe() is accepted (" + subscribed.error().code + ")");
        return 1;
    }
    auto handle = std::move(subscribed.value());
    check(handle.is_registered(), "the registration reports itself registered");

    if (monitor.snapshot().state == expected) {
        emit("already there  : state=" + target);
        check(true, "the leg already reads the expected state before any new sample");
        return failures == 0 ? 0 : 1;
    }
    const auto hit = watcher->wait_state(expected, timeout);
    check(hit.has_value(), "the leg reaches " + target + " within " + std::to_string(timeout.count() / 1000) + "s");
    if (!hit.has_value()) {
        return 1;
    }
    emit("landed         : " + describe_change(*hit, start));
    check(hit->current.generation >= 1, "the transition carries generation >= 1");
    check(!hit->current.stale, "the transition is backed by a fresh sample");
    return failures == 0 ? 0 : 1;
}

/// @brief 探针的选段。
enum class Expect : std::uint8_t {
    Auto,  ///< 公共面契约段（不需要人工动作）
    Offline,  ///< 拔线 / 删缺省路由之后
    Online,  ///< 恢复之后
};

/// @brief 把 `--expect` 的取值译成选段。
/// @param text 命令行取值。
/// @return 合法取值为对应选段；否则空值（调用方按用法错误返回）。
auto parse_expect(std::string_view text) -> std::optional<Expect> {
    if (text == "auto") {
        return Expect::Auto;
    }
    if (text == "offline") {
        return Expect::Offline;
    }
    if (text == "online") {
        return Expect::Online;
    }
    return std::nullopt;
}

}  // namespace

// 口径与 tools/verify/ 其余探针同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    EngineScope engine_scope;
    const auto cli = aurora_verify::parse_command_line(
        aurora::cli::CommandSpec{
            .name = "aurora_verify",
            .about = "Connectivity live probe: the compiled leg reads a real route / interface table",
            .options =
                {
                    aurora::cli::OptionSchema{
                        .long_name = "expect",
                        .kind = aurora::cli::ValueKind::String,
                        .help = "auto (facade contracts) | offline | online (wait for that state after unplugging / "
                                "restoring)",
                        .default_text = "auto"},
                    aurora::cli::OptionSchema{.long_name = "timeout",
                                              .kind = aurora::cli::ValueKind::Int,
                                              .help = "Seconds to wait for the expected state",
                                              .default_text = "20",
                                              .minimum = 1,
                                              .maximum = 300},
                }},
        argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const auto &arguments = *cli.arguments;
    const auto expect_text = arguments.get<std::string>("expect");
    const auto timeout_seconds = arguments.get<int>("timeout");
    if (!expect_text || !timeout_seconds) {
        return aurora_verify::CommandLine::AURORA_EXIT_USAGE;
    }
    const auto mode = parse_expect(expect_text.value());
    if (!mode) {
        AURORA_LOG_ERROR("verify", "unknown --expect value: " + expect_text.value() + " (auto|offline|online)");
        return aurora_verify::CommandLine::AURORA_EXIT_USAGE;
    }

    aurora::init_console();
    emit("== Aurora connectivity live probe ==");
    emit("expect: " + expect_text.value() + "  timeout: " + std::to_string(timeout_seconds.value()) + "s");
    try {
        auto &monitor = aurora::net::ConnectivityMonitor::instance();
        emit("options: sample_interval=" + std::to_string(monitor.options().sample_interval.count()) + "ms" +
             " offline_evidence=" + std::to_string(monitor.options().offline_evidence) +
             " online_evidence=" + std::to_string(monitor.options().online_evidence));
        const bool no_leg = monitor.snapshot().source == aurora::net::ConnectivitySource::None;
        int code = 0;
        if (*mode == Expect::Auto) {
            code = run_auto(budget(timeout_seconds.value()));
        } else {
            code = run_expect(
                *mode == Expect::Offline ? aurora::net::NetworkState::Offline : aurora::net::NetworkState::Online,
                budget(timeout_seconds.value()));
        }
        if (code == 0 && no_leg && *mode == Expect::Auto) {
            emit("result: ENV-UNAVAILABLE (exit 2)");
            return 2;
        }
        emit(code == 0 ? "result: PASS (exit 0)" : "result: FAIL (exit 1)");
        return code;
    } catch (const std::exception &error) {
        emit(std::string{"FAIL: connectivity probe threw: "} + error.what());
        return 1;
    } catch (...) {
        emit("FAIL: connectivity probe threw an unknown exception");
        return 1;
    }
}

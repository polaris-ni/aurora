#pragma once

// ============================================================
// 网络用例的宿主驱动等待口径（tests/framework/net_wait.h）
// ------------------------------------------------------------
// 事件驱动传输在测试里有两种送达方式：常驻引擎线程，或**用例线程自己**声明宿主驱动
// （`set_host_driven(true)`）后取注册口的事件源、`wait_ready` 一次、把就绪集合 `dispatch()` 回去。
// 后者把送达时刻钉在用例线程上，断言才不需要跨线程同步，代价是每个文件都要重写同一段泵代码。
// 传输族落地的五个文件里这段各写了一遍（窗口 20 ms、预算 2 s / 3 s 不一），本头把它收成一份。
//
// 与 `async_wait.h` 的分工：那一份管「怎么等一个条件」（通知驱动 / 有限轮询 / 超时自报），本头管
// 「等的时候谁来泵」。`drive_until()` 返回 `WaitOutcome`，因此可以直接喂给 `AURORA_TEST_WAIT_UNTIL`。
//
// 只在 `host_driven` 形态下使用本头的泵：常驻引擎自己会等待这些句柄，同一枚事件源被两方等待会
// 让一次就绪投递两次（`06-app-platform.md` §8.8.4 的互斥口径）。
//
// 框架属仓库私有设施，不进 include/、不进 aurora_api.json。
// ============================================================

#include <chrono>
#include <cstddef>
#include <functional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "async_wait.h"
#include "aurora/app/event_source_registry.h"
#include "aurora/net/engine.h"
#include "aurora/net/poll.h"

namespace aurora::test::net {

/// @brief 单次宿主等待的窗口：够短以便把「这一步没发生」快速回给谓词，够长以免空转刷 CPU。
inline constexpr std::chrono::milliseconds AURORA_PUMP_WINDOW{20};

/// @brief 泵尾巴：一次宿主等待之后追加执行的可调用体（脚本化时钟的到期任务在此落地）。
///
/// 为什么要有这一枚而不是让每个用例自己记得推时钟：`Engine::post` 的立即任务在**没有任何事件源登记**
/// 时也要有人执行（解析器、竞速编排都是这一步先发生），漏掉它的泵会一直睡到预算耗尽，红成「超时」而
/// 不是「忘了跑」。装替身时钟的用例经 `FakeEngineScope` 注册本钩子，拆除即清空。
using PumpTail = std::function<void()>;

/// @brief 泵尾巴的存储位（进程级单点，用例线程安装）。
/// @return 存放 `PumpTail` 的引用；空可调用体 = 没有额外步骤。
inline auto pump_tail_slot() -> PumpTail & {
    // 惰性构造的函数内 static：首次调用才建，跨 TU 初始化顺序问题在此不存在（本检查的担心面）。
    // 仅浏览器口径命中——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static PumpTail tail;
    return tail;
}

/// @brief 宿主驱动守卫：作用域内由用例线程担任驱动方，退出时把驱动方交回引擎。
///
/// 注册口是进程级状态，同一个 runner 里顺序执行的其它用例必须看到缺省态，故析构一律复位。
/// @note Thread: 单线程安装（用例线程）
/// @note Side-effects: 改写进程级注册口的驱动方开关
struct HostDriven {
    /// @brief 声明用例线程自带等待集。
    HostDriven() { aurora::app::EventSourceRegistry::instance().set_host_driven(true); }
    ~HostDriven() { aurora::app::EventSourceRegistry::instance().set_host_driven(false); }
    HostDriven(const HostDriven &) = delete;
    auto operator=(const HostDriven &) -> HostDriven & = delete;
    HostDriven(HostDriven &&) = delete;
    auto operator=(HostDriven &&) -> HostDriven & = delete;
};

/// @brief 引擎生命周期守卫：收敛常驻线程并把投递接缝与驱动方一并交回缺省态。
///
/// 用于「引擎自己驱动」的用例：这类用例结束后若留下在跑的引擎线程与装着 Dispatcher 的接缝，
/// 后续用例会收到落在别人线程上的就绪投递。
/// @note Thread: 单线程安装（用例线程）
/// @note Side-effects: 停机常驻引擎并清空进程级注册口接缝
struct EngineGuard {
    EngineGuard() = default;
    ~EngineGuard() {
        aurora::net::Engine::instance().shutdown();
        aurora::app::EventSourceRegistry::instance().set_dispatcher({});
        aurora::app::EventSourceRegistry::instance().set_host_driven(false);
    }
    EngineGuard(const EngineGuard &) = delete;
    auto operator=(const EngineGuard &) -> EngineGuard & = delete;
    EngineGuard(EngineGuard &&) = delete;
    auto operator=(EngineGuard &&) -> EngineGuard & = delete;
};

/// @brief 取一次宿主等待并把就绪集合交回注册口（就绪回调随即在本线程执行）。
///
/// 先跑泵尾巴再取事件源：脚本化时钟的立即任务往往就是「下一次登记事件源」的前置，顺序反过来会让
/// 一轮泵变成纯睡眠。没有登记中的事件源时睡 1 ms 再回，避免把「刚发起、登记还没发生」的这一轮变成
/// 零延迟自旋。
/// @param registry 进程级事件源注册口。
inline auto pump_once(aurora::app::EventSourceRegistry &registry) -> void {
    if (const auto &tail = pump_tail_slot(); tail) {
        tail();
    }
    const auto sources = registry.sources();
    if (sources.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        return;
    }
    std::vector<aurora::net::PollDescriptor> descriptors;
    descriptors.reserve(sources.size());
    for (const auto &source : sources) {
        descriptors.push_back(aurora::net::PollDescriptor{
            .handle = source.handle, .want_read = source.interest.read, .want_write = source.interest.write});
    }
    const auto outcomes = aurora::net::wait_ready(descriptors, AURORA_PUMP_WINDOW);
    if (!outcomes) {
        return;
    }
    const auto &values = outcomes.value();
    std::vector<aurora::app::ReadySource> ready;
    for (std::size_t i = 0; i < values.size() && i < sources.size(); ++i) {
        const auto &outcome = values[i];
        if (!outcome.readable && !outcome.writable && !outcome.error && !outcome.hangup) {
            continue;
        }
        ready.push_back(aurora::app::ReadySource{
            .token = sources[i].token,
            .ready = aurora::app::SourceReady{.readable = outcome.readable,
                                              .writable = outcome.writable,
                                              .error = outcome.error,
                                              .hangup = outcome.hangup},
        });
    }
    registry.dispatch(ready);
}

/// @brief 反复泵宿主等待直到谓词成立或预算耗尽。
///
/// 先判后泵，故「条件已经成立」的形态一次泵都不发起；预算耗尽后仍补判一次，把「谓词在最后一轮
/// 泵里才成立」这种临界情形判成成立而不是误报超时。
/// @tparam Pred 可调用体，返回可转 `bool` 的值；在调用线程求值。
/// @param subject 等待主题，进超时文本。
/// @param pred 目标条件。
/// @param budget 预算；缺省 `AURORA_WAIT_BUDGET`。中间轮次应显式改短，见 `pump_idle()`。
/// @return 结果与现场（`polls` 是实际泵次数）。
template <typename Pred>
[[nodiscard]] auto drive_until(std::string_view subject, const Pred &pred,
                               std::chrono::milliseconds budget = AURORA_WAIT_BUDGET) -> aurora::test::WaitOutcome {
    auto &registry = aurora::app::EventSourceRegistry::instance();
    const auto started = std::chrono::steady_clock::now();
    aurora::test::WaitOutcome outcome{.budget = budget, .subject = subject};
    while (std::chrono::steady_clock::now() - started < budget) {
        if (static_cast<bool>(pred())) {
            outcome.satisfied = true;
            break;
        }
        pump_once(registry);
        ++outcome.polls;
    }
    if (!outcome.satisfied && static_cast<bool>(pred())) {
        outcome.satisfied = true;
    }
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return outcome;
}

/// @brief 让宿主有界空转一段窗口（「不该发生的事」只能靠窗口观察，没有谓词可等）。
/// @param window 窗口长度。
inline auto pump_idle(std::chrono::milliseconds window) -> void {
    auto &registry = aurora::app::EventSourceRegistry::instance();
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline) {
        pump_once(registry);
    }
}

}  // namespace aurora::test::net

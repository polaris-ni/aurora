/// 测试类型: unit
/// 目标单元: include/aurora/net/engine.h
/// 测试说明: aurora::net 自带 poll 线程引擎（CHG-010 底座切片）的行为契约回归：能力矩阵按已落地
///           形态如实回报（未实现的 tls 不得先于实现点亮；本机 IPC 取决于本腿运行期是否真有该地址族，
///           故该位与平台层探测同口径）、`post` 触发惰性启动并把任务投到**非调用线程**执行、
///           `post_after` 按单调钟到期投递且不早于登记意图、
///           `shutdown` 幂等且收敛后可再次惰性启动、常驻循环真实驱动事件源（登记→对端写入→回调
///           抵达，全程不调 `wait_and_dispatch`，即 §2「引擎自持工作线程」的形态）、注销与就绪同帧
///           发生时循环不被瞬态等待失败打断且后续登记照常送达（§18-1「注销 = 停止产生新回调」）、
///           宿主声明自带等待集时引擎让位（一条事件源只由一个驱动方等待，交回后引擎重新送达）。
///           事件源用例先装一枚就地执行的 `Dispatcher`，据此验证引擎「宿主已装则不改」的接缝装配纪律。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 fd 语义，用例体退化为断言不支持形态。

#include <atomic>
#include <chrono>
#include <functional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/capabilities.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/engine.h"
#include "aurora/net/poll.h"
#include "aurora/net/socket.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_engine {
namespace {

using aurora::net::Engine;

/// @brief 有界等待预算：覆盖引擎线程唤醒与内核回环往返，又不至于把挂死伪装成慢。
constexpr std::chrono::milliseconds AURORA_WAIT_BUDGET{1000};

/// @brief 定时用例的下界余量（Windows 等待粒度可比毫秒粗，故只断言「不早得多」而非精确）。
constexpr std::chrono::milliseconds AURORA_TIMER_SLACK{40};

/// @brief 一对已建立的回环 TCP 连接（client 发起、经 backlog 由同线程 accept 取得 server）。
struct LoopbackPair {
    aurora::net::StreamSocket client;
    aurora::net::StreamSocket server;
};

/// @brief 在临时端口起监听、连接、接受，得到一对连通的回环流；监听口随即关闭。
/// @param session 已就绪的运行时会话（Windows 侧 WSAStartup 归属，用例环境前提而非被测行为）。
/// @return 连通的 client/server 对。
auto make_connected_pair(aurora::net::Session &session) -> LoopbackPair {
    AURORA_TEST_REQUIRE(session.ok());
    auto bound = aurora::net::Listener::listen_loopback(0, 4);
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());

    auto connected = aurora::net::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(connected.ok());
    auto accepted = listener.accept();
    AURORA_TEST_REQUIRE(accepted.ok());
    return LoopbackPair{.client = std::move(connected.value()), .server = std::move(accepted.value())};
}

/// @brief 向对端写入一段字节（失败即刻转红：写不出去就无法制造「就绪」这一被测前提）。
/// @param socket 目标流。
/// @param payload 待发送文本。
auto write_payload(aurora::net::StreamSocket &socket, std::string_view payload) -> void {
    auto sent = socket.write_all(std::span<const char>(payload.data(), payload.size()));
    AURORA_TEST_REQUIRE(sent.ok());
}

/// @brief 有界轮询等待谓词成立（被测工作在引擎线程发生，同线程不可能同步断言）。
/// @param pred 目标条件。
/// @param budget 最长等待时长。
/// @return 预算内成立为 true。
auto wait_until(const std::function<bool()> &pred, std::chrono::milliseconds budget) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return pred();
}

/// @brief 引擎与注册口接缝的守卫：析构收敛引擎、清 Dispatcher 并把驱动方交回引擎，
///        不把在跑线程或「宿主驱动」开关留给同 runner 的其它用例。
struct EngineGuard {
    EngineGuard() = default;
    ~EngineGuard() {
        Engine::instance().shutdown();
        auto &registry = aurora::app::EventSourceRegistry::instance();
        registry.set_dispatcher({});
        registry.set_host_driven(false);
    }
    EngineGuard(const EngineGuard &) = delete;
    auto operator=(const EngineGuard &) -> EngineGuard & = delete;
    EngineGuard(EngineGuard &&) = delete;
    auto operator=(EngineGuard &&) -> EngineGuard & = delete;
};

}  // namespace

// 能力矩阵如实回报：本腿的事件源/字节流/监听/数据报/名称解析位为真，未落地形态（TLS）恒假，本机 IPC
// 位与本腿运行期探测同口径（头文件提供 `AF_UNIX` 与真能开出该地址族是两件事；「探测说能用」的行为半
// 身在 `utest_net_transport` 的路径往返用例里）。wasm 腿则一切为假且名为 "none"（§2「不提供假降级」）。
AURORA_TEST_CASE(capabilities_report_only_landed_forms) {
#ifdef AURORA_PLATFORM_WASM
    const auto &engine = Engine::instance();
    AURORA_TEST_CHECK_EQ(engine.name(), "none");
    const auto caps = engine.capabilities();
    AURORA_TEST_CHECK_FALSE(caps.tcp);
    AURORA_TEST_CHECK_FALSE(caps.listens);
    AURORA_TEST_CHECK_FALSE(caps.event_source);
#else
    const auto &engine = Engine::instance();
    AURORA_TEST_CHECK_EQ(engine.name(), "poll");
    const auto caps = engine.capabilities();
    AURORA_TEST_CHECK_TRUE(caps.tcp);
    AURORA_TEST_CHECK_TRUE(caps.listens);
    AURORA_TEST_CHECK_TRUE(caps.streaming);
    AURORA_TEST_CHECK_TRUE(caps.custom_headers);
    AURORA_TEST_CHECK_TRUE(caps.event_source);
    AURORA_TEST_CHECK_TRUE(caps.udp);
    AURORA_TEST_CHECK_TRUE(caps.dns);
    AURORA_TEST_CHECK_EQ(caps.local_ipc, aurora::net::detail::local_ipc_supported());
    AURORA_TEST_CHECK_FALSE(caps.tls);
#endif
}

// 查询能力不启动线程（惰性启动的判据是「首次需要它」而非「首次问它」）。
AURORA_TEST_CASE(querying_capabilities_does_not_start_the_thread) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());
#else
    EngineGuard guard;
    (void)Engine::instance().capabilities();
    (void)Engine::instance().name();
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());
#endif
}

// `post` 触发惰性启动，任务体在**非调用线程**执行（UI 线程唯一契约：等待不发生在调用方）。
AURORA_TEST_CASE(post_starts_thread_and_runs_off_caller_thread) {
#ifdef AURORA_PLATFORM_WASM
    auto posted = Engine::instance().post([] {});
    AURORA_TEST_CHECK_FALSE(posted.ok());
    AURORA_TEST_CHECK_EQ(posted.error().code, "net-platform-unavailable");
#else
    EngineGuard guard;
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> done{false};
    std::thread::id worker{};
    auto posted = Engine::instance().post([&] {
        worker = std::this_thread::get_id();
        done.store(true);
    });
    AURORA_TEST_REQUIRE(posted.ok());
    AURORA_TEST_CHECK_TRUE(Engine::instance().running());
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return done.load(); }, AURORA_WAIT_BUDGET));
    AURORA_TEST_CHECK_TRUE(done.load());
    AURORA_TEST_CHECK_NE(worker, caller);
#endif
}

// 空任务受理但不占队列、也不拉起线程；停机后再 `post` 会重新惰性启动（两种结局都必须显式表达）。
AURORA_TEST_CASE(empty_task_is_no_op_and_post_restarts_a_stopped_engine) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_CHECK_TRUE(Engine::instance().post({}).ok());
#else
    EngineGuard guard;
    AURORA_TEST_CHECK_TRUE(Engine::instance().post({}).ok());
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());  // 空任务不该把线程拉起来

    std::atomic<int> runs{0};
    AURORA_TEST_REQUIRE(Engine::instance().post([&] { runs.fetch_add(1); }).ok());
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return runs.load() == 1; }, AURORA_WAIT_BUDGET));

    Engine::instance().shutdown();
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());
    auto rejected = Engine::instance().post([&] { runs.fetch_add(1); });
    // 停机后重启是合法形态（随首个新任务再惰性启动）；两种结局都必须显式表达而非静默。
    if (rejected.ok()) {
        AURORA_TEST_CHECK_TRUE(wait_until([&] { return runs.load() == 2; }, AURORA_WAIT_BUDGET));
    } else {
        AURORA_TEST_CHECK_EQ(rejected.error().code, "net-platform-unavailable");
    }
#endif
}

// `post_after` 不早于登记意图到期，且在预算内必然抵达（同一条循环承载定时器，不额外起线程）。
AURORA_TEST_CASE(post_after_fires_on_monotonic_deadline) {
#ifdef AURORA_PLATFORM_WASM
    auto scheduled = Engine::instance().post_after(std::chrono::milliseconds{10}, [] {});
    AURORA_TEST_CHECK_FALSE(scheduled.ok());
    AURORA_TEST_CHECK_EQ(scheduled.error().code, "net-platform-unavailable");
#else
    EngineGuard guard;
    const auto delay = std::chrono::milliseconds{120};
    const auto started = std::chrono::steady_clock::now();
    std::atomic<bool> fired{false};
    auto scheduled = Engine::instance().post_after(delay, [&] { fired.store(true); });
    AURORA_TEST_REQUIRE(scheduled.ok());
    // 立即复查一次：引擎线程不应在登记意图之前就把任务跑掉。
    AURORA_TEST_CHECK_FALSE(fired.load());
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return fired.load(); }, AURORA_WAIT_BUDGET));
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
    AURORA_TEST_CHECK_TRUE(elapsed >= delay - AURORA_TIMER_SLACK);
#endif
}

// 常驻循环真实驱动事件源：登记→对端写入→回调抵达，**全程不调 `wait_and_dispatch`**。
// 先装就地 Dispatcher，据此验证「宿主已装则不改」——回调线程因此是引擎线程而非主线程。
AURORA_TEST_CASE(resident_loop_delivers_registered_source) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no fd wait semantics; the engine reports unavailable there");
#else
    EngineGuard guard;
    aurora::net::Session session;
    auto pair = make_connected_pair(session);
    auto &registry = aurora::app::EventSourceRegistry::instance();
    registry.set_dispatcher([](const std::function<void()> &fn) { fn(); });

    std::atomic<int> hits{0};
    std::atomic<bool> readable_seen{false};
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> off_caller{true};
    auto reg = registry.register_source(pair.server.native_handle(), aurora::app::WaitInterest{.read = true},
                                        [&](const aurora::app::SourceReady &ready) {
                                            readable_seen.store(ready.readable);
                                            off_caller.store(std::this_thread::get_id() != caller);
                                            hits.fetch_add(1);
                                        });
    AURORA_TEST_REQUIRE(reg.ok());

    // 启动常驻循环（引擎不接管注册口的驱动就不是事件驱动形态；post 是 §2 的惰性启动口）。
    AURORA_TEST_REQUIRE(Engine::instance().post([] {}).ok());
    write_payload(pair.client, "x");
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return hits.load() >= 1; }, AURORA_WAIT_BUDGET));
    AURORA_TEST_CHECK_TRUE(readable_seen.load());
    AURORA_TEST_CHECK_TRUE(off_caller.load());

    // 注销 = 停止产生新回调（§18-1）：句柄析构后再写入不应有第二次抵达。
    {
        auto registration = std::move(reg.value());
    }
    AURORA_TEST_CHECK_EQ(registry.live_count(), std::size_t{0});
    const int settled = hits.load();
    write_payload(pair.client, "y");
    AURORA_TEST_CHECK_FALSE(wait_until([&] { return hits.load() > settled; }, std::chrono::milliseconds{120}));
#endif
}

// 注销与就绪同帧发生：常驻循环可能正持着含已关闭句柄的快照，瞬态等待失败不得终止循环，
// 之后新登记的事件源仍要照常送达（这是 §8.8.3「基元容错」在常驻形态下的落地判据）。
AURORA_TEST_CASE(unregister_during_wait_keeps_the_loop_alive) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no fd wait semantics; the engine reports unavailable there");
#else
    EngineGuard guard;
    aurora::net::Session session;
    auto pair = make_connected_pair(session);
    auto &registry = aurora::app::EventSourceRegistry::instance();
    registry.set_dispatcher([](const std::function<void()> &fn) { fn(); });
    AURORA_TEST_REQUIRE(Engine::instance().post([] {}).ok());

    std::atomic<int> discarded{0};
    {
        auto fleeting = registry.register_source(pair.server.native_handle(),
                                                 aurora::app::WaitInterest{.read = true, .write = true},
                                                 [&](const aurora::app::SourceReady &) { discarded.fetch_add(1); });
        AURORA_TEST_REQUIRE(fleeting.ok());
    }  // 立即注销：与循环的等待窗口同帧竞态

    std::atomic<int> hits{0};
    auto second = registry.register_source(pair.server.native_handle(), aurora::app::WaitInterest{.read = true},
                                           [&](const aurora::app::SourceReady &) { hits.fetch_add(1); });
    AURORA_TEST_REQUIRE(second.ok());
    write_payload(pair.client, "z");
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return hits.load() >= 1; }, AURORA_WAIT_BUDGET));
#endif
}

// 收敛后可再次惰性启动：`shutdown` 幂等，重启后队列与事件源驱动都恢复正常（退出路径与重入判据）。
AURORA_TEST_CASE(shutdown_is_idempotent_and_restart_serves_new_work) {
#ifdef AURORA_PLATFORM_WASM
    Engine::instance().shutdown();
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());
#else
    EngineGuard guard;
    Engine::instance().shutdown();  // 未启动即为空操作
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());

    std::atomic<int> runs{0};
    AURORA_TEST_REQUIRE(Engine::instance().post([&] { runs.fetch_add(1); }).ok());
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return runs.load() == 1; }, AURORA_WAIT_BUDGET));

    Engine::instance().shutdown();
    Engine::instance().shutdown();  // 二次收敛仍为空操作，不得 join 已移交的线程
    AURORA_TEST_CHECK_FALSE(Engine::instance().running());
    AURORA_TEST_CHECK_EQ(aurora::app::EventSourceRegistry::instance().live_count(), std::size_t{0});

    AURORA_TEST_REQUIRE(Engine::instance().post([&] { runs.fetch_add(1); }).ok());
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return runs.load() == 2; }, AURORA_WAIT_BUDGET));
#endif
}

// 驱动方互斥（§8.8.4 ③）：宿主声明自带等待集后，常驻引擎不再等待注册口条目——同一句柄的就绪
// 只由宿主自己的 `sources()`/`dispatch()` 投一次；交还引擎（置回假）后引擎重新接管并送达。
// 两边同时等待会让一次就绪投递两次，这条开关就是防它的那道闸。
AURORA_TEST_CASE(host_driven_registry_is_not_waited_by_engine) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no fd wait semantics; the engine reports unavailable there");
#else
    EngineGuard guard;
    aurora::net::Session session;
    auto pair = make_connected_pair(session);
    auto &registry = aurora::app::EventSourceRegistry::instance();
    registry.set_dispatcher([](const std::function<void()> &fn) { fn(); });
    AURORA_TEST_REQUIRE(Engine::instance().post([] {}).ok());  // 让常驻循环先在跑，否则「让位」无从谈起

    std::atomic<int> hits{0};
    registry.set_host_driven(true);
    auto reg = registry.register_source(pair.server.native_handle(), aurora::app::WaitInterest{.read = true},
                                        [&](const aurora::app::SourceReady &) { hits.fetch_add(1); });
    AURORA_TEST_REQUIRE(reg.ok());
    write_payload(pair.client, "a");
    AURORA_TEST_CHECK_FALSE(wait_until([&] { return hits.load() > 0; }, std::chrono::milliseconds{120}));

    // 宿主驱动：取快照 → 基元等待 → 按令牌回填投递，恰好一次。
    const auto sources = registry.sources();
    AURORA_TEST_REQUIRE_EQ(sources.size(), std::size_t{1});
    const std::vector<aurora::net::PollDescriptor> want{aurora::net::PollDescriptor{
        .handle = sources[0].handle, .want_read = sources[0].interest.read, .want_write = sources[0].interest.write}};
    const auto polled = aurora::net::wait_ready(want, AURORA_WAIT_BUDGET);
    AURORA_TEST_REQUIRE(polled.ok());
    const auto &outcome = polled.value()[0];
    std::vector<aurora::app::ReadySource> ready;
    if (outcome.readable || outcome.writable || outcome.error || outcome.hangup) {
        ready.push_back(aurora::app::ReadySource{.token = sources[0].token,
                                                 .ready = aurora::app::SourceReady{.readable = outcome.readable,
                                                                                   .writable = outcome.writable,
                                                                                   .error = outcome.error,
                                                                                   .hangup = outcome.hangup}});
    }
    registry.dispatch(ready);
    AURORA_TEST_CHECK_EQ(hits.load(), 1);

    // 交还引擎：句柄上的数据未被读过（电平触发仍可读），引擎重取快照后应立刻送达第二次。
    registry.set_host_driven(false);
    AURORA_TEST_CHECK_TRUE(wait_until([&] { return hits.load() >= 2; }, AURORA_WAIT_BUDGET));
#endif
}

}  // namespace aurora::test_cases::utest_net_engine

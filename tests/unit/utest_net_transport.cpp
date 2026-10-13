/// 测试类型: unit
/// 目标单元: include/aurora/net/transport.h
/// 测试说明: aurora::net 事件驱动字节流（传输族核心切片）的行为契约回归：连接与接受在同一台机器
///           的回环上恰一次送达并可双向收发（含 `peer_endpoint` / `is_loopback_peer` /
///           `native_handle` 三条观测口）；半关闭合法——本端停写后对端读得 **0 字节且是 OK 而不是
///           错误**，反向仍可继续收发，本端 `shutdown(ShutdownMode::Read)` 同样以 0 字节收尾已排队的读；背压是
///           **拉模型**——对端不取时待发字节越过高水位即被 `net-buffer-limit` 拒、对端排空后重新
///           接受，且待发量至多超出高水位一枚请求（大于高水位的单次写入必须仍有可能成功）；
///           `pause_read()` 期间字节留在内核、`resume_read()` 后照原样送达；`cancel()` 让挂起的读
///           恰一次以 `net-cancelled` 收尾并关闭连接；参数与空态有确定形态（空缓冲读 =
///           `net-buffer-limit`，空写入 = OK(0)，空态句柄不崩不悬挂）；`StreamListener::close()` 让
///           未交付的 accept 回调恰一次收到取消结果；引擎驱动形态下用例线程不做任何 `dispatch()`
///           也照样送达，且不可达目标**恰一次**以失败抵达（永不触发的回调是这类接口最难定位的缺陷）。
///           除引擎那条外全部走宿主驱动形态（`set_host_driven(true)` + `sources()` / `wait_ready` /
///           `dispatch()`），据此把送达时刻钉在用例线程上，不必等后台线程。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 fd 等待语义，用例体退化为
///    AURORA_TEST_SKIP。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/engine.h"
#include "aurora/net/poll.h"
#include "aurora/net/transport.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_transport {
namespace {

using aurora::net::Cancellation;
using aurora::net::Endpoint;
using aurora::net::StreamListener;
using aurora::net::StreamOptions;
using aurora::net::StreamTransport;

/// @brief 有界等待预算：覆盖回环往返与宿主轮询，又不至于把挂死伪装成慢。
constexpr std::chrono::milliseconds AURORA_WAIT_BUDGET{2000};

/// @brief 单次宿主等待的窗口：足够短以便轮询谓词，足够长以免空转刷 CPU。
constexpr std::chrono::milliseconds AURORA_PUMP_WINDOW{20};

/// @brief 背压用例的单次写入长度（同时用作写高水位，便于把「overshoot 至多一枚」写成断言）。
constexpr std::size_t AURORA_CHUNK{4096};

/// @brief 宿主驱动形态的守卫：退出时把驱动方交回引擎，不把这个开关留给同 runner 的其它用例。
struct HostDriven {
    HostDriven() { aurora::app::EventSourceRegistry::instance().set_host_driven(true); }
    ~HostDriven() { aurora::app::EventSourceRegistry::instance().set_host_driven(false); }
    HostDriven(const HostDriven &) = delete;
    auto operator=(const HostDriven &) -> HostDriven & = delete;
    HostDriven(HostDriven &&) = delete;
    auto operator=(HostDriven &&) -> HostDriven & = delete;
};

/// @brief 引擎驱动用例的守卫：收敛常驻线程并把投递接缝与驱动方都交回缺省态。
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

/// @brief 宿主驱动形态下的连接口径：本层定时器落在引擎线程，会让「送达时刻」不可同线程断言。
/// @return 连接超时置 0（即「不设时限，由对端决定何时出结果」）的选项。
auto host_options() -> StreamOptions {
    StreamOptions options;
    options.connect_timeout = std::chrono::milliseconds{0};
    return options;
}

/// @brief 取一次宿主等待并把就绪集合交回注册口（就绪回调随即在本线程执行）。
/// @param registry 进程级事件源注册口。
auto pump_once(aurora::app::EventSourceRegistry &registry) -> void {
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

/// @brief 轮询宿主等待直到谓词成立或预算耗尽（被测工作在回调里发生，同线程无法同步断言）。
/// @param registry 进程级事件源注册口。
/// @param pred 目标条件。
/// @return 预算内成立为 true。
auto pump_until(aurora::app::EventSourceRegistry &registry, const std::function<bool()> &pred) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + AURORA_WAIT_BUDGET;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        pump_once(registry);
    }
    return pred();
}

/// @brief 有界轮询等待谓词成立（引擎驱动形态：本线程完全不碰 `dispatch()`）。
/// @param pred 目标条件。
/// @return 预算内成立为 true。
auto wait_until(const std::function<bool()> &pred) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + AURORA_WAIT_BUDGET;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return pred();
}

/// @brief 一对已建立的回环流（宿主驱动形态，两端都在本线程拿到结果）。
struct HostPair {
    StreamListener listener;
    StreamTransport client;
    StreamTransport server;
};

/// @brief 绑定临时端口、发起连接并交付服务端流，得到连通的一对回环流。
/// @param registry 进程级事件源注册口（须已由 `HostDriven` 置为宿主驱动）。
/// @return 连通的一对；任一侧未达成即 REQUIRE 转红（制造不出被测前提时不该继续跑）。
auto make_host_pair(aurora::app::EventSourceRegistry &registry) -> HostPair {
    auto bound = StreamListener::bind(Endpoint::loopback(0), 4, host_options());
    AURORA_TEST_REQUIRE(bound.ok());
    HostPair pair{.listener = std::move(bound.value())};
    AURORA_TEST_REQUIRE_NE(pair.listener.local_port(), std::uint16_t{0});

    std::atomic<int> accepted{0};
    pair.listener.accept([&pair, &accepted](auto result) {
        AURORA_TEST_REQUIRE(result.ok());
        pair.server = std::move(result.value());
        ++accepted;
    });

    std::atomic<int> connected{0};
    StreamTransport::connect(Endpoint::loopback(pair.listener.local_port()), host_options(), Cancellation{},
                             [&pair, &connected](auto result) {
                                 AURORA_TEST_REQUIRE(result.ok());
                                 pair.client = std::move(result.value());
                                 ++connected;
                             });

    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return accepted.load() == 1 && connected.load() == 1; }));
    AURORA_TEST_REQUIRE(pair.client.is_open());
    AURORA_TEST_REQUIRE(pair.server.is_open());
    return pair;
}

/// @brief 一次读的观测记录：计数本身也是断言对象（「恰一次」是本层最难失手的契约）。
struct ReadLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    bool failed = false;
    std::string code;
};

/// @brief 一次写的观测记录。
struct WriteLog {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    bool failed = false;
    std::string code;
};

/// @brief 把一次读的结果落到日志里。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `StreamTransport::read` 的回调。
auto read_into(ReadLog &log) -> aurora::net::ReadDone {
    return [&log](auto result) {
        if (!result) {
            log.failed = true;
            log.code = std::string{result.error().code};
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 把一次写的结果落到日志里。
/// @param log 目标日志（须存活到回调抵达）。
/// @return 可直接交给 `StreamTransport::write` 的回调。
auto write_into(WriteLog &log) -> aurora::net::WriteDone {
    return [&log](auto result) {
        if (!result) {
            log.failed = true;
            log.code = std::string{result.error().code};
        } else {
            log.bytes.store(result.value());
        }
        ++log.calls;
    };
}

/// @brief 发一段文本并轮询直到写完（制造「对端有字节可读」这一前提）。
/// @param registry 进程级事件源注册口。
/// @param stream 目标流。
/// @param payload 待发送文本。
auto send_payload(aurora::app::EventSourceRegistry &registry, StreamTransport &stream, std::string_view payload)
    -> void {
    WriteLog log;
    stream.write(std::span<const char>(payload.data(), payload.size()), write_into(log));
    AURORA_TEST_REQUIRE(pump_until(registry, [&log]() { return log.calls.load() >= 1; }));
    AURORA_TEST_REQUIRE_FALSE(log.failed);
    AURORA_TEST_REQUIRE_EQ(log.bytes.load(), payload.size());
}

}  // namespace

// 连接与接受各恰一次送达，建立后双向收发都成立；三条观测口如实回报同一条回环连接。
AURORA_TEST_CASE(connect_accept_and_round_trip) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no file-descriptor wait semantics; the transport layer cannot bind");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    AURORA_TEST_CHECK_TRUE(pair.client.is_open());
    AURORA_TEST_CHECK_TRUE(pair.server.is_open());
    AURORA_TEST_CHECK_NE(pair.client.native_handle(), -1);
    AURORA_TEST_CHECK_TRUE(pair.client.is_loopback_peer());
    auto peer = pair.client.peer_endpoint();
    AURORA_TEST_REQUIRE(peer.ok());
    AURORA_TEST_CHECK_EQ(peer.value().port(), pair.listener.local_port());

    ReadLog server_read;
    std::vector<char> inbox(64);
    pair.server.read(std::span<char>(inbox), read_into(server_read));
    send_payload(registry, pair.client, "ping");
    AURORA_TEST_CHECK(pump_until(registry, [&server_read]() { return server_read.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(server_read.failed);
    AURORA_TEST_CHECK_EQ(server_read.bytes.load(), 4U);
    AURORA_TEST_CHECK_EQ(std::string(inbox.data(), server_read.bytes.load()), "ping");

    ReadLog client_read;
    std::vector<char> echo(64);
    pair.client.read(std::span<char>(echo), read_into(client_read));
    send_payload(registry, pair.server, "pong");
    AURORA_TEST_CHECK(pump_until(registry, [&client_read]() { return client_read.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(client_read.failed);
    AURORA_TEST_CHECK_EQ(std::string(echo.data(), client_read.bytes.load()), "pong");
#endif
}

// 半关闭：本端停写 → 对端读得 0 字节且是 OK 而不是错误；反向仍可收发；已停写一侧再写是确定的用法错误。
AURORA_TEST_CASE(half_close_reads_zero_and_other_direction_still_works) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket shutdown semantics");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    auto stopped = pair.client.shutdown(aurora::net::ShutdownMode::Write);
    AURORA_TEST_REQUIRE(stopped.ok());
    AURORA_TEST_CHECK_TRUE(pair.client.is_open());  // 停写不关连接

    ReadLog server_eof;
    std::vector<char> inbox(64);
    pair.server.read(std::span<char>(inbox), read_into(server_eof));
    AURORA_TEST_CHECK(pump_until(registry, [&server_eof]() { return server_eof.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(server_eof.failed);
    AURORA_TEST_CHECK_EQ(server_eof.bytes.load(), 0U);

    // FIN 之后再读仍恒 0：这是「对端不会再发」的稳定观测，而不是第二次 EOF 事件。
    ReadLog again;
    pair.server.read(std::span<char>(inbox), read_into(again));
    AURORA_TEST_CHECK(pump_until(registry, [&again]() { return again.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(again.failed);
    AURORA_TEST_CHECK_EQ(again.bytes.load(), 0U);

    ReadLog client_read;
    pair.client.read(std::span<char>(inbox), read_into(client_read));
    const std::string_view still_open = "still-open";
    send_payload(registry, pair.server, still_open);
    AURORA_TEST_CHECK(pump_until(registry, [&client_read]() { return client_read.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(client_read.failed);
    AURORA_TEST_CHECK_EQ(client_read.bytes.load(), still_open.size());

    WriteLog rejected;
    const std::string_view nope = "nope";
    pair.client.write(std::span<const char>(nope.data(), nope.size()), write_into(rejected));
    AURORA_TEST_CHECK_EQ(rejected.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(rejected.failed);
    AURORA_TEST_CHECK_EQ(rejected.code, "net-option-failed");
#endif
}

// 本端停读复用 EOF 形态（不新造错误码）：已排队的读恰一次得 0，另一侧方向照常收发。
AURORA_TEST_CASE(shutdown_read_satisfies_queued_reads_with_zero) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket shutdown semantics");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    ReadLog queued;
    std::vector<char> inbox(64);
    pair.client.read(std::span<char>(inbox), read_into(queued));
    AURORA_TEST_CHECK_EQ(queued.calls.load(), 0);  // 无数据时不该提前送达

    auto stopped = pair.client.shutdown(aurora::net::ShutdownMode::Read);
    AURORA_TEST_REQUIRE(stopped.ok());
    AURORA_TEST_CHECK_EQ(queued.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(queued.failed);
    AURORA_TEST_CHECK_EQ(queued.bytes.load(), 0U);

    ReadLog server_read;
    pair.server.read(std::span<char>(inbox), read_into(server_read));
    const std::string_view after_shutdown = "after-shutdown-read";
    send_payload(registry, pair.client, after_shutdown);
    AURORA_TEST_CHECK(pump_until(registry, [&server_read]() { return server_read.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(server_read.bytes.load(), after_shutdown.size());
#endif
}

// 背压（拉模型）：对端不取时待发字节越过高水位即被 net-buffer-limit 拒，且待发量至多超一枚请求；
// 对端开始取之后写侧重新被接受。
AURORA_TEST_CASE(write_backpressure_rejects_then_recovers) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket buffers");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    pair.client.set_high_water_mark(AURORA_CHUNK);
    const std::vector<char> filler(AURORA_CHUNK, 'a');
    int rejections = 0;
    // 服务端一只字都不取：内核缓冲填满后本层开始攒待发字节，攒到高水位即拒。轮数给足——
    // 回环缓冲深度随平台与自动调参而异，判据是「一定会撞限」而不是「第几轮撞限」。
    for (int attempt = 0; attempt < 4096 && rejections == 0; ++attempt) {
        WriteLog log;
        pair.client.write(std::span<const char>(filler), write_into(log));
        if (log.failed) {
            AURORA_TEST_CHECK_EQ(log.code, "net-buffer-limit");
            ++rejections;
        }
        AURORA_TEST_CHECK_LE(pair.client.pending_write_bytes(), 2 * AURORA_CHUNK);
        pump_once(registry);
    }
    AURORA_TEST_CHECK_GT(rejections, 0);

    int accepted_again = 0;
    for (int attempt = 0; attempt < 512 && accepted_again == 0; ++attempt) {
        ReadLog drain;
        std::vector<char> inbox(std::size_t{64} * 1024);
        pair.server.read(std::span<char>(inbox), read_into(drain));
        WriteLog log;
        pair.client.write(std::span<const char>(filler), write_into(log));
        if (!log.failed) {
            ++accepted_again;
        }
        AURORA_TEST_CHECK_FALSE(log.failed && log.code != "net-buffer-limit");
        AURORA_TEST_CHECK_LE(pair.client.pending_write_bytes(), 2 * AURORA_CHUNK);
        pump_once(registry);
    }
    AURORA_TEST_CHECK_GT(accepted_again, 0);
#endif
}

// 暂停读：字节留在内核、本层不消费；恢复后原样送达（与停读的单向关闭相对）。
AURORA_TEST_CASE(pause_read_holds_bytes_until_resume) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket buffers");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    pair.client.pause_read();
    AURORA_TEST_CHECK_TRUE(pair.client.read_paused());

    ReadLog paused;
    std::vector<char> inbox(64);
    pair.client.read(std::span<char>(inbox), read_into(paused));
    send_payload(registry, pair.server, "held");
    pump_once(registry);
    pump_once(registry);
    AURORA_TEST_CHECK_EQ(paused.calls.load(), 0);  // 暂停期间不消费内核字节

    pair.client.resume_read();
    AURORA_TEST_CHECK_FALSE(pair.client.read_paused());
    AURORA_TEST_CHECK(pump_until(registry, [&paused]() { return paused.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(paused.failed);
    AURORA_TEST_CHECK_EQ(paused.bytes.load(), 4U);
    AURORA_TEST_CHECK_EQ(std::string(inbox.data(), paused.bytes.load()), "held");
#endif
}

// 取消挂起的读：恰一次以 net-cancelled 抵达并关闭本端连接；重复 cancel() 不产生第二次送达。
AURORA_TEST_CASE(cancel_resumes_pending_read_exactly_once) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to cancel");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    auto token = pair.client.cancellation();
    AURORA_TEST_CHECK_TRUE(token.shares_state_with(pair.client.cancellation()));

    ReadLog hung;
    std::vector<char> inbox(64);
    pair.client.read(std::span<char>(inbox), read_into(hung));
    pump_once(registry);
    AURORA_TEST_CHECK_EQ(hung.calls.load(), 0);

    token.cancel();
    AURORA_TEST_CHECK_EQ(hung.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(hung.failed);
    AURORA_TEST_CHECK_EQ(hung.code, "net-cancelled");
    AURORA_TEST_CHECK_FALSE(pair.client.is_open());

    token.cancel();
    AURORA_TEST_CHECK_EQ(hung.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(pair.server.is_open());  // 对端对象自身的状态不由本端取消决定
#endif
}

// 参数与空态契约：空缓冲读是用法错误（0 字节只留给 EOF）、空写入不产生系统调用、空态句柄不崩不悬挂。
AURORA_TEST_CASE(argument_and_empty_handle_contracts) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket to exercise the contracts");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_host_pair(registry);

    ReadLog empty_read;
    std::vector<char> nothing;
    pair.client.read(std::span<char>(nothing), read_into(empty_read));
    AURORA_TEST_CHECK_EQ(empty_read.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(empty_read.failed);
    AURORA_TEST_CHECK_EQ(empty_read.code, "net-buffer-limit");

    WriteLog empty_write;
    const std::vector<char> no_bytes;
    pair.client.write(std::span<const char>(no_bytes), write_into(empty_write));
    AURORA_TEST_CHECK_EQ(empty_write.calls.load(), 1);
    AURORA_TEST_CHECK_FALSE(empty_write.failed);
    AURORA_TEST_CHECK_EQ(empty_write.bytes.load(), 0U);

    StreamTransport orphan;
    AURORA_TEST_CHECK_FALSE(orphan.is_open());
    AURORA_TEST_CHECK_EQ(orphan.native_handle(), -1);
    AURORA_TEST_CHECK_FALSE(orphan.is_loopback_peer());
    AURORA_TEST_CHECK_EQ(orphan.pending_write_bytes(), 0U);
    AURORA_TEST_CHECK_FALSE(orphan.read_paused());
    auto orphan_token = orphan.cancellation();
    orphan_token.cancel();  // 空态令牌的 cancel 不产生任何效果，也不得崩溃

    ReadLog orphan_read;
    std::vector<char> inbox(8);
    orphan.read(std::span<char>(inbox), read_into(orphan_read));
    AURORA_TEST_CHECK_EQ(orphan_read.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(orphan_read.failed);
    WriteLog orphan_write;
    orphan.write(std::span<const char>("x", 1), write_into(orphan_write));
    AURORA_TEST_CHECK_EQ(orphan_write.calls.load(), 1);
    AURORA_TEST_CHECK_TRUE(orphan_write.failed);
    auto orphan_shutdown = orphan.shutdown(aurora::net::ShutdownMode::Both);
    AURORA_TEST_CHECK_FALSE(orphan_shutdown.ok());

    StreamListener dead_listener;
    AURORA_TEST_CHECK_FALSE(dead_listener.is_valid());
    AURORA_TEST_CHECK_EQ(dead_listener.local_port(), std::uint16_t{0});
    std::atomic<int> orphan_accept{0};
    dead_listener.accept([&orphan_accept](auto result) {
        AURORA_TEST_CHECK(!result);
        ++orphan_accept;
    });
    AURORA_TEST_CHECK_EQ(orphan_accept.load(), 1);
#endif
}

// 在途连接被取消：恰一次以 net-cancelled 抵达，且事件源登记随结果落地一并释放（不留悬挂等待项）。
AURORA_TEST_CASE(cancelling_in_flight_connect_releases_registration) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime cannot register an event source");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    // 绑一只「从不 accept」的监听口：握手会在内核 backlog 里完成，但宿主驱动形态下没人 dispatch，
    // 连接因此稳定地停在「已发起、未出结果」这一态——那正是本用例要考察的窗口。
    auto bound = StreamListener::bind(Endpoint::loopback(0), 4, host_options());
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());
    const std::size_t baseline = registry.live_count();

    Cancellation token;
    std::atomic<int> calls{0};
    std::string code;
    StreamTransport::connect(Endpoint::loopback(listener.local_port()), host_options(), token, [&](auto result) {
        if (!result) {
            code = std::string{result.error().code};
        }
        ++calls;
    });
    AURORA_TEST_CHECK_EQ(registry.live_count(), baseline + 1);  // 在途连接确实占着一枚登记

    token.cancel();
    AURORA_TEST_CHECK_EQ(calls.load(), 1);
    AURORA_TEST_CHECK_EQ(code, "net-cancelled");
    token.cancel();
    AURORA_TEST_CHECK_EQ(calls.load(), 1);
    AURORA_TEST_CHECK_EQ(registry.live_count(), baseline);

    // 已取消的令牌再发起连接：登记钩子时就地执行，连接根本不建立，仍恰一次出结果。
    std::atomic<int> late_calls{0};
    StreamTransport::connect(Endpoint::loopback(listener.local_port()), host_options(), token,
                             [&](auto result) {
                                 if (!result) {
                                     code = std::string{result.error().code};
                                 }
                                 ++late_calls;
                             });
    AURORA_TEST_CHECK_EQ(late_calls.load(), 1);
    AURORA_TEST_CHECK_EQ(code, "net-cancelled");
    AURORA_TEST_CHECK_EQ(registry.live_count(), baseline);
#endif
}

// 关闭监听口：尚未交付的 accept 回调恰一次收到取消结果，不留悬挂回调；close 幂等。
AURORA_TEST_CASE(closing_listener_completes_pending_accept_once) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime cannot bind a listener");
#else
    HostDriven host;
    auto bound = StreamListener::bind(Endpoint::loopback(0), 4, host_options());
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());
    AURORA_TEST_CHECK_TRUE(listener.is_valid());
    AURORA_TEST_CHECK_NE(listener.native_handle(), -1);

    std::atomic<int> calls{0};
    std::string code;
    listener.accept([&](auto result) {
        if (!result) {
            code = std::string{result.error().code};
        }
        ++calls;
    });
    listener.close();
    AURORA_TEST_CHECK_EQ(calls.load(), 1);
    AURORA_TEST_CHECK_EQ(code, "net-cancelled");
    AURORA_TEST_CHECK_FALSE(listener.is_valid());
    AURORA_TEST_CHECK_EQ(listener.native_handle(), -1);
    listener.close();
    AURORA_TEST_CHECK_EQ(calls.load(), 1);
#endif
}

// 引擎驱动形态（本库各平台腿的统一形态）：用例线程不做任何 dispatch，结果照样送达；不可达目标也
// 恰一次出结果；空回调不构成任何承诺，不建句柄也不登记事件源。
AURORA_TEST_CASE(engine_driven_delivery_without_host_pump) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no engine thread to drive delivery");
#else
    EngineGuard guard;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    registry.set_host_driven(false);
    AURORA_TEST_CHECK_EQ(aurora::net::Engine::instance().name(), "poll");

    auto bound = StreamListener::bind(Endpoint::loopback(0), 4, StreamOptions{});
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());

    std::atomic<int> accepted{0};
    StreamTransport server;
    listener.accept([&](auto result) {
        if (result) {
            server = std::move(result.value());
        }
        ++accepted;
    });

    std::atomic<int> connected{0};
    StreamTransport client;
    StreamTransport::connect(Endpoint::loopback(listener.local_port()), StreamOptions{}, Cancellation{},
                             [&](auto result) {
                                 if (result) {
                                     client = std::move(result.value());
                                 }
                                 ++connected;
                             });
    AURORA_TEST_CHECK(wait_until([&]() { return accepted.load() == 1 && connected.load() == 1; }));
    AURORA_TEST_REQUIRE(client.is_open());
    AURORA_TEST_REQUIRE(server.is_open());

    ReadLog server_read;
    std::vector<char> inbox(64);
    server.read(std::span<char>(inbox), read_into(server_read));
    WriteLog client_write;
    client.write(std::span<const char>("engine", 6), write_into(client_write));
    AURORA_TEST_CHECK(wait_until([&]() { return server_read.calls.load() == 1 && client_write.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(server_read.failed);
    AURORA_TEST_CHECK_EQ(server_read.bytes.load(), 6U);

    // 文档保留段地址（TEST-NET）作为「大概率无人应答」的目标：出结果的方式随网络形态而异
    // （定时器到点、或被路由立即拒），但**必须**恰一次出结果——永不出结果的回调才是缺陷。
    StreamOptions impatient;
    impatient.connect_timeout = std::chrono::milliseconds{200};
    auto blackhole = Endpoint::from_ip("192.0.2.1", 80);
    AURORA_TEST_REQUIRE(blackhole.ok());
    std::atomic<int> blackhole_calls{0};
    std::string blackhole_code;
    StreamTransport::connect(blackhole.value(), impatient, Cancellation{}, [&](auto result) {
        if (!result) {
            blackhole_code = std::string{result.error().code};
        }
        ++blackhole_calls;
    });
    AURORA_TEST_CHECK(wait_until([&]() { return blackhole_calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(blackhole_calls.load(), 1);
    if (!blackhole_code.empty()) {
        AURORA_TEST_CHECK_TRUE(blackhole_code == "net-connect-timeout" || blackhole_code == "net-connect-failed");
    }

    const std::size_t before = registry.live_count();
    StreamTransport::connect(Endpoint::loopback(listener.local_port()), StreamOptions{}, Cancellation{}, {});
    AURORA_TEST_CHECK_EQ(registry.live_count(), before);
#endif
}

}  // namespace aurora::test_cases::utest_net_transport

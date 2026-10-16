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
///           本机 IPC 形态走**同一条状态机**：路径端点可双向收发，地址族专属观测口各给确定答案
///           （`peer_endpoint` 落 `net-option-conflict`、`peer_credentials` 按本腿内核是否登记身份回答、
///           `local_port` 恒 0）；`StreamOptions::native` 在系统调用之前生效，TCP 专属选项落在本机 IPC 上
///           是确定的冲突而不是静默忽略，尺寸或方向矛盾的选项同样在发起调用前被拒。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 fd 等待语义，用例体退化为
///    AURORA_TEST_SKIP。
/// ⚠️ 对端身份的判据要和内核登记的那一份对得上，故 POSIX 腿直接取本进程的 uid/gid/pid（`<unistd.h>`
///    排在 `aurora/core/platform.h` 之后——那段宏正是本判据的前提，先判后定义会恒假）；这是测试侧的
///    观测口，不属于 net 层的平台代码（socket 头的唯一落点仍是 `src/aurora/net/socket.cpp`）。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/isolation.h"
#include "framework/net_wait.h"

#ifndef AURORA_PLATFORM_WINDOWS
#include <unistd.h>
#endif

namespace aurora::test_cases::utest_net_transport {
namespace {

using aurora::net::Cancellation;
using aurora::net::Endpoint;
using aurora::net::StreamListener;
using aurora::net::StreamOptions;
using aurora::net::StreamTransport;
using aurora::test::net::EngineGuard;
using aurora::test::net::HostDriven;
using aurora::test::net::pump_once;

/// @brief 背压用例的单次写入长度（同时用作写高水位，便于把「overshoot 至多一枚」写成断言）。
constexpr std::size_t AURORA_CHUNK{4096};

/// @brief 本文件两次等待的主题名：超时文本据此指明「是宿主泵没等到还是引擎线程没送到」。
constexpr std::string_view AURORA_PUMP_SUBJECT = "net-transport-host-pump";
/// @brief 引擎驱动形态的等待主题名。
constexpr std::string_view AURORA_ENGINE_SUBJECT = "net-transport-engine-driven";

/// @brief 宿主驱动形态下的连接口径：本层定时器落在引擎线程，会让「送达时刻」不可同线程断言。
/// @return 连接超时置 0（即「不设时限，由对端决定何时出结果」）的选项。
auto host_options() -> StreamOptions {
    StreamOptions options;
    options.connect_timeout = std::chrono::milliseconds{0};
    return options;
}

/// @brief 轮询宿主等待直到谓词成立或预算耗尽（被测工作在回调里发生，同线程无法同步断言）。
///
/// 泵、窗口与预算全部取自 `framework/net_wait.h`；本文件保留这一枚转发器只为维持调用点形态，
/// 谓词成立与否仍由调用方的 REQUIRE / CHECK 定性（部分调用点是条件式的，转发器里报告会造成假红）。
/// @param registry 进程级事件源注册口（共用泵自取同一单例，形参保留以维持调用点形态）。
/// @param pred 目标条件。
/// @return 预算内成立为 true。
auto pump_until(aurora::app::EventSourceRegistry &registry, const std::function<bool()> &pred) -> bool {
    (void)registry;  // NOLINT(bugprone-unused-parameter) 形参为维持调用点形态而留
    return aurora::test::net::drive_until(AURORA_PUMP_SUBJECT, pred).satisfied;
}

/// @brief 有界轮询等待谓词成立（引擎驱动形态：本线程完全不碰 `dispatch()`）。
/// @param pred 目标条件。
/// @return 预算内成立为 true。
auto wait_until(const std::function<bool()> &pred) -> bool {
    return aurora::test::wait_until(AURORA_ENGINE_SUBJECT, pred).satisfied;
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
        // 完成回调可能由 `~StreamListener` 在析构边界内派发，致命断言抛出的 CaseAbort 穿过该边界即
        // `std::terminate`；这里只记录失败，前提是否成立由下面的 `pump_until` 断言负责转红。
        AURORA_TEST_CHECK(result.ok());
        if (!result) {
            return;
        }
        pair.server = std::move(result.value());
        ++accepted;
    });

    std::atomic<int> connected{0};
    StreamTransport::connect(Endpoint::loopback(pair.listener.local_port()), host_options(), Cancellation{},
                             [&pair, &connected](auto result) {
                                 // 口径同上：connect 的失败派发同样可能落在析构路径内。
                                 AURORA_TEST_CHECK(result.ok());
                                 if (!result) {
                                     return;
                                 }
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

/// @brief 本机 IPC 形态是否可用（该位由运行期探测给出；探测不过的用例整体跳过而不是转红）。
/// @return 本腿能开出 AF_UNIX 流式套接字为 true。
auto local_ipc_available() -> bool { return aurora::net::Engine::instance().capabilities().local_ipc; }

/// @brief 本用例临时目录内的一个本机 IPC 名字。
///
/// 刻意取最短的文件名：路径总长受本腿 `sockaddr_un::sun_path` 宽度约束，而临时目录名本身已带
/// 时间戳与序号，长名会让用例在长仓库路径的机器上无缘无故地变红。
/// @param name 目录内的名字。
/// @return 平台原生分隔符的完整路径；临时目录不可用时为空串。
auto local_ipc_name(std::string_view name) -> std::string {
    const auto &dir = aurora::testing::isolation::temp_dir();
    if (dir.empty()) {
        return {};
    }
    return (std::filesystem::path{dir} / std::filesystem::path{name}).string();
}

/// @brief 本机 IPC 形态下连通的一对端点（形态同 `HostPair`，只是端点换成路径）。
struct LocalPair {
    StreamListener listener;
    StreamTransport client;
    StreamTransport server;
};

/// @brief 绑定本机 IPC 名字、发起连接并交付服务端流，得到连通的一对。
/// @param registry 进程级事件源注册口（须已由 `HostDriven` 置为宿主驱动）。
/// @param name 本用例临时目录内的路径。
/// @return 连通的一对；造不出前提即 REQUIRE 转红。
auto make_local_pair(aurora::app::EventSourceRegistry &registry, const std::string &name) -> LocalPair {
    const auto target = Endpoint::local(name);
    AURORA_TEST_REQUIRE(target.ok());
    auto bound = StreamListener::bind(target.value(), 4, host_options());
    AURORA_TEST_REQUIRE(bound.ok());
    LocalPair pair{.listener = std::move(bound.value())};

    std::atomic<int> accepted{0};
    pair.listener.accept([&pair, &accepted](auto result) {
        // 口径同 `make_host_pair`：完成回调可能落在析构边界内，致命断言在那里会直接 terminate。
        AURORA_TEST_CHECK(result.ok());
        if (!result) {
            return;
        }
        pair.server = std::move(result.value());
        ++accepted;
    });

    std::atomic<int> connected{0};
    StreamTransport::connect(target.value(), host_options(), Cancellation{}, [&pair, &connected](auto result) {
        AURORA_TEST_CHECK(result.ok());
        if (!result) {
            return;
        }
        pair.client = std::move(result.value());
        ++connected;
    });

    AURORA_TEST_REQUIRE(pump_until(registry, [&]() { return accepted.load() == 1 && connected.load() == 1; }));
    AURORA_TEST_REQUIRE(pair.client.is_open());
    AURORA_TEST_REQUIRE(pair.server.is_open());
    return pair;
}

/// @brief 绑定一个监听口并回报结果码（成功为空串；失败时局部句柄随即关闭）。
/// @param endpoint 监听端点。
/// @param options 待验证的原生选项集合。
/// @return 结果码文本。
auto bind_code(const Endpoint &endpoint, const StreamOptions &options) -> std::string {
    auto bound = StreamListener::bind(endpoint, 4, options);
    if (bound) {
        return {};
    }
    return std::string{bound.error().code};
}

/// @brief 发起一次连接并等它出结果，回报结果码（成功为空串）。
/// @param registry 进程级事件源注册口。
/// @param endpoint 目标端点。
/// @param options 待验证的原生选项集合。
/// @return 结果码文本。
auto connect_code(aurora::app::EventSourceRegistry &registry, const Endpoint &endpoint, const StreamOptions &options)
    -> std::string {
    std::atomic<int> calls{0};
    std::string code;
    StreamTransport::connect(endpoint, options, Cancellation{}, [&](auto result) {
        if (!result) {
            code = std::string{result.error().code};
        }
        ++calls;
    });
    AURORA_TEST_CHECK(pump_until(registry, [&]() { return calls.load() >= 1; }));
    AURORA_TEST_CHECK_EQ(calls.load(), 1);
    return code;
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
    // 写/读的完成回调经 Tasks 派发：发送缓冲满时要等下一次可写就绪，回调与在途读缓冲都会活过
    // 当轮循环体。故观测对象与读缓冲统一直布在用例作用域、逐轮重置——放进循环体即栈悬垂
    // （在途回调写进已销毁的日志），逐轮重置保持「每轮一座标」的断言语义不变。
    WriteLog log;
    int rejections = 0;
    // 服务端一只字都不取：内核缓冲填满后本层开始攒待发字节，攒到高水位即拒。轮数给足——
    // 回环缓冲深度随平台与自动调参而异，判据是「一定会撞限」而不是「第几轮撞限」。
    for (int attempt = 0; attempt < 4096 && rejections == 0; ++attempt) {
        log.calls.store(0);
        log.bytes.store(0);
        log.failed = false;
        log.code.clear();
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
    ReadLog drain;
    std::vector<char> inbox(std::size_t{64} * 1024);
    for (int attempt = 0; attempt < 512 && accepted_again == 0; ++attempt) {
        drain.calls.store(0);
        drain.bytes.store(0);
        drain.failed = false;
        drain.code.clear();
        log.calls.store(0);
        log.bytes.store(0);
        log.failed = false;
        log.code.clear();
        pair.server.read(std::span<char>(inbox), read_into(drain));
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
    StreamTransport::connect(Endpoint::loopback(listener.local_port()), host_options(), token, [&](auto result) {
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

// 本机 IPC 与 TCP 共用同一条状态机：路径端点可双向收发，地址族专属的观测口各给一个确定答案。
// 端口在这里本就没有对应物，所以 `local_port()` 回报 0 而不是伪造一个；可移植地址形态同理落
// `net-option-conflict`（「查询失败」会把用法不匹配说成网络故障）。
AURORA_TEST_CASE(local_ipc_stream_round_trip_and_family_specific_answers) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no local IPC socket; the transport layer cannot bind");
#else
    if (!local_ipc_available()) {
        AURORA_TEST_SKIP("this platform leg provides no AF_UNIX socket at runtime");
    }
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    const auto name = local_ipc_name("s");
    AURORA_TEST_REQUIRE_FALSE(name.empty());
    auto pair = make_local_pair(registry, name);

    AURORA_TEST_CHECK_EQ(pair.listener.local_port(), std::uint16_t{0});
    AURORA_TEST_CHECK_NE(pair.client.native_handle(), -1);
    const auto peer = pair.client.peer_endpoint();
    AURORA_TEST_REQUIRE_FALSE(peer.ok());
    AURORA_TEST_CHECK_EQ(peer.error().code, "net-option-conflict");
    // 回环判据只回答「IP 地址是否落在回环段」；本机 IPC 的连接本就只可能在同机发生，问身份用
    // `peer_credentials()`。
    AURORA_TEST_CHECK_FALSE(pair.client.is_loopback_peer());

    ReadLog server_read;
    std::vector<char> inbox(64);
    pair.server.read(std::span<char>(inbox), read_into(server_read));
    send_payload(registry, pair.client, "local-ping");
    AURORA_TEST_CHECK(pump_until(registry, [&server_read]() { return server_read.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(server_read.failed);
    AURORA_TEST_CHECK_EQ(std::string(inbox.data(), server_read.bytes.load()), "local-ping");

    ReadLog client_read;
    std::vector<char> echo(64);
    pair.client.read(std::span<char>(echo), read_into(client_read));
    send_payload(registry, pair.server, "local-pong");
    AURORA_TEST_CHECK(pump_until(registry, [&client_read]() { return client_read.calls.load() == 1; }));
    AURORA_TEST_CHECK_EQ(std::string(echo.data(), client_read.bytes.load()), "local-pong");
#endif
}

// 对端身份回答的是「谁连上了我」：登记于连接建立那一刻，只有内核确实登记过的腿才给数值，其余腿以
// `anonymous` 如实回报而不是伪造全零身份。IP 连接上问身份是形态不匹配，落 `net-option-conflict`——
// 静默回一份空身份会把「以为做了身份校验」的授权判断变成空转。
AURORA_TEST_CASE(local_ipc_peer_credentials_answer_only_where_the_kernel_registers_them) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no local IPC socket to read an identity from");
#else
    if (!local_ipc_available()) {
        AURORA_TEST_SKIP("this platform leg provides no AF_UNIX socket at runtime");
    }
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    auto pair = make_local_pair(registry, local_ipc_name("p"));

    const auto credentials = pair.server.peer_credentials();
    AURORA_TEST_REQUIRE(credentials.ok());
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
    // 两端同在本进程内，故内核登记的那一份必然等于本进程的标识。
    AURORA_TEST_CHECK_FALSE(credentials.value().anonymous);
    AURORA_TEST_CHECK_EQ(credentials.value().user_id, static_cast<std::uint32_t>(::getuid()));
    AURORA_TEST_CHECK_EQ(credentials.value().group_id, static_cast<std::uint32_t>(::getgid()));
    AURORA_TEST_CHECK_EQ(credentials.value().process_id, static_cast<std::uint32_t>(::getpid()));
#else
    AURORA_TEST_CHECK_TRUE(credentials.value().anonymous);
#endif

    auto tcp_pair = make_host_pair(registry);
    const auto mismatched = tcp_pair.client.peer_credentials();
    AURORA_TEST_REQUIRE_FALSE(mismatched.ok());
    AURORA_TEST_CHECK_EQ(mismatched.error().code, "net-option-conflict");
#endif
}

// 原生选项在发起系统调用之前生效：合法集合在 TCP 上被接受并让连接照常收发（含接受侧对新句柄的重放
// ——内核的选项继承表两腿不同，`TCP_NODELAY` 在 Linux 上不随 accept 传下来）。
AURORA_TEST_CASE(native_options_take_effect_before_the_syscall) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket options to set");
#else
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();

    StreamOptions listener_options;
    listener_options.native.reuse_address = true;
    listener_options.native.receive_buffer_bytes = 64 * 1024;
    listener_options.native.send_buffer_bytes = 64 * 1024;
    listener_options.native.linger = std::chrono::milliseconds{50};
    const auto bound = Endpoint::loopback(0);
    AURORA_TEST_CHECK_EQ(bind_code(bound, listener_options), "");

    StreamOptions client_options = host_options();
    client_options.native.no_delay = true;
    client_options.native.keep_alive_enabled = true;
    client_options.native.keep_alive_idle = std::chrono::seconds{1};
    client_options.native.keep_alive_interval = std::chrono::milliseconds{500};
    client_options.native.source = Endpoint::loopback(0);  // 源地址只在未绑定的客户端句柄上有意义
#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
    client_options.native.keep_alive_probes = 3;
#endif
    // 「只验证选项被接受」的连接另开一只监听口：`connect_code` 拿到的流随即析构，握手却留在下一只
    // 监听口的 backlog 里，混用同一只会让后面的 accept 交付那条已销毁的连接。
    auto probe = StreamListener::bind(Endpoint::loopback(0), 4, host_options());
    AURORA_TEST_REQUIRE(probe.ok());
    auto probe_listener = std::move(probe.value());
    AURORA_TEST_CHECK_EQ(connect_code(registry, Endpoint::loopback(probe_listener.local_port()), client_options), "");

    auto live = StreamListener::bind(Endpoint::loopback(0), 4, listener_options);
    AURORA_TEST_REQUIRE(live.ok());
    auto listener = std::move(live.value());

    // 接受侧带着监听口的选项集合交付，连接本身仍要能用（重放失败会被 accept 直接拒掉）。
    std::atomic<int> delivered{0};
    StreamTransport server;
    StreamTransport client;
    listener.accept([&](auto result) {
        if (result) {
            server = std::move(result.value());
        }
        ++delivered;
    });
    StreamTransport::connect(Endpoint::loopback(listener.local_port()), client_options, Cancellation{},
                             [&](auto result) {
                                 if (result) {
                                     client = std::move(result.value());
                                 }
                                 ++delivered;
                             });
    AURORA_TEST_CHECK(pump_until(registry, [&]() { return delivered.load() == 2; }));
    AURORA_TEST_REQUIRE(client.is_open());
    AURORA_TEST_REQUIRE(server.is_open());
    ReadLog log;
    std::vector<char> inbox(8);
    server.read(std::span<char>(inbox), read_into(log));
    send_payload(registry, client, "opt");
    AURORA_TEST_CHECK(pump_until(registry, [&log]() { return log.calls.load() == 1; }));
    AURORA_TEST_CHECK_FALSE(log.failed);
    AURORA_TEST_CHECK_EQ(std::string(inbox.data(), log.bytes.load()), "opt");
#endif
}

// TCP 专属选项落在本机 IPC 上是确定的冲突，不是静默忽略：后者会让「以为开了低延迟」的调用方一直
// 等到第一次现场卡顿才发现配置没生效。缓冲窗口类选项在两族都有对应物，必须照常接受。
AURORA_TEST_CASE(tcp_only_native_options_conflict_on_local_ipc) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has neither local IPC nor socket options");
#else
    if (!local_ipc_available()) {
        AURORA_TEST_SKIP("this platform leg provides no AF_UNIX socket at runtime");
    }
    HostDriven host;
    auto &registry = aurora::app::EventSourceRegistry::instance();
    const auto target = Endpoint::local(local_ipc_name("c"));
    AURORA_TEST_REQUIRE(target.ok());

    StreamOptions nudge;
    nudge.native.no_delay = true;
    AURORA_TEST_CHECK_EQ(bind_code(target.value(), nudge), "net-option-conflict");

    StreamOptions keepalive;
    keepalive.connect_timeout = std::chrono::milliseconds{0};
    keepalive.native.keep_alive_enabled = true;
    AURORA_TEST_CHECK_EQ(connect_code(registry, target.value(), keepalive), "net-option-conflict");

    StreamOptions window;
    window.native.receive_buffer_bytes = 8192;
    window.native.send_buffer_bytes = 8192;
    AURORA_TEST_CHECK_EQ(bind_code(target.value(), window), "");
#endif
}

// 尺寸或方向矛盾的选项在发起系统调用前就被拒：0 字节的缓冲窗口、负值的 linger、「显式关保活却设
// 探活参数」这三类都没有确定含义，交给内核等于让同一份配置在两条腿上有两种结果。
AURORA_TEST_CASE(contradictory_native_options_are_refused_before_the_syscall) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no socket options to contradict");
#else
    const auto bound = Endpoint::loopback(0);

    StreamOptions zero_window;
    zero_window.native.receive_buffer_bytes = 0;
    AURORA_TEST_CHECK_EQ(bind_code(bound, zero_window), "net-option-conflict");

    StreamOptions negative_window;
    negative_window.native.send_buffer_bytes = -1;
    AURORA_TEST_CHECK_EQ(bind_code(bound, negative_window), "net-option-conflict");

    StreamOptions negative_linger;
    negative_linger.native.linger = std::chrono::milliseconds{-1};
    AURORA_TEST_CHECK_EQ(bind_code(bound, negative_linger), "net-option-conflict");

    StreamOptions tuning_while_disabled;
    tuning_while_disabled.native.keep_alive_enabled = false;
    tuning_while_disabled.native.keep_alive_idle = std::chrono::seconds{1};
    AURORA_TEST_CHECK_EQ(bind_code(bound, tuning_while_disabled), "net-option-conflict");

    // 源地址需要一个尚未绑定的客户端句柄：监听口自己就是被绑定的那一侧。
    StreamOptions source_on_listener;
    source_on_listener.native.source = Endpoint::loopback(0);
    AURORA_TEST_CHECK_EQ(bind_code(bound, source_on_listener), "net-option-conflict");

#if defined(AURORA_PLATFORM_LINUX) || defined(AURORA_PLATFORM_ANDROID)
    StreamOptions zero_probes;
    zero_probes.native.keep_alive_enabled = true;
    zero_probes.native.keep_alive_probes = 0;
    AURORA_TEST_CHECK_EQ(bind_code(bound, zero_probes), "net-option-conflict");
#else
    // 本腿没有对应的每连接旋钮：要么如实回报设置失败（Windows 的重试上限是全局的），要么整档不支持
    // （其余腿）。两者都不是「静默按别的答案执行」。
    StreamOptions probes;
    probes.native.keep_alive_enabled = true;
    probes.native.keep_alive_probes = 3;
    probes.native.keep_alive_idle = std::chrono::seconds{1};
    const auto code = bind_code(bound, probes);
    AURORA_TEST_CHECK_TRUE(code.empty() || code == "net-option-failed");
#endif
#endif
}

// 路径长度分两档校验：值类型只按最宽一条腿的容量把关，装配 `sockaddr` 时再按本腿实际宽度判一次。
// 108 字节的文本正好落在两档之间——它进得了值类型，却装不进任何一条腿的 `sun_path`（要留结尾 NUL）。
AURORA_TEST_CASE(local_ipc_path_limits_are_enforced_at_the_value_type) {
    AURORA_TEST_CHECK_FALSE(Endpoint::local(std::string_view{}).ok());
    AURORA_TEST_CHECK_FALSE(Endpoint::local(std::string(109, 'p')).ok());
    const auto fits_value_type = Endpoint::local(std::string(108, 'p'));
    AURORA_TEST_REQUIRE(fits_value_type.ok());
    AURORA_TEST_CHECK_TRUE(fits_value_type.value().is_local_path());
}

// 值类型放行但本腿装不下的路径，必须在发起系统调用之前落 `net-endpoint-invalid`；名字已被占用（上一次
// 崩溃留下的 socket 文件，或同名的普通文件）时 bind 落 `net-bind-failed`——两条腿在这两件事上给出同一
// 件事实，本层不自动删掉别人留下的命名键（并发启动的两个进程之间「谁有权删」无解）。
AURORA_TEST_CASE(local_ipc_leg_capacity_and_stale_name_are_reported_by_bind) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no local IPC name to bind");
#else
    if (!local_ipc_available()) {
        AURORA_TEST_SKIP("this platform leg provides no AF_UNIX socket at runtime");
    }
    const auto too_long_for_this_leg = Endpoint::local(std::string(108, 'p'));
    AURORA_TEST_REQUIRE(too_long_for_this_leg.ok());
    AURORA_TEST_CHECK_EQ(bind_code(too_long_for_this_leg.value(), StreamOptions{}), "net-endpoint-invalid");

    const auto stale = local_ipc_name("stale");
    AURORA_TEST_REQUIRE_FALSE(stale.empty());
    const auto occupied = Endpoint::local(stale);
    AURORA_TEST_REQUIRE(occupied.ok());
    std::ofstream keeper{std::filesystem::path{stale}};
    AURORA_TEST_REQUIRE(keeper.is_open());
    keeper << "x";
    keeper.close();
    auto bound = StreamListener::bind(occupied.value(), 4, StreamOptions{});
    AURORA_TEST_REQUIRE_FALSE(bound.ok());
    AURORA_TEST_CHECK_EQ(bound.error().code, "net-bind-failed");
#endif
}

}  // namespace aurora::test_cases::utest_net_transport

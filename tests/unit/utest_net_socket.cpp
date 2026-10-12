/// 测试类型: unit
/// 目标单元: include/aurora/net/socket.h
/// 测试说明: aurora::net 跨平台 socket 操作层（CHG-006）行为契约回归：临时端口监听与
///           `port()` 系统分配值回报、`wait_readable` 纯超时（false 非错误）与超时后到达、
///           connect→accept 往返 + `write_all`/`read_some` 字节完整（1MiB+7 压分块边界）、
///           对端关闭以 read 返回 0 表达（与错误态严格区分）、连接失败落 `net-connect-failed`、
///           收发超时落独立码 `net-recv-timeout`（与 `net-recv-failed` 以 code 区分，不依赖措辞；
///           并核对「毫秒意图」真实生效——历史坑：Windows 上 5 秒被读成 5 毫秒）、
///           `is_loopback_peer` 判定、`Session` 嵌套引用计数、监听口重复 bind 落 `net-bind-failed`。
///           全程同线程即可（TCP 握手由内核 backlog 完成，无需 accept 线程）；端口一律 0 =
///           系统分配，只连 127.0.0.1。平台差异都被本层吸收，断言两侧应同色。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 BSD socket 语义，用例体退化
///    为 AURORA_TEST_SKIP。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/socket.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_socket {
namespace {

/// @brief 等待类用例的统一轮询窗口（足够覆盖内核回环往返、又不至于拖慢失败路径）。
constexpr std::chrono::milliseconds AURORA_NET_POLL_TIMEOUT{80};

/// @brief 在临时端口起一个回环监听口；建立失败即 REQUIRE 转红（用例的环境前提，不是被测行为）。
/// @param session 已就绪的运行时会话（Windows 侧 WSAStartup 归属）。
/// @return 就绪的 Listener（`port()` 为非 0 系统分配值）。
auto make_listener(aurora::net::Session &session) -> aurora::net::Listener {
    AURORA_TEST_REQUIRE(session.ok());
    auto bound = aurora::net::Listener::listen_loopback(0, 4);
    AURORA_TEST_REQUIRE(bound.ok());
    auto listener = std::move(bound.value());
    AURORA_TEST_REQUIRE_NE(listener.port(), std::uint16_t{0});
    return listener;
}

}  // namespace

// 监听口绑定临时端口后须回报系统分配的实际端口（0 入参 → 非 0 出参）；移动转移所有权后
// 句柄/端口随迁，close 幂等并转入无效态，默认构造占位恒为「无效 + 端口 0」。
AURORA_TEST_CASE(listen_reports_assigned_port) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    aurora::net::Session session;
    auto listener = make_listener(session);
    AURORA_TEST_CHECK_TRUE(listener.is_valid());
    const std::uint16_t bound_port = listener.port();

    auto moved = std::move(listener);
    AURORA_TEST_CHECK_TRUE(moved.is_valid());
    AURORA_TEST_CHECK_EQ(moved.port(), bound_port);
    AURORA_TEST_CHECK_FALSE(listener.is_valid());  // NOLINT(bugprone-use-after-move, clang-analyzer-cplusplus.Move): 移动后仅断言其无效态，即为被测语义

    moved.close();
    moved.close();  // 幂等
    AURORA_TEST_CHECK_FALSE(moved.is_valid());

    aurora::net::Listener placeholder;
    AURORA_TEST_CHECK_FALSE(placeholder.is_valid());
    AURORA_TEST_CHECK_EQ(placeholder.port(), std::uint16_t{0});
#endif
}

// 无连接挂起时 wait_readable 是「纯超时 = OK(false)」而非错误；连接一到即翻真并可被 accept
// 取出（accept 出的对端句柄有效且经 is_loopback_peer 复核）。
AURORA_TEST_CASE(wait_readable_polls_then_accepts) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    aurora::net::Session session;
    auto listener = make_listener(session);

    const auto idle = listener.wait_readable(AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(idle.ok());
    AURORA_TEST_CHECK_FALSE(idle.value());  // 纯超时不是错误

    auto client = aurora::net::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());

    const auto ready = listener.wait_readable(AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(ready.ok());
    AURORA_TEST_CHECK_TRUE(ready.value());

    auto peer = listener.accept();
    AURORA_TEST_REQUIRE(peer.ok());
    AURORA_TEST_CHECK_TRUE(peer.value().is_valid());
    AURORA_TEST_CHECK_TRUE(peer.value().is_loopback_peer());

    // 无效句柄的查询面：默认构造 StreamSocket 既非回环对端也不报错路径乱入。
    aurora::net::StreamSocket invalid;
    AURORA_TEST_CHECK_FALSE(invalid.is_loopback_peer());
#endif
}

// write_all/read_some 字节完整往返：1MiB+7 刻意越过 ≤1MiB 分块边界（末轮剩 7 字节），
// 双向都走一遍并校验内容逐字节一致——分块循环里的任何指针推进/窄化错误都会在此转红。
AURORA_TEST_CASE(write_all_read_some_roundtrip_exact_bytes) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    constexpr std::size_t payload_size = (1U << 20U) + 7U;
    std::string payload(payload_size, '\0');
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<char>(1 + (i % 250));
    }

    aurora::net::Session session;
    auto listener = make_listener(session);
    auto client = aurora::net::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());
    auto peer = listener.accept();
    AURORA_TEST_REQUIRE(peer.ok());

    AURORA_TEST_CHECK_TRUE(client.value().write_all(std::span<const char>(payload)).ok());

    std::string received;
    std::vector<char> buf(std::size_t{64} * 1024U);
    while (received.size() < payload_size) {
        const auto got = peer.value().read_some(std::span<char>(buf));
        AURORA_TEST_REQUIRE(got.ok());
        AURORA_TEST_REQUIRE_NE(got.value(), std::size_t{0});
        received.append(buf.data(), got.value());
    }
    AURORA_TEST_CHECK_EQ(received.size(), payload_size);
    AURORA_TEST_CHECK_TRUE(received == payload);

    // 反向 echo：服务端写回、客户端读回同长度（两方向共用同一分块实现，须同色）。
    AURORA_TEST_CHECK_TRUE(peer.value().write_all(std::span<const char>(payload)).ok());
    std::size_t echoed = 0;
    while (echoed < payload_size) {
        const auto got = client.value().read_some(std::span<char>(buf));
        AURORA_TEST_REQUIRE(got.ok());
        AURORA_TEST_REQUIRE_NE(got.value(), std::size_t{0});
        echoed += got.value();
    }
    AURORA_TEST_CHECK_EQ(echoed, payload_size);

    // 空 span 边界：write_all 空 = OK、read_some 空 = 0，均不触碰系统调用。
    AURORA_TEST_CHECK_TRUE(client.value().write_all(std::span<const char>{}).ok());
    const auto empty_read = client.value().read_some(std::span<char>{});
    AURORA_TEST_CHECK_TRUE(empty_read.ok());
    AURORA_TEST_CHECK_EQ(empty_read.value(), std::size_t{0});
#endif
}

// 对端有序关闭必须以「read_some 返回 0」表达，与错误态（Result 失败）严格区分——
// 这是全部调用方把「0 字节」当作「连接结束」而非「出错了」的判据来源。
AURORA_TEST_CASE(peer_close_surfaces_as_zero_byte_read) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    constexpr char k_greeting[] = "hi";
    constexpr std::size_t greeting_size = sizeof(k_greeting) - 1;

    aurora::net::Session session;
    auto listener = make_listener(session);
    auto client = aurora::net::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());

    {
        auto peer = listener.accept();
        AURORA_TEST_REQUIRE(peer.ok());
        AURORA_TEST_CHECK_TRUE(peer.value().write_all(std::span<const char>(k_greeting, greeting_size)).ok());
    }  // peer 出作用域析构 → 关闭 → 客户端读向收到 FIN

    char buf[16];
    const auto head = client.value().read_some(std::span<char>(buf, greeting_size));
    AURORA_TEST_REQUIRE(head.ok());
    AURORA_TEST_CHECK_EQ(head.value(), greeting_size);

    const auto closed = client.value().read_some(std::span<char>(buf, sizeof(buf)));
    AURORA_TEST_REQUIRE(closed.ok());  // 是 OK 态……
    AURORA_TEST_CHECK_EQ(closed.value(), std::size_t{0});  // ……且 0 = 对端关闭，而非错误
#endif
}

// 连一个已关闭的端口应失败且落 `net-connect-failed`（message 配 OS 原生码）。
// 与 utest_inspector_client 的 WSL2 备注同理：刚关闭的临时端口可能被 NAT 快速复用而 connect
// 成功——那属于「端口此刻确实有人监听」，不是本层语义；故成功分支不强红，只在失败时钉死码。
AURORA_TEST_CASE(connect_failure_reports_net_connect_failed) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    aurora::net::Session session;
    std::uint16_t dead_port = 0;
    {
        auto listener = make_listener(session);
        dead_port = listener.port();
    }  // listener 出作用域析构 → shutdown+close

    const auto attempt = aurora::net::StreamSocket::connect_loopback(dead_port);
    if (!attempt.ok()) {
        AURORA_TEST_CHECK_EQ(attempt.error().code_enum, ErrorCode::NetConnectFailed);
        AURORA_TEST_CHECK_EQ(attempt.error().code, "net-connect-failed");
        AURORA_TEST_CHECK_NE(attempt.error().message.find("os_error="), std::string::npos);
    } else {
        AURORA_TEST_CHECK_TRUE(attempt.value().is_valid());  // 端口被复用：成功也必须是可用句柄
    }
#endif
}

// 收发超时是独立错误码 `net-recv-timeout`，不得与 `net-recv-failed` 混同（以 code 区分、
// 不依赖措辞）。同时钉住「毫秒意图真实生效」：本层入参是 std::chrono::milliseconds，
// 历史坑是 Winsock 上 5 秒被按 timeval 字节形态读成 5 毫秒——这里断言等待确实花了
// ≥ timeout/2（若意图被缩三个数量级，elapsed 会远小于该下界）。
AURORA_TEST_CASE(io_timeout_reports_distinct_code) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    constexpr auto k_timeout = std::chrono::milliseconds{250};

    aurora::net::Session session;
    auto listener = make_listener(session);
    auto client = aurora::net::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());

    AURORA_TEST_REQUIRE(client.value().set_io_timeout(k_timeout).ok());

    char buf[16];
    const auto started = std::chrono::steady_clock::now();
    const auto timed_out = client.value().read_some(std::span<char>(buf, sizeof(buf)));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    AURORA_TEST_REQUIRE(!timed_out.ok());
    AURORA_TEST_CHECK_EQ(timed_out.error().code_enum, ErrorCode::NetRecvTimeout);
    AURORA_TEST_CHECK_NE(timed_out.error().code_enum, ErrorCode::NetRecvFailed);
    AURORA_TEST_CHECK_GE(elapsed, k_timeout / 2);  // 意图按毫秒生效（下界防「缩小数量级」回归）
#endif
}

// Session 引用计数：嵌套/并存会话全部 ok；内层析构后外层仍可用（Windows 侧即
// 「计数未归零不得提前 WSACleanup」——析构后仍能成功建监听口即是行为学证明；POSIX 恒 ok 同样成立）。
AURORA_TEST_CASE(nested_sessions_stay_ok) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    aurora::net::Session outer;
    AURORA_TEST_CHECK_TRUE(outer.ok());
    {
        aurora::net::Session inner;
        AURORA_TEST_CHECK_TRUE(inner.ok());
        AURORA_TEST_CHECK_TRUE(outer.ok());
    }  // inner 析构：引用计数尚未归零
    AURORA_TEST_CHECK_TRUE(outer.ok());
    const auto listener = aurora::net::Listener::listen_loopback(0, 2);
    AURORA_TEST_CHECK_TRUE(listener.ok());
#endif
}

// 同一端口二次 bind 应落 `net-bind-failed`（回环监听口的独占性）。127.0.0.1 上活跃监听口
// 存在时 Linux/Windows 均回 EADDRINUSE；若个别环境（快速复用窗口）竟成功，则只核对其句柄有效。
AURORA_TEST_CASE(double_bind_reports_net_bind_failed) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    aurora::net::Session session;
    auto first = make_listener(session);
    const auto again = aurora::net::Listener::listen_loopback(first.port(), 2);
    if (!again.ok()) {
        AURORA_TEST_CHECK_EQ(again.error().code_enum, ErrorCode::NetBindFailed);
        AURORA_TEST_CHECK_NE(again.error().message.find("os_error="), std::string::npos);
    } else {
        AURORA_TEST_CHECK_TRUE(again.value().is_valid());
    }
#endif
}

}  // namespace aurora::test_cases::utest_net_socket

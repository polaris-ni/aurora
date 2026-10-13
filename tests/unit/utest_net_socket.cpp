/// 测试类型: unit
/// 目标单元: include/aurora/net/blocking.h
/// 测试说明: aurora::net::blocking 跨平台 socket 操作层（CHG-006 建立，CHG-010 §4 将其降为
///           仅供工具与测试宿主使用的同步阻塞面）行为契约回归：临时端口监听与
///           `port()` 系统分配值回报、`wait_readable` 纯超时（false 非错误）与超时后到达、
///           connect→accept 往返 + `write_all`/`read_some` 字节完整（1MiB+7 压分块边界）、
///           对端关闭以 read 返回 0 表达（与错误态严格区分）、连接失败落 `net-connect-failed`、
///           收发超时落独立码 `net-recv-timeout`（与 `net-recv-failed` 以 code 区分，不依赖措辞；
///           并核对「毫秒意图」真实生效——历史坑：Windows 上 5 秒被读成 5 毫秒）、
///           `is_loopback_peer` 判定、`detail::Session` 嵌套引用计数、监听口重复 bind 落
///           `net-bind-failed`、迁移期别名 `aurora::net::{StreamSocket,Listener}` 与新类型
///           同体（§20 时间表：别名只做转发，不改语义）、
///           **地址族无关面**（`connect(Endpoint)` / `bind(Endpoint, backlog)` / `peer_endpoint()`：
///           不合用的端点在发起系统调用前拒、IPv6 作用域号只认十进制、`[::1]` 往返与回环判定、
///           双栈监听口上的 `::ffff:127.0.0.1` 内嵌形态仍认回环；无 IPv6 或 v6-only 主机按环境缺失
///           跳过）。
///           全程同线程即可（TCP 握手由内核 backlog 完成，无需 accept 线程）；唯一例外是
///           1MiB+7 往返用例的读侧住独立线程——Windows 回环无读者时仅缓冲约 320KiB，
///           单线程先写后读必死锁（Linux 缓冲自动调大无此限制）。端口一律 0 =
///           系统分配，只连 127.0.0.1。平台差异都被本层吸收，断言两侧应同色。
///           运行时由实现侧随句柄钉住，用例不再构造任何「会话」前置件。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE` 声明（registry_integrity 静态扫源码，
///    被 #ifdef 摘掉的声明在另一种构建里必然单边失踪）；wasm 无 BSD socket 语义，用例体退化
///    为 AURORA_TEST_SKIP。

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/blocking.h"
#include "aurora/net/detail/runtime.h"
#include "aurora/net/socket.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_socket {
namespace {

/// @brief 等待类用例的统一轮询窗口（足够覆盖内核回环往返、又不至于拖慢失败路径）。
constexpr std::chrono::milliseconds AURORA_NET_POLL_TIMEOUT{80};

/// @brief 在临时端口起一个回环监听口；建立失败即 REQUIRE 转红（用例的环境前提，不是被测行为）。
/// @return 就绪的 Listener（`port()` 为非 0 系统分配值）。
auto make_listener() -> aurora::net::blocking::Listener {
    auto bound = aurora::net::blocking::Listener::listen_loopback(0, 4);
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
    auto listener = make_listener();
    AURORA_TEST_CHECK_TRUE(listener.is_valid());
    const std::uint16_t bound_port = listener.port();

    auto moved = std::move(listener);
    AURORA_TEST_CHECK_TRUE(moved.is_valid());
    AURORA_TEST_CHECK_EQ(moved.port(), bound_port);
    AURORA_TEST_CHECK_FALSE(listener.is_valid());  // NOLINT(bugprone-use-after-move, clang-analyzer-cplusplus.Move):
                                                   // 移动后仅断言其无效态，即为被测语义

    moved.close();
    moved.close();  // 幂等
    AURORA_TEST_CHECK_FALSE(moved.is_valid());

    aurora::net::blocking::Listener placeholder;
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
    auto listener = make_listener();

    const auto idle = listener.wait_readable(AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(idle.ok());
    AURORA_TEST_CHECK_FALSE(idle.value());  // 纯超时不是错误

    auto client = aurora::net::blocking::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());

    const auto ready = listener.wait_readable(AURORA_NET_POLL_TIMEOUT);
    AURORA_TEST_REQUIRE(ready.ok());
    AURORA_TEST_CHECK_TRUE(ready.value());

    auto peer = listener.accept();
    AURORA_TEST_REQUIRE(peer.ok());
    AURORA_TEST_CHECK_TRUE(peer.value().is_valid());
    AURORA_TEST_CHECK_TRUE(peer.value().is_loopback_peer());

    // 无效句柄的查询面：默认构造 StreamSocket 既非回环对端也不报错路径乱入。
    aurora::net::blocking::StreamSocket invalid;
    AURORA_TEST_CHECK_FALSE(invalid.is_loopback_peer());
#endif
}

// write_all/read_some 字节完整往返：1MiB+7 刻意越过 ≤1MiB 分块边界（末轮剩 7 字节），
// 双向都走一遍并校验内容逐字节一致——分块循环里的任何指针推进/窄化错误都会在此转红。
// ⚠️ 读侧必须住在独立线程：实测 Windows 回环内核在无读者时只吞约 320KiB（非阻塞
// `send` 进 WSAEWOULDBLOCK 前的可入队量）即停滞，远小于 1MiB+7 载荷；单线程「先写后读」
// 会让阻塞写永远等一个尚未开始的读者而死锁（Linux 回环缓冲自动调大可吞整个载荷，
// 故该死锁只在 Windows 腿显形）。两个方向各起一个读线程、写完即 join：同一时刻
// 无任何套接字被两个线程共用（读线程只读、写只在 join 之后），不破 `StreamSocket`
// 单所有权约定。线程内不做断言（框架计数器非线程安全），健康态经出参带回主线程核账；
// 收发都挂超时，读者夭折时写方以超时失败转红而非把 join 挂死。
AURORA_TEST_CASE(write_all_read_some_roundtrip_exact_bytes) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    AURORA_TEST_REQUIRE_THREADS();

    constexpr std::size_t payload_size = (1U << 20U) + 7U;
    // 失败路径有界：读者夭折后写方不得无限阻塞（否则 join 把用例挂死到 runner 超时）。
    constexpr auto transfer_timeout = std::chrono::seconds{10};
    std::string payload(payload_size, '\0');
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<char>(1 + (i % 250));
    }

    auto listener = make_listener();
    auto client = aurora::net::blocking::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());
    auto peer = listener.accept();
    AURORA_TEST_REQUIRE(peer.ok());
    AURORA_TEST_REQUIRE(client.value().set_io_timeout(transfer_timeout).ok());
    AURORA_TEST_REQUIRE(peer.value().set_io_timeout(transfer_timeout).ok());

    // 读满即止；读出错或对端提前关闭（0 字节）经 healthy 报告，由主线程断言。
    auto drain_into = [](const aurora::net::blocking::StreamSocket &from, std::string &out, bool &healthy) {
        std::vector<char> buf(std::size_t{64} * 1024U);
        while (out.size() < payload_size) {
            const auto got = from.read_some(std::span<char>(buf));
            if (!got.ok() || got.value() == std::size_t{0}) {
                healthy = false;
                return;
            }
            out.append(buf.data(), got.value());
        }
    };

    std::string received;
    bool healthy = true;
    std::thread reader(drain_into, std::ref(peer.value()), std::ref(received), std::ref(healthy));
    AURORA_TEST_CHECK_TRUE(client.value().write_all(std::span<const char>(payload)).ok());
    reader.join();
    AURORA_TEST_CHECK_TRUE(healthy);
    AURORA_TEST_CHECK_EQ(received.size(), payload_size);
    AURORA_TEST_CHECK_TRUE(received == payload);

    // 反向 echo：服务端写回、客户端读回同长度（两方向共用同一分块实现，须同色）。
    std::string echoed;
    bool echo_healthy = true;
    std::thread echo_reader(drain_into, std::ref(client.value()), std::ref(echoed), std::ref(echo_healthy));
    AURORA_TEST_CHECK_TRUE(peer.value().write_all(std::span<const char>(payload)).ok());
    echo_reader.join();
    AURORA_TEST_CHECK_TRUE(echo_healthy);
    AURORA_TEST_CHECK_EQ(echoed.size(), payload_size);
    AURORA_TEST_CHECK_TRUE(echoed == payload);

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

    auto listener = make_listener();
    auto client = aurora::net::blocking::StreamSocket::connect_loopback(listener.port());
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
    // 运行时随句柄创建自动钉住（Windows 侧 WSAStartup 归属实现侧），用例不再需要维持任何
    // 「会话活过 connect」的前置条件——历史上这条约束存在时，会话先于连接析构会让 socket()
    // 落 WSANOTINITIALISED（net-socket-failed），把被测的 connect 失败路径整个掩盖掉。
    std::uint16_t dead_port = 0;
    {
        auto listener = make_listener();
        dead_port = listener.port();
    }  // listener 出作用域析构 → shutdown+close

    const auto attempt = aurora::net::blocking::StreamSocket::connect_loopback(dead_port);
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

    auto listener = make_listener();
    auto client = aurora::net::blocking::StreamSocket::connect_loopback(listener.port());
    AURORA_TEST_REQUIRE(client.ok());

    AURORA_TEST_REQUIRE(client.value().set_io_timeout(k_timeout).ok());

    char buf[16];
    const auto started = std::chrono::steady_clock::now();
    const auto timed_out = client.value().read_some(std::span<char>(buf, sizeof(buf)));
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    AURORA_TEST_REQUIRE(!timed_out.ok());
    AURORA_TEST_CHECK_EQ(timed_out.error().code_enum, ErrorCode::NetRecvTimeout);
    AURORA_TEST_CHECK_NE(timed_out.error().code_enum, ErrorCode::NetRecvFailed);
    AURORA_TEST_CHECK_GE(elapsed, k_timeout / 2);  // 意图按毫秒生效（下界防「缩小数量级」回归）
#endif
}

// 运行时引用计数（库内细节 `net::detail::Session`，公共面已不再暴露）：嵌套/并存会话全部 ok；
// 内层析构后外层仍可用（Windows 侧即「计数未归零不得提前 WSACleanup」——析构后仍能成功建监听口
// 即是行为学证明；POSIX 恒 ok 同样成立）。
AURORA_TEST_CASE(nested_sessions_stay_ok) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    aurora::net::detail::Session outer;
    AURORA_TEST_CHECK_TRUE(outer.ok());
    {
        aurora::net::detail::Session inner;
        AURORA_TEST_CHECK_TRUE(inner.ok());
        AURORA_TEST_CHECK_TRUE(outer.ok());
    }  // inner 析构：引用计数尚未归零
    AURORA_TEST_CHECK_TRUE(outer.ok());
    const auto listener = aurora::net::blocking::Listener::listen_loopback(0, 2);
    AURORA_TEST_CHECK_TRUE(listener.ok());
#endif
}

// 同一端口二次 bind 应落 `net-bind-failed`（回环监听口的独占性）。127.0.0.1 上活跃监听口
// 存在时 Linux/Windows 均回 EADDRINUSE；若个别环境（快速复用窗口）竟成功，则只核对其句柄有效。
AURORA_TEST_CASE(double_bind_reports_net_bind_failed) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    auto first = make_listener();
    const auto again = aurora::net::blocking::Listener::listen_loopback(first.port(), 2);
    if (!again.ok()) {
        AURORA_TEST_CHECK_EQ(again.error().code_enum, ErrorCode::NetBindFailed);
        AURORA_TEST_CHECK_NE(again.error().message.find("os_error="), std::string::npos);
    } else {
        AURORA_TEST_CHECK_TRUE(again.value().is_valid());
    }
#endif
}

// 不合用的端点在发起系统调用前就被拒：端口 0（connect 的目的地必须确定）、本机 IPC 形态
// （不是 TCP 地址）、默认构造占位（无地址）三条都落 `net-endpoint-invalid`，不得退化成
// 「connect 失败」这种把用法错误报成网络错误的形态。
AURORA_TEST_CASE(connect_rejects_unusable_endpoints) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    const auto zero_port = aurora::net::blocking::StreamSocket::connect(aurora::net::Endpoint::loopback(0));
    AURORA_TEST_REQUIRE_FALSE(bool(zero_port));
    AURORA_TEST_CHECK_EQ(zero_port.error().code_enum, ErrorCode::NetEndpointInvalid);

    // 路径文本只是「本机 IPC 族」的载体，本用例不创建也不写任何文件（connect 在装配地址前就拒）。
    auto local = aurora::net::Endpoint::local("test_temp/aurora-net-probe.sock");
    AURORA_TEST_REQUIRE(bool(local));
    const auto ipc = aurora::net::blocking::StreamSocket::connect(local.value());
    AURORA_TEST_REQUIRE_FALSE(bool(ipc));
    AURORA_TEST_CHECK_EQ(ipc.error().code_enum, ErrorCode::NetEndpointInvalid);

    const aurora::net::Endpoint placeholder;
    const auto invalid = aurora::net::blocking::StreamSocket::connect(placeholder);
    AURORA_TEST_REQUIRE_FALSE(bool(invalid));
    AURORA_TEST_CHECK_EQ(invalid.error().code_enum, ErrorCode::NetEndpointInvalid);
#endif
}

// IPv6 作用域号口径：数字 zone 被接受并进入系统调用（失败也只能是网络层的失败），接口名
// zone 在两平台上都直接拒——POSIX 能查表而 Winsock 需额外依赖，容忍单边可用等于让同一输入
// 有两种结果。
AURORA_TEST_CASE(ipv6_zone_scope_rules) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    auto named = aurora::net::Endpoint::from_ip("fe80::1%eth0", 80);
    AURORA_TEST_REQUIRE(bool(named));
    const auto rejected = aurora::net::blocking::StreamSocket::connect(named.value());
    AURORA_TEST_REQUIRE_FALSE(bool(rejected));
    AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::NetEndpointInvalid);

    auto numeric = aurora::net::Endpoint::from_ip("fe80::1%1", 80);
    AURORA_TEST_REQUIRE(bool(numeric));
    const auto attempted = aurora::net::blocking::StreamSocket::connect(numeric.value());
    if (!attempted.ok()) {
        // 只核「已通过端点校验」：链路上的不可达因机器而异，不在本用例的判据里。
        AURORA_TEST_CHECK_NE(attempted.error().code_enum, ErrorCode::NetEndpointInvalid);
    }
#endif
}

// IPv6 回环往返：`bind([::1]:0)` → `connect([::1]:port)` → 双向字节完整，且两侧 `peer_endpoint()`
// 都回报 `::1`、`is_loopback_peer()` 均为真（回环判定不再只看 127/8）。主机无 IPv6 时按
// 环境缺失跳过，不假绿也不误红。
AURORA_TEST_CASE(ipv6_loopback_roundtrip) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    auto bound = aurora::net::blocking::Listener::bind(aurora::net::Endpoint::loopback_v6(0), 4);
    if (!bound.ok()) {
        AURORA_TEST_SKIP("no IPv6 loopback on this host: bind([::1]:0) was refused");
    }
    auto listener = std::move(bound.value());
    AURORA_TEST_REQUIRE_NE(listener.port(), std::uint16_t{0});

    auto dialed = aurora::net::blocking::StreamSocket::connect(aurora::net::Endpoint::loopback_v6(listener.port()));
    AURORA_TEST_REQUIRE(bool(dialed));
    auto client = std::move(dialed.value());

    const std::string_view payload = "dual-stack";
    AURORA_TEST_REQUIRE(client.write_all(payload).ok());
    auto peer = listener.accept();
    AURORA_TEST_REQUIRE(bool(peer));
    auto server = std::move(peer.value());

    char buffer[16]{};
    const auto read = server.read_some(std::span<char>(buffer, payload.size()));
    AURORA_TEST_REQUIRE(bool(read));
    AURORA_TEST_CHECK_EQ(read.value(), payload.size());
    AURORA_TEST_CHECK_EQ(std::string_view(buffer, payload.size()), payload);
    AURORA_TEST_CHECK_TRUE(server.is_loopback_peer());
    AURORA_TEST_CHECK_TRUE(client.is_loopback_peer());

    auto client_peer = client.peer_endpoint();
    AURORA_TEST_REQUIRE(bool(client_peer));
    AURORA_TEST_CHECK_EQ(client_peer.value().family(), aurora::net::AddressFamily::IPv6);
    AURORA_TEST_CHECK_EQ(client_peer.value().port(), listener.port());
    AURORA_TEST_CHECK(client_peer.value().is_loopback());
#endif
}

// 双栈监听口上的 IPv4 对端：绑 `::` 后由 127.0.0.1 接入，`getpeername` 可能回报 IPv4 内嵌形态
// （`::ffff:127.0.0.1`），回环判定须照样为真——这是本层从「只看 127/8」升级来的第二条判据。
// 主机把 IPv6 套接字设成 v6-only 时该通路不存在，按环境缺失跳过。
AURORA_TEST_CASE(dual_stack_listener_still_recognizes_loopback) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    auto bound = aurora::net::blocking::Listener::bind(aurora::net::Endpoint::any(true, 0), 4);
    if (!bound.ok()) {
        AURORA_TEST_SKIP("cannot bind the IPv6 wildcard address on this host");
    }
    auto listener = std::move(bound.value());
    auto dialed = aurora::net::blocking::StreamSocket::connect_loopback(listener.port());
    if (!dialed.ok()) {
        AURORA_TEST_SKIP("wildcard listener is IPv6-only here, so 127.0.0.1 cannot reach it");
    }
    auto client = std::move(dialed.value());
    auto peer = listener.accept();
    AURORA_TEST_REQUIRE(bool(peer));
    auto server = std::move(peer.value());

    AURORA_TEST_CHECK_TRUE(server.is_loopback_peer());
    auto endpoint = server.peer_endpoint();
    AURORA_TEST_REQUIRE(bool(endpoint));
    AURORA_TEST_CHECK_TRUE(endpoint.value().is_loopback());
    // 两族之一：内嵌形态按 IPv6 回报，独立 v4 通路按 IPv4 回报，二者都算认出来了。
    const auto family = endpoint.value().family();
    AURORA_TEST_CHECK_TRUE(family == aurora::net::AddressFamily::IPv4 || family == aurora::net::AddressFamily::IPv6);
#endif
}

// 迁移期别名：`aurora::net::{StreamSocket,Listener}` 必须与 `blocking` 同名类型是同一个类型，
// 否则就是「旧别名还在、新面已改语义」的中间态。别名带弃用标记，故引用它的这段整体落进
// 告警抑制窗口；别名在下一版移除时本用例一并删除。
AURORA_TEST_CASE(deprecated_aliases_resolve_to_blocking_types) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
#ifdef AURORA_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    static_assert(std::is_same_v<aurora::net::StreamSocket, aurora::net::blocking::StreamSocket>);
    static_assert(std::is_same_v<aurora::net::Listener, aurora::net::blocking::Listener>);
    AURORA_TEST_CHECK_TRUE((std::is_same_v<aurora::net::StreamSocket, aurora::net::blocking::StreamSocket>));
    AURORA_TEST_CHECK_TRUE((std::is_same_v<aurora::net::Listener, aurora::net::blocking::Listener>));

    // 同义不只在类型上：经旧名建监听口，拿到的是同一套行为（有效句柄 + 非 0 分配端口）。
    const auto bound = aurora::net::Listener::listen_loopback(0, 2);
    AURORA_TEST_REQUIRE(bound.ok());
    AURORA_TEST_CHECK_TRUE(bound.value().is_valid());
    AURORA_TEST_CHECK_NE(bound.value().port(), std::uint16_t{0});
#ifdef AURORA_COMPILER_MSVC
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
#endif
}

}  // namespace aurora::test_cases::utest_net_socket

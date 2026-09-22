/// 测试类型: unit
/// 目标单元: tools/servers/inspector_client.h
/// 测试说明: 覆盖最小 Inspector HTTP 客户端的「发不出手之前」拒单口径（非回环主机 / 端口 0 /
/// target 未以 '/' 开头）与**收发超时的平台形态**。后者是本文件的回归位：Winsock 的
/// `SO_RCVTIMEO` / `SO_SNDTIMEO` 收的是 `DWORD` 毫秒数，POSIX 收的是 `struct timeval`，
/// 把 `timeval` 递给 Winsock 它只把前 4 字节当毫秒读——于是「5 秒」在 Windows 上实际生效成
/// 「5 毫秒」，慢于 5ms 的正常响应被 `WSAETIMEDOUT` 掐成 `empty response`。用例因此起一个
/// 「60ms 后才回数据」的回环监听口，断言客户端照样拿到 200（60ms ≫ 5ms、≪ 5s，两种形态在此分岔）。
/// 端口一律 0（系统分配临时端口，无冲突）；只连 127.0.0.1。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE`：registry_integrity 是「静态扫源码
/// 的用例名字面量」与「runner --list」逐条比对，被 `#ifdef` 摘掉的声明在另一种构建里必然单边
/// 失踪。故 wasm 只让用例体退化为 AURORA_TEST_SKIP，声明恒可见（与 utest_x11_surface 同一手法）。

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

#ifndef AURORA_PLATFORM_WASM

#include "inspector_client.h"  // tools/servers（该目录经 AuroraTests.cmake 加入 runner 的 include 路径）

#ifdef AURORA_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief Winsock 会话（引用计数式启停，随作用域清理）。
struct WinsockSession {
    WinsockSession() {
        WSADATA data{};
        started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (started) {
            ::WSACleanup();
        }
    }
    // 纯作用域 guard，禁止拷贝/移动（避免重复 WSACleanup）。
    WinsockSession(const WinsockSession &) = delete;
    auto operator=(const WinsockSession &) -> WinsockSession & = delete;
    WinsockSession(WinsockSession &&) = delete;
    auto operator=(WinsockSession &&) -> WinsockSession & = delete;
    bool started = false;
};
#endif  // AURORA_PLATFORM_WINDOWS

#ifdef AURORA_PLATFORM_WINDOWS
using SocketHandle = SOCKET;
inline constexpr SocketHandle AURORA_INVALID_SOCKET = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle AURORA_INVALID_SOCKET = -1;
#endif

/// @brief 关闭监听/连接句柄（两侧平台拼写不同）。
auto close_handle(SocketHandle handle) -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

/// @brief 打开一个只绑回环、端口由系统分配的监听口，并把实际端口写回 `port_out`。
[[nodiscard]] auto open_loopback_listener(std::uint16_t &port_out) -> SocketHandle {
    const SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == AURORA_INVALID_SOCKET) {
        return AURORA_INVALID_SOCKET;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 临时端口：避免与并发用例或本机服务撞号
#ifdef AURORA_PLATFORM_WINDOWS
    int name_len = static_cast<int>(sizeof(addr));
#else
    socklen_t name_len = sizeof(addr);
#endif
    // bind / getsockname 的 socket API 契约要求把 `sockaddr_in` 擦除为通用 `sockaddr` 指针，
    // 无类型安全替代（协议族字段即运行期判别依据）。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const int bound = ::bind(listener, reinterpret_cast<sockaddr *>(&addr), static_cast<socklen_t>(sizeof(addr)));
    if (bound != 0 || ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &name_len) != 0) {
        close_handle(listener);
        return AURORA_INVALID_SOCKET;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    port_out = ntohs(addr.sin_port);
    return listener;
}

/// @brief 服务端：至多等 5s 连接 → 睡 `delay` → 回一段最小 200 → 关闭。
///
/// 有界等待是刻意要求：客户端若在连接前就失败（本机 socket 资源不足等），本线程必须能自己退出，
/// 否则 `join()` 会把整个用例挂死到 runner 超时。
auto serve_after(SocketHandle listener, std::chrono::milliseconds delay) -> void {
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok";

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval wait{};
    wait.tv_sec = 5;
#ifdef AURORA_PLATFORM_WINDOWS
    // Windows 的 `select` 忽略 `nfds`：句柄不是描述符索引，传 `listener + 1` 反而可能越出 fd_set。
    const int ready = ::select(0, &readable, nullptr, nullptr, &wait);
#else
    const int ready = ::select(static_cast<int>(listener) + 1, &readable, nullptr, nullptr, &wait);
#endif
    if (ready <= 0) {
        return;
    }
    const SocketHandle peer = ::accept(listener, nullptr, nullptr);
    if (peer == AURORA_INVALID_SOCKET) {
        return;
    }
    std::this_thread::sleep_for(delay);
    // 一次发完：socket API 只收裸指针 + 长度，无更安全替代。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    ::send(peer, reinterpret_cast<const char *>(response.data()), static_cast<int>(response.size()), 0);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    close_handle(peer);
}

}  // namespace

#endif  // AURORA_PLATFORM_WASM

namespace aurora::test_cases::utest_inspector_client {

AURORA_TEST_CASE(refuses_before_touching_a_socket) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("浏览器运行时无 BSD socket 语义，本客户端不参与 wasm 构建");
#else
    // 三种入口校验都在建连之前拒单，`error` 非空即未发出任何字节。
    const auto non_loopback = aurora::tools::inspector::http_request("GET", "inspector.example", 6280, "/api/tree");
    AURORA_TEST_CHECK_FALSE(non_loopback.ok());
    AURORA_TEST_CHECK_NE(non_loopback.error.find("non-loopback"), std::string::npos);

    const auto zero_port = aurora::tools::inspector::http_request("GET", "127.0.0.1", 0, "/api/tree");
    AURORA_TEST_CHECK_FALSE(zero_port.ok());
    AURORA_TEST_CHECK_NE(zero_port.error.find("port"), std::string::npos);

    const auto bad_target = aurora::tools::inspector::http_request("GET", "127.0.0.1", 6280, "api/tree");
    AURORA_TEST_CHECK_FALSE(bad_target.ok());
    AURORA_TEST_CHECK_NE(bad_target.error.find("target"), std::string::npos);

    // 回环主机名三种写法都接受（拒单口径只认「是否本机」，刻意不做 DNS）。
    AURORA_TEST_CHECK_TRUE(aurora::tools::inspector::is_loopback_host("127.0.0.1"));
    AURORA_TEST_CHECK_TRUE(aurora::tools::inspector::is_loopback_host("localhost"));
    AURORA_TEST_CHECK_TRUE(aurora::tools::inspector::is_loopback_host("::1"));
    AURORA_TEST_CHECK_FALSE(aurora::tools::inspector::is_loopback_host("0.0.0.0"));
#endif
}

AURORA_TEST_CASE(io_timeout_survives_a_slow_response) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("浏览器运行时无 BSD socket 语义，本客户端不参与 wasm 构建");
#else
    AURORA_TEST_REQUIRE_THREADS();
#ifdef AURORA_PLATFORM_WINDOWS
    const WinsockSession wsa;
    AURORA_TEST_REQUIRE(wsa.started);
#endif

    std::uint16_t port = 0;
    const SocketHandle listener = open_loopback_listener(port);
    AURORA_TEST_REQUIRE(listener != AURORA_INVALID_SOCKET);
    AURORA_TEST_REQUIRE_NE(port, std::uint16_t{0});

    // 60ms：远大于「被误当毫秒读的 5 秒」里真正生效那 5ms，又远小于本客户端的 5s 超时。
    auto server = std::thread([listener] { serve_after(listener, std::chrono::milliseconds{60}); });
    const auto response = aurora::tools::inspector::http_request("GET", "127.0.0.1", port, "/api/tree");
    server.join();
    close_handle(listener);

    // 断言一律用 CHECK 而非 REQUIRE：失败也要走完上面的 join/关闭，不把挂死或句柄泄漏留给后续用例。
    AURORA_TEST_CHECK_TRUE(response.ok());
    AURORA_TEST_CHECK_EQ(response.status, 200);
    AURORA_TEST_CHECK_EQ(response.body, std::string("ok"));
#endif
}

}  // namespace aurora::test_cases::utest_inspector_client

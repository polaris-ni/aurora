#pragma once

// Aurora Inspector 的最小 HTTP 客户端（**仅 `tools/` 层使用**，供 `aurora_mcp` 连接正在运行的应用）。
//
// 设计约束：
//   - **只连 loopback**。这是「把运行中的 UI 交给 AI 操作」的边界，绝不允许连到非本机；
//     因此不做 DNS、不接受主机名，只认 127.0.0.1 / localhost / ::1，且一律连到 127.0.0.1。
//   - 零第三方依赖：Windows 用 Winsock2，POSIX 用 BSD socket。
//   - 核心库零感知：本文件不进 `include/`、不进 `src/`，不改变核心的「零依赖」承诺。
//   - 只读到连接关闭为止（`InspectorServer` 固定回 `Connection: close`），并设 4MiB 上限。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
// `WIN32_LEAN_AND_MEAN` 是 Windows SDK 约定的宏名，改名即失效（它由 `windows.h` 一侧按名探测）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming) SDK 规定名，不可按命名表改
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace aurora::tools::inspector {

/// @brief 一次 HTTP 的响应。
struct HttpResponse {
    int status = 0;  ///< HTTP 状态码（0 = 未收到响应）
    std::string body;  ///< 响应体
    std::string error;  ///< 传输层错误说明；空表示成功

    [[nodiscard]] auto ok() const -> bool { return error.empty(); }
};

/// @brief 默认端口：与 `InspectorServer::start()` 的默认值一致。
inline constexpr std::uint16_t AURORA_DEFAULT_PORT = 6280;

/// @brief 环境变量：覆盖默认端口（`BUILD_OPTIONS.md` §5）。
inline constexpr std::string_view AURORA_PORT_ENV = "AURORA_INSPECTOR_PORT";

/// @brief 是否为允许连接的回环主机名。刻意**不做 DNS 解析** —— 解析会把「localhost」之外的
///        名字也变得可用，而本客户端的信任模型只允许本机。
[[nodiscard]] inline auto is_loopback_host(std::string_view host) -> bool {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

namespace detail {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle AURORA_INVALID_SOCKET = INVALID_SOCKET;
inline auto close_socket(SocketHandle s) -> void { closesocket(s); }
#else
using SocketHandle = int;
inline constexpr SocketHandle AURORA_INVALID_SOCKET = -1;
inline auto close_socket(SocketHandle s) -> void { ::close(s); }
#endif

// 首操作数即目标宽度：乘法在 64 位里完成，不产生「32 位算完再隐式加宽」的中间形态。
inline constexpr std::size_t AURORA_MAX_RESPONSE_BODY = std::size_t{4} * 1024 * 1024;  // 4 MiB

}  // namespace detail

/// @brief 向本机 Inspector 发一次 HTTP 请求。
///
/// @param method  "GET" / "PUT" / "POST"
/// @param host    主机名；必须是回环地址，否则直接拒绝
/// @param port    端口
/// @param target  路径（含 query），如 `/api/tree?window=2`
/// @param body    请求体（GET 时为空）
///
/// @return 响应；`error` 非空表示传输层失败（连不上 / 超时 / 非回环地址被拒）。
///
/// @note Thread: safe（每次调用独立建连，自带 WSA 初始化）
/// @note Side-effects: 网络 I/O（仅限 loopback）
[[nodiscard]] inline auto http_request(std::string_view method, std::string_view host, std::uint16_t port,
                                       std::string_view target, std::string_view body = {}) -> HttpResponse {
    HttpResponse out;
    if (!is_loopback_host(host)) {
        out.error = "refusing non-loopback host: " + std::string(host);
        return out;
    }
    if (port == 0) {
        out.error = "invalid port 0";
        return out;
    }
    if (target.empty() || target.front() != '/') {
        out.error = "target must start with '/'";
        return out;
    }

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        out.error = "WSAStartup failed";
        return out;
    }
    bool wsa_ready = true;
#else
    bool wsa_ready = false;
#endif

    auto finish = [&](HttpResponse r) -> HttpResponse {
        if (wsa_ready) {
#ifdef _WIN32
            WSACleanup();
#endif
        }
        return r;
    };

    const detail::SocketHandle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (sock == detail::AURORA_INVALID_SOCKET) {
        return finish(HttpResponse{.error = "socket() failed"});
    }
#else
    if (sock < 0) {
        return finish(HttpResponse{.error = "socket() failed"});
    }
#endif

    // 收发超时：挂死的工具不该无限等下去。
    //
    // ⚠️ 该选项在两个平台上**不同形**，不存在可移植的同一份字节：Winsock 的 `SO_RCVTIMEO` /
    // `SO_SNDTIMEO` 收的是 `DWORD` **毫秒数**，POSIX 收的是 `struct timeval`。把 `timeval`
    // 递给 Winsock，它只把前 4 字节当 DWORD 读，于是 `tv_sec = 5` 在 Windows 上实际生效成
    // **5 毫秒**（`tv_usec` 那 4 字节被丢弃）——而本客户端的主要使用面恰恰是 Windows，慢于
    // 5ms 的响应会被 `WSAETIMEDOUT` 掐掉、退化成「empty response」。故按平台各给其形态。
    constexpr int io_timeout_ms = 5000;
#ifdef _WIN32
    const DWORD io_timeout = io_timeout_ms;
#else
    timeval io_timeout{};
    io_timeout.tv_sec = io_timeout_ms / 1000;
    io_timeout.tv_usec = (io_timeout_ms % 1000) * 1000;
#endif
    // BSD socket 边界：`optval` 形参在 POSIX 是 `const void *`、Winsock 是 `const char *`，
    // 二者都只认按字节传参，取超时变量的地址按平台形态过界是唯一方式。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const int rcv_rc = ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&io_timeout),
                                    static_cast<int>(sizeof(io_timeout)));
    const int snd_rc = ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&io_timeout),
                                    static_cast<int>(sizeof(io_timeout)));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (rcv_rc != 0 || snd_rc != 0) {
        // 设不上超时就不在无界等待上继续走：那正是本段代码要防的事故形态，宁可显式失败。
        detail::close_socket(sock);
        return finish(HttpResponse{.error = "setsockopt(SO_RCVTIMEO/SO_SNDTIMEO) failed"});
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 恒定连回环，不经 DNS
    // 同上：`sockaddr_in` 是 `sockaddr` 的 AF_INET 具体形态，前导字段布局由协议族约定保证，
    // POSIX 的连接口只收通用 `sockaddr *`。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        detail::close_socket(sock);
        return finish(HttpResponse{.error = "connect() failed on 127.0.0.1:" + std::to_string(port)});
    }

    std::string request;
    request += std::string(method);
    request += ' ';
    request += std::string(target);
    request += " HTTP/1.1\r\nHost: 127.0.0.1:";
    request += std::to_string(port);
    request += "\r\nConnection: close\r\n";
    if (!body.empty()) {
        request += "Content-Type: application/json\r\nContent-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }
    request += "\r\n";
    request += std::string(body);

    std::size_t sent_total = 0;
    while (sent_total < request.size()) {
        // 剩余待发包成 span 切片：区间长度由 subspan 自身携带，免裸指针偏移。
        const auto remaining = std::span<const char>(request).subspan(sent_total);
        const auto n = ::send(sock, remaining.data(), static_cast<int>(remaining.size()), 0);
        if (n <= 0) {
            detail::close_socket(sock);
            return finish(HttpResponse{.error = "send() failed"});
        }
        sent_total += static_cast<std::size_t>(n);
    }

    // 读到连接关闭（`InspectorServer` 固定回 Connection: close）。
    std::string raw;
    char buf[4096];
    while (raw.size() < detail::AURORA_MAX_RESPONSE_BODY) {
        const auto n = ::recv(sock, buf, static_cast<int>(sizeof(buf)), 0);
        if (n == 0) {
            break;  // 对端关闭
        }
        if (n < 0) {
            break;  // 超时或错误：已收到的部分仍尝试解析
        }
        raw.append(buf, static_cast<std::size_t>(n));
    }
    detail::close_socket(sock);

    if (raw.empty()) {
        return finish(HttpResponse{.error = "empty response"});
    }

    // 解析状态行与响应体。
    const std::size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return finish(HttpResponse{.error = "malformed response (no header terminator)"});
    }
    const std::string head = raw.substr(0, header_end);
    out.body = raw.substr(header_end + 4);

    // "HTTP/1.1 200 OK" → 200
    const std::size_t sp1 = head.find(' ');
    if (sp1 != std::string::npos) {
        const std::size_t sp2 = head.find(' ', sp1 + 1);
        const std::string code = head.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
        try {
            out.status = std::stoi(code);
        } catch (...) {
            out.status = 0;
        }
    }
    return finish(out);
}

}  // namespace aurora::tools::inspector

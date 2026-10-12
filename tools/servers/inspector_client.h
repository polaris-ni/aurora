#pragma once

// Aurora Inspector 的最小 HTTP 客户端（**仅 `tools/` 层使用**，供 `aurora_mcp` 连接正在运行的应用）。
//
// 设计约束：
//   - **只连 loopback**。这是「把运行中的 UI 交给 AI 操作」的边界，绝不允许连到非本机；
//     因此不做 DNS、不接受主机名，只认 127.0.0.1 / localhost / ::1，且一律连到 127.0.0.1。
//   - 传输层经核心公共层 `aurora::net`（CHG-006）：Winsock/BSD 适配与两平台形态差异
//     （毫秒超时、Windows 运行时配对等）全部收敛在 `src/aurora/net/socket.cpp`，本文件零平台
//     socket 词汇，只做 HTTP 组帧/收发编排/解析。
//   - 核心库不感知本文件：本文件不进 `include/`、不进 `src/`；核心承诺的是「零第三方依赖」，
//     `aurora::net` 是核心自带能力（仅链系统 socket 库）。
//   - 只读到连接关闭为止（`InspectorServer` 固定回 `Connection: close`），并设 4MiB 上限。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "aurora/net/socket.h"

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

// 首操作数即目标宽度：乘法在 64 位里完成，不产生「32 位算完再隐式加宽」的中间形态。
inline constexpr std::size_t AURORA_MAX_RESPONSE_BODY = std::size_t{4} * 1024 * 1024;  // 4 MiB

/// @brief 收发超时：挂死的工具不该无限等下去（毫秒统一口径，平台形态换算在 `aurora::net` 实现侧）。
inline constexpr int AURORA_IO_TIMEOUT_MS = 5000;

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
/// @note Thread: safe（每次调用独立建连，自带 socket 运行时初始化）
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

    // RAII 运行时：Windows 引用计数初始化/清理，POSIX 零成本——取代逐请求手工配对。
    aurora::net::Session runtime;
    if (!runtime.ok()) {
        out.error = "socket runtime initialization failed";
        return out;
    }

    auto connected = aurora::net::StreamSocket::connect_loopback(port);
    if (!connected) {
        // 原措辞保留：`connect() failed on 127.0.0.1:<port>` 是 e2e/单测的传输层成因白名单项。
        out.error = "connect() failed on 127.0.0.1:" + std::to_string(port);
        return out;
    }
    aurora::net::StreamSocket sock = std::move(connected.value());

    if (!sock.set_io_timeout(std::chrono::milliseconds{detail::AURORA_IO_TIMEOUT_MS})) {
        // 设不上超时就不在无界等待上继续走：那正是本段代码要防的事故形态，宁可显式失败。
        out.error = "io timeout setup failed";
        return out;
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

    if (!sock.write_all(std::span<const char>(request))) {
        out.error = "send() failed";
        return out;
    }

    // 读到连接关闭（`InspectorServer` 固定回 Connection: close）。
    std::string raw;
    char buf[4096];
    while (raw.size() < detail::AURORA_MAX_RESPONSE_BODY) {
        const auto got = sock.read_some(std::span<char>(buf));
        if (!got) {
            break;  // 超时或错误：已收到的部分仍尝试解析
        }
        if (got.value() == 0) {
            break;  // 对端关闭
        }
        raw.append(buf, got.value());
    }
    sock.close();

    if (raw.empty()) {
        out.error = "empty response";
        return out;
    }

    // 解析状态行与响应体。
    const std::size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        out.error = "malformed response (no header terminator)";
        return out;
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
    return out;
}

}  // namespace aurora::tools::inspector

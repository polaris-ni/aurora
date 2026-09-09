/// 测试类型: integration
/// 目标单元: include/aurora/inspector/inspector_server.h
/// 测试说明: InspectorServer 不可信输入健壮性（安全回归）——畸形 JSON 字段类型、
///           非对象 body、越界 style、非法 font_weight、带 query 的 REST 路由、
///           Content-Length 大小写混排与 body 内伪装头：一律返回 4xx/5xx JSON
///           响应且进程存活，绝不 std::terminate。真实 loopback HTTP 往返驱动。
///           AURORA_BUILD_INSPECTOR_SERVER=OFF 时整文件降级为 skip 桩

#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

#ifdef AURORA_BUILD_INSPECTOR_SERVER

#ifdef AURORA_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET kInvalidSocket = -1;
inline auto closesocket(SOCKET s) -> int { return ::close(s); }
#endif

#include "aurora/aurora.h"
#include "aurora/inspector/inspector_server.h"
#include "aurora/window/surface.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#endif

namespace aurora::test_cases::itest_inspector_robustness {

#ifdef AURORA_BUILD_INSPECTOR_SERVER
namespace {

/// @brief 共享测试树：Column 根 + Text 子节点（静态存储期，供 worker 线程 root_getter 读取）。
auto shared_tree() -> std::shared_ptr<Column> & {
    static std::shared_ptr<Column> tree = [] {
        auto col = std::make_shared<Column>();
        col->add(Node{std::make_shared<Text>("hello")});
        return col;
    }();
    return tree;
}

auto tree_getter() -> Node { return Node{shared_tree()}; }

#if defined(AURORA_PLATFORM_WINDOWS)
/// @brief Winsock 会话（引用计数式启停，随作用域清理）。
struct WinsockSession {
    WinsockSession() {
        WSADATA data{};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (started_) {
            WSACleanup();
        }
    }
    bool started_ = false;
};
#endif

/// @brief 对 127.0.0.1:port 发送原始请求文本并回收完整响应（服务端 Connection: close）。
auto http_raw(std::uint16_t port, const std::string &raw) -> std::string {
#if defined(AURORA_PLATFORM_WINDOWS)
    const WinsockSession wsa;
#endif
    const SOCKET sock =
#if defined(AURORA_PLATFORM_WINDOWS)
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return {};
    }
#else
        ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return {};
    }
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        closesocket(sock);
        return {};
    }
    (void)send(sock, raw.data(), static_cast<int>(raw.size()), 0);

    std::string resp;
    char buf[4096];
    for (;;) {
        const int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        resp.append(buf, static_cast<std::size_t>(n));
        if (resp.size() > (1U << 20U)) {
            break;  // 安全上限，防异常服务端无限输出
        }
    }
    closesocket(sock);
    return resp;
}

/// @brief 标准 HTTP 请求便捷封装。
auto http_request(std::uint16_t port, const std::string &method, const std::string &path, const std::string &body)
    -> std::string {
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\nContent-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;
    return http_raw(port, req.str());
}

/// @brief "HTTP/1.1 400 Bad Request" → 400；解析失败返回 0（视为连接中断，必失败）。
auto status_of(const std::string &resp) -> int {
    const auto sp1 = resp.find(' ');
    if (sp1 == std::string::npos) {
        return 0;
    }
    const auto sp2 = resp.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        return 0;
    }
    try {
        return std::stoi(resp.substr(sp1 + 1, sp2 - sp1 - 1));
    } catch (...) {
        return 0;
    }
}

auto body_of(const std::string &resp) -> std::string {
    const auto hend = resp.find("\r\n\r\n");
    return (hend == std::string::npos) ? std::string{} : resp.substr(hend + 4);
}

/// @brief 每用例一台随机端口服务器；start 失败（无回环 socket 环境）则整例跳过。
class ScopedServer {
  public:
    ScopedServer() {
        started_ = server_.start(0);
        if (!started_) {
            AURORA_TEST_SKIP("InspectorServer start() failed (environment without loopback sockets?)");
        }
    }
    ~ScopedServer() {
        if (started_) {
            server_.stop();
        }
    }
    ScopedServer(const ScopedServer &) = delete;
    auto operator=(const ScopedServer &) -> ScopedServer & = delete;

    [[nodiscard]] auto port() const -> std::uint16_t { return server_.port(); }

  private:
    InspectorServer server_{tree_getter};
    bool started_ = false;
};

}  // namespace

#endif  // AURORA_BUILD_INSPECTOR_SERVER（辅助设施段；用例恒注册，体内降级 SKIP）

AURORA_TEST_CASE(debug_flags_type_mismatch_returns_400) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER)
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    const ScopedServer server;

    // 布尔字段传字符串：类型不符必须 400，不得 terminate。
    {
        const auto r = http_request(server.port(), "POST", "/api/debug/flags", R"({"layout_guides":"x"})");
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
        AURORA_TEST_CHECK(!body_of(r).empty());
    }
    // 布尔字段传整数：同样 400。
    {
        const auto r = http_request(server.port(), "POST", "/api/debug/flags", R"({"overdraw": 3})");
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
    }
    // 非对象 body（数组）同样 400。
    {
        const auto r = http_request(server.port(), "POST", "/api/debug/flags", R"([1,2,3])");
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
    }
    // 合法请求仍工作。
    {
        const auto r = http_request(server.port(), "POST", "/api/debug/flags", R"({"layout_guides":true})");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
#endif
}

AURORA_TEST_CASE(to_code_style_type_mismatch_and_out_of_range_fallback) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER)
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    const ScopedServer server;

    // style 传字符串：必须 400。
    {
        const std::string body =
            R"({"node":{"type":"Column","props":{},"children":[{"type":"Button","props":{"label":"OK"},"children":[]}]},"style":"fast"})";
        const auto r = http_request(server.port(), "POST", "/api/to_code", body);
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
    }
    // style 越界整数：回退 Fluent，保持向后兼容（200）。
    {
        const std::string body = R"({"node":{"type":"Button","props":{"label":"OK"},"children":[]},"style":99})";
        const auto r = http_request(server.port(), "POST", "/api/to_code", body);
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
    // node 为字符串等非对象类型也不得崩溃（200/400/500 均可，唯连接不得中断）。
    {
        const auto r = http_request(server.port(), "POST", "/api/to_code", R"("just-a-string")");
        const int st = status_of(r);
        AURORA_TEST_CHECK(st == 200 || st == 400 || st == 500);
    }
#endif
}

AURORA_TEST_CASE(widget_put_invalid_font_weight_does_not_terminate) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER)
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    const ScopedServer server;

    // font_weight="bold" 走 Text::apply_props → json_to_font_weight：
    // 不得抛 std::invalid_argument 崩溃；回退 Normal(200) 或显式拒绝(400)。
    const auto r = http_request(server.port(), "PUT", "/api/widget/0/font_weight", R"("bold")");
    const int st = status_of(r);
    AURORA_TEST_CHECK(st == 200 || st == 400);
#endif
}

AURORA_TEST_CASE(query_string_and_mixed_case_content_length) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER)
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    const ScopedServer server;

    // 带 query 的 REST 路由（回归：曾因用未剥离 query 的 path 比较而误 404）。
    {
        const auto r = http_request(server.port(), "GET", "/api/tree?x=1", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
    {
        const auto r = http_request(server.port(), "GET", "/api/components?foo=bar", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
    {
        const auto r = http_request(server.port(), "GET", "/api/widget/0?x=1", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);  // query 不得混入树路径
    }

    // Content-Length 大小写混排仍可解析（header_value 大小写不敏感）。
    {
        const std::string body = R"({"node":{"type":"Button","props":{"label":"X"},"children":[]}})";
        std::ostringstream req;
        req << "POST /api/to_code HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            << "cOnTeNt-LeNgTh: " << body.size() << "\r\nConnection: close\r\n\r\n"
            << body;
        const auto r = http_raw(server.port(), req.str());
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
#endif
}

AURORA_TEST_CASE(body_length_trap_and_liveness_probe) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER)
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    const ScopedServer server;

    // body 中伪装的 Content-Length 文本不得干扰解析（仅在头部区检索）。
    {
        const std::string body =
            R"({"note":"Content-Length: 999999","node":{"type":"Button","props":{},"children":[]}})";
        const auto r = http_request(server.port(), "POST", "/api/to_code", body);
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }

    // 存活探针：上述畸形请求之后服务必须仍然正常响应。
    {
        const auto r = http_request(server.port(), "GET", "/api/tree", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
#endif
}

}  // namespace aurora::test_cases::itest_inspector_robustness

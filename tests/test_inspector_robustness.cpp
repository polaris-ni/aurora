// test_inspector_robustness.cpp — InspectorServer 不可信输入健壮性（安全回归）
//
// 背景：Inspector HTTP 服务监听 127.0.0.1，同一桌面任意本地进程（乃至 DNS rebinding
// 之外的脚本）都可发请求。此前路由内多处对 JSON 字段裸调 get<bool>() / value("style",0)
// / std::stoi，类型不符即抛异常且 worker 线程未捕获 → std::terminate 整个应用。
// 本测试以畸形输入逐一轰击各端点，断言：返回 4xx/5xx JSON 响应、进程存活。
//
// 当 AURORA_INSPECTOR_SERVER_ENABLED 未定义（AURORA_BUILD_INSPECTOR_SERVER=OFF）时自跳过。

#include "aurora/core/platform.h"
#include "aurora/inspector/inspector_server.h"

#ifndef AURORA_INSPECTOR_SERVER_ENABLED

#include "test_harness.h"

AURORA_TEST() { AURORA_TEST_PRINTF("skip: AURORA_INSPECTOR_SERVER_ENABLED not defined (server not built)"); }

#else

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#ifdef AURORA_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
struct WSADATA {
    int dummy = 0;
};
inline int WSAStartup(uint16_t, void *) { return 0; }
inline void WSACleanup() {}
#define MAKEWORD(a, b) (0)
inline int closesocket(SOCKET s) { return ::close(s); }
#endif

#include "aurora/aurora.h"
#include "test_harness.h"

namespace {

/// 最小 loopback HTTP 客户端：发送原始请求文本，读全响应。
auto http_raw(uint16_t port, const std::string &raw) -> std::string {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return {};
    }
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        closesocket(sock);
        WSACleanup();
        return {};
    }
    send(sock, raw.data(), static_cast<int>(raw.size()), 0);
    std::string resp;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, static_cast<std::size_t>(n));
    }
    closesocket(sock);
    WSACleanup();
    return resp;
}

auto http_request(uint16_t port, const std::string &method, const std::string &path, const std::string &body)
    -> std::string {
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\nContent-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;
    return http_raw(port, req.str());
}

auto status_of(const std::string &resp) -> int {
    // "HTTP/1.1 400 Bad Request" → 400
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

}  // namespace

AURORA_TEST() {
    // 构造含 Text 子控件的树：PUT font_weight 路径经 text.cpp apply_props → json_to_font_weight。
    auto root_widget = std::make_shared<aurora::Column>();
    auto text_child = std::make_shared<aurora::Text>("hello");
    root_widget->add(aurora::Node{text_child});
    aurora::Node root{root_widget};

    std::function<aurora::Node()> getter = [&]() -> aurora::Node { return root; };
    aurora::InspectorServer server(getter);

    const bool started = server.start(0);
    if (!started) {
        AURORA_TEST_PRINTF("skip: InspectorServer start() failed (environment without loopback sockets?)");
    }
    const uint16_t port = server.port();

    // ---- 1. flags：字段类型不符必须 400，不得 terminate ----
    {
        auto r = http_request(port, "POST", "/api/debug/flags", R"({"layout_guides":"x"})");
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
        AURORA_TEST_CHECK(!body_of(r).empty());
    }
    {
        auto r = http_request(port, "POST", "/api/debug/flags", R"({"overdraw": 3})");
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
    }
    {
        // 非对象 body（数组）同样 400
        auto r = http_request(port, "POST", "/api/debug/flags", R"([1,2,3])");
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
    }
    {
        // 合法请求仍工作
        auto r = http_request(port, "POST", "/api/debug/flags", R"({"layout_guides":true})");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }

    // ---- 2. to_code：style 类型不符必须 400；越界整数回退 Fluent（200）----
    {
        const std::string body =
            R"({"node":{"type":"Column","props":{},"children":[{"type":"Button","props":{"label":"OK"},"children":[]}]},"style":"fast"})";
        auto r = http_request(port, "POST", "/api/to_code", body);
        AURORA_TEST_CHECK_EQ(status_of(r), 400);
    }
    {
        const std::string body = R"({"node":{"type":"Button","props":{"label":"OK"},"children":[]},"style":99})";
        auto r = http_request(port, "POST", "/api/to_code", body);
        AURORA_TEST_CHECK_EQ(status_of(r), 200);  // 越界 style 回退 Fluent，保持向后兼容
    }
    {
        // node 为字符串等非对象类型也不得崩溃（to_code 内部按 Json 处理）
        auto r = http_request(port, "POST", "/api/to_code", R"("just-a-string")");
        AURORA_TEST_CHECK(status_of(r) == 200 || status_of(r) == 400 || status_of(r) == 500);
    }

    // ---- 3. widget PUT font_weight="bold"：不得抛 std::invalid_argument 崩溃 ----
    {
        auto r = http_request(port, "PUT", "/api/widget/0/font_weight", R"("bold")");
        const int st = status_of(r);
        AURORA_TEST_CHECK(st == 200 || st == 400);  // 回退 Normal(200) 或显式拒绝(400)，绝不允许连接中断/无响应
    }

    // ---- 4. 存活探针：上述畸形请求之后服务必须仍然正常响应 ----
    {
        auto r = http_request(port, "GET", "/api/tree", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }

    // ---- 5. 带 query 的 REST 路由（回归：曾因用未剥离 query 的 path 比较而误 404）----
    {
        auto r = http_request(port, "GET", "/api/tree?x=1", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
    {
        auto r = http_request(port, "GET", "/api/components?foo=bar", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }
    {
        auto r = http_request(port, "GET", "/api/widget/0?x=1", "");
        AURORA_TEST_CHECK_EQ(status_of(r), 200);  // query 不得混入树路径
    }

    // ---- 6. Content-Length 大小写混排仍可解析（header_value 大小写不敏感）----
    {
        const std::string body = R"({"node":{"type":"Button","props":{"label":"X"},"children":[]}})";
        std::ostringstream req;
        req << "POST /api/to_code HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            << "cOnTeNt-LeNgTh: " << body.size() << "\r\nConnection: close\r\n\r\n"
            << body;
        auto r = http_raw(port, req.str());
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }

    // ---- 7. body 中伪装的 Content-Length 文本不得干扰解析（仅在头部区检索）----
    {
        const std::string body =
            R"({"note":"Content-Length: 999999","node":{"type":"Button","props":{},"children":[]}})";
        auto r = http_request(port, "POST", "/api/to_code", body);
        AURORA_TEST_CHECK_EQ(status_of(r), 200);
    }

    server.stop();
}

#endif  // AURORA_INSPECTOR_SERVER_ENABLED

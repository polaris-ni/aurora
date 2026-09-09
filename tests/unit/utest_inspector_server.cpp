/// 测试类型: unit
/// 目标单元: include/aurora/inspector/inspector_server.h
/// 测试说明: 覆盖 InspectorServer 生命周期与 HTTP 基本路径——初始停机态、start(0) 随机端口
/// 启停、重复 start 失败、stop 幂等、析构收编 worker、/api/tree 与 /api/components 的
/// 请求-响应、404/405/400/403 错误请求、/api/debug/state 的 surface getter 装配错误路径。
/// 端口一律用 0（系统分配临时端口，无冲突）；无文件句柄副作用。客户端为本 TU 内最小
/// 回环 socket 实现，随用例关闭清理。
/// AURORA_BUILD_INSPECTOR_SERVER=OFF 时整文件降级为 skip 桩。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

#ifdef AURORA_BUILD_INSPECTOR_SERVER

#include "aurora/inspector/inspector_server.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"  // set_surface_getter 的 Surface 完整类型

#ifdef AURORA_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#endif  // AURORA_BUILD_INSPECTOR_SERVER

namespace aurora::test_cases::utest_inspector_server {

#ifdef AURORA_BUILD_INSPECTOR_SERVER
namespace {

/// @brief 共享测试树：Column 根 + Text 子节点（静态存储期，供 worker 线程 root_getter 读取）。
auto shared_tree() -> std::shared_ptr<Column>& {
    static std::shared_ptr<Column> tree = []() -> std::shared_ptr<aurora::Column> {
        auto col = std::make_shared<Column>();
        col->add(Node{std::make_shared<Text>("hello")});
        return col;
    }();
    return tree;
}

/// @brief root_getter：每次请求返回共享树的 Node 副本。
auto tree_getter() -> Node { return Node{shared_tree()}; }

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief Winsock 会话（引用计数式启停，随作用域清理）。
struct WinsockSession {
    WinsockSession() {
        WSADATA data{};
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (started) {
            WSACleanup();
        }
    }
    // 纯作用域 guard，禁止拷贝/移动（避免重复 WSACleanup）。
    WinsockSession(const WinsockSession&) = delete;
    auto operator=(const WinsockSession&) -> WinsockSession& = delete;
    WinsockSession(WinsockSession&&) = delete;
    auto operator=(WinsockSession&&) -> WinsockSession& = delete;
    bool started = false;
};
#endif

/// @brief 发送全部字节；失败返回 false（对端断开/出错）。
auto send_all(int sock, const std::string& data) -> bool {
    std::size_t left = data.size();
    const char* p = data.data();
    while (left > 0) {
#ifdef AURORA_PLATFORM_WINDOWS
        const int n = ::send(sock, p, static_cast<int>(left), 0);
#else
        const auto n = static_cast<int>(::send(sock, p, left, 0));
#endif
        if (n <= 0) {
            return false;
        }
        // 逐段发送需前移缓冲指针，socket API 即要求裸指针+长度，无更安全替代。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

/// @brief 对 127.0.0.1:port 发送原始 HTTP 请求并回收完整响应（服务端 Connection: close，
/// 读到对端关闭即完整）。
[[nodiscard]] auto http_roundtrip(std::uint16_t port, const std::string& request) -> std::string {
#ifdef AURORA_PLATFORM_WINDOWS
    const WinsockSession wsa;
#endif
#ifdef AURORA_PLATFORM_WINDOWS
    const auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    AURORA_TEST_REQUIRE_NE(sock, INVALID_SOCKET);
#else
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    AURORA_TEST_REQUIRE_GE(sock, 0);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    // connect() 的 socket API 契约要求将 sockaddr_in 擦除为通用 sockaddr 指针，无类型安全替代。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    AURORA_TEST_REQUIRE_EQ(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    AURORA_TEST_REQUIRE_TRUE(send_all(sock, request));

    std::string response;
    char buf[4096];
    for (;;) {
#ifdef AURORA_PLATFORM_WINDOWS
        const int n = ::recv(sock, buf, sizeof(buf), 0);
#else
        const auto n = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
#endif
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.size() > (1U << 20U)) {
            break;  // 安全上限，防异常服务端无限输出
        }
    }
#ifdef AURORA_PLATFORM_WINDOWS
    ::closesocket(sock);
#else
    ::close(sock);
#endif
    AURORA_TEST_REQUIRE_FALSE(response.empty());
    return response;
}

/// @brief 带 Host 头的 GET 便捷封装。
[[nodiscard]] auto http_get(std::uint16_t port, const std::string& target) -> std::string {
    return http_roundtrip(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
}

}  // namespace
#endif  // AURORA_BUILD_INSPECTOR_SERVER（辅助设施段；用例恒注册，体内降级 SKIP）

AURORA_TEST_CASE(initial_state_is_stopped) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_CHECK_EQ(server.is_running(), false);
    AURORA_TEST_CHECK_EQ(server.port(), 0);
#endif
}

AURORA_TEST_CASE(start_and_stop_lifecycle) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    // 端口传 0：由系统分配临时端口，规避并行用例间的端口冲突。
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    AURORA_TEST_CHECK_EQ(server.is_running(), true);
    AURORA_TEST_CHECK_NE(server.port(), 0);

    server.stop();
    AURORA_TEST_CHECK_EQ(server.is_running(), false);
    AURORA_TEST_CHECK_EQ(server.port(), 0);
#endif
}

AURORA_TEST_CASE(start_twice_while_running_fails) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    // 已运行时再次 start 必须拒绝（而不是悄悄重启或泄漏 socket）。
    AURORA_TEST_CHECK_EQ(server.start(0), false);
    AURORA_TEST_CHECK_EQ(server.is_running(), true);
    server.stop();
#endif
}

AURORA_TEST_CASE(stop_is_idempotent_and_restartable) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_CHECK_NO_THROW(server.stop());  // 未启动时 stop 直接返回

    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    AURORA_TEST_CHECK_NO_THROW(server.stop());
    AURORA_TEST_CHECK_NO_THROW(server.stop());  // 重复 stop 安全（幂等）

    // 停止后可重新启动（socket 已关闭、worker 已回收）。
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    AURORA_TEST_CHECK_EQ(server.is_running(), true);
    server.stop();
#endif
}

AURORA_TEST_CASE(destructor_joins_worker_and_releases_state) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    {
        InspectorServer scoped(tree_getter);
        AURORA_TEST_REQUIRE_TRUE(scoped.start(0));
        AURORA_TEST_CHECK_EQ(scoped.is_running(), true);
    }  // 析构须内部 stop：join worker、关 socket、还原 Winsock 引用

    // 析构后可立即再次起服（无悬挂线程/句柄阻塞）。
    InspectorServer next(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(next.start(0));
    AURORA_TEST_CHECK_EQ(next.is_running(), true);
    next.stop();
#endif
}

AURORA_TEST_CASE(tree_endpoint_serves_widget_tree_json) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_get(server.port(), "/api/tree");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("application/json") != std::string::npos);
    // 完整树含根与子控件类型。
    AURORA_TEST_CHECK_TRUE(resp.find("Column") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("Text") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(components_endpoint_returns_json_array) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_get(server.port(), "/api/components");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("application/json") != std::string::npos);
    // schema 列表体以 JSON 数组开头（核心控件注册后非空）。
    AURORA_TEST_CHECK_TRUE(resp.find("\r\n\r\n[") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(unknown_endpoint_returns_404) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_get(server.port(), "/api/no_such_endpoint");
    AURORA_TEST_CHECK_TRUE(resp.find("404") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("error") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(wrong_method_returns_405) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp =
        http_roundtrip(server.port(), "POST /api/tree HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
    AURORA_TEST_CHECK_TRUE(resp.find("405") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(bad_pick_params_return_400) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // pick 需要数值 x/y：非数值参数必须回 400 而非 500/崩溃。
    const std::string resp = http_get(server.port(), "/api/debug/pick?x=abc&y=0");
    AURORA_TEST_CHECK_TRUE(resp.find("400") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(missing_host_header_returns_403) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // DNS rebinding 防护：缺失/非回环 Host 一律拒绝。
    const std::string resp = http_roundtrip(server.port(), "GET /api/tree HTTP/1.1\r\n\r\n");
    AURORA_TEST_CHECK_TRUE(resp.find("403") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(debug_state_requires_surface_getter) {
#ifndef AURORA_BUILD_INSPECTOR_SERVER
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER 未开启：Inspector HTTP server 未构建");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 未装配 getter：明确 400。
    const std::string without = http_get(server.port(), "/api/debug/state");
    AURORA_TEST_CHECK_TRUE(without.find("400") != std::string::npos);

    // getter 装配后返回 null Surface：路由层区分回 500。
    server.set_surface_getter([]() -> Surface* { return nullptr; });
    const std::string with_null = http_get(server.port(), "/api/debug/state");
    AURORA_TEST_CHECK_TRUE(with_null.find("500") != std::string::npos);
    server.stop();
#endif
}

}  // namespace aurora::test_cases::utest_inspector_server

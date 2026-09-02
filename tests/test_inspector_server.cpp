// test_inspector_server.cpp — InspectorServer 基础测试
//
// 当 AURORA_INSPECTOR_SERVER_ENABLED 未定义时（即 AURORA_BUILD_INSPECTOR_SERVER=OFF），
// 仅验证头文件可包含，测试直接通过。
// 定义时执行完整的构造/启动/停止生命周期测试。

#include <cstdio>
#include <functional>

#include "aurora/core/platform.h"
#include "aurora/inspector/inspector_server.h"

#include "test_harness.h"

#ifndef AURORA_INSPECTOR_SERVER_ENABLED

AURORA_TEST() {
    std::fprintf(stderr, "=== test_inspector_server (header-only, server not built) ===\n");
    std::fprintf(stderr, "  PASS: header included successfully\n");
    std::fprintf(stderr, "=== all tests passed ===\n");
}

#else

#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#ifdef AURORA_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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
#include "aurora/debug/debug_paint.h"
#include "aurora/widget/widget.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL: %s (line %d): %s\n", #cond, __LINE__, msg);                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define TEST_PASS(name) std::fprintf(stderr, "  PASS: %s\n", name)

static auto make_dummy_root() -> aurora::Node { return aurora::Node{}; }

// ---- 测试辅助：最小 HTTP 客户端（loopback）----
// 直接连 127.0.0.1:port 发请求、读全响应；校验 Host 回环、拒 Origin（与服务器一致）。
static auto http_send(uint16_t port, const std::string &method, const std::string &path, const std::string &body)
    -> std::string {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return {};
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
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;
    const std::string req_str = req.str();
    send(sock, req_str.data(), static_cast<int>(req_str.size()), 0);
    std::string resp;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<std::size_t>(n));
    closesocket(sock);
    WSACleanup();
    return resp;
}

struct HttpParsed {
    int status = 0;
    std::string content_type;
    std::string body;
};

static auto http_parse(const std::string &resp) -> HttpParsed {
    HttpParsed r;
    const auto sp = resp.find(' ');
    if (sp == std::string::npos) return r;
    const auto sp2 = resp.find(' ', sp + 1);
    if (sp2 == std::string::npos) return r;
    r.status = std::stoi(resp.substr(sp + 1, sp2 - sp - 1));
    const auto hend = resp.find("\r\n\r\n");
    if (hend == std::string::npos) return r;
    const std::string head = resp.substr(0, hend);
    const auto ct = head.find("Content-Type:");
    if (ct != std::string::npos) {
        auto cs = ct + 13;
        while (cs < head.size() && (head[cs] == ' ' || head[cs] == '\t'))
            ++cs;
        auto ce = head.find("\r\n", cs);
        r.content_type = head.substr(cs, ce - cs);
    }
    int clen = 0;
    const auto cl = head.find("Content-Length:");
    if (cl != std::string::npos) {
        auto cs = cl + 15;
        while (cs < head.size() && head[cs] == ' ')
            ++cs;
        auto ce = head.find("\r\n", cs);
        clen = std::stoi(head.substr(cs, ce - cs));
    }
    r.body = resp.substr(hend + 4, static_cast<std::size_t>(clen));
    return r;
}

AURORA_TEST() {
    std::fprintf(stderr, "=== test_inspector_server (full) ===\n");

    // Test 1: 构造后初始状态
    {
        std::function<aurora::Node()> getter = make_dummy_root;
        aurora::InspectorServer server(getter);
        TEST_ASSERT(!server.is_running(), "server should not be running after construction");
        TEST_ASSERT(server.port() == 0, "port should be 0 when not started");
        TEST_PASS("construction and initial state");
    }

    // Test 2: stop() 在未 start() 时调用应安全（幂等）
    {
        std::function<aurora::Node()> getter = make_dummy_root;
        aurora::InspectorServer server(getter);
        server.stop();
        TEST_ASSERT(!server.is_running(), "server should not be running after stop()");
        TEST_PASS("stop() without start() is safe");
    }

    // Test 3: 析构函数在未 start() 时安全
    {
        std::function<aurora::Node()> getter = make_dummy_root;
        {
            aurora::InspectorServer server(getter);
        }
        TEST_PASS("destruction without start() is safe");
    }

    // Test 4: start/stop 基本流程（port=0 让系统分配端口）
    {
        aurora::Node root = make_dummy_root();
        std::function<aurora::Node()> getter = [&]() -> aurora::Node { return root; };
        aurora::InspectorServer server(getter);

        const bool started = server.start(0);
        if (started) {
            TEST_ASSERT(server.is_running(), "server should be running after start()");
            TEST_ASSERT(server.port() != 0, "port should be non-zero after start()");
            server.stop();
            TEST_ASSERT(!server.is_running(), "server should not be running after stop()");
            TEST_ASSERT(server.port() == 0, "port should be 0 after stop()");
            TEST_PASS("start/stop lifecycle (port=0)");
        } else {
            std::fprintf(stderr, "  SKIP: start() returned false\n");
        }
    }

    // Test 5: 重复 start() 应返回 false
    {
        std::function<aurora::Node()> getter = make_dummy_root;
        aurora::InspectorServer server(getter);
        const bool started = server.start(0);
        if (started) {
            const bool second = server.start(0);
            TEST_ASSERT(!second, "second start() should return false");
            server.stop();
            TEST_PASS("double start() returns false");
        } else {
            std::fprintf(stderr, "  SKIP: start() returned false\n");
        }
    }

    // Test 6: 调试端点（state/snapshot/perf/timeline/diagnostics/why/tree/pick/flags）
    {
        // 构造一棵含可拾取子控件的树，经 Window + present_root 完成真正 paint
        // （paint_bounds 有效，pick 才能命中；render_to_logical_snapshot 只 layout 不 paint）。
        Window window{ std::make_unique<HeadlessSurface>("", Size{ 400.0f, 300.0f }) };
        auto root_widget = std::make_shared<Column>();
        Node root{ root_widget };
        // 用 Button 作可拾取子控件：set_on_click 后置位 on_click，wants_click()==true，
        // 才会进入命中链（裸 Container/Column 仅背景、不想要点击，不会命中）。
        auto child = std::make_shared<Button>();
        child->set_label("PickMe");
        child->set_on_click([]() {});
        child->width(px(80.0f));
        child->height(px(40.0f));
        root_widget->add(Node{ child });
        const auto pr = window.present_root(root);
        TEST_ASSERT(static_cast<bool>(pr), "present_root should succeed");

        std::function<Node()> getter = [&]() -> Node { return root; };
        InspectorServer server(getter);
        server.set_surface_getter([&]() -> Surface * { return &window.surface(); });

        const bool started = server.start(0);
        if (!started) {
            std::fprintf(stderr, "  SKIP: start() returned false (Test 6)\n");
        } else {
            const uint16_t p = server.port();
            TEST_ASSERT(p != 0, "port should be non-zero after start()");

            // GET /api/debug/state — Surface 运行时状态（DEBUG 真实、Release available=false，均 200 JSON）
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/state", ""));
                TEST_ASSERT(r.status == 200, "state: status 200");
                TEST_ASSERT(r.content_type == "application/json", "state: application/json");
                bool ok = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(r.body);
                    ok = j.is_object();
                } catch (...) {
                }
                TEST_ASSERT(ok, "state: body parses as JSON object");
                TEST_PASS("/api/debug/state");
            }
            // GET /api/debug/perf
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/perf", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "perf: 200 JSON");
                TEST_PASS("/api/debug/perf");
            }
            // GET /api/debug/timeline
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/timeline", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "timeline: 200 JSON");
                TEST_PASS("/api/debug/timeline");
            }
            // GET /api/debug/diagnostics
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/diagnostics", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "diagnostics: 200 JSON");
                TEST_PASS("/api/debug/diagnostics");
            }
            // GET /api/debug/why
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/why", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "why: 200 JSON");
                TEST_PASS("/api/debug/why");
            }
            // GET /api/debug/tree — 收编 widget_tree
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/tree", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "tree: 200 JSON");
                TEST_PASS("/api/debug/tree");
            }
            // POST /api/debug/flags — 运行时设置叠层开关（两端点均返回 status=ok）
            {
                auto r = http_parse(http_send(p, "POST", "/api/debug/flags", "{\"layout_guides\":true}"));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "flags: 200 JSON");
                bool ok = false;
                std::string st;
                try {
                    nlohmann::json j = nlohmann::json::parse(r.body);
                    ok = j.is_object();
                    if (j.contains("status")) st = j["status"].get<std::string>();
                } catch (...) {
                }
                TEST_ASSERT(ok && st == "ok", "flags: status == ok");
                TEST_PASS("/api/debug/flags");
            }

            // snapshot / pick 仅在 DEBUG 下有真实行为（Release 下 capture / widget_picker 为 no-op）。
#ifdef AURORA_ENABLE_DEBUG
            // GET /api/debug/snapshot — 截图 PNG（image/png，非空）
            {
                auto r = http_parse(http_send(p, "GET", "/api/debug/snapshot", ""));
                TEST_ASSERT(r.status == 200, "snapshot: status 200 (DEBUG)");
                TEST_ASSERT(r.content_type == "image/png", "snapshot: image/png");
                TEST_ASSERT(!r.body.empty(), "snapshot: non-empty PNG body");
                TEST_PASS("/api/debug/snapshot");
            }
            // GET /api/debug/pick — 控件拾取（命中 child 中心，chain 含 child 类型名）
            {
                const Rect b = child->paint_bounds();
                const float cx = b.origin.x + b.size.width * 0.5f;
                const float cy = b.origin.y + b.size.height * 0.5f;
                std::ostringstream q;
                q << "/api/debug/pick?x=" << cx << "&y=" << cy;
                auto r = http_parse(http_send(p, "GET", q.str(), ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "pick: 200 JSON");
                bool ok = false, hit = false, saw_child = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(r.body);
                    ok = j.is_object();
                    hit = j.value("hit", false);
                    if (j.contains("chain") && j["chain"].is_array()) {
                        for (const auto &n : j["chain"]) {
                            if (n.contains("type_name") && n["type_name"].get<std::string>() == child->type_name())
                                saw_child = true;
                        }
                    }
                } catch (...) {
                }
                TEST_ASSERT(ok && hit, "pick: hit at child center (DEBUG)");
                TEST_ASSERT(saw_child, "pick: chain includes the child control");
                TEST_PASS("/api/debug/pick");
            }
#else
            std::fprintf(stderr, "  SKIP: snapshot/pick assertions (Release: capture/picker no-op)\n");
#endif

            server.stop();
            TEST_ASSERT(!server.is_running(), "server should be stopped after Test 6");
        }
    }

    // Test 7: 基础 REST 端点（tree/widget/{path}/PUT widget/{path}/{prop}/components/yaml/to_code）
    // 补齐此前未覆盖的 6 个端点，端到端断言 JSON 响应且 PUT 改写后 GET 可见、to_code 多 style 生效。
    {
        Window window{ std::make_unique<HeadlessSurface>("", Size{ 400.0f, 300.0f }) };
        auto root_widget = std::make_shared<Column>();
        auto child = std::make_shared<Button>();
        child->set_label("Hello");
        child->width(px(80.0f));
        child->height(px(40.0f));
        root_widget->add(Node{ child });
        Node root{ root_widget };
        const auto pr = window.present_root(root);
        TEST_ASSERT(static_cast<bool>(pr), "present_root should succeed (Test 7)");

        std::function<Node()> getter = [&]() -> Node { return root; };
        InspectorServer server(getter);
        const bool started = server.start(0);
        if (!started) {
            std::fprintf(stderr, "  SKIP: start() returned false (Test 7)\n");
        } else {
            const uint16_t p = server.port();
            TEST_ASSERT(p != 0, "port should be non-zero after start() (Test 7)");

            // GET /api/tree — 完整 widget 树 JSON（body 为原始树，含 type/props/children，无 status 包裹）
            {
                auto r = http_parse(http_send(p, "GET", "/api/tree", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "tree: 200 JSON");
                bool ok = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(r.body);
                    ok = j.is_object() && j.contains("type") && j.contains("children") &&
                         j["type"].get<std::string>() == "Column" && j["children"].size() == 1;
                } catch (...) {
                }
                TEST_ASSERT(ok, "tree: body parses as JSON tree (Column root + 1 child)");
                TEST_PASS("/api/tree");
            }

            // GET /api/widget/{path} — 单 widget 属性（路径 0 为第一个子节点 Button）
            const std::string wpath = "/api/widget/0";
            {
                auto r = http_parse(http_send(p, "GET", wpath, ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "widget: 200 JSON");
                bool ok = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(r.body);
                    // get_widget_props 返回 { "descriptor": {type,...}, "values": {...} }
                    ok = j.contains("values") && j["values"].is_object();
                } catch (...) {
                }
                TEST_ASSERT(ok, "widget: body has values object");
                TEST_PASS("/api/widget/{path}");
            }

            // PUT /api/widget/{path}/{prop} — 改写属性后 GET 可见
            {
                const std::string put_path = "/api/widget/0/label";
                auto r = http_parse(http_send(p, "PUT", put_path, "\"Changed\""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "widget PUT: 200 JSON");
                auto g = http_parse(http_send(p, "GET", wpath, ""));
                bool saw = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(g.body);
                    // get_widget_props 返回 { "descriptor": ..., "values": { ... } }
                    if (j.contains("values") && j["values"].contains("label"))
                        saw = j["values"]["label"].get<std::string>() == "Changed";
                } catch (...) {
                }
                TEST_ASSERT(saw, "widget PUT: GET reflects changed label");
                TEST_PASS("/api/widget/{path}/{prop}");
            }

            // GET /api/components — 组件 schema 列表
            {
                auto r = http_parse(http_send(p, "GET", "/api/components", ""));
                TEST_ASSERT(r.status == 200 && r.content_type == "application/json", "components: 200 JSON");
                bool ok = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(r.body);
                    ok = j.is_array();
                } catch (...) {
                }
                TEST_ASSERT(ok, "components: body parses as JSON array");
                TEST_PASS("/api/components");
            }

            // GET /api/yaml — 树 YAML 字符串
            {
                auto r = http_parse(http_send(p, "GET", "/api/yaml", ""));
                TEST_ASSERT(r.status == 200, "yaml: status 200");
                TEST_ASSERT(r.content_type == "text/yaml", "yaml: text/yaml content-type");
                TEST_ASSERT(!r.body.empty(), "yaml: non-empty body");
                TEST_PASS("/api/yaml");
            }

            // POST /api/to_code — 多 style 生成不同代码（无 style 默认 Fluent）
            {
                const std::string body =
                    R"({"node":{"type":"Column","props":{},"children":[{"type":"Button","props":{"label":"OK"},"children":[]}]}})";
                auto fluent = http_parse(http_send(p, "POST", "/api/to_code", body));
                TEST_ASSERT(fluent.status == 200 && fluent.content_type == "application/json", "to_code: 200 JSON");
                std::string fluent_code;
                bool ok = false;
                try {
                    nlohmann::json j = nlohmann::json::parse(fluent.body);
                    ok = j.contains("code") && j["code"].is_string();
                    fluent_code = j.value("code", std::string());
                } catch (...) {
                }
                TEST_ASSERT(ok, "to_code: body has code string");

                // style=1 (StepByStep) 与 style=2 (DesignatedInit) 应与 Fluent 不同
                auto sb = http_parse(
                    http_send(p, "POST", "/api/to_code", body.substr(0, body.size() - 1) + R"(,"style":1})"));
                auto di = http_parse(
                    http_send(p, "POST", "/api/to_code", body.substr(0, body.size() - 1) + R"(,"style":2})"));
                std::string sb_code, di_code;
                try {
                    nlohmann::json js = nlohmann::json::parse(sb.body);
                    nlohmann::json jd = nlohmann::json::parse(di.body);
                    sb_code = js.value("code", std::string());
                    di_code = jd.value("code", std::string());
                } catch (...) {
                }
                TEST_ASSERT(!sb_code.empty() && sb_code != fluent_code,
                            "to_code: style=1 (StepByStep) differs from Fluent");
                TEST_ASSERT(!di_code.empty() && di_code != fluent_code,
                            "to_code: style=2 (DesignatedInit) differs from Fluent");
                TEST_ASSERT(sb_code != di_code, "to_code: style=1 and style=2 produce different code");
                TEST_PASS("/api/to_code (multi-style)");
            }

            server.stop();
            TEST_ASSERT(!server.is_running(), "server should be stopped after Test 7");
        }
    }

    std::fprintf(stderr, "=== all tests passed ===\n");
    return 0;
}

#endif // AURORA_INSPECTOR_SERVER_ENABLED

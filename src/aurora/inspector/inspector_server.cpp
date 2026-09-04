// Aurora Inspector Server — minimal localhost-only HTTP server for remote widget tree access.
// Cross-platform: Winsock2 on Windows, BSD sockets on POSIX. Zero third-party dependencies.

#include "aurora/inspector/inspector_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
// Windows: Winsock2（须在 windows.h 之前）
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
// POSIX: BSD sockets
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

// ---- Winsock 词汇 → POSIX 映射：使下方主体逻辑（socket / bind / listen / accept /
//      recv / send / select / htonl / htons / ntohs / inet_addr）在两种平台编译运行一致 ----
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
constexpr int SD_BOTH = SHUT_RDWR;
struct WSADATA {
    int dummy = 0;
};
inline int WSAStartup(uint16_t, void *) { return 0; }
inline void WSACleanup() {}
inline int WSAGetLastError() { return errno; }
#define MAKEWORD(a, b) (0)
inline int closesocket(SOCKET s) { return ::close(s); }
// htonl / htons / ntohs / inet_addr / INADDR_LOOPBACK 由 <arpa/inet.h> / <netinet/in.h> 提供
#endif

#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>

#include "aurora/core/log.h"
#include "aurora/debug/debug_backend.h"
#include "aurora/debug/debug_paint.h"
#include "aurora/debug/debug_runtime.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/state/async.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/yaml.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

namespace aurora {

// ---------------------------------------------------------------------------
// Impl — 隐藏所有 Winsock2 类型
// ---------------------------------------------------------------------------
struct InspectorServer::Impl {
    std::function<Node()> root_getter;
    std::function<Surface *()> surface_getter;  // 可选：debug 端点访问运行时 Surface
    SOCKET listen_socket = INVALID_SOCKET;
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex tree_mutex;  // 保护 widget 树访问
    uint16_t bound_port = 0;

    void accept_loop();
    void handle_client(SOCKET client);

    // HTTP 路由：返回完整 HTTP 响应字符串
    auto route_request(const std::string &method, const std::string &path, const std::string &body) -> std::string;

    // HTTP 响应构造辅助
    static auto make_response(int status_code, const std::string &status_text, const std::string &content_type,
                              const std::string &body) -> std::string;
    static auto json_response(int status_code, const std::string &status_text, const nlohmann::json &j) -> std::string;
    static auto error_response(int status_code, const std::string &message) -> std::string;

    // URL 路径分段
    static auto split_path(const std::string &path) -> std::vector<std::string>;

    // 取请求头字段值（大小写不敏感，未找到返回空）
    static auto header_value(const std::string &request, std::string_view name) -> std::string;

    // Host 头是否指向本机回环（DNS rebinding 防护）
    static auto is_loopback_host(const std::string &host) -> bool;
};

// ---------------------------------------------------------------------------
// HTTP 响应辅助
// ---------------------------------------------------------------------------
auto InspectorServer::Impl::make_response(int status_code, const std::string &status_text,
                                          const std::string &content_type, const std::string &body) -> std::string {
    std::ostringstream os;
    // 刻意不发送 Access-Control-Allow-Origin：本服务无鉴权，仅以「连接来自 127.0.0.1」
    // 作为访问控制，而浏览器本身就在 127.0.0.1 上。若回 `ACAO: *`，开发者访问的任意网页
    // 都能用一次简单 GET 读走 /api/tree（含输入框文本等全部控件属性）并外传。
    // 省略该头后，浏览器会拦截跨源读取，本地 curl / 调试工具不受影响。
    os << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n"
       << "X-Content-Type-Options: nosniff\r\n"
       << "\r\n"
       << body;
    return os.str();
}

auto InspectorServer::Impl::json_response(int status_code, const std::string &status_text, const nlohmann::json &j)
    -> std::string {
    const std::string body = j.dump();
    return make_response(status_code, status_text, "application/json", body);
}

auto InspectorServer::Impl::error_response(int status_code, const std::string &message) -> std::string {
    nlohmann::json err = nlohmann::json::object();
    err["error"] = message;
    err["status"] = status_code;
    std::string status_text;
    switch (status_code) {
        case 400:
            status_text = "Bad Request";
            break;
        case 403:
            status_text = "Forbidden";
            break;
        case 404:
            status_text = "Not Found";
            break;
        case 405:
            status_text = "Method Not Allowed";
            break;
        case 413:
            status_text = "Payload Too Large";
            break;
        case 500:
            status_text = "Internal Server Error";
            break;
        default:
            status_text = "Error";
            break;
    }
    return json_response(status_code, status_text, err);
}

// ---------------------------------------------------------------------------
// URL 路径分段辅助
// ---------------------------------------------------------------------------
auto InspectorServer::Impl::split_path(const std::string &path) -> std::vector<std::string> {
    std::vector<std::string> segments;
    std::string::size_type start = 0;
    // 跳过前导 '/'
    if (!path.empty() && path[0] == '/') {
        start = 1;
    }
    while (start < path.size()) {
        auto pos = path.find('/', start);
        if (pos == std::string::npos) {
            pos = path.size();
        }
        std::string seg = path.substr(start, pos - start);
        if (!seg.empty()) {
            segments.push_back(std::move(seg));
        }
        start = pos + 1;
    }
    return segments;
}

// ---------------------------------------------------------------------------
// 请求头解析辅助
// ---------------------------------------------------------------------------
auto InspectorServer::Impl::header_value(const std::string &request, std::string_view name) -> std::string {
    const auto lower = [](std::string s) -> std::string {
        for (char &ch : s) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return s;
    };
    // 仅在头部区域内查找，避免 body 中的同名文本被误认作请求头。
    const auto head_end = request.find("\r\n\r\n");
    const std::string head = lower(request.substr(0, head_end == std::string::npos ? request.size() : head_end));
    const std::string key = "\r\n" + lower(std::string(name)) + ":";
    const auto pos = head.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    auto start = pos + key.size();
    while (start < head.size() && (head[start] == ' ' || head[start] == '\t')) {
        ++start;
    }
    auto end = head.find("\r\n", start);
    if (end == std::string::npos) {
        end = head.size();
    }
    return head.substr(start, end - start);
}

auto InspectorServer::Impl::is_loopback_host(const std::string &host) -> bool {
    // 去掉端口部分（IPv6 字面量形如 [::1]:6280）。
    std::string h = host;
    if (!h.empty() && h.front() == '[') {
        const auto close = h.find(']');
        h = (close == std::string::npos) ? h : h.substr(1, close - 1);
    } else if (const auto colon = h.rfind(':'); colon != std::string::npos) {
        h = h.substr(0, colon);
    }
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}

// ---------------------------------------------------------------------------
// 主线程 marshal
// ---------------------------------------------------------------------------
// 把调试读取（Surface / 树 / 全局状态）派发到主线程执行后回收结果。若进程级 main_poster
// 已安装（运行中的 Application），则经 poster 入队并阻塞等待主线程执行；否则（测试 / 无头 /
// 无事件循环）直接在当前线程执行——与 au::post_to_main 的回退语义一致，保证 Surface 的
// main-thread-only 约束不被违反，且无事件循环时仍可同步测试。
template <typename T>
static auto marshal_get(std::function<T()> fn) -> T {
    std::function<void(std::function<void()>)> poster;
    {
        std::scoped_lock lock(aurora::detail::main_poster_mutex());
        poster = aurora::detail::main_poster();
    }
    if (!poster) {
        return fn();
    }
    std::promise<T> p;
    auto f = p.get_future();
    poster([&]() -> auto {
        try {
            p.set_value(fn());
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    return f.get();
}

// 从 path（可能含 ? 查询串）提取路由段（'?' 之前部分）。
static auto strip_query(const std::string &path) -> std::string {
    const auto q = path.find('?');
    return (q == std::string::npos) ? path : path.substr(0, q);
}

// 解析单个查询参数值（如 query_param("/p?x=10&y=20", "x") → "10"）。
static auto query_param(const std::string &path, std::string_view key) -> std::string {
    const auto q = path.find('?');
    if (q == std::string::npos) {
        return {};
    }
    const std::string query = path.substr(q + 1);
    const std::string k{key};
    std::string::size_type start = 0;
    while (start < query.size()) {
        auto end = query.find('&', start);
        if (end == std::string::npos) {
            end = query.size();
        }
        const std::string pair = query.substr(start, end - start);
        const auto eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == k) {
            return pair.substr(eq + 1);
        }
        start = end + 1;
    }
    return {};
}

// 可靠发送：send 可能部分写入（响应体超过套接字缓冲时必然发生，如 snapshot PNG），
// 循环写满为止；对端断开 / 出错返回 false。
static auto send_all(SOCKET client, const std::string &data) -> bool {
    const char *p = data.data();
    std::size_t left = data.size();
    // NOLINTNEXTLINE(readability-identifier-naming): kChunk 为局部常量，命名依既有约定
    constexpr std::size_t kChunk = 1u << 20;  // 每次 ≤1MiB，避免 int 截断
    while (left > 0) {
        const int n = send(client, p, static_cast<int>(std::min(left, kChunk)), 0);
        if (n <= 0) {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 套接字发送按字节推进指针是有意设计
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// HTTP 路由
// ---------------------------------------------------------------------------
auto InspectorServer::Impl::route_request(const std::string &method, const std::string &path, const std::string &body)
    -> std::string {
    const std::string route = strip_query(path);

    // ---- 调试端点（封装 aurora::debug 被动门面，不重复实现逻辑）----
    // 所有 Surface / 树 / 全局状态读取经主线程 marshal（marshal_get），与 Surface 的
    // main-thread-only 约束一致；无事件循环时回退为直接执行（测试 / 无头）。

    // GET /api/debug/state — Surface 运行时状态
    if (route == "/api/debug/state") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/state");
        }
        if (!surface_getter) {
            return error_response(400, "Surface getter not configured");
        }
        Surface *s = surface_getter();
        if (s == nullptr) {
            return error_response(500, "Surface is null");
        }
        try {
            auto j = marshal_get<nlohmann::json>([&]() -> Json { return aurora::debug::surface_state(*s); });
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("surface_state failed: ") + e.what());
        }
    }

    // GET /api/debug/snapshot?source=fb|win — 截图 PNG（image/png）
    if (route == "/api/debug/snapshot") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/snapshot");
        }
        if (!surface_getter) {
            return error_response(400, "Surface getter not configured");
        }
        Surface *s = surface_getter();
        if (s == nullptr) {
            return error_response(500, "Surface is null");
        }
        const std::string src = query_param(path, "source");
        const auto capture_src =
            (src == "win") ? aurora::debug::CaptureSource::OnScreenWindow : aurora::debug::CaptureSource::Framebuffer;
        try {
            auto png = marshal_get<std::vector<std::uint8_t>>([&]() -> std::vector<std::uint8_t> {
                // 临时文件名须不可预测（固定名可被本地进程抢先创建/替换——符号链接攻击面），
                // 且每次请求唯一（避免并发/连续请求相互覆盖）。steady_clock 计数 + 进程内自增序号。
                static std::atomic<unsigned> seq{0};
                const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
                const std::string name =
                    "aurora_insp_snap_" + std::to_string(ticks) + "_" + std::to_string(seq.fetch_add(1)) + ".png";
                const std::string tmp = (std::filesystem::temp_directory_path() / name).string();
                const auto r = aurora::debug::capture(*s, tmp, capture_src);
                if (!r) {
                    throw std::runtime_error(r.error().message);
                }
                std::ifstream f(tmp, std::ios::binary);
                if (!f) {
                    std::error_code rm_ec;
                    std::filesystem::remove(tmp, rm_ec);  // 打开失败也清理，不留垃圾文件
                    throw std::runtime_error("cannot open captured PNG");
                }
                std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return bytes;
            });
            if (png.empty()) {
                return error_response(500, "snapshot produced empty PNG");
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 像素缓冲 uint8_t* → char*
            // 构造字符串是有意转换
            std::string body_bytes(reinterpret_cast<const char *>(png.data()), png.size());
            return make_response(200, "OK", "image/png", body_bytes);
        } catch (const std::exception &e) {
            return error_response(500, std::string("snapshot failed: ") + e.what());
        }
    }

    // GET /api/debug/perf — 性能快照（聚合 FrameStats + PerfLog）
    if (route == "/api/debug/perf") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/perf");
        }
        try {
            auto j = marshal_get<nlohmann::json>([]() -> Json { return aurora::debug::perf_snapshot(); });
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("perf_snapshot failed: ") + e.what());
        }
    }

    // GET /api/debug/timeline — 帧相位时间线
    if (route == "/api/debug/timeline") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/timeline");
        }
        try {
            auto j = marshal_get<nlohmann::json>([]() -> Json { return aurora::debug::frame_phase_timeline(); });
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("frame_phase_timeline failed: ") + e.what());
        }
    }

    // GET /api/debug/diagnostics — 诊断只读快照
    if (route == "/api/debug/diagnostics") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/diagnostics");
        }
        try {
            auto j = marshal_get<nlohmann::json>([]() -> Json { return aurora::debug::diagnostics(); });
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("diagnostics failed: ") + e.what());
        }
    }

    // GET /api/debug/why — why_trace（重排 / 重绘因果链）
    if (route == "/api/debug/why") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/why");
        }
        try {
            auto j = marshal_get<nlohmann::json>([]() -> Json { return aurora::debug::why_trace(); });
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("why_trace failed: ") + e.what());
        }
    }

    // GET /api/debug/tree — Widget 树 JSON
    if (route == "/api/debug/tree") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/tree");
        }
        std::scoped_lock lock(tree_mutex);
        Node root = root_getter();
        if (!root) {
            return error_response(500, "Widget tree root is null");
        }
        try {
            auto j = marshal_get<nlohmann::json>([&]() -> Json { return aurora::debug::widget_tree(root); });
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("widget_tree failed: ") + e.what());
        }
    }

    // GET /api/debug/pick?x=&y= — 控件拾取
    if (route == "/api/debug/pick") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/debug/pick");
        }
        std::scoped_lock lock(tree_mutex);
        Node root = root_getter();
        if (!root) {
            return error_response(500, "Widget tree root is null");
        }
        float x = 0.0F;
        float y = 0.0F;
        try {
            x = std::stof(query_param(path, "x"));
            y = std::stof(query_param(path, "y"));
        } catch (...) {
            return error_response(400, "pick requires numeric x and y query params");
        }
        try {
            auto res = marshal_get<aurora::debug::DebugPickResult>([&]() -> aurora::debug::DebugPickResult {
                // root_bounds：优先用 Surface 尺寸，否则用根控件尺寸
                aurora::Rect root_bounds{
                    .origin = aurora::Point{.x = 0.0F, .y = 0.0F},
                    .size = aurora::Size{.width = root->size().width, .height = root->size().height}};
                if (surface_getter) {
                    if (Surface *s = surface_getter()) {
                        const auto sz = s->size();
                        root_bounds = aurora::Rect{.origin = aurora::Point{.x = 0.0F, .y = 0.0F}, .size = sz};
                    }
                }
                aurora::BuildContext ctx;
                return aurora::debug::widget_picker(root.widget(), root_bounds, ctx, aurora::Point{.x = x, .y = y});
            });
            nlohmann::json j;
            j["hit"] = res.hit;
            j["chain"] = nlohmann::json::array();
            for (const auto &n : res.chain) {
                nlohmann::json node;
                node["type_name"] = n.type_name;
                node["bounds"] = nlohmann::json{{"x", n.bounds.origin.x},
                                                {"y", n.bounds.origin.y},
                                                {"w", n.bounds.size.width},
                                                {"h", n.bounds.size.height}};
                j["chain"].push_back(std::move(node));
            }
            return json_response(200, "OK", j);
        } catch (const std::exception &e) {
            return error_response(500, std::string("pick failed: ") + e.what());
        }
    }

    // POST /api/debug/flags — 运行时设置 DebugPaintFlags（实时开关叠层）
    if (route == "/api/debug/flags") {
        if (method != "POST") {
            return error_response(405, "Method not allowed for /api/debug/flags");
        }
        nlohmann::json input;
        try {
            input = nlohmann::json::parse(body);
        } catch (const nlohmann::json::parse_error &e) {
            return error_response(400, std::string("Invalid JSON body: ") + e.what());
        }
        // 请求体来自网络（不可信输入）。此前 get<bool>() 在字段类型不符时抛
        // nlohmann::type_error，未捕获即 worker 线程 std::terminate 整个应用——
        // 本地任意进程发 {"layout_guides":"x"} 即可崩溃宿主。改为显式校验回 400。
        if (!input.is_object()) {
            return error_response(400, "flags body must be a JSON object");
        }
        auto read_flag = [&input](const char *key, bool &dst) -> bool {
            const auto it = input.find(key);
            if (it == input.end()) {
                return true;  // 字段缺省保持默认值
            }
            if (!it->is_boolean()) {
                return false;
            }
            dst = it->get<bool>();
            return true;
        };
        aurora::debug::DebugPaintFlags f;
        if (!read_flag("layout_guides", f.layout_guides) || !read_flag("relayout_boundaries", f.relayout_boundaries) ||
            !read_flag("layer_borders", f.layer_borders) || !read_flag("repaint_highlight", f.repaint_highlight) ||
            !read_flag("overdraw", f.overdraw)) {
            return error_response(400, "flag fields must be booleans");
        }
        aurora::debug::set_flags(f);  // 全局写入；下一次 present_root 即绘制叠层
        nlohmann::json ok = nlohmann::json::object();
        ok["status"] = "ok";
        ok["flags"] = nlohmann::json{{"layout_guides", f.layout_guides},
                                     {"relayout_boundaries", f.relayout_boundaries},
                                     {"layer_borders", f.layer_borders},
                                     {"repaint_highlight", f.repaint_highlight},
                                     {"overdraw", f.overdraw}};
        return json_response(200, "OK", ok);
    }

    // GET /api/tree — 完整 widget 树 JSON（route 已剥离 query，/api/tree?x=1 不再误 404）
    if (route == "/api/tree") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/tree");
        }
        std::scoped_lock lock(tree_mutex);
        Node root = root_getter();
        if (!root) {
            return error_response(500, "Widget tree root is null");
        }
        nlohmann::json tree = Inspector::tree_json_full(root);
        return json_response(200, "OK", tree);
    }

    // GET /api/widget/{path} — 单 widget 属性
    // PUT /api/widget/{path}/{prop} — 回写属性
    // 路径段取自剥离 query 后的 route，避免查询串混入树路径。
    if (route.starts_with("/api/widget/")) {
        const std::string remainder = route.substr(std::string("/api/widget/").size());
        auto segments = split_path(remainder);
        if (segments.empty()) {
            return error_response(400, "Widget path is empty");
        }

        std::scoped_lock lock(tree_mutex);
        Node root = root_getter();
        if (!root) {
            return error_response(500, "Widget tree root is null");
        }

        if (method == "GET") {
            // 整个 remainder 作为树路径
            Node target = Inspector::find_node(root, remainder);
            if (!target) {
                return error_response(404, "Widget not found at path: " + remainder);
            }
            nlohmann::json props = Inspector::get_prop(target.widget());
            return json_response(200, "OK", props);
        }

        if (method == "PUT") {
            // 最后一段是属性名，前面是树路径
            if (segments.size() < 2) {
                return error_response(400, "PUT requires /api/widget/{tree_path}/{prop_name}");
            }
            const std::string &prop_name = segments.back();
            // 树路径 = 除最后一段外的所有段
            std::string tree_path;
            for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
                if (i > 0) {
                    tree_path += '/';
                }
                tree_path += segments[i];
            }
            Node target = Inspector::find_node(root, tree_path);
            if (!target) {
                return error_response(404, "Widget not found at path: " + tree_path);
            }

            // 解析 body 为 JSON value
            nlohmann::json value;
            try {
                value = nlohmann::json::parse(body);
            } catch (const nlohmann::json::parse_error &e) {
                return error_response(400, std::string("Invalid JSON body: ") + e.what());
            }
            auto result = Inspector::set_prop(target.widget(), prop_name, value);
            if (!result) {
                return error_response(400, "Failed to set property: " + result.error().message);
            }
            nlohmann::json ok = nlohmann::json::object();
            ok["status"] = "ok";
            ok["widget_path"] = tree_path;
            ok["property"] = prop_name;
            return json_response(200, "OK", ok);
        }

        return error_response(405, "Method not allowed for /api/widget");
    }

    // GET /api/components — 组件 schema 列表
    if (route == "/api/components") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/components");
        }
        std::vector<nlohmann::json> schemas = Inspector::components();
        nlohmann::json arr = nlohmann::json::array();
        for (auto &s : schemas) {
            arr.push_back(std::move(s));
        }
        return json_response(200, "OK", arr);
    }

    // GET /api/yaml — 当前树的 YAML 格式
    if (route == "/api/yaml") {
        if (method != "GET") {
            return error_response(405, "Method not allowed for /api/yaml");
        }
        std::scoped_lock lock(tree_mutex);
        Node root = root_getter();
        if (!root) {
            return error_response(500, "Widget tree root is null");
        }
        Json tree = Inspector::tree_json_full(root);
        std::string yaml = aurora::serialization::to_yaml(tree);
        return make_response(200, "OK", "text/yaml", yaml);
    }

    // POST /api/to_code — UI 树 → C++ 代码
    // 注意：路由匹配用 strip_query 后的 route，与其它端点一致（/api/to_code?x=1 不再误 404）。
    if (route == "/api/to_code") {
        if (method != "POST") {
            return error_response(405, "Method not allowed for /api/to_code");
        }
        nlohmann::json input;
        try {
            input = nlohmann::json::parse(body);
        } catch (const nlohmann::json::parse_error &e) {
            return error_response(400, std::string("Invalid JSON body: ") + e.what());
        }
        // 输入可以是完整树或仅节点
        const nlohmann::json &node = input.is_object() && input.contains("node") ? input["node"] : input;
        // 契约：请求体可含 style（0=Fluent/1=StepByStep/2=DesignatedInit），默认 Fluent。
        // 非法数值回退 Fluent（向后兼容）；字段存在但非整数时此前经 value("style", 0)
        // 抛 type_error 未捕获而 terminate，现显式校验回 400。
        int style_int = 0;
        if (input.is_object()) {
            const auto it = input.find("style");
            if (it != input.end()) {
                if (!it->is_number_integer()) {
                    return error_response(400, "style must be an integer (0=Fluent, 1=StepByStep, 2=DesignatedInit)");
                }
                style_int = it->get<int>();
                if (style_int < 0 || style_int > static_cast<int>(serialization::CodeStyle::DesignatedInit)) {
                    style_int = 0;  // 越界回退 Fluent
                }
            }
        }
        const auto style = static_cast<serialization::CodeStyle>(style_int);
        // 使用 codegen.h 的 to_code（需要 Json 输入；按 style 生成多风格代码）
        const std::string code = serialization::to_code(node, style);
        nlohmann::json result = nlohmann::json::object();
        result["code"] = code;
        return json_response(200, "OK", result);
    }

    return error_response(404, "Unknown endpoint: " + path);
}

// ---------------------------------------------------------------------------
// 客户端连接处理
// ---------------------------------------------------------------------------
void InspectorServer::Impl::handle_client(SOCKET client) {
    // 读取请求数据（简单实现：一次 recv 足够处理小请求）
    // NOLINTNEXTLINE(readability-identifier-naming): kBufSize 为局部常量，命名依既有约定
    constexpr int kBufSize = 8192;
    char buf[kBufSize];
    std::string request;
    int total = 0;

    // 先读取头部
    while (total < kBufSize - 1) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 套接字接收按字节推进指针是有意设计
        const int n = recv(client, buf + total, kBufSize - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): 定长请求缓冲按运行时常量下标访问是有意设计
        buf[total] = '\0';
        // 检查是否已收到完整头部（以 \r\n\r\n 结束）
        if (std::string(buf, total).find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }

    if (total <= 0) {
        closesocket(client);
        return;
    }

    request.assign(buf, total);

    // 解析请求行：METHOD /path HTTP/1.1
    std::string method;
    std::string req_path;
    {
        auto sp1 = request.find(' ');
        if (sp1 == std::string::npos) {
            const std::string resp = error_response(400, "Malformed request line");
            send_all(client, resp);
            closesocket(client);
            return;
        }
        method = request.substr(0, sp1);
        auto sp2 = request.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) {
            const std::string resp = error_response(400, "Malformed request line");
            send_all(client, resp);
            closesocket(client);
            return;
        }
        req_path = request.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    // DNS rebinding 防护：仅接受 Host 指向回环地址的请求。攻击者控制的域名可以解析到
    // 127.0.0.1，从而让浏览器把本服务当作同源目标；此时 Host 头仍是攻击者域名。
    // 同时拒绝带 Origin 头的请求——本服务只服务本机调试工具，不服务任何网页。
    {
        const std::string host = header_value(request, "Host");
        if (host.empty() || !is_loopback_host(host)) {  // 缺失 Host 同样拒绝（HTTP/1.1 客户端必发）
            AURORA_LOG_WARN("inspector", "Rejecting request with non-loopback Host header");
            const std::string resp = error_response(403, "Forbidden: unexpected Host header");
            send_all(client, resp);
            closesocket(client);
            return;
        }
        if (!header_value(request, "Origin").empty()) {
            AURORA_LOG_WARN("inspector", "Rejecting cross-origin (browser) request");
            const std::string resp = error_response(403, "Forbidden: browser origins are not allowed");
            send_all(client, resp);
            closesocket(client);
            return;
        }
    }

    // 解析 Content-Length（用于 POST/PUT body）。复用 header_value()：仅在头部区域内
    // 大小写不敏感检索——此前在整个请求（含 body）中 find("Content-Length:")，body 中
    // 的同名文本可被误认作请求头。解析失败按 0 处理。
    // 上限保护：content_length 来自请求头（不可信输入）。无上限时一个本地进程即可用
    // "Content-Length: 2000000000" 触发 body_buf 巨量分配——分配失败抛出的 bad_alloc
    // 在 worker 线程未被捕获会 std::terminate 整个应用；即便成功也构成内存耗尽 DoS。
    // NOLINTNEXTLINE(readability-identifier-naming): kMaxBodyBytes 为局部常量，命名依既有约定
    constexpr int kMaxBodyBytes = 4 * 1024 * 1024;  // 4MiB，远超任何合法 Inspector 请求
    int content_length = 0;
    {
        const std::string cl_value = header_value(request, "Content-Length");
        if (!cl_value.empty()) {
            try {
                content_length = std::stoi(cl_value);
            } catch (...) {
                content_length = 0;
            }
        }
    }
    if (content_length < 0 || content_length > kMaxBodyBytes) {
        AURORA_LOG_WARN("inspector", "Rejecting oversized request body: ", content_length);
        const std::string resp = error_response(413, "Payload Too Large");
        send_all(client, resp);
        closesocket(client);
        return;
    }

    // 读取 body（如果 Content-Length > 已读数据）
    auto header_end = request.find("\r\n\r\n");
    std::string body;
    if (header_end != std::string::npos) {
        body = request.substr(header_end + 4);
    }
    const int body_needed = content_length - static_cast<int>(body.size());
    if (body_needed > 0) {
        // 继续读取 body 剩余部分
        std::vector<char> body_buf(body_needed);
        int received = 0;
        while (received < body_needed) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 套接字接收按字节推进指针是有意设计
            const int n = recv(client, body_buf.data() + received, body_needed - received, 0);
            if (n <= 0) {
                break;
            }
            received += n;
        }
        body.append(body_buf.data(), received);
    }

    // 路由并发送响应。路由内部虽对已知输入做显式校验，仍可能有未预见的异常
    // （JSON 深层结构、std::bad_alloc 等）——worker 线程未捕获异常会 std::terminate
    // 整个应用，此处兜底转换为 500 响应。
    std::string response;
    try {
        response = route_request(method, req_path, body);
    } catch (const std::exception &e) {
        AURORA_LOG_ERROR("inspector", "route_request failed: ", e.what());
        response = error_response(500, std::string("internal error: ") + e.what());
    } catch (...) {
        response = error_response(500, "internal error");
    }
    send_all(client, response);
    closesocket(client);
}

// ---------------------------------------------------------------------------
// accept 循环（worker 线程）
// ---------------------------------------------------------------------------
void InspectorServer::Impl::accept_loop() {
    AURORA_LOG_INFO("inspector", "InspectorServer accept loop started on port ", bound_port);
    while (running.load()) {
        // 使用 select 使 accept 可中断（超时 200ms 检查 running 标志）
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);
        struct timeval tv{};  // 值初始化，随后显式设置超时
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 200ms

#ifdef AURORA_PLATFORM_WINDOWS
        const int sel = select(0, &read_fds, nullptr, nullptr, &tv);
#else
        // POSIX 要求 nfds = 最高 fd + 1（Windows 忽略该参数）
        const int sel = select(listen_socket + 1, &read_fds, nullptr, nullptr, &tv);
#endif
        if (sel < 0) {
            if (!running.load()) {
                break;
            }
            const int err = WSAGetLastError();
            AURORA_LOG_ERROR("inspector", "select() failed with error: ", err);
            break;
        }
        if (sel == 0) {
            continue;  // timeout，检查 running
        }

        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): WinSock API 要求 sockaddr* 强转，无法避免
        SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr *>(&client_addr), &addr_len);
        // NOLINTNEXTLINE(modernize-use-integer-sign-comparison): SOCKET 与 INVALID_SOCKET 的 WinSock 惯用比较
        if (client == INVALID_SOCKET) {
            if (!running.load()) {
                break;
            }
            AURORA_LOG_WARN("inspector", "accept() failed, error: ", WSAGetLastError());
            continue;
        }

        // 验证连接来自 localhost
        if (client_addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            AURORA_LOG_WARN("inspector", "Rejecting non-localhost connection");
            closesocket(client);
            continue;
        }

        handle_client(client);
    }
    AURORA_LOG_INFO("inspector", "InspectorServer accept loop exiting");
}

// ---------------------------------------------------------------------------
// InspectorServer 公共接口
// ---------------------------------------------------------------------------
InspectorServer::InspectorServer(std::function<Node()> root_getter) : impl_(std::make_unique<Impl>()) {
    impl_->root_getter = std::move(root_getter);
}

InspectorServer::~InspectorServer() { stop(); }

auto InspectorServer::start(uint16_t port) const -> bool {
    if (impl_->running.load()) {
        AURORA_LOG_WARN("inspector", "InspectorServer already running on port ", impl_->bound_port);
        return false;
    }

    // 初始化 Winsock
    WSADATA wsa_data;
    const int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        AURORA_LOG_ERROR("inspector", "WSAStartup failed with error: ", wsa_result);
        return false;
    }

    // 创建 TCP socket
    impl_->listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison): SOCKET 与 INVALID_SOCKET 的 WinSock 惯用比较
    if (impl_->listen_socket == INVALID_SOCKET) {
        AURORA_LOG_ERROR("inspector", "socket() failed with error: ", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // 绑定到 localhost（仅本机访问）
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
    addr.sin_port = htons(port);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): WinSock API 要求 sockaddr* 强转，无法避免
    if (bind(impl_->listen_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        AURORA_LOG_ERROR("inspector", "bind() failed on port ", port, ", error: ", WSAGetLastError());
        closesocket(impl_->listen_socket);
        impl_->listen_socket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(impl_->listen_socket, 5) == SOCKET_ERROR) {
        AURORA_LOG_ERROR("inspector", "listen() failed, error: ", WSAGetLastError());
        closesocket(impl_->listen_socket);
        impl_->listen_socket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    // 获取实际绑定的端口（若传入 0 则系统分配）
    sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): WinSock API 要求 sockaddr* 强转，无法避免
    if (getsockname(impl_->listen_socket, reinterpret_cast<sockaddr *>(&bound_addr), &addr_len) == 0) {
        impl_->bound_port = ntohs(bound_addr.sin_port);
    } else {
        impl_->bound_port = port;
    }

    impl_->running.store(true);
    impl_->worker = std::thread([this]() -> void { impl_->accept_loop(); });

    AURORA_LOG_INFO("inspector", "InspectorServer started on http://127.0.0.1:", impl_->bound_port);
    return true;
}

void InspectorServer::stop() const {
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison): SOCKET 与 INVALID_SOCKET 的 WinSock 惯用比较
    if (!impl_->running.load() && impl_->listen_socket == INVALID_SOCKET) {
        return;
    }

    AURORA_LOG_INFO("inspector", "Stopping InspectorServer...");
    impl_->running.store(false);

    // 先 shutdown 再 closesocket，确保 worker 线程的 select/accept 被中断
    // NOLINTNEXTLINE(modernize-use-integer-sign-comparison): SOCKET 与 INVALID_SOCKET 的 WinSock 惯用比较
    if (impl_->listen_socket != INVALID_SOCKET) {
        ::shutdown(impl_->listen_socket, SD_BOTH);
        closesocket(impl_->listen_socket);
        impl_->listen_socket = INVALID_SOCKET;
    }

    // 等待 worker 线程退出
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    WSACleanup();
    impl_->bound_port = 0;
    AURORA_LOG_INFO("inspector", "InspectorServer stopped");
}

auto InspectorServer::is_running() const -> bool { return impl_->running.load(); }

auto InspectorServer::port() const -> uint16_t { return impl_->bound_port; }

void InspectorServer::set_surface_getter(std::function<Surface *()> getter) const {
    impl_->surface_getter = std::move(getter);
}

}  // namespace aurora

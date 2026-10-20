/// 测试类型: unit
/// 目标单元: include/aurora/inspector/inspector_server.h
/// 测试说明: 覆盖 InspectorServer 生命周期与 HTTP 基本路径——初始停机态、start(0) 随机端口
/// 启停、重复 start 失败、stop 幂等、析构收编 worker、/api/tree 与 /api/components 的
/// 请求-响应、404/405/400/403 错误请求、/api/debug/state 的 surface getter 装配错误路径，
/// 以及 /api/input/{click,scroll,drag,text} 交互模拟（派发到目标控件、请求体校验、目标定位
/// 失败与派发失败的错误映射）和 /api/find 的 key/type/text 定位（路径与寻址端点同口径）。
/// 树导出脱敏同样在此覆盖：安装策略后四面导出折叠 props/values 的字符串叶子、放行键逐条生效、
/// 结构键保持可读，未安装策略时输出与改造前逐字节相同，纯输入变换的 /api/to_code 不参与折叠。
/// 树端点的线程落点同样在此覆盖：装上传递器后必须换到主线程下树，无传递器时回退内联执行。
/// 端口一律用 0（系统分配临时端口，无冲突）；无文件句柄副作用。客户端经同步阻塞传输层
/// `aurora::net::blocking`，随用例作用域关闭清理。
/// AURORA_BUILD_INSPECTOR_SERVER=OFF，或无线程能力构建（Emscripten 未开 -pthread，AURORA_CAP_THREADS=0），
/// 或 wasm 平台（AURORA_PLATFORM_WASM）时，整文件降级为 skip 桩——服务既依赖后台 worker 线程，
/// 也依赖 bind/listen/accept 的 BSD socket 语义；wasm 即便开了 -pthread 也只补上线程、不补套接字，
/// 故此处须按平台（而非仅按线程能力）判定，否则 worker 会阻塞在永不就绪的套接字操作上。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aurora/core/json.h"  // 端点响应体的结构化断言
#include "aurora/core/platform.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

#ifdef AURORA_BUILD_INSPECTOR_SERVER

#include "aurora/event/event.h"  // ScrollEvent
#include "aurora/inspector/inspector_server.h"
#include "aurora/net/blocking.h"
#include "aurora/net/detail/observability_trace.h"  // 夹具：直接向进程级活动存储写入一笔交换
#include "aurora/net/http_observability.h"
#include "aurora/net/observability.h"
#include "aurora/state/async.h"  // detail::main_poster（跨线程投递器，marshal 的落点）
#include "aurora/widget/button.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"
#include "aurora/widget/widget.h"  // LeafWidget（滚动探针基类）
#include "aurora/window/surface.h"  // set_surface_getter 的 Surface 完整类型

#endif  // AURORA_BUILD_INSPECTOR_SERVER

namespace aurora::test_cases::utest_inspector_server {

#if defined(AURORA_BUILD_INSPECTOR_SERVER) && AURORA_CAP_THREADS && !defined(AURORA_PLATFORM_WASM)
namespace {

/// @brief 共享测试树：Column 根 + Text 子节点（静态存储期，供 worker 线程 root_getter 读取）。
auto shared_tree() -> std::shared_ptr<Column> & {
    static std::shared_ptr<Column> tree = []() -> std::shared_ptr<aurora::Column> {
        auto col = std::make_shared<Column>();
        col->add(Node{std::make_shared<Text>("hello")});
        return col;
    }();
    return tree;
}

/// @brief root_getter：每次请求返回共享树的 Node 副本。
auto tree_getter() -> Node { return Node{shared_tree()}; }

/// @brief `recording_tree_getter` 的调用线程流水。
auto getter_threads() -> std::vector<std::thread::id> & {
    static std::vector<std::thread::id> threads;
    return threads;
}

/// @brief 记录每次调用线程的 root_getter（配合 `QueuedPoster` 证明树访问确实换了线程）。
auto recording_tree_getter() -> Node {
    getter_threads().push_back(std::this_thread::get_id());
    return tree_getter();
}

/// @brief 「只入队、由用例主线程排水」的跨线程投递器：装上它，`marshal_get` 才真的换线程执行。
/// 析构时复原原投递器，避免污染同进程内的其它用例。
struct QueuedPoster {
    QueuedPoster() {
        std::scoped_lock lock(aurora::detail::main_poster_mutex());
        // 刻意在持锁后才读：早读会拿到随后被别的安装者换掉的投递器（初始化列表在加锁前求值）。
        previous = aurora::detail::main_poster();  // NOLINT(cppcoreguidelines-prefer-member-initializer)
        aurora::detail::main_poster() = [this](std::function<void()> fn) -> void {
            {
                std::scoped_lock task_lock(mutex);
                tasks.push_back(std::move(fn));
            }
            cv.notify_all();
        };
    }
    ~QueuedPoster() {
        std::scoped_lock lock(aurora::detail::main_poster_mutex());
        aurora::detail::main_poster() = previous;
    }
    QueuedPoster(const QueuedPoster &) = delete;
    auto operator=(const QueuedPoster &) -> QueuedPoster & = delete;
    // 移动同样禁止：析构时按 `previous` 复原安装点，移动后的壳对象会留下悬空的安装/复原配对。
    QueuedPoster(QueuedPoster &&) = delete;
    auto operator=(QueuedPoster &&) -> QueuedPoster & = delete;

    /// @brief 在当前线程执行一个排队任务；`timeout_ms` 内无任务返回 false（调用方据此判失败）。
    auto drain_one(int timeout_ms = 5000) -> bool {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !tasks.empty(); })) {
                return false;
            }
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        task();
        return true;
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    std::function<void(std::function<void()>)> previous;
};

/// @brief 对 127.0.0.1:port 发送原始 HTTP 请求并回收完整响应（服务端 Connection: close，
/// 读到对端关闭即完整）。`recv_timeout_ms > 0` 时给收发加超时（`aurora::net::blocking` 毫秒统一口径），
/// 超时按空响应失败。
[[nodiscard]] auto http_roundtrip(std::uint16_t port, const std::string &request, int recv_timeout_ms = 0)
    -> std::string {
    auto connected = aurora::net::blocking::StreamSocket::connect_loopback(port);
    AURORA_TEST_REQUIRE_TRUE(static_cast<bool>(connected));
    auto client = std::move(connected.value());
    if (recv_timeout_ms > 0) {
        static_cast<void>(client.set_io_timeout(std::chrono::milliseconds{recv_timeout_ms}));
    }
    AURORA_TEST_REQUIRE_TRUE(static_cast<bool>(client.write_all(std::span<const char>(request))));

    std::string response;
    char buf[4096];
    for (;;) {
        const auto got = client.read_some(std::span<char>(buf));
        if (!got || got.value() == 0) {
            break;  // 出错 / 超时 / 对端关闭
        }
        response.append(buf, got.value());
        if (response.size() > (1U << 20U)) {
            break;  // 安全上限，防异常服务端无限输出
        }
    }
    client.close();
    AURORA_TEST_REQUIRE_FALSE(response.empty());
    return response;
}

/// @brief 带 Host 头的 GET 便捷封装。
[[nodiscard]] auto http_get(std::uint16_t port, const std::string &target) -> std::string {
    return http_roundtrip(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
}

/// @brief 带接收超时的 GET：供非用例主线程调用 —— 服务端不回响应时自行退出，不把 runner 挂死。
[[nodiscard]] auto http_get_timed(std::uint16_t port, const std::string &target, int timeout_ms) -> std::string {
    return http_roundtrip(port, "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", timeout_ms);
}

/// @brief 带 Host 头与 JSON body 的 POST 便捷封装。
[[nodiscard]] auto http_post(std::uint16_t port, const std::string &target, const std::string &body) -> std::string {
    return http_roundtrip(port, "POST " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
                                    "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                                    "\r\n\r\n" + body);
}

/// @brief 滚动探针：把 `on_scroll` 命中次数与末次 `delta_y` 经序列化属性外显。
///
/// 真实 `Scroll::offset_y()` 不在其序列化属性里（`Scroll` 只序列化 `step`），故 HTTP 面
/// 读不回滚动位置。要验证「滚动请求确实落到目标控件并改变了它的状态」，最直接的判据是
/// 让目标控件自己把可观测状态作为属性发布出来——这正是本探针的用途。
class ScrollProbe : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "ScrollProbe"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 40.0F, .height = 40.0F});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, Color{180, 180, 180, 255});
    }
    auto on_scroll(ScrollEvent &e) -> void override {
        ++scroll_hits_;
        last_delta_y_ = e.delta_y;
        e.is_handled = true;
    }
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("scroll_hits", scroll_hits_);
        props.set("last_delta_y", last_delta_y_);
    }

  private:
    int scroll_hits_ = 0;
    float last_delta_y_ = 0.0F;
};

/// @brief 拖拽探针：把 `on_pointer_event` 的 Press/Move/Release 命中次数与 Move 相对 Press
/// 的位移经序列化属性外显。
///
/// `Inspector::simulate_drag` 派发 Press（中心）→ Move（中心+delta）→ Release（终点），
/// 据此确认合成拖拽的三段事件确实落到目标控件且位移与请求一致。
class DragProbe : public LeafWidget {
  public:
    [[nodiscard]] auto type_name() const -> const char * override { return "DragProbe"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 40.0F, .height = 40.0F});
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, Color{120, 120, 120, 255});
    }
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press) {
            ++press_hits_;
            press_x_ = e.position.x;
            press_y_ = e.position.y;
        } else if (e.action == MouseAction::Move) {
            ++move_hits_;
            move_dx_ = e.position.x - press_x_;
            move_dy_ = e.position.y - press_y_;
        } else if (e.action == MouseAction::Release) {
            ++release_hits_;
        }
        e.is_handled = true;
    }
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("press_hits", press_hits_);
        props.set("move_hits", move_hits_);
        props.set("release_hits", release_hits_);
        props.set("move_dx", move_dx_);
        props.set("move_dy", move_dy_);
    }

  private:
    int press_hits_ = 0;
    int move_hits_ = 0;
    int release_hits_ = 0;
    float press_x_ = 0.0F;
    float press_y_ = 0.0F;
    float move_dx_ = 0.0F;
    float move_dy_ = 0.0F;
};

/// @brief 交互模拟用例的树：Column[ Checkbox(0) / TextInput(1) / ScrollProbe(2) / DragProbe(3) ]。
///
/// 逐用例自建而非复用静态共享树：模拟会改控件状态，静态树会让状态在用例间泄漏
/// （`--repeat` 下尤其明显）。`root` 由 shared_ptr 持有，供 HTTP 工作线程经
/// root_getter 读取。
struct InputTree {
    std::shared_ptr<Node> root;
    std::shared_ptr<Checkbox> checkbox;
    std::shared_ptr<TextInput> input;
    std::shared_ptr<ScrollProbe> scroller;
    std::shared_ptr<DragProbe> dragger;

    /// @brief 供 InspectorServer 使用的取值函数（按值返回 Node 副本即共享底层控件）。
    [[nodiscard]] auto getter() const -> std::function<Node()> {
        const std::shared_ptr<Node> held = root;
        return [held]() -> Node { return *held; };
    }
};

// 分析器把 MSVC STL 的 `make_shared`（内部 `new _Ref_count_obj2`）当裸 new，又认不出聚合返回时
// shared_ptr 已随 `.root` 移交所有权，于是虚报「`_Rep` 泄漏」。实测：本套件 --repeat=200（4200 次
// 构造 + 每用例一棵 Column/Checkbox/TextInput/ScrollProbe 树）进程驻留内存平台化——峰值 19.2 MB、
// 末值 13.5 MB 不随轮次上涨，且真泄漏时控件树会被 control block 拖住不退。故确认为误报，就地豁免。
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
[[nodiscard]] auto make_input_tree() -> InputTree {
    auto checkbox = std::make_shared<Checkbox>();
    auto input = std::make_shared<TextInput>();
    auto scroller = std::make_shared<ScrollProbe>();
    auto dragger = std::make_shared<DragProbe>();
    auto col = std::make_shared<Column>();
    col->add(Node{checkbox});
    col->add(Node{input});
    col->add(Node{scroller});
    col->add(Node{dragger});
    return InputTree{
        .root = std::make_shared<Node>(Node{col}),
        .checkbox = checkbox,
        .input = input,
        .scroller = scroller,
        .dragger = dragger,
    };
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

/// @brief /api/find 用例的树：Column[ Button"确定"(0, id=ok) / TextInput(1) /
///         Column[ Button"取消"(0, id=cancel) ] (2) ]——覆盖嵌套路径与多命中。
///
/// 逐用例自建（find 虽只读，但保持「每用例一棵树」的套件约定）。id 经外层 Node
/// `set_id` 后再入列，容器存储的 Node 副本携带该标识。
struct FindTree {
    std::shared_ptr<Node> root;
    std::shared_ptr<Button> ok;
    std::shared_ptr<TextInput> input;
    std::shared_ptr<Button> cancel;

    [[nodiscard]] auto getter() const -> std::function<Node()> {
        const std::shared_ptr<Node> held = root;
        return [held]() -> Node { return *held; };
    }
};

// 分析器同 make_input_tree 的误报与豁免理由（共享控件 + 聚合返回），就地豁免。
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
[[nodiscard]] auto make_find_tree() -> FindTree {
    // CJK-LITERAL: cjk-fixture - Han button labels let /api/find exercise UTF-8 query matching
    auto ok_button = std::make_shared<Button>("确定");
    auto input = std::make_shared<TextInput>();
    auto cancel_button = std::make_shared<Button>("取消");
    auto inner = std::make_shared<Column>();
    Node cancel_node{cancel_button};
    cancel_node.set_id("cancel");
    inner->add(cancel_node);  // add 只收 const 引用（拷贝入列），std::move 无效
    auto col = std::make_shared<Column>();
    Node ok_node{ok_button};
    ok_node.set_id("ok");
    col->add(ok_node);
    col->add(Node{input});
    col->add(Node{inner});
    return FindTree{
        .root = std::make_shared<Node>(Node{col}),
        .ok = ok_button,
        .input = input,
        .cancel = cancel_button,
    };
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

/// @brief 拒绝一切 Sensitive 的导出策略：`present` 原样返回，故导出里看到的恒是库给出的折叠形态。
class DenySensitivePolicy final : public aurora::net::ExportPolicy {
  public:
    [[nodiscard]] auto present(aurora::net::ExportSurface /*surface*/, std::string_view /*key*/,
                               aurora::net::FieldClass /*field_class*/, std::string_view value) const
        -> std::string override {
        return std::string{value};
    }
};

/// @brief 只放行给定键的导出策略；键集合之外的 Sensitive 仍折叠。
class AllowSelectedKeysPolicy final : public aurora::net::ExportPolicy {
  public:
    /// @brief 构造。
    /// @param keys 允许放行的键词汇（仅 `InspectorJson` 面生效）。
    explicit AllowSelectedKeysPolicy(std::vector<std::string> keys) : keys_(std::move(keys)) {}

    [[nodiscard]] auto allows(aurora::net::ExportSurface surface, std::string_view key,
                              aurora::net::FieldClass /*field_class*/) const -> bool override {
        return surface == aurora::net::ExportSurface::InspectorJson && std::ranges::find(keys_, key) != keys_.end();
    }

    [[nodiscard]] auto present(aurora::net::ExportSurface /*surface*/, std::string_view /*key*/,
                               aurora::net::FieldClass /*field_class*/, std::string_view value) const
        -> std::string override {
        return std::string{value};
    }

  private:
    std::vector<std::string> keys_;  ///< 放行键集合
};

/// @brief 安装一枚进程级导出策略并在作用域结束时清回「未设」。
///
/// 清回是判据的一部分：未安装策略时 Inspector 的整条 walk 不执行，导出与改造前逐字节相同。
class ScopedExportPolicy {
  public:
    /// @brief 安装。
    /// @param policy 策略持有；为空即清回「未设」。
    explicit ScopedExportPolicy(std::shared_ptr<const aurora::net::ExportPolicy> policy) {
        aurora::net::install_export_policy(std::move(policy));
    }
    ~ScopedExportPolicy() { aurora::net::install_export_policy(nullptr); }
    ScopedExportPolicy(const ScopedExportPolicy &) = delete;
    auto operator=(const ScopedExportPolicy &) -> ScopedExportPolicy & = delete;
    ScopedExportPolicy(ScopedExportPolicy &&) = delete;
    auto operator=(ScopedExportPolicy &&) -> ScopedExportPolicy & = delete;
};

/// @brief 每例前后把进程级活动存储与配置清回缺省，避免用例间串味（Release 下两枚函数是 no-op）。
class ScopedActivityReset {
  public:
    ScopedActivityReset() { reset(); }
    ~ScopedActivityReset() { reset(); }
    ScopedActivityReset(const ScopedActivityReset &) = delete;
    auto operator=(const ScopedActivityReset &) -> ScopedActivityReset & = delete;
    ScopedActivityReset(ScopedActivityReset &&) = delete;
    auto operator=(ScopedActivityReset &&) -> ScopedActivityReset & = delete;

  private:
    static auto reset() -> void {
        aurora::net::http::clear_network_activity();
        aurora::net::http::set_activity_options(aurora::net::http::ActivityOptions{});
    }
};

/// @brief 向进程级活动存储写入一笔已收摊的交换（存储不依赖订阅者，夹具无需挂总线）。
///
/// 句柄在作用域内收摊：`finish()` 之后再析构不会再发第二枚 end，也不会被记成 abandoned。
/// Release 构建下 `ExchangeTrace` 是空壳，本函数什么都不写——端点用例在那种构建下走 unavailable 分支。
/// @param method 请求方法。
/// @param request_id 链路标识。
/// @param url 目标 URL（取 host 与 origin-form）。
/// @param hop 重定向跳序号。
/// @param pass 物理尝试序号。
/// @param status 应答状态码。
auto seed_activity(std::string_view method, std::string_view request_id, std::string_view url, std::uint16_t hop,
                   std::uint16_t pass, std::uint16_t status) -> void {
    auto parsed = aurora::Url::parse(url);
    AURORA_TEST_REQUIRE_TRUE(parsed.ok());
    aurora::net::http::ExchangeTrace trace{method, request_id, parsed.value(), hop, pass, false, false};
    trace.mark(aurora::net::http::HttpStage::ConnectionAcquired);
    trace.mark(aurora::net::http::HttpStage::RequestSent);
    trace.set_status(status);
    trace.mark(aurora::net::http::HttpStage::FirstByte);
    trace.mark(aurora::net::http::HttpStage::BodyDone);
    trace.finish();
}

/// @brief 从原始 HTTP 响应里解析 JSON 体（错误体也是 JSON，不按状态码分流）。
/// @param response 完整响应文本。
/// @return 解析后的 JSON；缺分隔符或解析失败即本用例致命失败。
[[nodiscard]] auto response_json(const std::string &response) -> Json {
    const auto separator = response.find("\r\n\r\n");
    AURORA_TEST_REQUIRE(separator != std::string::npos);
    auto parsed = json::parse(response.substr(separator + 4));
    AURORA_TEST_REQUIRE_TRUE(parsed.ok());
    return parsed.value();
}

}  // namespace
#endif  // AURORA_BUILD_INSPECTOR_SERVER（辅助设施段；用例恒注册，体内降级 SKIP）

AURORA_TEST_CASE(initial_state_is_stopped) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_CHECK_EQ(server.is_running(), false);
    AURORA_TEST_CHECK_EQ(server.port(), 0);
#endif
}

AURORA_TEST_CASE(start_and_stop_lifecycle) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp =
        http_roundtrip(server.port(), "POST /api/tree HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
    AURORA_TEST_CHECK_TRUE(resp.find("405") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(route_registry_matches_every_registered_endpoint) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 逐枚精确端点用「它声明的动词」打一次：只要注册表漏登或动词位掩码写错，请求就会落 404/405，
    // 而「命中后业务层怎么说」（400/404/500）不是本用例的判据——这里只验证分派本身。
    struct Probe {
        const char *target;
        const char *verb;
    };
    const Probe probes[] = {
        {.target = "/api/debug/state", .verb = "GET"},
        {.target = "/api/debug/snapshot", .verb = "GET"},
        {.target = "/api/debug/perf", .verb = "GET"},
        {.target = "/api/debug/timeline", .verb = "GET"},
        {.target = "/api/debug/diagnostics", .verb = "GET"},
        {.target = "/api/debug/why", .verb = "GET"},
        {.target = "/api/windows", .verb = "GET"},
        {.target = "/api/debug/tree", .verb = "GET"},
        {.target = "/api/debug/pick?x=0&y=0", .verb = "GET"},
        {.target = "/api/debug/flags", .verb = "POST"},
        {.target = "/api/tree", .verb = "GET"},
        {.target = "/api/widget/0", .verb = "GET"},
        {.target = "/api/input/click", .verb = "POST"},
        {.target = "/api/find?type=Text", .verb = "GET"},
        {.target = "/api/patch", .verb = "POST"},
        {.target = "/api/components", .verb = "GET"},
        {.target = "/api/yaml", .verb = "GET"},
        {.target = "/api/to_code", .verb = "POST"},
    };
    for (const Probe &probe : probes) {
        std::string request = std::string{probe.verb} + " " + probe.target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        if (std::string_view{probe.verb} == "POST") {
            request += "Content-Length: 0\r\n";
        }
        request += "\r\n";
        const std::string resp = http_roundtrip(server.port(), request);
        AURORA_TEST_CHECK_TRUE(resp.find("404") == std::string::npos);
        AURORA_TEST_CHECK_TRUE(resp.find("405") == std::string::npos);
    }
    server.stop();
#endif
}

AURORA_TEST_CASE(route_registry_gates_method_before_path_shape) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 动词本就不被支持 + 路径恰好退化：405 而不是 400。405 说的是「这个端点不接受这个动词」，
    // 路径形状对错改变不了这个更基本的事实。
    const std::string delete_widget =
        http_roundtrip(server.port(), "DELETE /api/widget/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    AURORA_TEST_CHECK_TRUE(delete_widget.find("405") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(delete_widget.find("Method not allowed for /api/widget") != std::string::npos);
    const std::string delete_input =
        http_roundtrip(server.port(), "DELETE /api/input/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    AURORA_TEST_CHECK_TRUE(delete_input.find("405") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(delete_input.find("Method not allowed for /api/input") != std::string::npos);

    // 支持的动词 + 退化路径仍是 400（路径形状由 handler 说，不是方法闸的事）。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/widget/").find("400") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_post(server.port(), "/api/widget/0", "{}").find("405") != std::string::npos);

    // 前缀族不吞近似路径：没有尾斜杠的 /api/widget 不命中，仍走 404 兜底。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/widget").find("404") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(route_registry_strips_query_before_matching) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 精确项与前缀族都在剥离查询串后的路径上匹配；查询串照旧交给 handler 解析。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/tree?x=1").find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/widget/0?x=1").find("200 OK") != std::string::npos);

    // 404 兜底回显的是含查询串的原样 path，便于调用方对照自己发的东西。
    const std::string unknown = http_get(server.port(), "/api/no_such?x=1");
    AURORA_TEST_CHECK_TRUE(unknown.find("404") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(unknown.find("/api/no_such?x=1") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(bad_pick_params_return_400) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
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
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 未装配 getter：明确 400。
    const std::string without = http_get(server.port(), "/api/debug/state");
    AURORA_TEST_CHECK_TRUE(without.find("400") != std::string::npos);

    // getter 装配后返回 null Surface：路由层区分回 500。
    server.set_surface_getter([]() -> Surface * { return nullptr; });
    const std::string with_null = http_get(server.port(), "/api/debug/state");
    AURORA_TEST_CHECK_TRUE(with_null.find("500") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(input_click_dispatches_to_target_and_toggles_state) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InputTree tree = make_input_tree();
    AURORA_TEST_REQUIRE_FALSE(tree.checkbox->value());  // 起始未勾选，翻转可观测

    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_post(server.port(), "/api/input/click", R"({"path":"0"})");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"status\":\"ok\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"action\":\"click\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"widget_path\":\"0\"") != std::string::npos);

    // 「派发成功」不等于「状态变了」——必须读回状态端点确认事件真的落到了该控件。
    const std::string props = http_get(server.port(), "/api/widget/0");
    AURORA_TEST_CHECK_MSG(props.find("\"checked\":true") != std::string::npos,
                          "click reached the Checkbox and toggled its checked state");
    server.stop();
#endif
}

AURORA_TEST_CASE(input_scroll_dispatches_to_target_and_reaches_on_scroll) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InputTree tree = make_input_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_post(server.port(), "/api/input/scroll", R"({"path":"2","dx":0,"dy":-24})");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"action\":\"scroll\"") != std::string::npos);

    // 探针把 on_scroll 命中次数与末次 delta_y 序列化外显，据此确认滚轮事件到达了目标控件。
    const std::string props = http_get(server.port(), "/api/widget/2");
    AURORA_TEST_CHECK_MSG(props.find("\"scroll_hits\":1") != std::string::npos,
                          "scroll event reached the target widget's on_scroll");
    AURORA_TEST_CHECK_TRUE(props.find("\"last_delta_y\":-24.0") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(input_text_dispatches_to_target_and_inserts_text) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InputTree tree = make_input_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_post(server.port(), "/api/input/text", R"({"path":"1","text":"hi"})");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"action\":\"text\"") != std::string::npos);

    const std::string props = http_get(server.port(), "/api/widget/1");
    AURORA_TEST_CHECK_MSG(props.find("\"value\":\"hi\"") != std::string::npos,
                          "text input reached the TextInput and its value reads back");
    server.stop();
#endif
}

AURORA_TEST_CASE(input_endpoint_rejects_malformed_requests) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InputTree tree = make_input_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::uint16_t port = server.port();

    // 未知动作。
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/nope", "{}").find("404") != std::string::npos);
    // 缺 path / path 类型不符。
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/click", "{}").find("400") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/click", R"({"path":7})").find("400") != std::string::npos);
    // scroll 增量类型不符。
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/scroll", R"({"path":"2","dx":"abc"})").find("400") !=
                           std::string::npos);
    // text 类型不符。
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/text", R"({"path":"1","text":5})").find("400") !=
                           std::string::npos);
    // body 非 JSON / 非对象。
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/click", "not json").find("400") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/input/click", "[1,2]").find("400") != std::string::npos);
    // 方法不符。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/input/click").find("405") != std::string::npos);

    // 全部被拒的请求都不得改变控件状态（未勾选的仍有未勾选）。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/widget/0").find("\"checked\":false") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(input_endpoint_accepts_empty_path_as_tree_root) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    // 空串路径即树根本身（与 find_node_by_path 的空路径语义一致），使根控件无需再包一层容器
    // 就能被驱动；这里直接把 TextInput 作根来验证。
    auto input = std::make_shared<TextInput>();
    auto root = std::make_shared<Node>(Node{input});
    InspectorServer server([root]() -> Node { return *root; });
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_post(server.port(), "/api/input/text", R"({"path":"","text":"root"})");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"widget_path\":\"\"") != std::string::npos);
    // /api/widget/ 空路径本身被拒（400），故经完整树读回根控件的属性。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/tree").find("\"value\":\"root\"") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(input_endpoint_maps_missing_target_and_simulate_failure) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InputTree tree = make_input_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::uint16_t port = server.port();

    // 路径指向不存在的节点：404（区别于「找到了但不可派发」的 400）。
    const std::string missing = http_post(port, "/api/input/click", R"({"path":"7"})");
    AURORA_TEST_CHECK_TRUE(missing.find("404") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(missing.find("Widget not found at path: 7") != std::string::npos);

    // 目标命中但派发失败：禁用态 TextInput 不消费文本输入（不置 handled）→ 400，且不改状态。
    tree.input->set_enabled(false);
    const std::string failed = http_post(port, "/api/input/text", R"({"path":"1","text":"x"})");
    AURORA_TEST_CHECK_TRUE(failed.find("400") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/widget/1").find("\"value\":\"\"") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(input_drag_dispatches_press_move_release_to_target) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InputTree tree = make_input_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    const std::string resp = http_post(server.port(), "/api/input/drag", R"({"path":"3","dx":30,"dy":-10})");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"action\":\"drag\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"widget_path\":\"3\"") != std::string::npos);

    // 探针把三段事件的命中次数与 Move 相对 Press 的位移序列化外显，据此确认合成拖拽
    // （Press → Move → Release）确实落到目标控件且位移与请求一致。
    const std::string props = http_get(server.port(), "/api/widget/3");
    AURORA_TEST_CHECK_MSG(props.find("\"press_hits\":1") != std::string::npos, "drag press reached the target widget");
    AURORA_TEST_CHECK_MSG(props.find("\"move_hits\":1") != std::string::npos, "drag move reached the target widget");
    AURORA_TEST_CHECK_MSG(props.find("\"release_hits\":1") != std::string::npos,
                          "drag release reached the target widget");
    AURORA_TEST_CHECK_TRUE(props.find("\"move_dx\":30.0") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(props.find("\"move_dy\":-10.0") != std::string::npos);

    // 拖拽增量类型不符 → 400（与 scroll 同一校验口径）。
    AURORA_TEST_CHECK_TRUE(http_post(server.port(), "/api/input/drag", R"({"path":"3","dx":"abc"})").find("400") !=
                           std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(find_endpoint_locates_by_type_key_text_and_combination) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    FindTree tree = make_find_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::uint16_t port = server.port();

    // 按 type：两个 Button（先序：根下 0 在嵌套 2/0 之前），返回索引路径数组。
    const std::string by_type = http_get(port, "/api/find?type=Button");
    AURORA_TEST_CHECK_TRUE(by_type.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(by_type.find("\"count\":2") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(by_type.find("\"path\":\"0\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(by_type.find("\"path\":\"2/0\"") != std::string::npos);

    // 按 key（Node::set_id 标识）。
    const std::string by_key = http_get(port, "/api/find?key=cancel");
    AURORA_TEST_CHECK_TRUE(by_key.find("\"count\":1") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(by_key.find("\"path\":\"2/0\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(by_key.find("\"id\":\"cancel\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?key=nope").find("\"count\":0") != std::string::npos);

    // 按 text（文本类属性启发式：Button 序列化 label）。
    // CJK-LITERAL: cjk-fixture - find by Han text must return the addressable tree paths
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?text=确定").find("\"path\":\"0\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?text=取消").find("\"path\":\"2/0\"") != std::string::npos);

    // 多参数 AND 语义。
    // CJK-LITERAL: cjk-fixture - AND-combined Han text query against the fixture labels
    const std::string combined = http_get(port, "/api/find?type=Button&text=取消");
    AURORA_TEST_CHECK_TRUE(combined.find("\"count\":1") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(combined.find("\"path\":\"2/0\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?type=Button&key=ok").find("\"path\":\"0\"") != std::string::npos);
    // CJK-LITERAL: cjk-fixture - a Han text query on the wrong type must find nothing
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?type=TextInput&text=确定").find("\"count\":0") !=
                           std::string::npos);

    // 根自身可命中（路径空串）；嵌套 Column 同样命中（路径 "2"）。
    const std::string root_hit = http_get(port, "/api/find?type=Column");
    AURORA_TEST_CHECK_TRUE(root_hit.find("\"count\":2") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(root_hit.find("\"path\":\"\"") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(root_hit.find("\"path\":\"2\"") != std::string::npos);

    // 返回路径与寻址端点同口径：拿 find 给出的路径可直接读属性。
    const std::string props = http_get(port, "/api/widget/2/0");
    // CJK-LITERAL: cjk-fixture - the serialized label of the Han-named button comes back unchanged
    AURORA_TEST_CHECK_MSG(props.find("\"label\":\"取消\"") != std::string::npos,
                          "find's index path must be addressable by /api/widget/{path}");
    server.stop();
#endif
}

// query 值的百分号解码：HTTP 客户端对空格 / 非 ASCII 一律按 `%XX` 编码，服务端不解码就永远
// 匹配不上这些值（真实场景：`/api/find?text=New%20auxiliary%20window` 定位多窗口载体的按钮）。
AURORA_TEST_CASE(find_endpoint_percent_decodes_query_values) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    FindTree tree = make_find_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::uint16_t port = server.port();

    // 中文标签的 UTF-8 逐字节编码形态（"确定" / "取消"）。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?text=%E7%A1%AE%E5%AE%9A").find("\"path\":\"0\"") !=
                           std::string::npos);
    // 十六进制大小写等价。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?text=%e7%a1%ae%e5%ae%9a").find("\"count\":1") !=
                           std::string::npos);
    // key 走同一取值通道，同样解码（"ok" = %6F%6B）。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?key=%6F%6B").find("\"path\":\"0\"") != std::string::npos);
    // 编码值与 tree 里的字面量不等价时不得命中（解码把 %20 还原成空格，匹配的是 "确定" 而非 "确定 "）。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?text=%E7%A1%AE%E5%AE%9A%21").find("\"count\":0") !=
                           std::string::npos);
    // 宽松解码：畸形百分号按字面量保留，不升级成 500。
    const std::string malformed = http_get(port, "/api/find?text=%zz");
    AURORA_TEST_CHECK_TRUE(malformed.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(malformed.find("\"count\":0") != std::string::npos);
    server.stop();

    // 含空格的 label：未解码时 "%20" 是字面量，任何带空格标签都定位不到。
    auto save = std::make_shared<Button>("Save all files");
    auto row = std::make_shared<Column>();
    row->add(Node{save});
    auto space_root = std::make_shared<Node>(Node{row});
    InspectorServer space_server([held = space_root]() -> Node { return *held; });
    AURORA_TEST_REQUIRE_TRUE(space_server.start(0));
    const std::uint16_t space_port = space_server.port();
    AURORA_TEST_CHECK_TRUE(http_get(space_port, "/api/find?text=Save%20all%20files").find("\"count\":1") !=
                           std::string::npos);
    space_server.stop();
#endif
}

AURORA_TEST_CASE(find_endpoint_rejects_malformed_requests) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    FindTree tree = make_find_tree();
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::uint16_t port = server.port();

    // 无任何参数 → 400。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find").find("400") != std::string::npos);
    // 空值参数视同未提供 → 400。
    AURORA_TEST_CHECK_TRUE(http_get(port, "/api/find?key=").find("400") != std::string::npos);
    // 方法不符 → 405。
    AURORA_TEST_CHECK_TRUE(http_post(port, "/api/find?type=Button", "{}").find("405") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(tree_endpoint_by_window_returns_target_window_tree) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    // 主窗用共享树（Column[Text "hello"]）；window=1 → Column[Text "window-one"]，
    // window=2 → Checkbox 根；其它 id → 空 Node（路由层 404）。
    InspectorServer server(tree_getter);
    server.set_window_tree_getter([](std::uint32_t id) -> Node {
        if (id == 1U) {
            auto col = std::make_shared<Column>();
            col->add(Node{std::make_shared<Text>("window-one")});
            return Node{col};
        }
        if (id == 2U) {
            return Node{std::make_shared<Checkbox>()};
        }
        return Node{};  // 无效 id
    });
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // window=1 命中对应树（含特异性标记文本）。
    const std::string w1 = http_get(server.port(), "/api/tree?window=1");
    AURORA_TEST_CHECK_TRUE(w1.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(w1.find("window-one") != std::string::npos);

    // window=2 命中另一棵结构不同的树（Checkbox 根）。
    const std::string w2 = http_get(server.port(), "/api/tree?window=2");
    AURORA_TEST_CHECK_TRUE(w2.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(w2.find("Checkbox") != std::string::npos);

    // 无效 id → 404（getter 返回空 Node）。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/tree?window=999").find("404") != std::string::npos);

    // 非数值 id → 400（参数解析失败，而非找不到窗口）。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/tree?window=abc").find("400") != std::string::npos);

    // 无 window 参数：回退主窗（共享树 Column[Text "hello"]），向后兼容既有 /api/tree。
    const std::string main = http_get(server.port(), "/api/tree");
    AURORA_TEST_CHECK_TRUE(main.find("200 OK") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(main.find("hello") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(tree_endpoint_window_param_requires_getter) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    // 未注册 window_tree_getter 时，带 window 参数的请求必须回 400（明确告知需配置），
    // 而非静默回退主窗或 404。
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::string resp = http_get(server.port(), "/api/tree?window=1");
    AURORA_TEST_CHECK_TRUE(resp.find("400") != std::string::npos);
    server.stop();
#endif
}

AURORA_TEST_CASE(patch_endpoint_applies_property_ops_to_live_widgets) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 一条最小补丁：改掉第一个子控件的 content（路径 "/0/content"，最后一段是属性名）。
    const std::string resp = http_post(server.port(), "/api/patch", R"([{"path":"/0/content","value":"patched"}])");
    AURORA_TEST_CHECK_TRUE(resp.find("200") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(resp.find("\"ops\":1") != std::string::npos);

    // 关键：值必须真的落到活控件上，而不只是回了个 200。
    Json props = Json::object();
    shared_tree()->child_nodes().at(0).widget().serialize_props(props);
    const Json *const content = props.at("content");
    const Json v = content != nullptr ? *content : Json{std::string{}};
    AURORA_TEST_CHECK_EQ(v, std::string{"patched"});

    // 非数组请求体必须被拒 —— 否则调用方无从知道补丁没生效。
    const std::string bad = http_post(server.port(), "/api/patch", R"({"path":"/0/content","value":"x"})");
    AURORA_TEST_CHECK_TRUE(bad.find("400") != std::string::npos);

    // 复原共享树，避免污染同进程内的其它用例。
    static_cast<void>(http_post(server.port(), "/api/patch", R"([{"path":"/0/content","value":"hello"}])"));
    server.stop();
#endif
}

// 目标：触碰活动树的端点必须经 `main_poster` 换到主线程执行，不得在 accept worker 线程下树。
// 回归的缺陷：真机拖拽 ReorderableList 时轮询 `/api/tree`，宿主在 0xC0000005 崩溃
// （栈：`route_request → Inspector::tree_json_full → dump_tree_json_full`，无 marshal 帧）——
// 帧内在增删节点，worker 线程读到已释放节点。用例靠「线程流水」判定：`root_getter` 记录的调用
// 线程必须全等于排水线程，且排水线程就是用例主线程；修复前这些调用发生在 worker 线程，必红。
AURORA_TEST_CASE(tree_routes_marshal_tree_traversal_to_the_poster_thread) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    QueuedPoster poster;
    getter_threads().clear();
    InspectorServer server(recording_tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    const std::uint16_t port = server.port();

    // 客户端另起线程：主线程若不排水，服务端就永远等不到树读取落点，客户端只会超时返回。
    std::vector<std::string> responses;
    std::exception_ptr client_error;
    std::thread client([&]() -> void {
        try {
            responses.push_back(http_get_timed(port, "/api/tree", 8000));
            responses.push_back(http_get_timed(port, "/api/widget/0", 8000));
            responses.push_back(http_get_timed(port, "/api/yaml", 8000));
        } catch (...) {
            client_error = std::current_exception();
        }
    });
    // 三个端点各一次树访问 → 各一次 marshal。任一次排水超时即说明该端点没走投递器。
    bool drained = true;
    for (int i = 0; i < 3; ++i) {
        drained = poster.drain_one(8000) && drained;
    }
    client.join();
    server.stop();

    AURORA_TEST_REQUIRE_TRUE(drained);
    AURORA_TEST_REQUIRE_TRUE(client_error == nullptr);
    AURORA_TEST_REQUIRE_EQ(responses.size(), 3U);
    for (const auto &resp : responses) {
        AURORA_TEST_CHECK_TRUE(resp.find("200") != std::string::npos);
    }
    AURORA_TEST_REQUIRE_EQ(getter_threads().size(), 3U);
    for (const auto &id : getter_threads()) {
        AURORA_TEST_CHECK_TRUE(id == std::this_thread::get_id());
    }
#endif
}

// 目标：无投递器（无事件循环）时树端点仍可同步服务——marshal 的回退分支不得变成硬依赖。
AURORA_TEST_CASE(tree_routes_work_without_a_poster_inline) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/tree").find("Column") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/widget/0").find("200") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/yaml").find("200") != std::string::npos);
    server.stop();
#endif
}

// 目标：Inspector 的四面树导出按同一份策略快照折叠属性字符串；未安装策略时整条 walk 不执行。
AURORA_TEST_CASE(tree_export_folds_property_strings_only_under_a_policy) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    auto tree = make_input_tree();
    tree.input->set_value("hunter2");
    tree.input->set_placeholder("type here");
    InspectorServer server(tree.getter());
    server.set_window_tree_getter([root = tree.root](std::uint32_t id) -> Node { return id == 7U ? *root : Node{}; });
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    // 对照组：没有策略时原值与结构键都在，walk 一次都不走。
    const std::string baseline_tree = http_get(server.port(), "/api/tree");
    AURORA_TEST_CHECK_TRUE(baseline_tree.find("hunter2") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(baseline_tree.find("\"type\":\"TextInput\"") != std::string::npos);
    const std::string baseline_widget = http_get(server.port(), "/api/widget/1");
    AURORA_TEST_CHECK_TRUE(baseline_widget.find("hunter2") != std::string::npos);
    const std::string baseline_yaml = http_get(server.port(), "/api/yaml");
    AURORA_TEST_CHECK_TRUE(baseline_yaml.find("hunter2") != std::string::npos);

    {
        ScopedExportPolicy installed{std::make_shared<DenySensitivePolicy>()};

        const std::string tree_resp = http_get(server.port(), "/api/tree");
        AURORA_TEST_CHECK_EQ(tree_resp.find("hunter2"), std::string::npos);
        AURORA_TEST_CHECK_TRUE(tree_resp.find("\"value\":\"***\"") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(tree_resp.find("\"placeholder\":\"***\"") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(tree_resp.find("\"type\":\"TextInput\"") != std::string::npos);  // 结构键不折叠

        // ?window= 分支与主窗口同源同口径。
        const std::string window_resp = http_get(server.port(), "/api/tree?window=7");
        AURORA_TEST_CHECK_EQ(window_resp.find("hunter2"), std::string::npos);
        AURORA_TEST_CHECK_TRUE(window_resp.find("\"value\":\"***\"") != std::string::npos);

        const std::string debug_resp = http_get(server.port(), "/api/debug/tree");
        AURORA_TEST_CHECK_EQ(debug_resp.find("hunter2"), std::string::npos);
        AURORA_TEST_CHECK_TRUE(debug_resp.find("\"type\":\"TextInput\"") != std::string::npos);

        // 单控件端点折叠的是 values；descriptor 的名字与属性名清单保留可读性。
        const std::string widget_resp = http_get(server.port(), "/api/widget/1");
        AURORA_TEST_CHECK_EQ(widget_resp.find("hunter2"), std::string::npos);
        AURORA_TEST_CHECK_TRUE(widget_resp.find("\"value\":\"***\"") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(widget_resp.find("\"name\":\"TextInput\"") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(widget_resp.find("\"property_names\"") != std::string::npos);

        const std::string yaml_resp = http_get(server.port(), "/api/yaml");
        AURORA_TEST_CHECK_EQ(yaml_resp.find("hunter2"), std::string::npos);
        AURORA_TEST_CHECK_TRUE(yaml_resp.find("***") != std::string::npos);
    }
    // 卸载后与基线逐字节相同：无策略即「整条 walk 不执行」。
    AURORA_TEST_CHECK_EQ(http_get(server.port(), "/api/tree"), baseline_tree);
    AURORA_TEST_CHECK_EQ(http_get(server.port(), "/api/widget/1"), baseline_widget);
    AURORA_TEST_CHECK_EQ(http_get(server.port(), "/api/yaml"), baseline_yaml);
    server.stop();
#endif
}

// 目标：两段式策略在树导出里逐键生效——放行的键拿原值、其余仍折叠；纯输入变换端点不 walk。
AURORA_TEST_CASE(tree_export_policy_gates_each_property_key) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    auto tree = make_input_tree();
    tree.input->set_value("hunter2");
    tree.input->set_placeholder("type here");
    InspectorServer server(tree.getter());
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

    {
        // 只放行 `tree.value`：同一个控件里的 `placeholder` 仍折叠，证明键是逐条裁决而非整对象一刀切。
        ScopedExportPolicy installed{std::make_shared<AllowSelectedKeysPolicy>(std::vector<std::string>{"tree.value"})};
        const std::string tree_resp = http_get(server.port(), "/api/tree");
        AURORA_TEST_CHECK_TRUE(tree_resp.find("\"value\":\"hunter2\"") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(tree_resp.find("\"placeholder\":\"***\"") != std::string::npos);

        const std::string widget_resp = http_get(server.port(), "/api/widget/1");
        AURORA_TEST_CHECK_TRUE(widget_resp.find("\"value\":\"hunter2\"") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(widget_resp.find("\"placeholder\":\"***\"") != std::string::npos);
    }
    {
        // `to_code` 把调用方自己送进来的树转成代码，是纯输入变换而非状态导出，不参与折叠。
        // 用步进风格（style=1）确保属性完整落码，否则 Fluent 会略去 setter-only 属性而使判据空转。
        ScopedExportPolicy installed{std::make_shared<DenySensitivePolicy>()};
        const std::string code_resp =
            http_post(server.port(), "/api/to_code", R"({"style":1,"type":"TextInput","props":{"value":"hunter2"}})");
        AURORA_TEST_CHECK_TRUE(code_resp.find("200 OK") != std::string::npos);
        AURORA_TEST_CHECK_TRUE(code_resp.find("hunter2") != std::string::npos);
    }
    server.stop();
#endif
}

// 目标：活动快照的过滤先于 limit——先按 host / status 过滤，再取最新 N 条；非法参数回 400。
AURORA_TEST_CASE(network_snapshot_filters_before_limit) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    ScopedActivityReset reset;
    seed_activity("GET", "rid-a", "http://api.example.test/one", 0, 0, 200);
    seed_activity("POST", "rid-b", "http://api.example.test/two", 0, 0, 503);
    seed_activity("GET", "rid-c", "http://cdn.example.test/three", 0, 0, 200);
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

#ifdef AURORA_ENABLE_DEBUG
    const Json all = response_json(http_get(server.port(), "/api/network"));
    AURORA_TEST_CHECK_TRUE(aurora::testing::require_field<bool>(all, "available"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::size_t>(all, "count"), std::size_t{3});
    const Json &all_entries = *aurora::testing::require_child(all, "entries");
    AURORA_TEST_CHECK_TRUE(all_entries.is_array());
    AURORA_TEST_REQUIRE_EQ(all_entries.size(), std::size_t{3});
    // 最新在前：rid-c 最后写入，排第一。
    AURORA_TEST_CHECK_EQ(
        aurora::testing::require_field<std::string>(*aurora::testing::require_child_at(all_entries, 0), "request_id"),
        std::string{"rid-c"});

    // host 精确匹配、大小写不敏感：两条同主机记录都保留，最新在前。
    const Json by_host = response_json(http_get(server.port(), "/api/network?host=API.EXAMPLE.TEST"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::size_t>(by_host, "count"), std::size_t{2});
    const Json &host_entries = *aurora::testing::require_child(by_host, "entries");
    AURORA_TEST_REQUIRE_EQ(host_entries.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(
        aurora::testing::require_field<std::string>(*aurora::testing::require_child_at(host_entries, 0), "request_id"),
        std::string{"rid-b"});
    AURORA_TEST_CHECK_EQ(
        aurora::testing::require_field<std::string>(*aurora::testing::require_child_at(host_entries, 1), "request_id"),
        std::string{"rid-a"});

    // 先过滤再截断：全表最新是 200 的 rid-c；status=503 过滤后才轮得到 rid-b。
    // 若次序颠倒（先 limit=1 再过滤），这里会得到 0 条——判据正是这一条。
    const Json filtered = response_json(http_get(server.port(), "/api/network?status=503&limit=1"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::size_t>(filtered, "count"), std::size_t{1});
    const Json &filtered_entries = *aurora::testing::require_child(filtered, "entries");
    AURORA_TEST_REQUIRE_EQ(filtered_entries.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::string>(
                             *aurora::testing::require_child_at(filtered_entries, 0), "request_id"),
                         std::string{"rid-b"});

    // limit=0 表示全部；无匹配的过滤条件回空表而不是错误。
    const Json all_again = response_json(http_get(server.port(), "/api/network?limit=0"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::size_t>(all_again, "count"), std::size_t{3});
    const Json empty = response_json(http_get(server.port(), "/api/network?host=other.example.test"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::size_t>(empty, "count"), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(aurora::testing::require_child(empty, "entries")->empty());

    // 非法参数：空值、非数字、负号与越界一律 400。
    for (const char *target : {"/api/network?limit=abc", "/api/network?limit=-1", "/api/network?limit=",
                               "/api/network?status=70000", "/api/network?status=", "/api/network?host="}) {
        const std::string resp = http_get(server.port(), target);
        AURORA_TEST_CHECK_TRUE(resp.find("400 Bad Request") != std::string::npos);
    }
#else
    // Release：两枚端点都给出与调试门面一致的 unavailable 形态，不因参数的完整与否分流。
    for (const char *target : {"/api/network", "/api/network?limit=1", "/api/network/request?id=rid-a"}) {
        const std::string resp = http_get(server.port(), target);
        AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
        const Json body = response_json(resp);
        AURORA_TEST_CHECK_FALSE(aurora::testing::require_field<bool>(body, "available"));
        AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::string>(body, "reason"),
                             std::string{"AURORA_ENABLE_DEBUG not enabled"});
    }
#endif
    server.stop();
#endif
}

// 目标：单条请求的多跳展开与 id 校验——命中回最新条目与升序时间线，缺参 400、未知 404 且不回显 id。
AURORA_TEST_CASE(network_request_expands_hops_and_validates_id) {
#if !defined(AURORA_BUILD_INSPECTOR_SERVER) || !AURORA_CAP_THREADS || defined(AURORA_PLATFORM_WASM)
    AURORA_TEST_SKIP(
        "The Inspector HTTP server cannot run here: not built, no thread capability, or wasm (no BSD socket "
        "semantics)");
#else
    ScopedActivityReset reset;
    seed_activity("GET", "rid-hop", "http://api.example.test/start", 0, 0, 302);
    seed_activity("GET", "rid-other", "http://cdn.example.test/other", 0, 0, 200);
    seed_activity("GET", "rid-hop", "http://api.example.test/final", 1, 0, 200);
    InspectorServer server(tree_getter);
    AURORA_TEST_REQUIRE_TRUE(server.start(0));

#ifdef AURORA_ENABLE_DEBUG
    const std::string hit = http_get(server.port(), "/api/network/request?id=rid-hop");
    AURORA_TEST_CHECK_TRUE(hit.find("200 OK") != std::string::npos);
    const Json body = response_json(hit);
    AURORA_TEST_CHECK_TRUE(aurora::testing::require_field<bool>(body, "available"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::string>(body, "request_id"), std::string{"rid-hop"});

    // entry 是该 id 最新一条活动条目：第二跳的状态、路径与跳序号都来自它。
    const Json &entry = *aurora::testing::require_child(body, "entry");
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::int64_t>(entry, "status"), std::int64_t{200});
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::int64_t>(entry, "hop"), std::int64_t{1});
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::string>(entry, "path"), std::string{"/final"});
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::string>(entry, "host"), std::string{"api.example.test"});

    // timelines 按 exchange_id 升序：第一跳是 302，第二跳是 200，阶段名走稳定令牌。
    const Json &timelines = *aurora::testing::require_child(body, "timelines");
    AURORA_TEST_CHECK_TRUE(timelines.is_array());
    AURORA_TEST_REQUIRE_EQ(timelines.size(), std::size_t{2});
    const Json &first_hop = *aurora::testing::require_child_at(timelines, 0);
    const Json &second_hop = *aurora::testing::require_child_at(timelines, 1);
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::int64_t>(first_hop, "hop"), std::int64_t{0});
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::int64_t>(second_hop, "hop"), std::int64_t{1});
    AURORA_TEST_CHECK_LE(aurora::testing::require_field<std::uint64_t>(first_hop, "exchange_id"),
                         aurora::testing::require_field<std::uint64_t>(second_hop, "exchange_id"));
    const Json &marks = *aurora::testing::require_child(second_hop, "marks");
    AURORA_TEST_CHECK_TRUE(marks.is_array());
    AURORA_TEST_REQUIRE_EQ(marks.size(), std::size_t{4});
    AURORA_TEST_CHECK_EQ(
        aurora::testing::require_field<std::string>(*aurora::testing::require_child_at(marks, 0), "stage"),
        std::string{"connection_acquired"});
    AURORA_TEST_CHECK_EQ(
        aurora::testing::require_field<std::string>(*aurora::testing::require_child_at(marks, 3), "stage"),
        std::string{"body_done"});

    // 缺 id / 空 id：400；未知 id：404，且响应全文不回显调用方给的标识。
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/network/request").find("400 Bad Request") !=
                           std::string::npos);
    AURORA_TEST_CHECK_TRUE(http_get(server.port(), "/api/network/request?id=").find("400 Bad Request") !=
                           std::string::npos);
    const std::string unknown = http_get(server.port(), "/api/network/request?id=probe-secret-identifier");
    AURORA_TEST_CHECK_TRUE(unknown.find("404 Not Found") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(unknown.find("probe-secret-identifier") == std::string::npos);
#else
    const std::string resp = http_get(server.port(), "/api/network/request?id=rid-hop");
    AURORA_TEST_CHECK_TRUE(resp.find("200 OK") != std::string::npos);
    const Json body = response_json(resp);
    AURORA_TEST_CHECK_FALSE(aurora::testing::require_field<bool>(body, "available"));
    AURORA_TEST_CHECK_EQ(aurora::testing::require_field<std::string>(body, "reason"),
                         std::string{"AURORA_ENABLE_DEBUG not enabled"});
#endif
    server.stop();
#endif
}

}  // namespace aurora::test_cases::utest_inspector_server

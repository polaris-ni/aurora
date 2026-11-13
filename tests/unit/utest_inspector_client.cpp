/// 测试类型: unit
/// 目标单元: tools/servers/inspector_client.h
/// 测试说明: 覆盖最小 Inspector HTTP 客户端的「发不出手之前」拒单口径（非回环主机 / 端口 0 /
/// target 未以 '/' 开头）与**收发超时的平台形态**（传输经 `aurora::net`）。后者是回归位：Winsock 的
/// `SO_RCVTIMEO` / `SO_SNDTIMEO` 收的是 `DWORD` 毫秒数，POSIX 收的是 `struct timeval`，
/// 把 `timeval` 递给 Winsock 它只把前 4 字节当毫秒读——于是「5 秒」在 Windows 上实际生效成
/// 「5 毫秒」，慢于 5ms 的正常响应被 `WSAETIMEDOUT` 掐成 `empty response`。用例因此起一个
/// 「60ms 后才回数据」的回环监听口，断言客户端照样拿到 200（60ms ≫ 5ms、≪ 5s，两种形态在此分岔）。
/// 端口一律 0（系统分配临时端口，无冲突）；只连 127.0.0.1。
/// 「服务不在」那条用例的 `error` 断言按**传输层成因白名单**判定（`connect() failed` 或
/// `empty response`）而非钉死单条措辞：契约是「未获得 HTTP 响应」，而 WSL2 的 NAT 会让
/// 刚关闭的回环端口被快速复用、`connect()` 竟成功并读到 0 字节——同一语义的第二种表述。
/// ⚠️ 平台分支写在**用例体内**而非包裹 `AURORA_TEST_CASE`：registry_integrity 是「静态扫源码
/// 的用例名字面量」与「runner --list」逐条比对，被 `#ifdef` 摘掉的声明在另一种构建里必然单边
/// 失踪。故 wasm 只让用例体退化为 AURORA_TEST_SKIP，声明恒可见（与 utest_x11_surface 同一手法）。
/// ⚠️ socket 运行时由库侧随句柄创建自动钉住（`aurora::net::blocking` 契约），用例不再需要持有
/// 任何「会话」前置件。迁移时曾要求体内持 `net::Session`，漏持在 Linux 腿无感、
/// Windows 腿恒红（`socket()` 落 WSANOTINITIALISED）；该约束现已由实现侧吸收。

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"
#include "framework/main_queue_pump.h"

#ifndef AURORA_PLATFORM_WASM

#include "e2e/inspector_driver.h"  // tools/include/e2e：header-only 能力层（本文件扩展的被测面）
#include "inspector_client.h"  // tools/servers（该目录经 AuroraTests.cmake 加入 runner 的 include 路径）

#ifdef AURORA_BUILD_INSPECTOR_SERVER
#include "aurora/inspector/inspector_server.h"
#include "aurora/widget/containers.h"  // Column（握手用最小树）
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"  // HeadlessSurface（无桌面端到端）
#endif

#include "aurora/net/blocking.h"

namespace {

/// @brief 打开一个只绑回环、端口由系统分配的监听口，并把实际端口写回 `port_out`
/// （经核心同步阻塞层 `aurora::net::blocking`，失败返回无效 `Listener`）。
/// @param port_out 实际绑定的系统分配端口（失败时置 0）。
[[nodiscard]] auto open_loopback_listener(std::uint16_t &port_out) -> aurora::net::blocking::Listener {
    auto listener = aurora::net::blocking::Listener::listen_loopback(0, 1);  // 临时端口：避免与并发用例或本机服务撞号
    if (!listener) {
        return {};
    }
    port_out = listener.value().port();
    return std::move(listener.value());
}

/// @brief 服务端：至多等 5s 连接 → 睡 `delay` → 回一段最小 200 → 关闭。
///
/// 有界等待是刻意要求：客户端若在连接前就失败（本机 socket 资源不足等），本线程必须能自己退出，
/// 否则 `join()` 会把整个用例挂死到 runner 超时。
auto serve_after(aurora::net::blocking::Listener &listener, std::chrono::milliseconds delay) -> void {
    constexpr std::string_view response = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok";

    const auto ready = listener.wait_readable(std::chrono::seconds{5});
    if (!ready || !ready.value()) {
        return;
    }
    auto peer = listener.accept();
    if (!peer) {
        return;
    }
    std::this_thread::sleep_for(delay);
    // 小报文一次写完；忽略返回值与旧实现同形（失败也无从报告）。peer 随作用域自动关闭。
    static_cast<void>(peer.value().write_all(std::span<const char>(response)));
}

/// @brief 环境变量写入的两侧平台拼写（写空串即删除语义由 _putenv_s/setenv 各自处理，
///        端口解析对空串本就按「未声明」裁决，无需真删）。
auto set_env_var(const char *name, const std::string &value) -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    ::_putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

#endif  // AURORA_PLATFORM_WASM

namespace aurora::test_cases::utest_inspector_client {

AURORA_TEST_CASE(refuses_before_touching_a_socket) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; this client is not built for wasm");
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
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; this client is not built for wasm");
#else
    AURORA_TEST_REQUIRE_THREADS();

    std::uint16_t port = 0;
    aurora::net::blocking::Listener listener = open_loopback_listener(port);
    AURORA_TEST_REQUIRE(listener.is_valid());
    AURORA_TEST_REQUIRE_NE(port, std::uint16_t{0});

    // 60ms：远大于「被误当毫秒读的 5 秒」里真正生效那 5ms，又远小于本客户端的 5s 超时。
    auto server = std::thread([&listener] { serve_after(listener, std::chrono::milliseconds{60}); });
    const auto response = aurora::tools::inspector::http_request("GET", "127.0.0.1", port, "/api/tree");
    server.join();
    listener.close();

    // 断言一律用 CHECK 而非 REQUIRE：失败也要走完上面的 join/关闭，不把挂死或句柄泄漏留给后续用例。
    AURORA_TEST_CHECK_TRUE(response.ok());
    AURORA_TEST_CHECK_EQ(response.status, 200);
    AURORA_TEST_CHECK_EQ(response.body, std::string("ok"));
#endif
}

// 端口解析三级链（客户端侧约定）：显式入参 > AURORA_INSPECTOR_PORT > 默认 6280；
// 环境变量脏值（非数字 / 越界 / 尾随杂物 / 空串）一律按「未声明」回落默认。
AURORA_TEST_CASE(resolve_port_prefers_argument_then_env_then_default) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; this client is not built for wasm");
#else
    using aurora::tools::e2e::resolve_port;
    const char *env_name = aurora::tools::inspector::AURORA_PORT_ENV.data();
    constexpr auto default_port = aurora::tools::inspector::AURORA_DEFAULT_PORT;

    // 显式入参最高优先：环境变量即便声明了也不看。
    set_env_var(env_name, "7777");
    AURORA_TEST_CHECK_EQ(resolve_port(std::uint16_t{5000}), std::uint16_t{5000});

    // 无入参：环境变量次之（合法值生效）。
    AURORA_TEST_CHECK_EQ(resolve_port(std::nullopt), std::uint16_t{7777});

    // 脏值按未声明处理：半途而废的声明不该被静默截断成合法端口。
    set_env_var(env_name, "abc");
    AURORA_TEST_CHECK_EQ(resolve_port(std::nullopt), default_port);
    set_env_var(env_name, "0");
    AURORA_TEST_CHECK_EQ(resolve_port(std::nullopt), default_port);
    set_env_var(env_name, "70000");
    AURORA_TEST_CHECK_EQ(resolve_port(std::nullopt), default_port);
    set_env_var(env_name, "6280 ");
    AURORA_TEST_CHECK_EQ(resolve_port(std::nullopt), default_port);

    // 空串 / 未声明 → 默认 6280。
    set_env_var(env_name, "");
    AURORA_TEST_CHECK_EQ(resolve_port(std::nullopt), default_port);
#endif
}

// 「服务不在」必须落在 TransportError（status 恒 0、无 body、error 指明传输层原因），
// 不得伪装成空树或空结果——这是驱动方区分「环境未就绪」与「被测界面为空」的判据。
AURORA_TEST_CASE(transport_error_distinguishable_when_server_down) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; this client is not built for wasm");
#else
    // 先开监听拿一个系统分配的端口号再立刻关掉：同一时刻几乎不可能有人抢注，
    // 由此得到「肯定没人监听」的回环端口。
    std::uint16_t port = 0;
    aurora::net::blocking::Listener listener = open_loopback_listener(port);
    AURORA_TEST_REQUIRE(listener.is_valid());
    listener.close();
    AURORA_TEST_REQUIRE_NE(port, std::uint16_t{0});

    const auto call = aurora::tools::e2e::get_tree("127.0.0.1", port);
    AURORA_TEST_CHECK_TRUE(call.kind == aurora::tools::e2e::CallResult::Kind::TransportError);
    AURORA_TEST_CHECK_EQ(call.status, 0);
    AURORA_TEST_CHECK_TRUE(call.body.empty());
    // `error` 须指明**传输层**原因，但**不钉死具体措辞**：契约是「未获得 HTTP 响应」，
    // 而达到该状态的传输层路径依平台/环境而异——
    //   * `connect() failed on <host>:<port>`：连接被拒（端口确定无人监听。Windows 与
    //     原生 Linux 恒走此支）；
    //   * `empty response`：连接建立但读端拿到 0 字节。**WSL2 实测走此支**——它的 NAT 层
    //     会让刚关闭的回环端口被快速复用，于是 `connect()` 竟成功，而对端并无服务在跑。
    // 两者是同一语义（服务不可达）的两种传输层表述，故此处按「已知传输层成因」白名单判定。
    //
    // 为什么值得放宽：本用例的正身是**跨平台**的。只认 `connect() failed` 等于把
    // 「Windows 上的措辞」当成契约本身，使这条判据在 WSL 上恒红——而红的原因（端口复用）
    // 与本客户端的实现质量无关。另注：白名单不是空转，若客户端把此类场景归成
    // Ok / HttpError（伪装成空树或空结果），前三条断言仍会转红。
    const bool names_transport_cause = call.error.find("connect() failed") != std::string::npos ||
                                       call.error.find("empty response") != std::string::npos;
    AURORA_TEST_CHECK_TRUE(names_transport_cause);
#endif
}

// 端到端握手：HeadlessSurface（内存后端，无桌面依赖）+ InspectorServer::start(0) 临时端口，
// 经能力层走一遍 树查询 → 定位 → 注入 → 抓帧 → 停机后可区分 的完整链路。
AURORA_TEST_CASE(end_to_end_handshake_headless_server) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; this client is not built for wasm");
#elif !defined(AURORA_BUILD_INSPECTOR_SERVER)
    AURORA_TEST_SKIP("AURORA_BUILD_INSPECTOR_SERVER=OFF: InspectorServer is not built, no end-to-end handshake");
#else
    namespace e2e = aurora::tools::e2e;

    // 树/抓帧端点经 `marshal_opt` 回投进程级唯一队列——headless 用例没有真帧循环，
    // 领一枚值班级替身（`DutyPump`：租约 + 后台排水线程）；用例线程阻塞在 HTTP 应答上，不能自排。
    aurora::test::mq::DutyPump pump;

    auto tree = std::make_shared<aurora::Column>();
    tree->add(aurora::Node{std::make_shared<aurora::Text>("e2e client handshake")});
    aurora::InspectorServer server([tree]() -> aurora::Node { return aurora::Node{tree}; });
    auto surface = std::make_shared<aurora::HeadlessSurface>("", aurora::Size{.width = 320.0F, .height = 200.0F});
    server.set_surface_getter([surface]() -> aurora::Surface * { return surface.get(); });
    // 先出一帧：抓帧端点读的是帧缓冲，未 begin_frame 的 Surface 无数据可读。
    AURORA_TEST_REQUIRE(surface->begin_frame(320, 200).ok());
    surface->painter().fill_rect(
        aurora::Rect{.origin = {.x = 0.0F, .y = 0.0F}, .size = {.width = 320.0F, .height = 200.0F}},
        aurora::Color{245, 245, 247, 255});
    AURORA_TEST_REQUIRE(surface->present().ok());
    AURORA_TEST_REQUIRE(server.start(0));
    const std::uint16_t port = server.port();
    AURORA_TEST_REQUIRE_NE(port, std::uint16_t{0});

    const std::string host = "127.0.0.1";

    // 树查询：OK 且返回的是我们的树（而非空结果 / 错误形状）。
    const auto got_tree = e2e::get_tree(host, port);
    AURORA_TEST_CHECK_TRUE(got_tree.ok());
    AURORA_TEST_CHECK_NE(got_tree.body.find("Column"), std::string::npos);

    // 定位：type=Column 恰命中根（count=1）。
    const auto found = e2e::find(host, port, "", "Column", "");
    AURORA_TEST_CHECK_TRUE(found.ok());
    AURORA_TEST_CHECK_NE(found.body.find("\"count\":1"), std::string::npos);

    // 属性读取与输入注入（simulate_* 进程内直派，Headless 下可用）。
    const auto widget = e2e::get_widget(host, port, "0");
    AURORA_TEST_CHECK_TRUE(widget.ok());
    const auto tapped = e2e::tap(host, port, "0");
    AURORA_TEST_CHECK_TRUE(tapped.ok());

    // 抓帧：body 即 PNG 字节（魔数 \x89PNG）。服务器端 debug::capture 受 AURORA_ENABLE_DEBUG
    // 门控（经 aurora 目标 PUBLIC 导出，此处 #ifdef 与库行为严格同步）：宏未生效的构建
    // （如 Release 下 DEBUG=AUTO→OFF）按设计返回 500「not enabled」，故按构建态分支断言。
#ifdef AURORA_ENABLE_DEBUG
    const auto snap = e2e::snapshot(host, port, "fb");
    AURORA_TEST_CHECK_TRUE(snap.ok());
    AURORA_TEST_CHECK_TRUE(snap.body.size() >= 4 && snap.body[0] == '\x89' && snap.body[1] == 'P' &&
                           snap.body[2] == 'N' && snap.body[3] == 'G');
#else
    const auto snap = e2e::snapshot(host, port, "fb");
    AURORA_TEST_CHECK_TRUE(!snap.ok());
    AURORA_TEST_CHECK_TRUE(snap.kind == e2e::CallResult::Kind::HttpError);
    AURORA_TEST_CHECK_NE(snap.body.find("AURORA_ENABLE_DEBUG not enabled"), std::string::npos);
#endif

    // 停机后同端口：落回 TransportError（可区分），而非空结果或挂死。
    server.stop();
    const auto after_stop = e2e::get_tree(host, port);
    AURORA_TEST_CHECK_TRUE(after_stop.kind == e2e::CallResult::Kind::TransportError);
#endif
}

}  // namespace aurora::test_cases::utest_inspector_client

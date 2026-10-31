/// 测试类型: unit
/// 目标单元: include/aurora/net/http.h（`PoolOptions`）+ include/aurora/net/http_client.h +
///           src/aurora/net/detail/http_pool.h + src/aurora/net/detail/http_pool.cpp
/// 测试说明: 连接池治理的端到端回归（§12「分段超时与连接池治理」、提案 §16 第 36 项）。
///           复用与不回池以对端的 `accepts()` 计数为准（两条请求一条连接 = 复用；两条请求两条连接 =
///           判据挡住），五判据各有专例：干净连接复用、开关缺省关（既有「一次 `send` 一条连接」口径）、
///           `Connection: close` 不回池、`UntilClose` 分帧不回池、leftover 富余字节不回池（容忍模式下
///           本交换照常成功——装配器把完整之后的字节交回复用判据，富余非空即当场关连接，绝不把尾巴喂给
///           下一位）；陈旧连接（对端在空闲期关掉）按幂等方法换新连接重跑本跳且一尝试至多一次；治理口径
///           各一例：空闲超时淘汰、TLS 配置代次换代不复用、每键并发上限满时排队且到点落 `http-timeout`
///           并在 detail 点名「排队」；流式路径对称复用一例。全部以真实时钟 + ScriptedPeer 回环进行，
///           零真实远端、零 DNS。
/// 跳过形态：无（全部用例在任何腿上都成立）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/net/tls.h"
#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_session {
namespace {

using aurora::CoroTask;
using aurora::ErrorCode;
using aurora::Result;
using aurora::net::Cancellation;
using aurora::net::TlsContext;
using aurora::net::http::Client;
using aurora::net::http::DataSink;
using aurora::net::http::PoolOptions;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::SinkFlow;
using aurora::net::http::StreamOutcome;
using aurora::net::http::Timeouts;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 为测试放行明文（判据口径与 `utest_net_http_client` 的同名助手一致）。
auto allow_test_cleartext(Client &client) -> void {
    aurora::net::NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 读一次请求（读到头部块尾即回），档案交回调用方（线程经锁交接）。
/// @return 读到请求为 true（读失败 / 对端收摊为 false）。
auto read_request(aurora::net::blocking::StreamSocket &socket, PeerStop &stop, std::string &recorded, std::mutex &guard)
    -> bool {
    // 兜底只防「请求永远不来」的挂死：accept 到首字节的墙钟可被慢腿调度拉长，短档会把没写完的请求
    // 读成超时（见 utest_net_http_proxy 同款说明）。
    (void)socket.set_io_timeout(std::chrono::seconds{10});
    std::vector<char> buffer(8192);
    std::string seen;
    while (!stop.load()) {
        const auto read = socket.read_some(std::span<char>{buffer});
        if (!read || read.value() == 0) {
            break;
        }
        seen.append(buffer.data(), read.value());
        if (seen.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    const std::scoped_lock held(guard);
    recorded = seen;
    return !seen.empty();
}

/// @brief 取档案的小写副本。
[[nodiscard]] auto lowered_record(const std::string &recorded, std::mutex &guard) -> std::string {
    const std::scoped_lock held(guard);
    std::string out;
    out.reserve(recorded.size());
    for (const char c : recorded) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// @brief 一条连接跑 N 个「读请求 → 应答」周期的对端脚本（keep-alive 复用判据的对端半边）。
///
/// 脚本在 serve() 循环里**内联执行**，故「同一条连接服务多枚请求」由脚本自己循环；跑满周期即返回，
/// 让 serve() 有机会接受后续连接（那条连接随后被各自路径收摊）。
/// @param cycles 计划服务的请求-应答周期数。
/// @param response 每个周期写出的应答字节。
/// @param recorded 最后一枚请求的交接缓冲。
/// @param guard 交接缓冲的锁。
/// @return 对端脚本。
[[nodiscard]] auto serve_cycles(int cycles, std::string response, std::string &recorded, std::mutex &guard)
    -> PeerScript {
    return [cycles, response = std::move(response), &recorded, &guard](aurora::net::blocking::StreamSocket &socket,
                                                                       PeerStop &stop) -> void {
        for (int served = 0; served < cycles && !stop.load(); ++served) {
            if (!read_request(socket, stop, recorded, guard)) {
                return;
            }
            (void)socket.write_all(std::span<const char>{response});
        }
    };
}

/// @brief 一条连接一个「读请求 → 应答」周期、答完即返（不回池判据的对端半边：第二条连接由 serve() 受理）。
/// @param recorded 最后一枚请求的交接缓冲。
/// @param guard 交接缓冲的锁。
/// @param response 每个周期写出的应答字节。
/// @param close_after_write 写完是否立即关连接（陈旧连接判据的对端那一半）。
/// @return 对端脚本。
[[nodiscard]] auto serve_one_then(std::string response, std::string &recorded, std::mutex &guard,
                                  bool close_after_write) -> PeerScript {
    return [response = std::move(response), &recorded, &guard, close_after_write](
               aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        if (!read_request(socket, stop, recorded, guard)) {
            return;
        }
        (void)socket.write_all(std::span<const char>{response});
        if (close_after_write) {
            socket.close();  // 对端在空闲期关掉：客户端侧的下一次取用面对一条已死的连接
        }
        // 答完即返：连接的收摊由客户端（不复用即关）或脚本返回（析构即关）完成，serve() 不被占住。
    };
}

/// @brief 完成一次物化交换并交出结果（真实时钟；泵与取结果的口径同 `utest_net_http_client`）。
[[nodiscard]] auto send_and_drive(const Client &client, Request request) -> Result<Response> {
    // 常驻引擎的回投须有在岗值班者才落得住——本助手自带的替身在泵的同时排空全局队列。
    const aurora::test::net::MainQueueLoop duty;
    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);
    // 未收尾时 `result()` 读的是空结果槽（帧外内存），先 REQUIRE 收尾再读，段错误不代替失败判据。
    AURORA_TEST_REQUIRE_MSG(task.is_done(), "coro task not finished when result was read");
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    // 本套件每一条 send 都应当成功：内层失败在这里就地暴露（拿错误码当判据输出），不让 `.value()`
    // 在 CHECK 里抛出把现场盖掉。
    const auto &answered = outer.value();
    AURORA_TEST_REQUIRE_MSG(
        bool(answered), answered ? std::string{}
                                 : "send failed: code=" + std::to_string(static_cast<int>(answered.error().code_enum)) +
                                       " " + answered.error().message);
    return answered;
}

/// @brief 完成一次流式交换并交出结果。
/// @param client 客户端。
/// @param request 请求。
/// @param sink 接收侧宿主。
/// @return 交换的 Result 层。
[[nodiscard]] auto stream_and_drive(const Client &client, Request request, DataSink &sink) -> Result<StreamOutcome> {
    // 同 `send_and_drive`——回投须有在岗值班者。
    const aurora::test::net::MainQueueLoop duty;
    auto task = client.open_stream(std::move(request), sink);
    AURORA_TEST_CHECK(aurora::test::net::drive_until("stream done", [&task] { return task.is_done(); }).satisfied);
    AURORA_TEST_REQUIRE_MSG(task.is_done(), "coro task not finished when result was read");
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 取已交付应答的体长（入口参数是 Result 层：`send_and_drive` 已把失败挡在 REQUIRE 里）。
/// @param answered 交换结果。
/// @return 体字节数。
[[nodiscard]] auto body_size(const Result<Response> &answered) -> std::size_t { return answered.value().body().size(); }

/// @brief 只收不问的宿主替身：复用判据问的是连接数与体内容，不需要宿主做决定。
class CollectingSink final : public DataSink {
  public:
    /// @brief 记一次头部。
    /// @param head 头部块。
    /// @return 恒 `Continue`。
    auto on_head(const Response &head) -> SinkFlow override {
        (void)head;
        ++heads;
        return SinkFlow::Continue;
    }
    /// @brief 收一段体。
    /// @param bytes 明文段。
    /// @return 恒 `Continue`。
    auto on_data(std::span<const char> bytes) -> SinkFlow override {
        body.append(bytes.data(), bytes.size());
        return SinkFlow::Continue;
    }
    /// @brief 记收尾。
    auto on_end() -> void override { ++ends; }

    int heads{0};  ///< `on_head` 次数
    int ends{0};  ///< `on_end` 次数
    std::string body;  ///< 已收明文
};

/// @brief 一枚内容长度分帧的 200 应答。
[[nodiscard]] auto ok_response(std::string_view body, std::string_view extra_fields = "") -> std::string {
    std::string out = "HTTP/1.1 200 OK\r\n";
    out += extra_fields;
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out += body;
    return out;
}

AURORA_TEST_CASE(two_requests_share_one_pooled_connection) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer{serve_cycles(2, ok_response("served"), recorded, guard)};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(first) == 6);
    AURORA_TEST_CHECK(body_size(second) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 1);  // 第二次坐上了同一条连接：建连只发生一次
}

AURORA_TEST_CASE(pool_off_keeps_one_connection_per_send) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer{per_connection({serve_one_then(ok_response("first"), recorded, guard, false),
                                      serve_one_then(ok_response("second"), recorded, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;  // 缺省 `PoolOptions{}` = 关：既有口径逐字不变
    allow_test_cleartext(client);
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(first) == 5);
    AURORA_TEST_CHECK(body_size(second) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 2);
}

AURORA_TEST_CASE(connection_close_is_never_returned_to_the_pool) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 判据的锐利处：对端在 `Connection: close` 的应答之后**并不关连接**（第二个周期照常服务）。判对了，
    // 第二枚请求必然开新连接（对端第二条剧本收到它）；判错了（复用 close 应答的连接），第二枚请求会坐上
    // 第一条连接的第二周期，新连接一条都不来——`accepts()` 与第二条剧本的留档都拿不到那枚请求。
    std::string first_log;
    std::string second_log;
    std::mutex guard;
    auto read_into = [](aurora::net::blocking::StreamSocket &socket, PeerStop &stop, std::string &log,
                        std::mutex &guard, std::chrono::milliseconds read_guard) -> bool {
        (void)socket.set_io_timeout(read_guard);
        std::vector<char> buffer(8192);
        std::string seen;
        while (seen.find("\r\n\r\n") == std::string::npos && !stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                return false;
            }
            seen.append(buffer.data(), read.value());
        }
        const std::scoped_lock held(guard);
        log += seen;
        return true;
    };
    // 第一条连接：答完 `Connection: close` 的应答后继续等第二枚请求（守约的服务器不该等来，错的实现会来）。
    auto sloppy_but_keepalive = [&](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        if (!read_into(socket, stop, first_log, guard, std::chrono::seconds{10})) {
            return;
        }
        (void)socket.write_all(std::span<const char>{ok_response("first", "Connection: close\r\n")});
        if (!read_into(socket, stop, first_log, guard, std::chrono::milliseconds{300})) {
            return;  // 判对时这条连接上不会有第二枚请求（短窗是剧情：等什么都不会来）
        }
        (void)socket.write_all(
            std::span<const char>{std::string{"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nwrongly"}});
    };
    ScriptedPeer peer{
        per_connection({sloppy_but_keepalive, serve_one_then(ok_response("second"), second_log, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(body_size(first) == 5);
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(second) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 2);  // 对端明示关闭：这条连接不回池
    AURORA_TEST_CHECK(lowered_record(second_log, guard).find("/next") != std::string::npos);
}

AURORA_TEST_CASE(until_close_framing_is_never_returned_to_the_pool) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    // 第一条连接：无 `Content-Length` 的应答 + 单向停写（UntilClose 靠 FIN 判全，读完那一刻连接就没了）。
    auto first = [&](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        if (!read_request(socket, stop, recorded, guard)) {
            return;
        }
        (void)socket.write_all(std::span<const char>{std::string{"HTTP/1.1 200 OK\r\n\r\nfirst"}});
        (void)aurora::net::detail::shutdown_handle(socket.native_handle(), aurora::net::ShutdownMode::Write);
        // 停写之后即返：serve() 去受理第二条连接（客户端不复用这条，连接由收摊路径关闭）。
    };
    ScriptedPeer peer{per_connection({first, serve_one_then(ok_response("second"), recorded, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    const auto answer_first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(std::string{answer_first.value().body().begin(), answer_first.value().body().end()} == "first");
    const auto answer_second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(answer_second) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 2);  // UntilClose：读到关闭才算完的消息没有「还干净」的证据
}

AURORA_TEST_CASE(leftover_pipelined_bytes_abandon_reuse_but_keep_the_exchange) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 判据的锐利处与 connection_close 那条同型：对端在塞进富余字节之后**继续服务**第二条连接上的请求。
    // 判错了（富余非空仍回池），第二枚请求会坐上第一条连接——它的请求行前面压着上一条应答的尾巴，
    // 对端读到的请求全文不再是 `GET /next`，且新连接一条都不来（accepts 停在 1）。
    std::string first_log;
    std::string second_log;
    std::mutex guard;
    auto read_into = [](aurora::net::blocking::StreamSocket &socket, PeerStop &stop, std::string &log,
                        std::mutex &guard, std::chrono::milliseconds read_guard) -> bool {
        (void)socket.set_io_timeout(read_guard);
        std::vector<char> buffer(8192);
        std::string seen;
        while (seen.find("\r\n\r\n") == std::string::npos && !stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                return false;
            }
            seen.append(buffer.data(), read.value());
        }
        const std::scoped_lock held(guard);
        log += seen;
        return true;
    };
    auto pipeline_then_keepalive = [&](aurora::net::blocking::StreamSocket &socket, PeerStop &stop) -> void {
        if (!read_into(socket, stop, first_log, guard, std::chrono::seconds{10})) {
            return;
        }
        std::string payload = ok_response("ok");
        payload += "HTTP/1.1 200 OK\r\n";  // 下一条消息的前缀：谁都没请它来
        (void)socket.write_all(std::span<const char>{payload});
        if (!read_into(socket, stop, first_log, guard, std::chrono::milliseconds{300})) {
            return;  // 判对时这条连接上不会有第二枚请求（短窗是剧情：等什么都不会来）
        }
        (void)socket.write_all(
            std::span<const char>{std::string{"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nwrongly"}});
    };
    ScriptedPeer peer{
        per_connection({pipeline_then_keepalive, serve_one_then(ok_response("second"), second_log, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    const auto answer = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(body_size(answer) == 2);  // 本交换照常成功：富余字节不吞也不报错
    const auto next = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(next) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 2);  // 富余非空即放弃复用：尾巴绝不能成为下一条请求的前缀
    AURORA_TEST_CHECK(lowered_record(second_log, guard).find("/next") != std::string::npos);
}

AURORA_TEST_CASE(stale_pooled_connection_is_replaced_once_for_idempotent_methods) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    // 第一条连接：应答完立刻关（对端在空闲期关掉）；第二条：正常服务。
    ScriptedPeer peer{per_connection({serve_one_then(ok_response("first"), recorded, guard, true),
                                      serve_one_then(ok_response("second"), recorded, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(body_size(first) == 5);
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(second) == 6);  // 陈旧连接被换新重跑：交换以成功收尾
    AURORA_TEST_CHECK(peer.accepts() == 2);
}

AURORA_TEST_CASE(idle_timeout_evicts_the_pooled_connection) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer{per_connection({serve_one_then(ok_response("first"), recorded, guard, false),
                                      serve_one_then(ok_response("second"), recorded, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true, .idle_timeout = std::chrono::milliseconds{80}});
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(body_size(first) == 5);
    std::this_thread::sleep_for(std::chrono::milliseconds{150});  // 让空闲寿命走完（真实时钟可见）
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(second) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 2);  // 空闲超时的连接被淘汰：第二次重新建连
}

AURORA_TEST_CASE(tls_configuration_bump_blocks_reuse) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer{per_connection({serve_one_then(ok_response("first"), recorded, guard, false),
                                      serve_one_then(ok_response("second"), recorded, guard, false)})};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    const auto first = send_and_drive(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(body_size(first) == 5);
    client.tls_context(TlsContext{});  // 换代：池条目的口径已过期
    const auto second = send_and_drive(client, Request::get(loopback_url(peer.port(), "/next")));
    AURORA_TEST_CHECK(body_size(second) == 6);
    AURORA_TEST_CHECK(peer.accepts() == 2);  // 改了安全口径还坐旧连接 = 新配置静默不生效，本库不这么做
}

AURORA_TEST_CASE(per_host_limit_queues_and_times_out_naming_the_queue) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    ScriptedPeer peer{aurora::test::scripts::silent()};  // 第一条连接被它占住：连接上了但不说话
    AURORA_TEST_REQUIRE(peer.valid());
    // 本例直发两条交换、不经 `send_and_drive` 助手，值班替身就地自领。
    const aurora::test::net::MainQueueLoop duty;
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true, .max_connections_per_host = 1});
    // 两条交换的时限刻意分档。这里曾共用一份 `connect=80ms / total=400ms`：占位方也在 400ms 到点，负载机
    // 上它先收摊、把唯一名额归还，排队方随即领到一条真连接去建连——结局仍是一枚 `http-timeout`，但它报在
    // total 档，detail 里不再有「排队」，判据就红在非库的那一半上。现让占位方何时收摊由本例自己决定：它拿
    // 一份远超排队窗口的 total，本例收摊时才主动取消，于是「名额在整个窗口内被握着」是一件可断言的事实。
    Timeouts queue_limits;
    queue_limits.connect = std::chrono::milliseconds{80};
    queue_limits.total = std::chrono::milliseconds{400};
    Timeouts holder_limits;
    holder_limits.connect = std::chrono::milliseconds{80};
    holder_limits.total = std::chrono::seconds{30};
    Cancellation release;
    Request holder = Request::get(loopback_url(peer.port()));
    holder.timeouts(holder_limits);
    Request queuing = Request::get(loopback_url(peer.port(), "/queued"));
    queuing.timeouts(queue_limits);
    auto held_task = client.send(std::move(holder), Cancellation{release});  // 占住唯一名额（在途，不是空闲）
    auto queued_task = client.send(std::move(queuing));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("queued exchange settled", [&queued_task] {
                          return queued_task.is_done();
                      }).satisfied);
    const auto queued_outer = queued_task.result();
    AURORA_TEST_REQUIRE_MSG(bool(queued_outer), queued_outer ? std::string{} : queued_outer.error().message);
    // 双层解包：外层是「协程跑完没有」（必成），内层才是「排队超时」那枚失败——超时住在内层。
    const auto &queued_answer = queued_outer.value();
    AURORA_TEST_CHECK(!bool(queued_answer));  // 排队等不到：以内层失败收场
    const auto &queued_error = queued_answer.error();
    AURORA_TEST_CHECK(queued_error.code_enum == ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK(queued_error.message.find("queued") != std::string::npos);  // detail 点名「排队等不到」
    // 排队那条收摊时占位方仍 in flight：这条把「排队」钉成唯一可能的成因——它若已收摊，排队方就没等过什么。
    AURORA_TEST_CHECK(!held_task.is_done());
    release.cancel();
    AURORA_TEST_CHECK(aurora::test::net::drive_until("holding exchange settled", [&held_task] {
                          return held_task.is_done();
                      }).satisfied);
    const auto held_outer = held_task.result();
    AURORA_TEST_REQUIRE_MSG(bool(held_outer), held_outer ? std::string{} : held_outer.error().message);
    const auto &held_answer = held_outer.value();
    AURORA_TEST_CHECK(!bool(held_answer));
    // 占位那条由本例主动收摊，故落 `net-cancelled`；它原先那句「按 total 档收摊」在本文件不再承担——
    // total 档的口径由 `utest_net_http_client` 的 `send_times_out_whole_request` 那条专例管。
    AURORA_TEST_CHECK(held_answer.error().code_enum == ErrorCode::NetCancelled);
}

AURORA_TEST_CASE(streaming_path_reuses_the_pooled_connection_too) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    std::string recorded;
    std::mutex guard;
    ScriptedPeer peer{serve_cycles(2, ok_response("streamed"), recorded, guard)};
    AURORA_TEST_REQUIRE(peer.valid());
    Client client;
    allow_test_cleartext(client);
    client.pool(PoolOptions{.enabled = true});
    CollectingSink first_sink;
    CollectingSink second_sink;
    const auto first = stream_and_drive(client, Request::get(loopback_url(peer.port())), first_sink);
    const auto second = stream_and_drive(client, Request::get(loopback_url(peer.port(), "/next")), second_sink);
    AURORA_TEST_CHECK(first && !first.value().stopped_by_sink);
    AURORA_TEST_CHECK(second && !second.value().stopped_by_sink);
    AURORA_TEST_CHECK(first_sink.body == "streamed");
    AURORA_TEST_CHECK(second_sink.body == "streamed");
    AURORA_TEST_CHECK(peer.accepts() == 1);  // 流式路径对称复用
}

}  // namespace
}  // namespace aurora::test_cases::utest_net_http_session

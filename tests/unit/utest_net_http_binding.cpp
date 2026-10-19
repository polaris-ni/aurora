/// 测试类型: unit
/// 目标单元: include/aurora/net/http_binding.h + src/aurora/net/http_binding.cpp
/// 测试说明: 声明式交付面的离线回归（§12「声明式绑定」与「默认取图」）。以 `ScriptedPeer`（WebSocket 一族
///           另借 `framework/ws_peer.h` 的帧脚本）在回环上按脚本应答真实字节，验七件事各自的判据：`deliver`
///           把物化应答折成作业值且非 2xx 仍算成功落定、传输失败才走错误侧；`bind_resource` 的取消是真的取消——
///           `reload()` 与宿主销毁都会掐掉前一次请求的那条连接（对端读到收摊、且串行监听口因此才腾出手接受
///           第二条），迟到结果被代次守卫丢弃；交换的会话自持到帧收尾（调用方的 `Client` 临时对象在发起那一刻
///           就销毁也不悬垂。本条是**悬垂防护**而非形态判据：把会话改成不自持属未定义行为，实测未必显形，
///           所有权形态的判据住在 `utest_coroutine`
///           的 `on_done_releases_its_own_capture_once_it_has_fired`）；`deliver_stream`
///           逐段提交、偏移连续、成功收尾恰一条 `last` 标记而失败改走 `fail()`；`deliver_sse`
///           一条事件一条记录、闸门在首条记录成形之前判死时一条都不出、中途取消保留已提交的事件；`deliver_ws`
///           一条**成形消息**一条记录（分片不造条目边界、心跳不进条目流）、干净收尾恰出一条带三枚线上事实的
///           `last` 标记，而握手被拒与非干净收摊只走 `fail()`；`image_fetcher` 只在 2xx 给
///           字节，其余状态落 `render-image-load-failed` 并点名状态码，其函数形态与 widget 侧的取图口结构一致
///           （`static_assert` 钉住，网络层不因此依赖 widget）。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_binding.h"
#include "aurora/net/http_client.h"
#include "aurora/net/policy.h"
#include "aurora/state/async.h"
#include "aurora/state/resource.h"
#include "aurora/widget/image_widget.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"
#include "framework/ws_peer.h"

namespace aurora::test_cases::utest_net_http_binding {
namespace {

using aurora::ErrorCode;
using aurora::net::Cancellation;
using aurora::net::NetPolicy;
using aurora::net::WebSocketOptions;
using aurora::net::WsMessageKind;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::SseOptions;
using aurora::net::http::SseRecord;
using aurora::net::http::StreamChunk;
using aurora::net::http::WsRecord;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::write_then_hold;
using aurora::test::scripts::write_then_reset;
using aurora::test::ws::AURORA_OP_BINARY;
using aurora::test::ws::AURORA_OP_CLOSE;
using aurora::test::ws::AURORA_OP_CONTINUATION;
using aurora::test::ws::AURORA_OP_PING;
using aurora::test::ws::AURORA_OP_TEXT;
using aurora::test::ws::close_code_of;
using aurora::test::ws::close_payload;
using aurora::test::ws::loopback_ws_url;
using aurora::test::ws::serve_ws;
using aurora::test::ws::server_frame;
using aurora::test::ws::WsLog;
using aurora::test::ws::WsPlan;
using Socket = aurora::net::blocking::StreamSocket;
using aurora::testing::require_value;
namespace m = aurora::testing::matchers;

/// @brief 拼一个指向回环端口的 GET 目标。
/// @param port 对端端口。
/// @param path 路径。
/// @return 绝对 URL。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 拼一份 `Content-Length` 分帧的应答。
/// @param status 状态行（含原因短语，不含状态版本前缀之外的分隔）。
/// @param body 消息体。
/// @param fields 额外头部字段（每行自带 `\\r\\n`，可为空）。
/// @return 应答字节。
[[nodiscard]] auto framed(std::string_view status, std::string_view body, std::string_view fields = {}) -> std::string {
    return "HTTP/1.1 " + std::string{status} + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" +
           std::string{fields} + "\r\n" + std::string{body};
}

/// @brief 拼一份 chunked 应答的前半：状态行 + 逐块分帧，**不含**终止块。
/// @param status 状态行。
/// @param chunks 依次交出的体片段。
/// @return 应答字节；读到这些之后交换仍挂在读上（中途取消的现场）。
[[nodiscard]] auto chunked_prefix(std::string_view status, const std::vector<std::string> &chunks) -> std::string {
    std::string payload = "HTTP/1.1 " + std::string{status} + "\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (const auto &chunk : chunks) {
        payload += std::to_string(chunk.size()) + "\r\n" + chunk + "\r\n";
    }
    return payload;
}

/// @brief 把一段明文按 chunked 逐块分帧（每块一块，末尾跟终止块）。
/// @param status 状态行。
/// @param chunks 依次交出的体片段。
/// @return 应答字节。
[[nodiscard]] auto chunked(std::string_view status, const std::vector<std::string> &chunks) -> std::string {
    return chunked_prefix(status, chunks) + "0\r\n\r\n";
}

/// @brief 拼一份声明了事件流的 chunked 应答（`deliver_sse` 那族判据的字节来源）。
///
/// 块长按 `RFC 9110` 的口径写成十六进制：本文件的 `chunked()` 走的是「块长十进制恰好也等于十六进制」的小
/// 载荷，而事件正文一行就有 9~18 字节，越过 9 之后那两条写法开始分家（十位的十进制读成十六进制会多要字节）。
/// @param chunks 依次交出的体片段（片段边界与事件边界错位才是这里的常态）。
/// @param terminate 是否补上终止块（假 = 对端还在推，正是「中途取消」的现场）。
/// @return 应答字节。
[[nodiscard]] auto event_stream_response(const std::vector<std::string> &chunks, bool terminate = true) -> std::string {
    static constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
    std::string payload{"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n"};
    for (const auto &chunk : chunks) {
        std::size_t remaining = chunk.size();
        std::string size_text;
        do {
            size_text.insert(size_text.begin(), HEX_DIGITS[remaining % 16U]);
            remaining /= 16U;
        } while (remaining != 0);
        payload += size_text;
        payload += "\r\n";
        payload += chunk;
        payload += "\r\n";
    }
    if (terminate) {
        payload += "0\r\n\r\n";
    }
    return payload;
}

/// @brief 为测试放行明文：缺省安全策略拒绝回环 `http://`，触网用例逐客户端点名。
/// @param client 待配置的客户端。
auto allow_test_cleartext(Client &client) -> void {
    NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 一条「读到对端收摊才收摊」的静默脚本：它同时是前一次请求被掐断的现场记录仪。
///
/// 为什么以对端视角取证：取消的效果在调用方只看得见「结果被丢弃」，而那条被丢弃的结果**本来就该**被代次守卫
/// 丢掉，两者无从分辨。对端读到的那次收摊（EOF）才是「连接真被掐了」的唯一硬事实；又因为本对端按连接串行跑
/// 脚本，第二条连接能被接受本身就是前一条已收摊的证明。
/// @param closed 观测位：读到收摊时置真。
/// @return 脚本。
[[nodiscard]] auto hold_until_closed(std::atomic<bool> &closed) -> PeerScript {
    return [&closed](Socket &socket, PeerStop &stop) -> void {
        // 静默等对端收摊：超时只是防挂死的兜底。50ms 一档在慢腿上会在客户端关闭到达前就默默放弃，
        // 观测位拿不到那次 EOF，把「取消没生效」误报成现场缺失。放到 10s：EOF 到达即返回，不受影响。
        (void)socket.set_io_timeout(std::chrono::seconds{10});
        std::vector<char> buffer(4096);
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read) {
                return;  // 出错或对端收摊都收摊：观测位只认 0 字节这一次
            }
            if (read.value() == 0) {
                closed.store(true, std::memory_order_release);
                return;
            }
        }
    };
}

/// @brief 泵宿主直到谓词成立（缺省预算 2 s，比用例里任何一枚时限都短）。
/// @tparam Pred 谓词。
/// @param subject 等待主题。
/// @param pred 目标条件。
/// @param budget 预算；一次升级握手比一次物化交换多出「读环跑起来」这一程，故会话族用例显式放宽。
/// @return 条件成立为真。
template <typename Pred>
[[nodiscard]] auto pump(std::string_view subject, const Pred &pred,
                        std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) -> bool {
    return aurora::test::net::drive_until(subject, pred, budget).satisfied;
}

/// @brief 会话族用例的等待预算：与 `utest_net_websocket` 同一条口径（回环上走到 3 s 就是没走到）。
constexpr std::chrono::milliseconds AURORA_WS_WAIT{3000};

/// @brief 会话族用例的收摊窗口：交回驱动方之前把在途那枚 close 的写环跑完。
constexpr std::chrono::milliseconds AURORA_WS_DRAIN{30};

/// @brief 宿主驱动 + 收尾收摊泵（与 `utest_net_websocket` 的 `DrivenHost` 同一形态、同一理由）。
///
/// 公共头把会话收摊定为「尽力投递 `close(1000)` 再掐、不阻塞、不等待」，而那枚 close 的写环要靠宿主 pump 才
/// 走得完——不泵就走不完，协程帧连同会话与套接口一起留在堆上，本文件的 `ScriptedPeer` 还要在停机时 join 那
/// 条持有连接的脚本线程。成员排在析构体之后拆除，顺序因此是对的。
/// @note Thread: 与 `HostDriven` 同（用例线程安装与拆除）
struct WsHost {
    WsHost() = default;
    // 一轮泵要恢复挂起的续体（`net_wait.h` 的 `tail()` 与 `registry.dispatch`），那条路径按接口可抛。
    // 析构期没有可回报的通道，而本用例的判据在泵之前就已落定——让它越出去变成 terminate 只是把结果盖掉。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~WsHost() { aurora::test::net::pump_idle(AURORA_WS_DRAIN); }
    WsHost(const WsHost &) = delete;
    auto operator=(const WsHost &) -> WsHost & = delete;
    WsHost(WsHost &&) = delete;
    auto operator=(WsHost &&) -> WsHost & = delete;
    aurora::test::net::HostDriven driven;  ///< 宿主驱动开关（随本对象安装与拆除）
};

/// @brief 取回作业落定的结果：等待一次 `then` 抵达，再交出那份 `Result`。
/// @tparam T 作业值类型。
/// @param subject 等待主题（进超时文本）。
/// @param job 待等的作业。
/// @return 落定结果；等待超时时由 `REQUIRE` 收场。
template <typename T>
[[nodiscard]] auto await_job(std::string_view subject, aurora::Task<T> job) -> Result<T> {
    std::atomic<bool> settled{false};
    Result<T> outcome{make_error(ErrorCode::RuntimeAsyncException, "job never settled")};
    job.then([&settled, &outcome](const Result<T> &r) -> void {
        outcome = r;
        settled.store(true, std::memory_order_release);
    });
    AURORA_TEST_REQUIRE_MSG(pump(subject, [&settled] { return settled.load(std::memory_order_acquire); }),
                            "job did not settle within the wait budget");
    return outcome;
}

}  // namespace

AURORA_TEST_CASE(deliver_projects_a_materialized_response) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", "hello")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto job = aurora::net::http::deliver<std::string>(
        client, Request::get(loopback_url(peer.port())), [](const Response &response) -> Result<std::string> {
            return Result<std::string>{std::string(response.body().begin(), response.body().end())};
        });

    const auto outcome = await_job<std::string>("deliver settles", std::move(job));
    AURORA_TEST_REQUIRE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.value(), std::string{"hello"});
}

AURORA_TEST_CASE(a_non_success_status_is_a_ready_value_not_a_failure) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("404 Not Found", "missing")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    // 与 `send` 同口径：状态码归调用方判，本面不新造「HTTP 状态错误」。资源因此进 Ready 而不是 Failed。
    auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("resource ready", [&resource] { return resource.phase() == aurora::ResourcePhase::Ready; }));
    AURORA_TEST_REQUIRE(resource.value() != nullptr);
    AURORA_TEST_CHECK_EQ(resource.value()->status(), std::uint16_t{404});
    AURORA_TEST_CHECK_EQ(std::string(resource.value()->body().begin(), resource.value()->body().end()),
                         std::string{"missing"});
}

AURORA_TEST_CASE(a_transport_failure_lands_on_the_error_side) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(write_then_reset(""));  // 连上即 RST：读侧拿到一次真实的套接口错误
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        pump("resource failed", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; }));
    AURORA_TEST_CHECK_NE(require_value(resource.error()).code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_CHECK(resource.value() == nullptr);
}

AURORA_TEST_CASE(a_reload_cuts_the_socket_of_the_previous_request) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::atomic<bool> first_closed{false};
    // 第一条连接静默到被掐断，第二条照常应答：串行监听口能接受第二条，本身就是前一条已收摊的证明。
    ScriptedPeer peer(
        per_connection({hold_until_closed(first_closed), reply_after_reading(1, framed("200 OK", "second"))}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("first attempt connects", [&peer] { return peer.accepts() >= 1; }));
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Loading);

    resource.reload();  // ← 取消前序 + 重发：这一步必须同时掐掉那条挂着读的连接
    AURORA_TEST_CHECK(
        pump("second attempt lands", [&resource] { return resource.phase() == aurora::ResourcePhase::Ready; }));
    AURORA_TEST_CHECK(first_closed.load(std::memory_order_acquire));  // 对端读到收摊：不是只丢了结果
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);  // 串行对端腾出了第二条连接
    AURORA_TEST_REQUIRE(resource.value() != nullptr);
    AURORA_TEST_CHECK_EQ(std::string(resource.value()->body().begin(), resource.value()->body().end()),
                         std::string{"second"});  // 前一次的迟到结果没有盖住这一次
}

AURORA_TEST_CASE(destroying_the_owner_releases_the_in_flight_socket) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    std::atomic<bool> closed{false};
    ScriptedPeer peer(hold_until_closed(closed));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    {
        auto resource = aurora::net::http::bind_resource(client, Request::get(loopback_url(peer.port())));
        AURORA_TEST_CHECK(pump("attempt connects", [&peer] { return peer.accepts() >= 1; }));
    }  // 最后一个句柄放手 → 内核析构 → 取消作业 → 取消钩子掐连接
    AURORA_TEST_CHECK(pump("socket released", [&closed] { return closed.load(std::memory_order_acquire); }));
    AURORA_TEST_CHECK_TRUE(closed.load(std::memory_order_acquire));
}

AURORA_TEST_CASE(the_exchange_survives_the_callers_client) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", "late")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    std::optional<aurora::Task<Response>> job;
    {
        Client temporary;
        allow_test_cleartext(temporary);
        // 临时客户端在这条语句末尾就销毁，而交换还在协程里跑：帧持有的是那份堆上的会话拷贝，不是这里的 this。
        job.emplace(aurora::net::http::deliver(temporary, Request::get(loopback_url(peer.port()))));
    }
    const auto outcome = await_job<Response>("deliver settles", std::move(*job));
    AURORA_TEST_REQUIRE(bool(outcome));
    AURORA_TEST_CHECK_EQ(std::string(outcome.value().body().begin(), outcome.value().body().end()),
                         std::string{"late"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);
    (void)client;  // 另一份常驻客户端只为证明两种形态都能起
}

AURORA_TEST_CASE(deliver_stream_commits_segments_then_one_final_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, chunked("200 OK", {"abc", "de", "fghij"})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    // 写出条目类型：流资源装的就是 `StreamChunk`，段与收尾标记同一种形态。
    aurora::StreamResource<StreamChunk> resource =
        aurora::net::http::deliver_stream(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("stream closes", [&resource] {
        const auto &items = resource.items();
        return !items.empty() && items.back().last;
    }));

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE(items.size() >= 2);  // 至少一段体 + 一枚收尾标记
    std::uint64_t next_offset = 0;
    std::string text;
    for (const auto &chunk : items) {
        AURORA_TEST_CHECK_EQ(chunk.offset, next_offset);  // 偏移连续：乱序或多出一段当场可见
        AURORA_TEST_CHECK_EQ(chunk.status, std::uint16_t{200});
        next_offset += chunk.bytes.size();
        text.append(chunk.bytes.begin(), chunk.bytes.end());
    }
    AURORA_TEST_CHECK(items.back().bytes.empty());  // 收尾标记只说「收摊了」，不带字节
    AURORA_TEST_CHECK_EQ(text, std::string{"abcdefghij"});
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Ready);
}

AURORA_TEST_CASE(a_cancelled_stream_fails_without_a_final_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    // 两块发完就不再说（缺终止块）：交换挂在读上，正是「中途取消」的现场。
    ScriptedPeer peer(write_then_hold(chunked_prefix("200 OK", {"abc", "de"})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    Cancellation token;
    auto resource = aurora::net::http::deliver_stream(client, Request::get(loopback_url(peer.port())), token);
    // 先让至少一段落地：取消要停在「已经交付过」之后，才能验「已提交数据保留、收尾标记不再出」。
    AURORA_TEST_CHECK(pump("first segment lands", [&resource] { return !resource.items().empty(); }));
    AURORA_TEST_REQUIRE_MSG(!resource.items().empty(), "no segment reached the stream before cancel");
    const std::size_t committed = resource.items().size();

    token.cancel();
    AURORA_TEST_CHECK(pump("stream fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; }));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_REQUIRE(!resource.items().empty());
    AURORA_TEST_CHECK_FALSE(resource.items().back().last);  // 失败的流不产假收尾标记
    AURORA_TEST_CHECK_EQ(resource.items().size(), committed);  // 取消之后不再提交，已提交的都留着
}

AURORA_TEST_CASE(deliver_sse_commits_records_then_one_final_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    // 段边界与事件边界错位：第一条一整条在一块里，第二条的 `id` / `event` / `data` 三行挤在下一块里。
    ScriptedPeer peer(
        reply_after_reading(1, event_stream_response({"data: a\n\n", "id: 7\nevent: tick\ndata: b\n\n"})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    SseOptions options;
    options.reconnect = false;  // 本条判据要的是「一条流交完就收尾」，重连会把收尾标记推到第二次交换之后
    client.sse(options);

    const aurora::StreamResource<SseRecord> resource =
        aurora::net::http::deliver_sse(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(pump("subscription closes", [&resource] {
        const auto &items = resource.items();
        return !items.empty() && items.back().last;
    }));

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE_EQ(items.size(), std::size_t{3});  // 两条事件 + 一枚收尾标记
    AURORA_TEST_CHECK_EQ(items[0].index, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(items[0].event, std::string{"message"});  // 服务端没命名时折出的缺省类型
    AURORA_TEST_CHECK_EQ(items[0].data, std::string{"a"});
    AURORA_TEST_CHECK_EQ(items[0].id, std::string{});
    AURORA_TEST_CHECK_EQ(items[0].status, std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(items[1].index, std::uint64_t{1});
    AURORA_TEST_CHECK_EQ(items[1].event, std::string{"tick"});
    AURORA_TEST_CHECK_EQ(items[1].data, std::string{"b"});
    AURORA_TEST_CHECK_EQ(items[1].id, std::string{"7"});  // 派发时刻的续传标识，不是「本条带没带」
    AURORA_TEST_CHECK_FALSE(items[1].last);
    AURORA_TEST_CHECK_TRUE(items[2].last);
    AURORA_TEST_CHECK_EQ(items[2].index, std::uint64_t{2});  // 序号 = 事件总条数：下一条本该落在哪儿
    AURORA_TEST_CHECK_EQ(items[2].id, std::string{"7"});  // 收尾标记带的是收摊时手上那枚续传标识
    AURORA_TEST_CHECK(items[2].data.empty());
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Ready);
}

AURORA_TEST_CASE(a_cancelled_subscription_fails_but_keeps_committed_records) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    // 一块发完就不再说话（缺终止块）：订阅挂在读上，正是「宿主已收到过东西、又要中途停手」的现场。
    ScriptedPeer peer(write_then_hold(event_stream_response({"data: a\n\n"}, false)));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    Cancellation token;
    const auto resource = aurora::net::http::deliver_sse(client, Request::get(loopback_url(peer.port())), token);
    AURORA_TEST_CHECK(pump("first record lands", [&resource] { return !resource.items().empty(); }));
    AURORA_TEST_REQUIRE_MSG(!resource.items().empty(), "no record reached the stream before cancel");
    const std::size_t committed = resource.items().size();

    token.cancel();
    AURORA_TEST_CHECK(
        pump("subscription fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; }));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::NetCancelled);
    AURORA_TEST_REQUIRE(!resource.items().empty());
    AURORA_TEST_CHECK_FALSE(resource.items().back().last);  // 失败的订阅不产假收尾标记
    AURORA_TEST_CHECK_EQ(resource.items().size(), committed);  // 取消不回收已交出去的事件
}

AURORA_TEST_CASE(deliver_sse_reports_a_refused_stream_without_any_record) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    // 看着像事件流的纯文本：闸门在第一条记录成形之前就把这条流判死，宿主一条假记录都不该收到。
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", "data: a\n\n", "Content-Type: text/plain\r\n")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    SseOptions options;
    options.reconnect = false;
    client.sse(options);

    const auto resource = aurora::net::http::deliver_sse(client, Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        pump("subscription fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; }));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::HttpProtocolError);
    AURORA_TEST_CHECK(resource.items().empty());
}

AURORA_TEST_CASE(deliver_ws_commits_each_message_then_one_final_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 对端说完两条消息再说 close：这次收摊干净，收尾那条要把三枚线上事实（状态码 / 原因 / 由谁开启）交出去，
    // 而它们只挂在标记上——消息条目里没有这两枚旋钮的位置。
    WsLog log;
    ScriptedPeer peer(
        serve_ws(WsPlan{.frames = {server_frame(AURORA_OP_TEXT, "first"), server_frame(AURORA_OP_TEXT, "second"),
                                   server_frame(AURORA_OP_CLOSE, close_payload(1001U, "all done"))}},
                 log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const aurora::StreamResource<WsRecord> resource =
        aurora::net::http::deliver_ws(client, Request::get(loopback_ws_url(peer.port())));
    AURORA_TEST_CHECK(pump(
        "subscription closes",
        [&resource] {
            const auto &items = resource.items();
            return !items.empty() && items.back().last;
        },
        AURORA_WS_WAIT));

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE_EQ(items.size(), std::size_t{3});  // 两条消息 + 一枚收尾标记
    AURORA_TEST_CHECK_EQ(items[0].index, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(items[0].kind, WsMessageKind::Text);
    AURORA_TEST_CHECK_EQ(items[0].payload, std::string{"first"});
    AURORA_TEST_CHECK_FALSE(items[0].last);
    AURORA_TEST_CHECK_FALSE(items[0].close_code.has_value());  // 线上码只属于收尾那次交换
    AURORA_TEST_CHECK_EQ(items[1].index, std::uint64_t{1});
    AURORA_TEST_CHECK_EQ(items[1].payload, std::string{"second"});
    AURORA_TEST_CHECK_TRUE(items[2].last);
    AURORA_TEST_CHECK_EQ(items[2].index, std::uint64_t{2});  // 序号 = 消息总条数：下一条本该落在哪儿
    AURORA_TEST_CHECK(items[2].payload.empty());
    AURORA_TEST_REQUIRE(items[2].close_code.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(items[2].close_code.value(), std::uint16_t{1001});
    AURORA_TEST_CHECK_EQ(items[2].close_reason, std::string{"all done"});
    AURORA_TEST_CHECK_TRUE(items[2].by_peer);  // 这次收摊由对端的 close 帧开启
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Ready);
}

AURORA_TEST_CASE(deliver_ws_keeps_the_wire_kind_and_merges_a_fragmented_message) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 条目边界是**消息**而不是帧：三条分片合成一条记录，文本与二进制各留自己的类型，越界的字节不该冒出来。
    WsLog log;
    ScriptedPeer peer(serve_ws(WsPlan{.frames = {server_frame(AURORA_OP_TEXT, "readable"),
                                                 server_frame(AURORA_OP_BINARY, std::string{"\xff\xfe"}),
                                                 server_frame(AURORA_OP_TEXT, "fra", false),
                                                 server_frame(AURORA_OP_CONTINUATION, "gmen", false),
                                                 server_frame(AURORA_OP_CONTINUATION, "t!"),
                                                 server_frame(AURORA_OP_CLOSE, close_payload(1000U))}},
                               log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const aurora::StreamResource<WsRecord> resource =
        aurora::net::http::deliver_ws(client, Request::get(loopback_ws_url(peer.port())));
    AURORA_TEST_CHECK(pump(
        "subscription closes",
        [&resource] {
            const auto &items = resource.items();
            return !items.empty() && items.back().last;
        },
        AURORA_WS_WAIT));

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE_EQ(items.size(), std::size_t{4});  // 三条消息 + 一枚收尾标记
    AURORA_TEST_CHECK_EQ(items[0].kind, WsMessageKind::Text);
    AURORA_TEST_CHECK_EQ(items[0].payload, std::string{"readable"});
    AURORA_TEST_CHECK_EQ(items[1].kind, WsMessageKind::Binary);
    AURORA_TEST_CHECK_EQ(items[1].payload, std::string{"\xff\xfe"});  // 二进制原样：不许按文本判一遍就丢掉
    AURORA_TEST_CHECK_EQ(items[2].kind, WsMessageKind::Text);
    AURORA_TEST_CHECK_EQ(items[2].payload, std::string{"fragment!"});  // 三片合成的那一条
    AURORA_TEST_CHECK_EQ(items[3].index, std::uint64_t{3});
    AURORA_TEST_CHECK_TRUE(items[3].last);
}

AURORA_TEST_CASE(deliver_ws_leaves_heartbeat_frames_out_of_the_record_stream) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 心跳夹在两条消息之间：自动 pong 是协议义务、库里已做完，把它落成条目会让 `Replace` 策略下的读数变成
    // 探活字节而不是内容。
    WsLog log;
    ScriptedPeer peer(serve_ws(
        WsPlan{.frames = {server_frame(AURORA_OP_TEXT, "one"), server_frame(AURORA_OP_PING, "probe"),
                          server_frame(AURORA_OP_TEXT, "two"), server_frame(AURORA_OP_CLOSE, close_payload(1000U))}},
        log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const aurora::StreamResource<WsRecord> resource =
        aurora::net::http::deliver_ws(client, Request::get(loopback_ws_url(peer.port())));
    AURORA_TEST_CHECK(pump(
        "subscription closes",
        [&resource] {
            const auto &items = resource.items();
            return !items.empty() && items.back().last;
        },
        AURORA_WS_WAIT));

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE_EQ(items.size(), std::size_t{3});  // 两条消息 + 一枚收尾标记：ping 不在其中
    AURORA_TEST_CHECK_EQ(items[0].payload, std::string{"one"});
    AURORA_TEST_CHECK_EQ(items[1].payload, std::string{"two"});
    AURORA_TEST_CHECK_TRUE(items[2].last);
    AURORA_TEST_CHECK_EQ(items[2].index, std::uint64_t{2});  // 序号里没有为 ping 留的空位
}

AURORA_TEST_CASE(deliver_ws_fails_without_records_when_the_peer_refuses_the_upgrade) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 对端根本不肯升级：那次会话从未存在过，故一条记录都不出，失败只走 `fail()`。
    WsLog log;
    ScriptedPeer peer(serve_ws(WsPlan{.reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const auto resource = aurora::net::http::deliver_ws(client, Request::get(loopback_ws_url(peer.port())));
    AURORA_TEST_CHECK(pump(
        "subscription fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; },
        AURORA_WS_WAIT));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(resource.items().empty());
}

AURORA_TEST_CASE(deliver_ws_refuses_a_non_get_request_without_touching_the_peer) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 方法闸在建连之前（口径照 `Client::connect_websocket()`：只认 `GET`，既不静默改成 `GET` 也不照发）。
    WsLog log;
    ScriptedPeer peer(serve_ws({}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const auto resource = aurora::net::http::deliver_ws(client, Request::post(loopback_ws_url(peer.port())));
    AURORA_TEST_CHECK(pump(
        "subscription fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; },
        AURORA_WS_WAIT));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK(resource.items().empty());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);  // 一条连接都没发起
}

AURORA_TEST_CASE(a_cancelled_session_closes_from_our_side_and_lands_the_marker) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 宿主停手 = 这条订阅结束，而「结束」在 WebSocket 上是一次真的 close 交换：本端发 close(1000)、对端回声、
    // 干净收尾出一条标记。本条同时是那枚取消钩子与壳的自持链的形态判据——钩子没接上或壳没人认领时，这里既
    // 看不见线上的 close，也等不到标记。
    WsLog log;
    ScriptedPeer peer(serve_ws(
        WsPlan{.frames = {server_frame(AURORA_OP_TEXT, "first")}, .watch_client_frames = true, .answer_close = true},
        log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    Cancellation token;
    const auto resource =
        aurora::net::http::deliver_ws(client, Request::get(loopback_ws_url(peer.port())), WebSocketOptions{}, token);
    AURORA_TEST_CHECK(pump("first message lands", [&resource] { return !resource.items().empty(); }, AURORA_WS_WAIT));
    AURORA_TEST_REQUIRE(!resource.items().empty());
    const std::size_t committed = resource.items().size();

    token.cancel();
    AURORA_TEST_CHECK(pump(
        "subscription closes",
        [&resource] {
            const auto &items = resource.items();
            return !items.empty() && items.back().last;
        },
        AURORA_WS_WAIT));

    AURORA_TEST_CHECK_EQ(log.count(AURORA_OP_CLOSE), std::size_t{1});  // 本端恰发出一枚 close
    const auto sent = log.frame(0U);
    AURORA_TEST_REQUIRE(sent.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(close_code_of(std::string_view{sent->payload}), 1000);

    const auto &items = resource.items();
    AURORA_TEST_REQUIRE_EQ(items.size(), committed + 1);  // 已提交的消息留着，只多出收尾那一条
    AURORA_TEST_CHECK_TRUE(items.back().last);
    AURORA_TEST_CHECK_FALSE(items.back().by_peer);  // 这次收摊由本端的取消开启
    AURORA_TEST_REQUIRE(items.back().close_code.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(items.back().close_code.value(), std::uint16_t{1000});  // 对端回声里那枚码
    AURORA_TEST_CHECK(items.back().close_reason.empty());  // 回声镜像状态码、不镜像原因
    AURORA_TEST_CHECK_EQ(resource.phase(), aurora::ResourcePhase::Ready);
}

AURORA_TEST_CASE(deliver_ws_fails_on_an_eof_without_a_close_and_keeps_records) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const WsHost host;
    // 对端把写向停了却没说 close：协议眼里这是异常断连，本面据此落 `fail()` 而**不造**一枚收尾标记，
    // 已提交的消息照留（与 `deliver_stream` / `deliver_sse` 同一条纪律）。
    WsLog log;
    ScriptedPeer peer(serve_ws(
        WsPlan{.frames = {server_frame(AURORA_OP_TEXT, "the last thing it said")}, .stop_writing_after_frames = true},
        log));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    const auto resource = aurora::net::http::deliver_ws(client, Request::get(loopback_ws_url(peer.port())));
    AURORA_TEST_CHECK(pump(
        "subscription fails", [&resource] { return resource.phase() == aurora::ResourcePhase::Failed; },
        AURORA_WS_WAIT));
    AURORA_TEST_CHECK_EQ(require_value(resource.error()).code_enum, ErrorCode::WsClosed);
    const auto &items = resource.items();
    AURORA_TEST_REQUIRE_EQ(items.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(items[0].payload, std::string{"the last thing it said"});
    AURORA_TEST_CHECK_FALSE(items.back().last);  // 非干净收摊不产假标记
}

AURORA_TEST_CASE(image_fetcher_returns_the_body_on_a_success_status) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    const std::string png{"\x89PNG\r\n\x1a\nFAKE"};
    ScriptedPeer peer(reply_after_reading(1, framed("200 OK", png)));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto fetch = aurora::net::http::image_fetcher(client);
    const auto outcome =
        await_job<std::vector<std::uint8_t>>("image lands", fetch(std::string_view{loopback_url(peer.port())}));
    AURORA_TEST_REQUIRE(bool(outcome));
    const std::vector<std::uint8_t> expected(png.begin(), png.end());
    AURORA_TEST_CHECK(outcome.value() == expected);  // 逐字节等价：符号性转换不能改一个字节
}

AURORA_TEST_CASE(image_fetcher_maps_a_not_found_to_an_image_load_error) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, framed("404 Not Found", "<html>nope</html>")));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    allow_test_cleartext(client);
    auto fetch = aurora::net::http::image_fetcher(client);
    const auto outcome =
        await_job<std::vector<std::uint8_t>>("image fails", fetch(std::string_view{loopback_url(peer.port())}));
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, ErrorCode::RenderImageLoadFailed);
    // 原因必须点名状态码：404 的 HTML 喂给解码器只会换来「不是图片」，现场就此丢了。
    AURORA_TEST_CHECK_THAT(outcome.error().message, m::has_substr("404"));
}

AURORA_TEST_CASE(the_fetcher_shape_matches_the_widget_slot_without_depending_on_it) {
    // 结构一致是本面能装进 `ImageView` 的全部前提；网络层不 include widget，故这里由测试把两侧对齐。
    // 取图口别名住在 `aurora`（声明于 widget 侧头文件），本用例 include 它只为拿到这个别名。
    static_assert(std::is_same_v<decltype(aurora::net::http::image_fetcher()), aurora::ImageFetcher>);
    AURORA_TEST_CHECK_TRUE(true);
}

}  // namespace aurora::test_cases::utest_net_http_binding

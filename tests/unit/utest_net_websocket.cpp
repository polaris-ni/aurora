/// 测试类型: unit
/// 目标单元: src/aurora/net/websocket.cpp（会话三条环与句柄四员）+ `Client::connect_websocket` 的握手腿
///           （src/aurora/net/http_client.cpp）
/// 测试说明: `RFC 6455` 客户端的离线回归，分两层。
///           **握手腿**（真回环连接 + 脚本化应答）验只有跨一次真实交换才成立的事实：四枚字段由库写定且覆写
///           调用方同名值、体形态字段恒不上线、`Sec-WebSocket-Key` 逐条连接新取；非 GET 与非法 scheme 起手即拒
///           且对端一条连接都没连上；对端没说 101 的五型答复各点名自己那一处（状态码 / `Upgrade` /
///           `Connection` / 扩展宣告 / `Accept`）；子协议三型结局（回提出过的一枚、没回且不要求即成功、
///           回没提出过的或要求必选却没回即拒）；本腿不跟重定向也不按 `RetryPolicy` 重放；取消掐在途那次
///           握手落 `net-cancelled`，整请求时限到点改报 `http-timeout`；本腿无流式能力落
///           `net-platform-unavailable`。
///           **会话层**验只有「升级之后还活着的一条连接」才有的形态：头部块同批到达的字节属于帧流（不丢半条
///           消息）；分片按序重组、文本与二进制分道；`send` 的「入队即回 + 回调报落地」两拍、发出侧恒带掩码、
///           超单帧上限自动分片；三枚闸各可达且队满只拒这一次发送（会话照跑）；非法 UTF-8 在写出之前拒；
///           收到 ping 恒回同载荷 pong；心跳周期与 `pong_timeout` 在脚本化时钟上逐值可断言；对端 close 的回声
///           带上那枚状态码、显式 `close()` 之后不再接受发送；句柄析构尽力投递 `close(1000)` 且不再报
///           `on_closed`；在交付回调里销毁句柄即停止派发而会话判定照走；`SinkFlow::Stop` 只关派发；对端没说
///           close 就收摊分两种 detail；读侧越限交 `ws-protocol-error`；空句柄与移动后的源句柄一律 `ws-closed`
///           而不是崩溃；握手结算之后宿主的取消令牌不再掐这条会话。**活动记账**（`AURORA_ENABLE_DEBUG` 限定）
///           验握手腿只记一条 101、无 `BodyDone` 的记录——升级之后是帧流，会话的收摊事实在 `WsCloseInfo`。
///           全用例零真实远端、零 DNS（目标恒为 `ws://127.0.0.1:<port>`），故禁网 CI 无需豁免；浏览器腿没有
///           BSD socket 语义，凡触网的用例按 `AURORA_TEST_SKIP` 登记。

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/capabilities.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_observability.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/policy.h"
#include "aurora/net/transport.h"
#include "aurora/net/websocket.h"
#include "aurora/state/coroutine.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"
#include "framework/ws_peer.h"

namespace aurora::test_cases::utest_net_websocket {
namespace {

using aurora::CoroTask;
using aurora::Error;
using aurora::Result;
using aurora::net::Cancellation;
using aurora::net::Capabilities;
using aurora::net::NetPolicy;
using aurora::net::WebSocket;
using aurora::net::WebSocketOptions;
using aurora::net::WebSocketSink;
using aurora::net::WsCloseInfo;
using aurora::net::WsMessageKind;
using aurora::net::WsSent;
using aurora::net::blocking::StreamSocket;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::RetryPolicy;
using aurora::net::http::SinkFlow;
using aurora::net::http::Timeouts;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::ws::AURORA_OP_BINARY;
using aurora::test::ws::AURORA_OP_CLOSE;
using aurora::test::ws::AURORA_OP_CONTINUATION;
using aurora::test::ws::AURORA_OP_PING;
using aurora::test::ws::AURORA_OP_PONG;
using aurora::test::ws::AURORA_OP_TEXT;
using aurora::test::ws::close_code_of;
using aurora::test::ws::close_payload;
using aurora::test::ws::field_count;
using aurora::test::ws::field_value;
using aurora::test::ws::key_of;
using aurora::test::ws::loopback_ws_url;
using aurora::test::ws::make_peers;
using aurora::test::ws::SeenFrame;
using aurora::test::ws::serve_ws;
using aurora::test::ws::server_frame;
using aurora::test::ws::WsLog;
using aurora::test::ws::WsPlan;

/// @brief 用例收摊时的收摊泵窗口：把句柄析构那枚「尽力投递」的 close 写完。
constexpr std::chrono::milliseconds AURORA_DRAIN_WINDOW{30};

/// @brief 等待预算：回环上的一次收发是毫秒级，走到 3 s 就是没走到。
constexpr std::chrono::milliseconds AURORA_WAIT{3000};

/// @brief 「不该发生的事」的观察窗口：这类判据没有谓词可等，只能靠一段时间内什么都没出现来证。
constexpr std::chrono::milliseconds AURORA_QUIET{80};

/// @brief 宿主驱动守卫 + 收尾收摊泵（声明在句柄与 sink 之前，故析构排在它们之后）。
///
/// 与 `net::HostDriven` 的差别只有一件事：交回驱动方之前先把宿主 pump 跑一小段窗口。公共头把析构定为
/// 「尽力投递 `close(1000)` 再掐、不阻塞、不等待」，而那枚 close 的写环要靠宿主 pump 才走得完——不泵就走不完，
/// 协程帧连同会话与套接口一起留在堆上。守卫的析构体先跑、成员 `driven` 后拆，顺序因此是对的。
/// @note Thread: 与 `HostDriven` 同（用例线程安装与拆除）
struct DrivenHost {
    DrivenHost() = default;
    // 一轮泵要恢复挂起的续体（`net_wait.h` 的 `tail()` 与 `registry.dispatch`），那条路径按接口可抛。
    // 析构期没有可回报的通道，而本用例的判据在泵之前就已落定——让它越出去变成 terminate 只是把结果盖掉。
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~DrivenHost() { aurora::test::net::pump_idle(AURORA_DRAIN_WINDOW); }
    DrivenHost(const DrivenHost &) = delete;
    auto operator=(const DrivenHost &) -> DrivenHost & = delete;
    DrivenHost(DrivenHost &&) = delete;
    auto operator=(DrivenHost &&) -> DrivenHost & = delete;
    aurora::test::net::HostDriven driven;  ///< 宿主驱动开关（随本对象安装与拆除）
};

/// @brief 取协程结果的外层并搬出那枚句柄（载荷 move-only，第二次读拿到的是已搬空的值，故只读一次）。
[[nodiscard]] auto inner(CoroTask<Result<WebSocket>> &task) -> Result<WebSocket> {
    auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return Result<WebSocket>{std::move(outer.value())};
}

/// @brief 为测试放行明文：缺省安全策略拒绝一切未列入白名单的 cleartext 目标，而本套件的对端恒在回环地址上。
/// @param client 待配置的客户端。
auto allow_test_cleartext(Client &client) -> void {
    NetPolicy permissive;
    permissive.allow_cleartext_hosts = {"127.0.0.1"};
    client.policy(std::move(permissive));
}

/// @brief 记录逐条交付事实的会话侧宿主替身。
///
/// 载荷视图只在回调期间有效，故一切留用的字节都在这里当场拷走。`kinds` 记 `"text"` / `"binary"` 而不是枚举，
/// 为的是断言文本能直接读、也能整份比对。
class RecordingSink final : public WebSocketSink {
  public:
    /// @brief 记一条成形的消息。
    /// @param kind 消息类型。
    /// @param payload 载荷。
    /// @return 按 `stop_after` 决定继续还是收摊。
    auto on_message(WsMessageKind kind, std::string_view payload) -> SinkFlow override {
        ++messages;
        kinds.emplace_back(kind == WsMessageKind::Text ? "text" : "binary");
        payloads.emplace_back(payload);
        if (messages == hook_at && message_hook) {
            const auto hook = std::move(message_hook);
            message_hook = {};
            hook();  // 就地销毁句柄那一族判据的落点
        }
        if (stop_after > 0 && messages >= stop_after) {
            return SinkFlow::Stop;
        }
        return SinkFlow::Continue;
    }

    /// @brief 记一枚 ping。
    /// @param payload 探活载荷。
    auto on_ping(std::string_view payload) -> void override {
        ++pings;
        ping_payloads.emplace_back(payload);
    }

    /// @brief 记一枚 pong。
    /// @param payload 应答载荷。
    auto on_pong(std::string_view payload) -> void override {
        ++pongs;
        pong_payloads.emplace_back(payload);
    }

    /// @brief 记一次会话收尾。
    /// @param close 收尾事实（只借用，分量各自拷进记录）。
    auto on_closed(const WsCloseInfo &close) -> void override {
        ++closes;
        codes.push_back(close.code.has_value() ? static_cast<int>(*close.code) : -1);
        reasons.push_back(close.reason);
        by_peer.push_back(close.by_peer);
        failures.push_back(close.failure);
    }

    /// @brief 收尾是否干净（`failure` 既非未知码也无文本）。
    /// @param index 第几次收尾（从 0 起）。
    [[nodiscard]] auto settled_cleanly(std::size_t index) const -> bool {
        if (index >= failures.size()) {
            return false;
        }
        const Error &failure = failures[index];
        return failure.code_enum == aurora::ErrorCode::GeneralUnknown && failure.message.empty();
    }

    int messages{0};  ///< `on_message` 次数
    int pings{0};  ///< `on_ping` 次数
    int pongs{0};  ///< `on_pong` 次数
    int closes{0};  ///< `on_closed` 次数
    std::vector<std::string> kinds;  ///< 逐条消息的类型名
    std::vector<std::string> payloads;  ///< 逐条消息的载荷
    std::vector<std::string> ping_payloads;  ///< 逐枚 ping 的载荷
    std::vector<std::string> pong_payloads;  ///< 逐枚 pong 的载荷
    std::vector<int> codes;  ///< 逐次收尾的线上状态码（-1 = 没带码）
    std::vector<std::string> reasons;  ///< 逐次收尾的原因文本
    std::vector<bool> by_peer;  ///< 逐次收尾是否由对端开启
    std::vector<Error> failures;  ///< 逐次收尾的失败事实
    int stop_after{0};  ///< 第几条消息起回 `Stop`（0 = 从不）
    int hook_at{0};  ///< 在第几条消息的回调里执行 `message_hook`（0 = 从不）
    std::function<void()> message_hook;  ///< 交付现场执行一次（就地销毁句柄用）
};

/// @brief 发起一次握手并泵到收尾。
///
/// **调用方必须自己装好 `HostDriven`**：句柄交回后读环还要靠同一份宿主泵推进，守卫若装在本函数里就在
/// 返回时把驱动方交回了。
/// @param client 客户端。
/// @param request 握手请求。
/// @param sink 宿主替身。
/// @param options 会话选型。
/// @param token 取消令牌。
/// @param budget 收尾预算。
/// @return 握手结局（协程未收尾时由 REQUIRE 收场）。
[[nodiscard]] auto connect_and_drive(const Client &client, Request request, WebSocketSink &sink,
                                     const WebSocketOptions &options = {}, Cancellation token = {},
                                     std::chrono::milliseconds budget = std::chrono::milliseconds{3000})
    -> Result<WebSocket> {
    auto task = client.connect_websocket(std::move(request), sink, options, std::move(token));
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(
                                "connect_websocket done", [&task] { return task.is_done(); }, budget)
                                .satisfied,
                            "the upgrade handshake never settled inside the wait budget");
    return inner(task);
}

/// @brief 会话层用例的场景束：一份交接缓冲 + 一条按剧本服务的回环对端 + 宿主驱动 + 客户端 + 宿主替身。
///
/// 成员次序即**收摊次序**（销毁按声明逆序）：`log` 最先声明故最后销毁——脚本线程持有它的引用，必须活得比对端久；
/// `peer` 排在 `host` 之前，于是场景内部的拆除是「替身 → 客户端 → 收尾泵 → 对端 → 缓冲」。用例体内声明的句柄
/// 比场景更早销毁，其 `detach()` 之后本层一律不再触碰那个替身，故替身先走是安全的；而收尾泵仍跑在对端停机之前，
/// 「尽力投递」的那枚 close 因此有窗口真的写上线。
/// @note Thread: 用例线程（脚本线程只经 `WsLog` 交接）
struct WsScene {
    /// @brief 开一条按剧本服务的回环对端，装上宿主驱动并放行回环明文。
    /// @param plan 对端剧本。
    explicit WsScene(WsPlan plan) : peer(serve_ws(std::move(plan), log)) {
        AURORA_TEST_REQUIRE_MSG(peer.valid(), "the loopback listener could not be opened");
        allow_test_cleartext(client);
    }

    WsScene(const WsScene &) = delete;
    auto operator=(const WsScene &) -> WsScene & = delete;
    WsScene(WsScene &&) = delete;
    auto operator=(WsScene &&) -> WsScene & = delete;
    /// @brief 平凡析构（default）：拆除只按成员声明逆序走，本结构自身不另持资源（收摊次序见上方注释）。
    ~WsScene() = default;

    /// @brief 指向本场景对端的目标地址。
    /// @return `ws://127.0.0.1:<实际端口>/chat`。
    [[nodiscard]] auto url() const -> std::string { return loopback_ws_url(peer.port()); }

    /// @brief 握手并把句柄交给用例。
    /// @param options 会话选型。
    /// @param token 取消令牌。
    /// @return 升级后的句柄（握手失败即致命收场：会话层的用例个个都要一枚真句柄）。
    [[nodiscard]] auto open(const WebSocketOptions &options = {}, Cancellation token = {}) -> WebSocket {
        auto connected = connect_and_drive(client, Request::get(url()), sink, options, std::move(token), AURORA_WAIT);
        AURORA_TEST_REQUIRE_MSG(bool(connected), connected ? std::string{} : connected.error().message);
        return std::move(connected.value());
    }

    WsLog log;  ///< 交接缓冲（脚本线程写、用例线程读）
    ScriptedPeer peer;  ///< 按剧本服务的对端
    DrivenHost host;  ///< 宿主驱动 + 收尾收摊泵
    Client client;  ///< 被测客户端（已放行回环明文）
    RecordingSink sink;  ///< 宿主替身
};

/// @brief 落地回报的记录位。
///
/// `WsSent` 恒在主线程执行（续体经宿主泵就地恢复），而本套件全程 host-driven，故普通成员即可，不必上原子。
struct Landed {
    /// @brief 取出可直接交给 `send` / `send_binary` 的回调。
    /// @return 记录一次调用的可调用体。
    [[nodiscard]] auto callback() -> WsSent {
        // 结局按值交出、本层只读它的分量而不接管，故形参收 const 引用（`WsSent` 只要求可调用体能吃下该实参）。
        return [this](const Result<void> &outcome) -> void {
            ++calls;
            clean = outcome.ok();
            failure = outcome ? std::string{} : outcome.error().message;
        };
    }

    int calls{0};  ///< 回报次数（一条消息恒为一次）
    bool clean{false};  ///< 最后一次回报是否为成功
    std::string failure;  ///< 失败回报的文本；成功时为空
};

/// @brief 断言一次失败既落在期望的错误码上、文本里又点名了期望的那处事实。
///
/// 两半必须同时看：只比码会把「队列满」与「已收尾」读成同一件事，只看文本又放得下码表漂移。
/// @param error 交出的错误。
/// @param expected 期望的错误码。
/// @param needle 期望出现在 detail 里的片段。
auto check_failure(const Error &error, aurora::ErrorCode expected, std::string_view needle) -> void {
    AURORA_TEST_CHECK_EQ(error.code_enum, expected);
    AURORA_TEST_CHECK_MSG(error.message.find(needle) != std::string::npos, error.message);
}

/// @brief 断言一次被拒的调用（先 REQUIRE 它真的被拒，再核码与 detail）。
/// @param outcome 调用交回的结局。
/// @param expected 期望的错误码。
/// @param needle 期望出现在 detail 里的片段。
auto check_rejected(const Result<void> &outcome, aurora::ErrorCode expected, std::string_view needle) -> void {
    AURORA_TEST_REQUIRE_MSG(!bool(outcome), "the call was accepted, expected a rejection");
    check_failure(outcome.error(), expected, needle);
}

/// @brief 泵到第 `count` 条消息交付给宿主。
auto drive_until_messages(const RecordingSink &sink, int count, std::chrono::milliseconds budget) -> void {
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(
                                "message delivered", [&sink, count] { return sink.messages >= count; }, budget)
                                .satisfied,
                            "the session never delivered that many messages");
}

/// @brief 泵到某项观察事实抵达（谓词不成立即致命收场，省掉每个用例把 `WaitOutcome` 拆一遍）。
/// @tparam Pred 谓词，在调用线程求值。
/// @param subject 等待主题，进超时文本。
/// @param pred 目标条件。
template <typename Pred>
auto drive_until_fact(std::string_view subject, const Pred &pred) -> void {
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(subject, pred, AURORA_WAIT).satisfied,
                            std::string{subject} + " never happened inside the wait budget");
}

/// @brief 泵到对端解出 `count` 枚我方帧。
auto drive_until_frames(const WsLog &log, std::size_t count, std::chrono::milliseconds budget) -> void {
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(
                                "peer decoded frames", [&log, count] { return log.frames().size() >= count; }, budget)
                                .satisfied,
                            "the peer never saw that many frames from this side");
}

/// @brief 泵到会话收尾事实抵达宿主。
auto drive_until_closed(RecordingSink &sink, std::chrono::milliseconds budget) -> void {
    AURORA_TEST_REQUIRE_MSG(aurora::test::net::drive_until(
                                "session closed", [&sink] { return sink.closes >= 1; }, budget)
                                .satisfied,
                            "the session never reported its closing inside the wait budget");
}

}  // namespace

// ---- 握手腿：字段写定、路由与判定 ----

AURORA_TEST_CASE(websocket_handshake_writes_the_four_library_fields_and_strips_the_body) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    ScriptedPeer peer(serve_ws({}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    const auto request = Request::get(loopback_ws_url(peer.port()))
                             .header("Upgrade", "keep-alive")  // 同名值被覆写，不并列
                             .header("Origin", "http://localhost")
                             .header("Content-Encoding", "gzip")
                             .body(std::vector<char>{'b', 'o', 'd', 'y'});
    auto handle = connect_and_drive(client, request, sink);
    AURORA_TEST_REQUIRE_MSG(bool(handle), handle ? std::string{} : handle.error().message);

    const std::string head = log.head(0);
    AURORA_TEST_CHECK_TRUE(head.rfind("GET /chat HTTP/1.1\r\n", 0U) == 0U);
    AURORA_TEST_CHECK_EQ(field_count(head, "Upgrade"), std::size_t{1});  // 覆写而非并列：两枚 key 验哪枚都是猜
    AURORA_TEST_CHECK_EQ(field_value(head, "Upgrade"), "websocket");
    AURORA_TEST_CHECK_EQ(field_value(head, "Connection"), "Upgrade");
    AURORA_TEST_CHECK_EQ(field_value(head, "Sec-WebSocket-Version"), "13");
    AURORA_TEST_CHECK_EQ(field_value(head, "Sec-WebSocket-Key").size(), std::size_t{24});  // 16 字节熵的 base64
    AURORA_TEST_CHECK_EQ(field_value(head, "Origin"), "http://localhost");  // 调用方自己的字段照发
    AURORA_TEST_CHECK_EQ(field_count(head, "Content-Length"), std::size_t{0});  // 握手恒不写体
    AURORA_TEST_CHECK_EQ(field_count(head, "Transfer-Encoding"), std::size_t{0});
    AURORA_TEST_CHECK_EQ(field_count(head, "Content-Encoding"), std::size_t{0});
}

AURORA_TEST_CASE(websocket_handshake_draws_a_fresh_key_for_every_connection) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    auto peers = make_peers({WsPlan{}, WsPlan{}}, log);

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    auto first = connect_and_drive(client, Request::get(loopback_ws_url(peers[0]->port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(first), first ? std::string{} : first.error().message);
    auto second = connect_and_drive(client, Request::get(loopback_ws_url(peers[1]->port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(second), second ? std::string{} : second.error().message);

    // 同一个客户端、同一个目标：key 相同就意味着第三方可凭请求内容预判 Accept、抢在真客户端之前占住会话。
    AURORA_TEST_CHECK_TRUE(!key_of(log.head(0)).empty());
    AURORA_TEST_CHECK_NE(key_of(log.head(0)), key_of(log.head(1)));
}

AURORA_TEST_CASE(websocket_handshake_refuses_a_non_get_request_without_touching_the_peer) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    ScriptedPeer peer(serve_ws({}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    const auto outcome = connect_and_drive(client, Request::post(loopback_ws_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK(outcome.error().message.find("GET") != std::string::npos);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);  // 方法闸在建连之前：一条连接都没发起
    AURORA_TEST_CHECK_EQ(sink.closes, 0);  // 那次会话从未存在过，`on_closed` 无从谈起
}

AURORA_TEST_CASE(websocket_handshake_refuses_a_plain_http_target_without_connecting) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    ScriptedPeer peer(serve_ws({}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    const auto outcome =
        connect_and_drive(client, Request::get("http://127.0.0.1:" + std::to_string(peer.port()) + "/chat"), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(outcome.error().message.find("not ws or wss") != std::string::npos);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);  // scheme 闸也在建连之前：不是「连上了才发现升不了」
}

AURORA_TEST_CASE(websocket_handshake_reports_a_peer_that_closes_without_answering) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 脚本读完请求就返回：`StreamSocket` 随脚本作用域销毁，本端拿到一次真实的 FIN（0 字节）。
    WsLog log;
    ScriptedPeer peer([&log](StreamSocket &socket, PeerStop &) -> void {
        (void)socket.set_io_timeout(std::chrono::seconds{10});
        std::vector<char> scratch(4096);
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto read = socket.read_some(std::span<char>{scratch});
            if (!read || read.value() == 0U) {
                break;
            }
            request.append(scratch.data(), read.value());
        }
        log.record_head(request);
    });
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    const auto outcome = connect_and_drive(client, Request::get(loopback_ws_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(outcome.error().message.find("closed the connection without answering the upgrade") !=
                      std::string::npos);
}

AURORA_TEST_CASE(websocket_handshake_names_the_field_that_refused_the_upgrade) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 四型「不肯升级」的答复共用一枚码，但点名的是不同字段：把 426 与「Upgrade 拼错了」混成一句，
    // 就等于把排查的唯一线索丢掉。
    WsLog log;
    auto peers = make_peers(
        {
            WsPlan{.reply = "HTTP/1.1 426 Upgrade Required\r\nConnection: Upgrade\r\n\r\n"},
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n\r\n"},
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: h261\r\nConnection: Upgrade\r\n\r\n"},
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive\r\n\r\n"},
        },
        log);

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    const std::vector<std::string> needles{"answered 426", "no Upgrade field", "names \"h261\"",
                                           "does not carry the upgrade token"};
    for (std::size_t index = 0U; index < needles.size(); ++index) {
        RecordingSink sink;
        const auto outcome = connect_and_drive(client, Request::get(loopback_ws_url(peers[index]->port())), sink);
        AURORA_TEST_REQUIRE_MSG(!bool(outcome), "connection " + std::to_string(index) + " unexpectedly upgraded");
        AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
        AURORA_TEST_CHECK_MSG(outcome.error().message.find(needles[index]) != std::string::npos,
                              outcome.error().message);
        AURORA_TEST_CHECK_EQ(sink.closes, 0);
    }
}

AURORA_TEST_CASE(websocket_handshake_refuses_an_unprovable_or_overreaching_accept) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // `Accept` 是握手腿上唯一的双向身份校验：对不上意味着对端没读懂那次请求，此后任何字节都不属于这条会话。
    WsLog log;
    auto peers = make_peers(
        {
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n"},
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: c29tZXRoaW5nZWxzZQ==\r\n\r\n"},
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Extensions: permessage-deflate\r\n"
                            "Sec-WebSocket-Accept: @accept@\r\n\r\n"},
        },
        log);

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    const std::vector<std::string> needles{"carries no Sec-WebSocket-Accept field", "does not match the value",
                                           "announced an extension"};
    for (std::size_t index = 0U; index < needles.size(); ++index) {
        RecordingSink sink;
        const auto outcome = connect_and_drive(client, Request::get(loopback_ws_url(peers[index]->port())), sink);
        AURORA_TEST_REQUIRE_MSG(!bool(outcome), "connection " + std::to_string(index) + " unexpectedly upgraded");
        AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
        AURORA_TEST_CHECK_MSG(outcome.error().message.find(needles[index]) != std::string::npos,
                              outcome.error().message);
    }
}

AURORA_TEST_CASE(websocket_handshake_honours_the_subprotocol_verdicts) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 三型结局同批验：回提出过的一枚 = 成功且带名；没回且不要求必选 = **成功**且名字为空（降级是合法的协商
    // 结局，库不替宿主判成失败）；没回而要求必选 = 拒。
    WsLog log;
    auto peers = make_peers(
        {
            WsPlan{.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                            "Sec-WebSocket-Protocol: chat\r\nSec-WebSocket-Accept: @accept@\r\n\r\n"},
            WsPlan{},
            WsPlan{},
        },
        log);

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    const WebSocketOptions offered{.subprotocols = {"chat", "json"}};
    RecordingSink sink;
    auto negotiated = connect_and_drive(client, Request::get(loopback_ws_url(peers[0]->port())), sink, offered);
    AURORA_TEST_REQUIRE_MSG(bool(negotiated), negotiated ? std::string{} : negotiated.error().message);
    AURORA_TEST_CHECK_EQ(std::string{negotiated.value().subprotocol()}, "chat");
    AURORA_TEST_CHECK_TRUE(log.head(0).find("Sec-WebSocket-Protocol: chat, json") != std::string::npos);

    auto degraded = connect_and_drive(client, Request::get(loopback_ws_url(peers[1]->port())), sink, offered);
    AURORA_TEST_REQUIRE_MSG(bool(degraded), degraded ? std::string{} : degraded.error().message);
    AURORA_TEST_CHECK_TRUE(degraded.value().subprotocol().empty());
    AURORA_TEST_CHECK_TRUE(log.head(1).find("Sec-WebSocket-Protocol: chat, json") != std::string::npos);

    const WebSocketOptions required{.subprotocols = {"chat"}, .require_subprotocol = true};
    const auto refused = connect_and_drive(client, Request::get(loopback_ws_url(peers[2]->port())), sink, required);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(refused.error().message.find("requires one of the offered subprotocols") != std::string::npos);
}

AURORA_TEST_CASE(websocket_handshake_refuses_a_subprotocol_the_client_never_offered) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    ScriptedPeer peer(
        serve_ws({.reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Protocol: binary\r\nSec-WebSocket-Accept: @accept@\r\n\r\n"},
                 log));
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    const auto outcome =
        connect_and_drive(client, Request::get(loopback_ws_url(peer.port())), sink, WebSocketOptions{});
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(outcome.error().message.find("never offered") != std::string::npos);
}

AURORA_TEST_CASE(websocket_handshake_neither_follows_redirects_nor_replays) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 一条已经升级的连接没法「换个地址再升一次」，故 3xx 一律是终止答复；重放同理——`ws-handshake-rejected`
    // 在错误表里标不可重试，本腿压根不进重试环（对照组：把 `attempts` 拧到 3 也只连一次）。
    WsLog log;
    ScriptedPeer peer(
        serve_ws({.reply = "HTTP/1.1 301 Moved Permanently\r\nLocation: ws://127.0.0.1:1/elsewhere\r\n\r\n"}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    auto request = Request::get(loopback_ws_url(peer.port()));
    request.retry_policy(RetryPolicy{.attempts = 3});
    const auto outcome = connect_and_drive(client, std::move(request), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::WsHandshakeRejected);
    AURORA_TEST_CHECK(outcome.error().message.find("answered 301") != std::string::npos);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 不跟重定向、不重放：一条都不多发起
}

AURORA_TEST_CASE(websocket_handshake_reports_a_total_timeout) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 与另两腿同一条改道判据：看门狗到点取消令牌，挂起的读以 `net-cancelled` 收尾，收尾据 `fired` 档号
    // 改报 `http-timeout`——「时限到的」与「宿主说停的」在码上必须可分。
    WsLog log;
    ScriptedPeer peer(serve_ws({.reply = std::string{}}, log));  // 读完请求之后一字不回：整请求时限在此可见
    AURORA_TEST_REQUIRE(peer.valid());
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const DrivenHost host;

    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    auto request = Request::get(loopback_ws_url(peer.port()));
    request.timeouts(Timeouts{.total = std::chrono::milliseconds{50}});
    auto task = client.connect_websocket(std::move(request), sink, WebSocketOptions{}, Cancellation{});
    AURORA_TEST_REQUIRE(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_REQUIRE(
        aurora::test::net::drive_until(
            "connect_websocket done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
            .satisfied);
    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK_EQ(sink.closes, 0);
}

AURORA_TEST_CASE(websocket_handshake_reports_a_cancellation_during_the_exchange) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    WsLog log;
    // 应答是一段「连接留着、但什么都不写」的沉默：握手卡在等头部块，取消正好落在那次在途读上。
    ScriptedPeer peer(serve_ws({.reply = std::string{}}, log));
    AURORA_TEST_REQUIRE(peer.valid());

    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    RecordingSink sink;
    Cancellation token;
    auto task = client.connect_websocket(Request::get(loopback_ws_url(peer.port())), sink, WebSocketOptions{}, token);
    AURORA_TEST_REQUIRE(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    token.cancel();
    AURORA_TEST_REQUIRE(
        aurora::test::net::drive_until(
            "connect_websocket done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
            .satisfied);
    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::NetCancelled);
    AURORA_TEST_CHECK_EQ(sink.closes, 0);
}

AURORA_TEST_CASE(websocket_handshake_refuses_a_leg_without_streaming_semantics) {
    FakeEngine engine;
    Capabilities caps;
    caps.streaming = false;
    engine.set_capabilities(caps);
    const FakeEngineScope scope(engine);

    RecordingSink sink;
    const DrivenHost host;
    Client client;
    allow_test_cleartext(client);
    const auto outcome = connect_and_drive(client, Request::get("ws://127.0.0.1:1/chat"), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::NetPlatformUnavailable);
    AURORA_TEST_CHECK_EQ(sink.closes, 0);  // 绝不退化成「物化一次交换再冒充会话」
}

// ---- 会话层：交付、发送两拍、上限闸、心跳与收摊 ----

AURORA_TEST_CASE(websocket_delivers_the_bytes_glued_to_the_upgrade_head) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 一次读不会刚好停在 `\r\n\r\n` 的边界上：头部块之后同批到齐的字节属于**帧流**。把它们当成握手的富余
    // 丢掉，宿主就凭空少收两条从没见过的消息。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "first"), server_frame(AURORA_OP_TEXT, "second")},
        .glue_frames = true,
    }};
    auto handle = scene.open();
    drive_until_messages(scene.sink, 2, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 2);
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[0U], "first");
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[1U], "second");
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
}

AURORA_TEST_CASE(websocket_reassembles_fragments_in_order_without_dispatching_each_one) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 中间片不派发生物：把「还没说完的半条」交给宿主，等于让每个宿主自己写一遍重组与乱序判定。
    WsScene scene{WsPlan{.frames = {server_frame(AURORA_OP_TEXT, "fra", false),
                                    server_frame(AURORA_OP_CONTINUATION, "gmen", false),
                                    server_frame(AURORA_OP_CONTINUATION, "t!")}}};
    auto handle = scene.open();
    drive_until_messages(scene.sink, 1, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 1);
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[0U], "fragment!");
    AURORA_TEST_CHECK_EQ(scene.sink.kinds[0U], "text");
}

AURORA_TEST_CASE(websocket_keeps_text_and_binary_messages_apart) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 二进制载荷里放着非法 UTF-8：文本那型会在整条消息上判，二进制必须**一个判定都不过**，否则 protobuf 之类
    // 的合法字节流会被本层当成协议违例掐掉。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "readable"), server_frame(AURORA_OP_BINARY, std::string{"\xff\xfe"})}}};
    auto handle = scene.open();
    drive_until_messages(scene.sink, 2, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.kinds[0U], "text");
    AURORA_TEST_CHECK_EQ(scene.sink.kinds[1U], "binary");
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[1U].size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[1U], std::string{"\xff\xfe"});
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
}

AURORA_TEST_CASE(websocket_reports_a_send_as_accepted_then_landed) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 两拍分开的理由：入队是「这次发送被不被接受」（同步可得、能据此立刻显示状态），落地是「字节交没交给传输」
    // （形态是传输错误或会话收摊）。合成一拍就得让 UI 等一次写出。
    // 核的是**两拍各有各的分量**而不是先后次序：`http_await.h` 的发起器对循环套接字上的写有同步完成这条快
    // 路径，落地回调完全可能在 `send()` 返回之前就跑完了，那既不是缺陷也不是判据——判据是「一次发送交出多少
    // 事实」：返回值一条，回调一条，且一条消息只报一次（分片消息的「只报一次」见下一条用例）。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    auto handle = scene.open();
    Landed landed;
    const auto accepted = handle.send("hello", landed.callback());
    AURORA_TEST_REQUIRE(bool(accepted));  // 第一拍：被接受，文本为空即成功

    drive_until_frames(scene.log, 1, AURORA_WAIT);
    AURORA_TEST_CHECK_EQ(landed.calls, 1);  // 第二拍：落地事实恰好一条
    AURORA_TEST_CHECK_TRUE(landed.clean);
    AURORA_TEST_CHECK_TRUE(landed.failure.empty());  // 干净落地不带文本
    const auto seen = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_TRUE(seen->masked);  // 客户端方向恒带掩码（`RFC 6455 §5.1`）
    AURORA_TEST_CHECK_EQ(seen->opcode, AURORA_OP_TEXT);
    AURORA_TEST_CHECK_TRUE(seen->final);
    AURORA_TEST_CHECK_EQ(seen->payload, "hello");
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(handle.queued_bytes(), std::size_t{0});
}

AURORA_TEST_CASE(websocket_splits_a_message_over_the_frame_limit_into_ordered_fragments) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 分片不是「超限即拒」的另一型，而是同一条消息的线上形态：头片带类型、续片用 continuation、只有最后一片
    // FIN 为真。三处任一处写错，对端解出来的就是三条怪消息或一条永不收尾的消息。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    const WebSocketOptions small{.max_frame_bytes = 10};
    auto handle = scene.open(small);
    const std::string text(25U, 'a');
    Landed landed;
    const auto accepted = handle.send(std::string_view{text}, landed.callback());
    AURORA_TEST_REQUIRE(bool(accepted));

    drive_until_frames(scene.log, 3, AURORA_WAIT);
    const auto frames = scene.log.frames();
    AURORA_TEST_REQUIRE_EQ(frames.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(frames[0U].opcode, AURORA_OP_TEXT);
    AURORA_TEST_CHECK_FALSE(frames[0U].final);
    AURORA_TEST_CHECK_EQ(frames[0U].payload.size(), std::size_t{10});
    AURORA_TEST_CHECK_EQ(frames[1U].opcode, AURORA_OP_CONTINUATION);
    AURORA_TEST_CHECK_FALSE(frames[1U].final);
    AURORA_TEST_CHECK_EQ(frames[1U].payload.size(), std::size_t{10});
    AURORA_TEST_CHECK_EQ(frames[2U].opcode, AURORA_OP_CONTINUATION);
    AURORA_TEST_CHECK_TRUE(frames[2U].final);
    AURORA_TEST_CHECK_EQ(frames[2U].payload.size(), std::size_t{5});
    AURORA_TEST_CHECK_EQ(frames[0U].payload + frames[1U].payload + frames[2U].payload, text);
    AURORA_TEST_CHECK_EQ(landed.calls, 1);  // 最后一片落地才算「这条消息已写出」
}

AURORA_TEST_CASE(websocket_refuses_non_utf8_text_before_writing_a_single_frame) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 写出侧的 UTF-8 闸落在**写出之前**：同一份坏字节交给对端去判，换来的是对端单方面踩一次刹车（协议违例
    // 即收摊），而本层本来一句话都不用发。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    auto handle = scene.open();
    check_rejected(handle.send(std::string_view{"bad\xff"}), aurora::ErrorCode::WsProtocolError,
                   "text payload is not valid UTF-8 at byte 3");
    aurora::test::net::pump_idle(AURORA_QUIET);

    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_TEXT), std::size_t{0});  // 一个字节都没上线
    AURORA_TEST_CHECK_TRUE(handle.is_open());  // 被拒的是这次发送，不是这条会话
    AURORA_TEST_REQUIRE(bool(handle.send("still fine")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);
    const auto seen = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(seen->payload, "still fine");
}

AURORA_TEST_CASE(websocket_refuses_a_message_over_the_message_limit) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 整消息上限读写共用同一枚旋钮：发出去之前拒一条 9 字节的消息，与收进来时拒一条 9 字节的消息，说的是
    // 同一句话「这条流上容不下这么长的消息」。落 `net-buffer-limit` 而不是协议违例：违例的是配置，不是对端。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    const WebSocketOptions tiny{.max_message_bytes = 8};
    auto handle = scene.open(tiny);
    check_rejected(handle.send(std::string_view{"012345678"}), aurora::ErrorCode::NetBufferLimit,
                   "message of 9 bytes exceeds the configured 8 byte message limit");
    AURORA_TEST_CHECK_TRUE(handle.is_open());
    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_TEXT), std::size_t{0});
}

AURORA_TEST_CASE(websocket_refuses_any_payload_when_the_frame_limit_is_zero) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 旋钮被拧到 0 时本层不静默放宽、也不退化成「一帧装下」：那会让旋钮说谎。零长载荷仍是一条合法的空消息，
    // 于是这一型分岔恰好把「拒的是非空载荷」这件事钉在可观察的形态上。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    const WebSocketOptions zero{.max_frame_bytes = 0};
    auto handle = scene.open(zero);
    check_rejected(handle.send(std::string_view{"one byte is already too much"}), aurora::ErrorCode::NetBufferLimit,
                   "the frame limit is 0, so no non-empty payload can be framed");
    AURORA_TEST_REQUIRE(bool(handle.send(std::string_view{})));

    drive_until_frames(scene.log, 1, AURORA_WAIT);
    const auto seen = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(seen->opcode, AURORA_OP_TEXT);
    AURORA_TEST_CHECK_TRUE(seen->final);
    AURORA_TEST_CHECK_TRUE(seen->payload.empty());
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(websocket_rejects_a_send_when_the_queue_slot_limit_is_reached) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 闸问的是**编帧之前**需要的条数（25 字节 / 上限 10 → 3 片，此处 100 字节 / 上限 16 → 7 片），因为先编帧
    // 再量长度等于为了判断一次越限而分配出整条消息。队满只拒这一次发送：会话、连接、后续发送都不受影响。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    const WebSocketOptions tight{.max_frame_bytes = 16, .max_queued_frames = 3};
    auto handle = scene.open(tight);
    check_rejected(handle.send(std::string(100U, 'x')), aurora::ErrorCode::WsSendQueueFull,
                   "the send queue frame slots needs 7 while 3 is the configured limit");

    AURORA_TEST_CHECK_TRUE(handle.is_open());
    AURORA_TEST_CHECK_EQ(handle.queued_bytes(), std::size_t{0});  // 被拒的发送一字未入队
    AURORA_TEST_REQUIRE(bool(handle.send("still open")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);
    const auto seen = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(seen->payload, "still open");
}

AURORA_TEST_CASE(websocket_rejects_a_send_when_the_queue_byte_limit_is_reached) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 字节那枚闸算的是线上形态（载荷 + 帧头 + 掩码密钥，逐片换档）：100 字节按 16 分成 7 片 = 6×22 + 10 = 142。
    // 拿载荷长度去减会让闸比真实占用宽松 20% 以上，队列的上限就不再是上限。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    const WebSocketOptions tight{.max_frame_bytes = 16, .max_queued_bytes = 64};
    auto handle = scene.open(tight);
    check_rejected(handle.send(std::string(100U, 'x')), aurora::ErrorCode::WsSendQueueFull,
                   "the send queue bytes needs 142 while 64 is the configured limit");

    AURORA_TEST_CHECK_TRUE(handle.is_open());
    AURORA_TEST_REQUIRE(bool(handle.send("small")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);
}

AURORA_TEST_CASE(websocket_answers_a_ping_with_the_same_payload_and_reports_it) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 恒自动回 pong：这条协议义务不交给宿主决定，否则「宿主这一次卡顿」就变成「对端把我方判成失联」。
    // `on_ping` 因此是纯观测口，而回出去的那枚 pong 载荷必须与 ping 一致（`RFC 6455 §5.5.3`）。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_PING, "are you there")},
        .watch_client_frames = true,
    }};
    auto handle = scene.open();
    drive_until_fact("ping reported", [&scene] { return scene.sink.pings >= 1; });

    AURORA_TEST_CHECK_EQ(scene.sink.pings, 1);
    AURORA_TEST_CHECK_EQ(scene.sink.ping_payloads[0U], "are you there");
    drive_until_frames(scene.log, 1, AURORA_WAIT);
    const auto echoed = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(echoed.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(echoed->opcode, AURORA_OP_PONG);
    AURORA_TEST_CHECK_TRUE(echoed->masked);
    AURORA_TEST_CHECK_EQ(echoed->payload, "are you there");
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(websocket_pings_on_the_engine_clock_only_when_the_interval_is_set) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 心跳本层不读任何时钟：到点与否由引擎定时器的次序给出，于是这一族判据在脚本化时钟上逐值可断言。
    // 缺省不发 ping 的另一半（不设 `heartbeat_interval` 就一枚都不发）由上一条用例的静默窗口共同证。
    FakeEngine engine;
    const FakeEngineScope scope(engine);  // 装在场景之前：收尾泵跑时这份替身还在，定时器才有人执行
    WsScene scene{WsPlan{.watch_client_frames = true, .answer_ping = true}};
    const WebSocketOptions beat{.heartbeat_interval = std::chrono::milliseconds{100}};
    auto handle = scene.open(beat);

    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_PING), std::size_t{0});  // 没推时钟就没有 ping
    engine.advance(std::chrono::milliseconds{100});
    drive_until_frames(scene.log, 1, AURORA_WAIT);
    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_PING), std::size_t{1});

    drive_until_fact("pong reported", [&scene] { return scene.sink.pongs >= 1; });
    AURORA_TEST_CHECK_EQ(scene.sink.pongs, 1);
    engine.advance(std::chrono::milliseconds{100});
    drive_until_fact("second heartbeat ping", [&scene] { return scene.log.count(AURORA_OP_PING) >= std::size_t{2}; });
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);  // 健康链路上的心跳不该被译成收摊
}

AURORA_TEST_CASE(websocket_reports_a_ping_that_never_got_an_pong) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // `pong_timeout` 守的是**我方发出的 ping**：醒来时问「这一枚等到了吗」（比对计数而非读时钟），没等到即
    // 失联。收尾落在 `ws-closed` 而不是传输错误——链路在物理上可能还好着，本层说的是「对端不再应答」。
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    WsScene scene{WsPlan{.watch_client_frames = true}};  // 收下 ping 但不答
    const WebSocketOptions beat{.heartbeat_interval = std::chrono::milliseconds{50},
                                .pong_timeout = std::chrono::milliseconds{30}};
    auto handle = scene.open(beat);

    engine.advance(std::chrono::milliseconds{50});
    drive_until_frames(scene.log, 1, AURORA_WAIT);
    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_PING), std::size_t{1});
    engine.advance(std::chrono::milliseconds{30});

    drive_until_closed(scene.sink, AURORA_WAIT);
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 1);
    check_failure(scene.sink.failures[0U], aurora::ErrorCode::WsClosed,
                  "no pong arrived within the configured window after a ping");
    AURORA_TEST_CHECK_FALSE(scene.sink.by_peer[0U]);
    AURORA_TEST_CHECK_FALSE(handle.is_open());
}

AURORA_TEST_CASE(websocket_mirrors_a_peer_initiated_close_and_reports_the_code) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 对端先说 close 时必须回一枚，且回声带上对端那枚状态码（对端没带才用 1000）；定局押到回声写完那一刻，
    // 否则那次 `abort_link()` 会把回声连同连接一起丢掉，对端只能把收摊记成异常断连。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_CLOSE, close_payload(1001U))},
        .watch_client_frames = true,
    }};
    auto handle = scene.open();
    drive_until_closed(scene.sink, AURORA_WAIT);
    drive_until_frames(scene.log, 1, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.closes, 1);
    AURORA_TEST_CHECK_EQ(scene.sink.codes[0U], 1001);
    AURORA_TEST_CHECK_TRUE(scene.sink.reasons[0U].empty());
    AURORA_TEST_CHECK_TRUE(scene.sink.by_peer[0U]);  // 这次收摊由对端的 close 帧开启
    AURORA_TEST_CHECK_TRUE(scene.sink.settled_cleanly(0));
    AURORA_TEST_CHECK_FALSE(handle.is_open());

    const auto echoed = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(echoed.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(echoed->opcode, AURORA_OP_CLOSE);
    AURORA_TEST_CHECK_EQ(echoed->payload.size(), std::size_t{2});  // 回声镜像状态码、不镜像原因
    AURORA_TEST_CHECK_EQ(close_code_of(std::string_view{echoed->payload}), 1001);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(websocket_stops_accepting_sends_once_this_side_started_the_close) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // `close()` 之后到回帧读到之前，`is_open()` 仍为真——「活着」说的是协议状态没终结，不是字节数。
    // 这一族判据同时钉住两件事：新发送一律被拒（免得对端收到 close 之后又冒出一条消息），以及回帧被记成
    // **本端发起**的收摊（`by_peer` 为假）。
    WsScene scene{WsPlan{.watch_client_frames = true, .answer_close = true}};
    auto handle = scene.open();
    AURORA_TEST_REQUIRE(bool(handle.close(1001U, "taking it home")));
    AURORA_TEST_CHECK_TRUE(handle.is_open());
    check_rejected(handle.send("late"), aurora::ErrorCode::WsClosed, "this side already started the close handshake");

    drive_until_closed(scene.sink, AURORA_WAIT);
    AURORA_TEST_CHECK_EQ(scene.sink.codes[0U], 1000);  // 对端那枚回帧的码
    AURORA_TEST_CHECK_FALSE(scene.sink.by_peer[0U]);
    AURORA_TEST_CHECK_TRUE(scene.sink.settled_cleanly(0));
    AURORA_TEST_CHECK_FALSE(handle.is_open());

    drive_until_frames(scene.log, 1, AURORA_WAIT);
    const auto sent = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(sent.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(sent->opcode, AURORA_OP_CLOSE);
    AURORA_TEST_CHECK_EQ(close_code_of(std::string_view{sent->payload}), 1001);
    AURORA_TEST_CHECK_EQ(std::string{sent->payload.substr(2U)}, "taking it home");
    // NOLINTEND(bugprone-unchecked-optional-access)
}

AURORA_TEST_CASE(websocket_refuses_close_frames_that_cannot_go_on_the_wire) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 三型形态违例（不可上线的码 / 越过控制帧上限的原因 / 非法 UTF-8 的原因）都不该改变会话状态：把它们写成
    // 「已经发起关闭」会让一次拼错的参数把一条好端端的会话终止掉，而本层其实一枚帧都没发。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    auto handle = scene.open();
    check_rejected(handle.close(1005U), aurora::ErrorCode::WsProtocolError,
                   "close code 1005 cannot be carried on the wire");
    check_rejected(handle.close(1000U, std::string(124U, 'r')), aurora::ErrorCode::WsProtocolError,
                   "close code plus reason is 126 bytes, above the 125 byte control frame limit");
    check_rejected(handle.close(1000U, std::string_view{"bad\xff"}), aurora::ErrorCode::WsProtocolError,
                   "close reason is not valid UTF-8 at byte 3");
    aurora::test::net::pump_idle(AURORA_QUIET);

    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_CLOSE), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(handle.is_open());
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
    AURORA_TEST_REQUIRE(bool(handle.send("still talking")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);
}

AURORA_TEST_CASE(websocket_destruction_posts_a_close_without_reporting_the_closing) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 析构语义 = 尽力投递 `close(1000)` 再掐，且**不做** `on_closed`：那个 sink 正在被销毁的过程中，往它里面
    // 报事实就是往一个已经不存在的宿主身上报。空原因也是这条路的形态——没人在说话，只有句柄没了。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    {
        auto handle = scene.open();
        AURORA_TEST_CHECK_TRUE(handle.is_open());
    }
    drive_until_frames(scene.log, 1, AURORA_WAIT);

    const auto posted = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(posted.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(posted->opcode, AURORA_OP_CLOSE);
    AURORA_TEST_CHECK_EQ(posted->payload.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(close_code_of(std::string_view{posted->payload}), 1000);
    // NOLINTEND(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
}

AURORA_TEST_CASE(websocket_stops_dispatching_when_the_handle_dies_inside_a_delivery) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 「在最后一条消息的回调里销毁句柄」是宿主最自然的写法（消息一到就把订阅关掉）。同一段字节里后面的帧
    // 因此既不能撞那个已经不存在的 sink，也不能替它回 pong——`detached_` 之后一律不碰。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "one"), server_frame(AURORA_OP_TEXT, "two"),
                   server_frame(AURORA_OP_PING, "after")},
        .watch_client_frames = true,
        .release_frames_on_request = true,
    }};
    WebSocket handle;
    scene.sink.hook_at = 2;
    scene.sink.message_hook = [&handle]() -> void {
        handle = WebSocket{};  // 移动赋值即「收摊自己那份再接管空句柄」，等价于就地析构
    };
    handle = scene.open();
    AURORA_TEST_REQUIRE(bool(handle.send("release the queued frames")));

    drive_until_messages(scene.sink, 2, AURORA_WAIT);
    aurora::test::net::pump_idle(AURORA_QUIET);
    AURORA_TEST_CHECK_EQ(scene.sink.messages, 2);
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[1U], "two");
    AURORA_TEST_CHECK_EQ(scene.sink.pings, 0);  // 句柄没了，那枚 ping 不再是任何人能观察的事实
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);

    drive_until_frames(scene.log, 2, AURORA_WAIT);
    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_PONG), std::size_t{0});
    AURORA_TEST_CHECK_EQ(scene.log.count(AURORA_OP_CLOSE), std::size_t{1});  // 析构那枚尽力投递的 close
}

AURORA_TEST_CASE(websocket_a_stop_flow_closes_dispatching_only) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 宿主回 `Stop` 说的是「这些消息我不再要了」，不是「把连接掐了」：控制帧照报、pong 照回、发送照走。
    // 把两者混成一件事的话，宿主想安静一会儿就得先断线再重连。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "one"), server_frame(AURORA_OP_TEXT, "two"),
                   server_frame(AURORA_OP_PING, "still listening")},
        .watch_client_frames = true,
    }};
    scene.sink.stop_after = 1;  // 必须在握手之前立好：帧与应答同批到达时，第一条消息可能在握手那趟泵里就交付
    auto handle = scene.open();
    drive_until_fact("ping reported", [&scene] { return scene.sink.pings >= 1; });
    aurora::test::net::pump_idle(AURORA_QUIET);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 1);
    AURORA_TEST_CHECK_EQ(scene.sink.payloads[0U], "one");
    AURORA_TEST_CHECK_EQ(scene.sink.pings, 1);
    AURORA_TEST_CHECK_EQ(scene.sink.pongs, 0);  // pong 是回给对端的，不是宿主再要一条消息
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
    AURORA_TEST_CHECK_TRUE(handle.is_open());
    AURORA_TEST_REQUIRE(bool(handle.send("the link is still usable")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);
}

AURORA_TEST_CASE(websocket_reports_a_peer_that_vanishes_without_a_close) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 对端把写向停了却没说 close：这条协议眼里这是异常断连，`ws-closed` 的 detail 要说清「没说 close」，
    // 而已经交付完的那条消息不回滚——宿主看见过的字节由宿主处置。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "the last thing it said")},
        .stop_writing_after_frames = true,
    }};
    auto handle = scene.open();
    drive_until_closed(scene.sink, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 1);
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 1);
    check_failure(scene.sink.failures[0U], aurora::ErrorCode::WsClosed,
                  "the peer closed the connection without a close frame");
    AURORA_TEST_CHECK_TRUE(scene.sink.failures[0U].message.find("still being reassembled") == std::string::npos);
    AURORA_TEST_CHECK_EQ(scene.sink.codes[0U], -1);  // 没走到 close 交换，线上码无从抵达
    AURORA_TEST_CHECK_FALSE(scene.sink.by_peer[0U]);
    AURORA_TEST_CHECK_FALSE(handle.is_open());
}

AURORA_TEST_CASE(websocket_names_a_half_message_when_the_peer_vanishes) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 停在半条消息上的收摊必须点名「还没说完」：把没说完的话补发成一条交出去，宿主就拿到一份对端从没说完的
    // 数据（与 SSE 的收摊同一条纪律）。这里既不派发也不报错，只在 detail 里留那条事实。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "spoken in part", false)},
        .stop_writing_after_frames = true,
    }};
    auto handle = scene.open();
    drive_until_closed(scene.sink, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 0);
    check_failure(scene.sink.failures[0U], aurora::ErrorCode::WsClosed, "while a message was still being reassembled");
    AURORA_TEST_CHECK_FALSE(handle.is_open());
}

AURORA_TEST_CASE(websocket_read_side_rejects_an_oversized_frame_before_the_bytes_arrive) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 单帧上限判在**声明长度**上，不等字节收齐：那 1 MiB 不该为了「也许对端会自己收手」而先分配出来。
    // detail 点名 `frame size`，与总帧越限同码不同句，为的是「发了个大家伙」和「把一条消息撑到装不下」可分。
    WsScene scene{WsPlan{.frames = {server_frame(AURORA_OP_TEXT, std::string(20U, 'z'))}}};
    const WebSocketOptions small{.max_frame_bytes = 8};
    auto handle = scene.open(small);
    drive_until_closed(scene.sink, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 0);
    check_failure(scene.sink.failures[0U], aurora::ErrorCode::WsProtocolError,
                  "frame size 20 bytes exceeds this connection's frame limit of 8");
    AURORA_TEST_CHECK_FALSE(handle.is_open());
}

AURORA_TEST_CASE(websocket_read_side_rejects_a_reassembled_message_over_the_limit) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 每片都合规、加起来不合规：这才是「整条消息」这一枚旋钮存在的理由。逐片判会放过它，逐片交付又会造出
    // 三条半消息——两型都不合规，本层选择在越限那一刻收摊并点名是重组量。
    WsScene scene{WsPlan{
        .frames = {server_frame(AURORA_OP_TEXT, "abcde", false), server_frame(AURORA_OP_CONTINUATION, "fghij")}}};
    const WebSocketOptions small{.max_frame_bytes = 16, .max_message_bytes = 8};
    auto handle = scene.open(small);
    drive_until_closed(scene.sink, AURORA_WAIT);

    AURORA_TEST_CHECK_EQ(scene.sink.messages, 0);
    check_failure(scene.sink.failures[0U], aurora::ErrorCode::WsProtocolError,
                  "reassembled message exceeds this connection's message limit of 8 bytes");
}

AURORA_TEST_CASE(websocket_reports_a_closed_error_for_every_member_of_an_empty_handle) {
    // 空句柄是「连接还没建成」与「连接已经没了」在同一段代码里的共同形态，四个成员一律交 `ws-closed` 而不是
    // 崩溃。本用例不建任何 socket，故浏览器腿同样可跑。
    WebSocket empty;
    AURORA_TEST_CHECK_FALSE(empty.is_open());
    AURORA_TEST_CHECK_EQ(empty.queued_bytes(), std::size_t{0});
    AURORA_TEST_CHECK_TRUE(empty.subprotocol().empty());
    check_rejected(empty.send("text"), aurora::ErrorCode::WsClosed, "holds no session");
    const std::vector<char> bytes{'b', 'i', 'n'};
    check_rejected(empty.send_binary(std::span<const char>{bytes}), aurora::ErrorCode::WsClosed, "holds no session");
    check_rejected(empty.close(1000U), aurora::ErrorCode::WsClosed, "holds no session");
}

AURORA_TEST_CASE(websocket_move_leaves_the_source_handle_empty_and_the_link_running) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 读环持有的是会话状态而不是这个句柄，故接管之后环照跑；源句柄落回空句柄——它再发送就该拿到「不持会话」，
    // 而不是把同一条连接变成两个持有者（两次关闭）。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    auto source = scene.open();
    auto target = std::move(source);  // NOLINT(performance-no-automatic-move) 显式写出「谁接管了谁」

    // 本用例的判据就是「被移走的那一枚落回空句柄」，故刻意在已移出的对象上取值；use-after-move 的
    // 告警面与这里的意图正相反（同族写法见 `tests/unit/utest_net_framer.cpp`）。
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_FALSE(source.is_open());
    check_rejected(source.send("from the source"), aurora::ErrorCode::WsClosed, "holds no session");
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    AURORA_TEST_CHECK_TRUE(target.is_open());
    AURORA_TEST_REQUIRE(bool(target.send("from the target")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);

    const auto seen = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(seen->payload, "from the target");
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
}

AURORA_TEST_CASE(websocket_a_late_cancellation_does_not_close_the_session) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
    // 令牌属于那次握手而不属于这条会话：交出句柄时本层撤销登记，宿主的后续 `cancel()` 不该把一条已经升级的
    // 连接掐掉——那是「我取消那次请求」被读成「我把这条订阅断了」。
    WsScene scene{WsPlan{.watch_client_frames = true}};
    Cancellation token;
    auto handle = scene.open(WebSocketOptions{}, token);
    token.cancel();
    aurora::test::net::pump_idle(AURORA_QUIET);

    AURORA_TEST_CHECK_TRUE(handle.is_open());
    AURORA_TEST_CHECK_EQ(scene.sink.closes, 0);
    AURORA_TEST_REQUIRE(bool(handle.send("after the host cancelled")));
    drive_until_frames(scene.log, 1, AURORA_WAIT);
    const auto seen = scene.log.frame(0U);
    AURORA_TEST_REQUIRE(seen.has_value());
    // 前序 AURORA_TEST_REQUIRE 已断言持有值，断言宏的展开对 tidy 的路径分析不透明，属误报。
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    AURORA_TEST_CHECK_EQ(seen->payload, "after the host cancelled");
}

AURORA_TEST_CASE(a_handshake_is_recorded_without_body_done) {
#ifdef AURORA_PLATFORM_WASM
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#endif
#ifndef AURORA_ENABLE_DEBUG
    AURORA_TEST_SKIP("network activity is compiled out when AURORA_ENABLE_DEBUG is off");
#else
    aurora::net::http::set_activity_options(aurora::net::http::ActivityOptions{});
    aurora::net::http::clear_network_activity();
    WsScene scene{WsPlan{}};
    {
        auto handle = scene.open();
        AURORA_TEST_CHECK_TRUE(handle.is_open());

        // 本腿只收**握手**这一条：一条记录、状态码 101、已收摊，且没有 `BodyDone`——升级之后是帧流，
        // 不是一次请求的体走到了终点。会话自身的收摊事实在 `WsCloseInfo`，不进请求活动表。
        const auto snapshot = aurora::net::http::activity_snapshot();
        AURORA_TEST_REQUIRE_EQ(snapshot.entries.size(), std::size_t{1});
        const auto &entry = snapshot.entries.front();
        AURORA_TEST_CHECK_EQ(entry.status, std::uint16_t{101});
        AURORA_TEST_CHECK_EQ(entry.hop, std::uint16_t{0});
        AURORA_TEST_CHECK_EQ(entry.pass, std::uint16_t{0});
        AURORA_TEST_CHECK_FALSE(entry.in_flight);
        AURORA_TEST_CHECK(entry.error_code.empty());
        AURORA_TEST_REQUIRE_FALSE(entry.request_id.empty());  // 客户端缺省补齐了链路标识
        const auto timeline = aurora::testing::require_value(aurora::net::http::timeline_of(entry.request_id));
        const auto has_stage = [&timeline](aurora::net::http::HttpStage stage) {
            return std::any_of(timeline.marks.begin(), timeline.marks.end(),
                               [stage](const aurora::net::http::StageMark &mark) { return mark.stage == stage; });
        };
        AURORA_TEST_CHECK(has_stage(aurora::net::http::HttpStage::RequestSent));
        AURORA_TEST_CHECK(has_stage(aurora::net::http::HttpStage::FirstByte));
        AURORA_TEST_CHECK_FALSE(has_stage(aurora::net::http::HttpStage::BodyDone));
    }
#endif
}

}  // namespace aurora::test_cases::utest_net_websocket

/// 测试类型: unit
/// 目标单元: include/aurora/net/http_stream.h + Client::open_stream（src/aurora/net/http_client.cpp）
/// 测试说明: HTTP 发起侧流式接收的离线回归（CHG-018 §6 / §12「open_stream 形态裁决」）。以 ScriptedPeer 在
///           回环上按脚本应答真实字节，验四条只有逐段交付才成立的形态：
///           - **完成事实是 `StreamOutcome`**：`response.body()` 恒空、`delivered_bytes` 与宿主拼出的文本逐值
///             相符、`on_head` / `on_data` / `on_end` 的调用次序与次数符合契约（失败结局不调 `on_end`）；
///           - **线上字节不设限**：`max_body_bytes()` 管的是物化路径的内存，对本入口没有任何约束（同一份应答
///             在 `send` 那边越限、在这边收完），而 `max_decompressed_bytes()` 仍逐层限解压产物并点名该旋钮；
///           - **宿主 `Stop` 是正常结局**：回成功的 `StreamOutcome` 且 `stopped_by_sink` 为真、后续段不再交付、
///             `on_end` 仍恰一次，绝不落成取消类错误码；
///           - **重定向跳的体不读**：头部一判定为重定向即换跳，最终一跳的头部与文本交给宿主，`wire_bytes`
///             仍累计全部跳（跨跳口径与 `delivered_bytes` 只算终跳的口径必须能分别证出来）。
///           另验三档超时同判据（到点改道 `http-timeout`）、进度以线上字节计且末值恰一次、支持集外的编码名在建链
///             就落 `http-content-encoding-unsupported`、无体应答不校验编码名，以及本腿无流式能力时直接落
///             `net-platform-unavailable` 而不悄悄退化成物化路径。内容编码那三项只在能力编入的腿成立，缺席的腿
///             按 `AURORA_TEST_SKIP` 登记。
///           重试那三条验的是流式路径独有的资格口径：**`on_head` 交出即耗尽**——头没交出去的超时可以把三扇
///             时限窗口跑满（虚拟时钟 ≥150 ms），头已经交出去的只剩一扇（≤90 ms）；越界的 `RetryPolicy` 在建协程
///             前就被拒（对端一条连接都没被连上）。物化路径的逐值退避与 `Retry-After` 尊重在
///             `utest_net_http_client.cpp`，退避算式与错误表在 `utest_net_http_retry.cpp`。
///           缓存（§12「条件请求与缓存校验」）验流式入口独有的三条：命中新鲜副本时**一条连接都不发起**，
///             但仍按同一个次序把已存那份体逐段交给同一个宿主（`wire_bytes` 恒 0、`delivered_bytes` 恰等于体长、
///             `response.from_cache()` 为真）；本入口**只读不写**，于是两次流式交换之间表里什么都没留下（第二次
///             照旧触网）；表里那条由物化路径灌入的陈旧条目会被带着校验子去问，拿回的 304 不进宿主——交出的
///             头部是已存那份 200、体是已存那段字节，并完的那一份重新获得新鲜度，第三次不再触网。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/capabilities.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_stream.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_stream {
namespace {

using aurora::CoroTask;
using aurora::Result;
using aurora::net::Capabilities;
using aurora::net::http::CacheOptions;
using aurora::net::http::Client;
using aurora::net::http::DataSink;
using aurora::net::http::Progress;
using aurora::net::http::ProgressPhase;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::RetryPolicy;
using aurora::net::http::SinkFlow;
using aurora::net::http::StreamOutcome;
using aurora::net::http::Timeouts;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::per_connection;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::silent;
using aurora::test::scripts::write_then_hold;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 取协程结果的外层（协程本身不抛，外层恒成功）。
[[nodiscard]] auto inner(CoroTask<Result<StreamOutcome>> &task) -> Result<StreamOutcome> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 发起一条流式交换并泵到收尾：宿主替身与出口结果都留在调用方手里。
/// @param client 客户端。
/// @param request 请求。
/// @param sink 宿主替身。
/// @return 交换的出口结果（失败时带错误码与文本）。
[[nodiscard]] auto open_and_drive(const Client &client, Request request, DataSink &sink) -> Result<StreamOutcome> {
    const aurora::test::net::HostDriven host;
    auto task = client.open_stream(std::move(request), sink);
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_stream done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);
    return inner(task);
}

/// @brief 走物化路径完成一次交换：缓存的重验证用例需要「表里先有一条」，而流式入口刻意不写表。
/// @param client 客户端。
/// @param request 请求。
/// @return 应答的 Result 层（协程未收尾时由 REQUIRE 收场）。
[[nodiscard]] auto prime_with_send(const Client &client, Request request) -> Result<Response> {
    const aurora::test::net::HostDriven host;
    auto task = client.send(std::move(request));
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

/// @brief 在脚本化时钟下把一条流式交换泵到收尾（重试与超时的判据都要由用例推进时间，不能让真实时长说话）。
/// @param engine 替身时钟。
/// @param task 待收尾的协程。
/// @param rounds 泵的上限轮次。
/// @return 协程是否收尾（未收尾时调用方**必须**用 REQUIRE 收场）。
///
/// 调用一律配 `AURORA_TEST_REQUIRE`：`CoroTask::result()` 在未收尾时读的是帧外内存，非致命的 CHECK 会继续
/// 走到取结果那一步，于是一场「没排空」显示为一次段错误而不是一条失败判据。
[[nodiscard]] auto pump_stream(FakeEngine &engine, CoroTask<Result<StreamOutcome>> &task, int rounds = 60) -> bool {
    for (int round = 0; round < rounds && !task.is_done(); ++round) {
        (void)engine.advance(std::chrono::milliseconds{10});
        aurora::test::net::pump_idle(std::chrono::milliseconds{2});
    }
    return task.is_done();
}

/// @brief 错误文本里是否点名了某枚旋钮/某一档（超时改道的判据要看 detail，不能只看码）。
/// @param haystack 错误文本。
/// @param needle 要点名的词。
/// @return 命中为 true。
[[nodiscard]] auto names_in(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief 记录逐段交付事实的宿主替身：三个入口各自的次数、拼出的文本、以及在哪一段收摊。
///
/// 它是本套件的观测面——流式的判据几乎全在「交了几段、交了什么、什么时候不再交」上，而这些只有宿主自己说得清。
/// 成员公开是为了让断言直读；本类型只在用例线程被驱动（续体经主线程投递器恢复），无并发面。
class RecordingSink final : public DataSink {
  public:
    /// @brief 记一次头部到达，并核对头的交付形态（体的字节在此恒为空，是契约而非巧合）。
    /// @param head 头部块。
    /// @return 恒继续（收摊的判定放在体上；`stop_at_head` 打开时才在头部就收摊）。
    auto on_head(const Response &head) -> SinkFlow override {
        ++heads;
        status = head.status();
        head_body_empty = head.body().empty();
        return stop_at_head ? SinkFlow::Stop : SinkFlow::Continue;
    }

    /// @brief 记一段体字节，按 `stop_after` 决定何时收摊。
    /// @param body 本段明文。
    /// @return 继续或收摊。
    auto on_data(std::span<const char> body) -> SinkFlow override {
        ++segments;
        max_segment = body.size() > max_segment ? body.size() : max_segment;
        text.append(body.data(), body.size());
        if (segments >= stop_after) {
            stopped = true;
            return SinkFlow::Stop;
        }
        return SinkFlow::Continue;
    }

    /// @brief 记一次收摊宣告。
    auto on_end() -> void override { ++ends; }

    int heads{0};  ///< `on_head` 调用次数
    int ends{0};  ///< `on_end` 调用次数
    std::size_t segments{0};  ///< `on_data` 调用次数
    std::size_t max_segment{0};  ///< 单段最大字节数（「不物化」的可测量形态）
    std::string text;  ///< 按交付次序拼接的明文
    std::uint16_t status{0};  ///< 头部块里的状态码
    bool head_body_empty{false};  ///< 头部交付时 `body()` 是否为空
    bool stopped{false};  ///< 本宿主是否回出过 `Stop`
    bool stop_at_head{false};  ///< 是否在头部就收摊
    std::size_t stop_after{std::numeric_limits<std::size_t>::max()};  ///< 第几段起收摊
};

// `hello aurora` 的金样包装：gzip（RFC 1952）与裸 DEFLATE（RFC 1951）、zlib 包装（RFC 1950）。
#ifdef AURORA_ENABLE_COMPRESSION
/// @brief 把无符号字节表转成消息体缓冲（压缩字节常 ≥ 0x80，写成 char 字面量会踩实现定义转换）。
/// @tparam N 表长。
/// @param table 字节表。
/// @return 等长缓冲。
template <std::size_t N>
[[nodiscard]] auto bytes(const unsigned char (&table)[N]) -> std::vector<char> {
    std::vector<char> out;
    out.reserve(N);
    for (const unsigned char unit : table) {
        out.push_back(static_cast<char>(unit));
    }
    return out;
}

const unsigned char AURORA_GZIP_HELLO[] = {31, 139, 8,  0,   0,  0,  0, 0, 2,   255, 203, 72,  205, 201, 201, 87,
                                           72, 44,  45, 202, 47, 74, 4, 0, 214, 230, 105, 148, 12,  0,   0,   0};
const unsigned char AURORA_RAW_HELLO[] = {203, 72, 205, 201, 201, 87, 72, 44, 45, 202, 47, 74, 4, 0};
const unsigned char AURORA_ZLIB_HELLO[] = {120, 156, 203, 72, 205, 201, 201, 87,  72, 44,
                                           45,  202, 47,  74, 4,   0,   30,  136, 4,  191};
#endif

/// @brief 拼一份 `Content-Length` 分帧的 200 应答：头部字段 + 原样字节体。
/// @param fields 头部字段行（每行以 `\\r\\n` 结尾，可空）。
/// @param body 消息体字节。
/// @return 可直接喂给对端脚本的应答字节。
[[nodiscard]] auto length_framed(std::string_view fields, const std::vector<char> &body) -> std::string {
    std::string out = "HTTP/1.1 200 OK\r\n";
    out += fields;
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out.append(body.data(), body.size());
    return out;
}

/// @brief 拼一份 chunked 分帧的 200 应答：整块 + 终止块。
/// @param fields 头部字段行（可空）。
/// @param body 分块解码后应交出的字节。
/// @return 应答字节。
[[nodiscard]] auto chunked_framed(std::string_view fields, const std::vector<char> &body) -> std::string {
    std::string size_text;
    for (std::size_t remaining = body.size(); remaining > 0; remaining /= 16) {
        // 分块长度是十六进制小写（`RFC 9110 §7.1`）：直接算字符而不查表，省一枚下标越界面。
        const int digit = static_cast<int>(remaining % 16);
        const char glyph = digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10);
        size_text.insert(size_text.begin(), glyph);
    }
    if (size_text.empty()) {
        size_text = "0";
    }
    std::string out = "HTTP/1.1 200 OK\r\n";
    out += fields;
    out += "Transfer-Encoding: chunked\r\n\r\n";
    out += size_text;
    out += "\r\n";
    out.append(body.data(), body.size());
    out += "\r\n0\r\n\r\n";
    return out;
}

/// @brief 取一段可辨识的长正文：长度要大到一次读装不下，「逐段」才是可观测的事实而不是修辞。
/// @param total 字节数。
/// @return 消息体字节（每 16 字节轮换一次字母，便于肉眼定位错位）。
[[nodiscard]] auto long_body(std::size_t total) -> std::vector<char> {
    std::vector<char> out;
    out.reserve(total);
    for (std::size_t index = 0; index < total; ++index) {
        out.push_back(static_cast<char>('a' + ((index / 16) % 26)));
    }
    return out;
}

/// @brief 取出下载方向的发射序列（频次与末值判据都要分方向看）。
/// @param seen 全部发射。
/// @return 下载发射副本（保持次序）。
[[nodiscard]] auto downloads_of(const std::vector<Progress> &seen) -> std::vector<Progress> {
    std::vector<Progress> out;
    for (const Progress &progress : seen) {
        if (progress.phase == ProgressPhase::Download) {
            out.push_back(progress);
        }
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(stream_delivers_a_content_length_body_segment_by_segment) {
    Client client;
    const auto body = long_body(40000);  // 远超一次读的 16 KiB：段数大于 1 才是「流式」而不是措辞
    ScriptedPeer peer(reply_after_reading(1, length_framed("", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    AURORA_TEST_CHECK_EQ(sink.heads, 1);  // 契约：`on_head` 恰一次，且在任一段体之前
    AURORA_TEST_CHECK_TRUE(sink.head_body_empty);
    AURORA_TEST_CHECK_TRUE(sink.segments > 1);
    AURORA_TEST_CHECK_EQ(sink.text.size(), body.size());
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_FALSE(sink.stopped);

    const auto &done = outcome.value();
    AURORA_TEST_CHECK_EQ(done.response.status(), std::uint16_t{200});
    AURORA_TEST_CHECK_TRUE(done.response.body().empty());  // 完成事实里不复制一份体：体的字节已交给宿主
    AURORA_TEST_CHECK_EQ(done.wire_bytes, body.size());
    AURORA_TEST_CHECK_EQ(done.delivered_bytes, body.size());
    AURORA_TEST_CHECK_FALSE(done.stopped_by_sink);
}

AURORA_TEST_CASE(stream_delivers_a_chunked_body) {
    Client client;
    const std::vector<char> body{'h', 'e', 'l', 'l', 'o'};
    ScriptedPeer peer(reply_after_reading(1, chunked_framed("", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.text, "hello");
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_EQ(outcome.value().delivered_bytes, std::uint64_t{5});
}

AURORA_TEST_CASE(stream_a_head_response_delivers_no_body_and_skips_coding_checks) {
    // HEAD 应答恒无体：中间层常照抄 `Content-Encoding`，那时无体应答上声明的编码名不该被校验，
    // 也不该为它建链——交付面是零段，收摊仍恰一次。
    Client client;
    ScriptedPeer peer(reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::head(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{0});
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_EQ(outcome.value().wire_bytes, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(outcome.value().delivered_bytes, std::uint64_t{0});
}

AURORA_TEST_CASE(stream_stops_reading_when_the_sink_returns_stop) {
    // 宿主收摊是正常结局：成功出口 + `stopped_by_sink`，剩余字节不再读取也不交付，而 `on_end` 仍宣告收摊。
    Client client;
    const auto body = long_body(40000);
    ScriptedPeer peer(reply_after_reading(1, length_framed("", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    sink.stop_after = 2;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{2});  // 停住后再无交付：段数恰好停在收摊那一处
    AURORA_TEST_CHECK_TRUE(sink.text.size() < body.size());
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    const auto &done = outcome.value();
    AURORA_TEST_CHECK_TRUE(done.stopped_by_sink);
    AURORA_TEST_CHECK_EQ(done.delivered_bytes, sink.text.size());
    AURORA_TEST_CHECK_TRUE(done.wire_bytes >= done.delivered_bytes);  // 线上口径不小于交付口径
}

AURORA_TEST_CASE(stream_stops_at_the_head_when_the_sink_returns_stop_there) {
    // 头部就收摊：一段都不交，链根本不必建，`on_end` 照样恰一次——「够了 / 用户取消了」不该被当成失败。
    Client client;
    const auto body = long_body(40000);
    ScriptedPeer peer(reply_after_reading(1, length_framed("", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    sink.stop_at_head = true;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{0});
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_TRUE(outcome.value().stopped_by_sink);
    AURORA_TEST_CHECK_EQ(outcome.value().delivered_bytes, std::uint64_t{0});
}

AURORA_TEST_CASE(stream_abandons_the_body_of_a_redirect_hop) {
    // 头部一判定为重定向即换跳：中间跳的体不交付，`delivered_bytes` 只算最终一跳，
    // 而 `wire_bytes` 跨全部跳（两枚口径分别证出来，才说明它们不是同一个数换了个名字）。
    Client client;
    ScriptedPeer target(reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nZ"));
    AURORA_TEST_REQUIRE(target.valid());
    const std::string redirect =
        "HTTP/1.1 302 Found\r\nLocation: " + loopback_url(target.port()) + "\r\nContent-Length: 3\r\n\r\nabc";
    ScriptedPeer origin(reply_after_reading(1, redirect));
    AURORA_TEST_REQUIRE(origin.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(origin.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    AURORA_TEST_CHECK_EQ(sink.text, "Z");
    AURORA_TEST_CHECK_EQ(sink.heads, 1);  // 中间跳的头部不进宿主：宿主只见最终一跳
    AURORA_TEST_CHECK_EQ(sink.status, std::uint16_t{200});
    const auto &done = outcome.value();
    AURORA_TEST_CHECK_EQ(done.response.status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(done.delivered_bytes, std::uint64_t{1});
    AURORA_TEST_CHECK_EQ(done.wire_bytes, std::uint64_t{4});  // 3（中间跳已到）+ 1（终跳）
    AURORA_TEST_CHECK_EQ(origin.accepts(), 1);
    AURORA_TEST_CHECK_EQ(target.accepts(), 1);
}

AURORA_TEST_CASE(stream_stops_at_the_redirect_limit) {
    Client client;
    // 每跳都回 302 指向自身路径：跟随必然撞到上限而不是收敛。
    ScriptedPeer peer(reply_after_reading(1, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome =
        open_and_drive(client, Request::get(loopback_url(peer.port(), "/loop")).max_redirects(2), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpRedirectLimit);
    AURORA_TEST_CHECK_EQ(peer.accepts(), 3);  // 初始一跳 + 两跳跟随，其后超限而止
    AURORA_TEST_CHECK_EQ(sink.ends, 0);  // 失败结局不宣告收摊
}

AURORA_TEST_CASE(stream_ignores_the_materialized_body_ceiling) {
    // `max_body_bytes()` 是物化路径的内存闸：流式路径不物化，故它对本入口零约束（同一份应答在 `send` 那边
    // 会越限，在这边收完）。设了它就让 `open_stream` 提前失败，等于把「收大文件」变成必须调大一枚无关旋钮。
    Client client;
    client.max_body_bytes(std::optional<std::uint64_t>{std::uint64_t{4}});
    const auto body = long_body(40000);
    ScriptedPeer peer(reply_after_reading(1, length_framed("", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(outcome.value().delivered_bytes, body.size());
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
}

AURORA_TEST_CASE(stream_refuses_a_coding_outside_the_supported_set_before_any_byte_is_handed) {
    // 支持集外的名在建链那一步就拒，且一段都不交：让宿主收下压缩字节的前缀再失败，交出去的那段没法收回。
    Client client;
    const std::vector<char> body{'n', 'o', 't', '-', 'b', 'r'};
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: br\r\n", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{0});
    AURORA_TEST_CHECK_EQ(sink.ends, 0);  // 失败不宣告收摊：字节流断在半途，收尾归调用方按 Result 分流
}

AURORA_TEST_CASE(stream_times_out_a_stalled_peer) {
    // 三档看门狗对两条交付路同判据：整请求到点落 `http-timeout`，不是 `net-cancelled`。
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    ScriptedPeer peer(write_then_hold("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"));  // 给头部不给体
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    auto task = client.open_stream(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.read_idle = std::chrono::milliseconds{50}}), sink);
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_stream done", [&task] { return task.is_done(); }, std::chrono::milliseconds{3000})
                          .satisfied);

    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK_EQ(sink.ends, 0);
}

AURORA_TEST_CASE(stream_reports_download_progress_on_wire_bytes_with_one_settled_value) {
    // 进度报的是线上字节（解压前口径），末值恒发一次：合流不能把「收了摊」这一事实合掉。
    Client client;
    const auto body = long_body(40000);
    ScriptedPeer peer(reply_after_reading(1, length_framed("", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    std::vector<Progress> seen;
    RecordingSink sink;
    auto request = Request::get(loopback_url(peer.port()));
    request.on_progress([&seen](const Progress &progress) -> void { seen.push_back(progress); });
    const auto outcome = open_and_drive(client, std::move(request), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);

    const auto down = downloads_of(seen);
    AURORA_TEST_REQUIRE(!down.empty());
    AURORA_TEST_CHECK_TRUE(down.front().total_known);  // `Content-Length` 给得出总量
    AURORA_TEST_CHECK_EQ(down.front().total_bytes, body.size());
    for (std::size_t index = 1; index < down.size(); ++index) {
        AURORA_TEST_CHECK_TRUE(down.at(index).transferred_bytes >= down.at(index - 1).transferred_bytes);
    }
    AURORA_TEST_CHECK_TRUE(down.back().finished);
    AURORA_TEST_CHECK_EQ(down.back().transferred_bytes, body.size());
    std::size_t settled = 0;
    for (const Progress &progress : down) {
        settled += progress.finished ? 1U : 0U;
    }
    AURORA_TEST_CHECK_EQ(settled, std::size_t{1});  // 末值恰一次：不多发也不缺发
}

AURORA_TEST_CASE(stream_refuses_to_run_on_a_leg_without_streaming_semantics) {
    // 本腿无流式能力时直接失败，绝不悄悄退化成物化路径：退化会把调用方的内存预算和预期一起毁掉。
    FakeEngine engine;
    Capabilities caps;
    caps.streaming = false;
    engine.set_capabilities(caps);
    const FakeEngineScope scope(engine);

    RecordingSink sink;
    auto task = Client{}.open_stream(Request::get("http://127.0.0.1:1/"), sink);
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "open_stream done", [&task] { return task.is_done(); }, std::chrono::milliseconds{1000})
                          .satisfied);
    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::NetPlatformUnavailable);
    AURORA_TEST_CHECK_EQ(sink.heads, 0);
    AURORA_TEST_CHECK_EQ(sink.ends, 0);
}

#ifdef AURORA_ENABLE_COMPRESSION

AURORA_TEST_CASE(stream_decompresses_a_gzip_body_with_two_byte_counters) {
    // 交付给宿主的是还原后的明文，而 `wire_bytes` 记的是线上字节：两枚口径在同一个用例里各证一次。
    Client client;
    const auto wire = bytes(AURORA_GZIP_HELLO);  // 线上 32 字节
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: gzip\r\n", wire)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{} : outcome.error().message);
    AURORA_TEST_CHECK_EQ(sink.text, "hello aurora");  // 压缩字节一个都没进宿主
    const auto &done = outcome.value();
    AURORA_TEST_CHECK_EQ(done.wire_bytes, wire.size());
    AURORA_TEST_CHECK_EQ(done.delivered_bytes, std::uint64_t{12});
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
}

AURORA_TEST_CASE(stream_deflate_locks_the_format_from_the_wrapper_header) {
    // 流式不「先试再回落」：首两枚输入字节构成合法 zlib 头即锁 zlib，否则锁裸 DEFLATE，两型各解通一次。
    const auto probe = [](std::string_view label, const std::vector<char> &wire) -> void {
        Client client;
        ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: deflate\r\n", wire)));
        AURORA_TEST_REQUIRE(peer.valid());

        RecordingSink sink;
        const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
        AURORA_TEST_REQUIRE_MSG(bool(outcome), outcome ? std::string{"deflate probe failed: "} + std::string{label}
                                                       : outcome.error().message);
        AURORA_TEST_CHECK_EQ(sink.text, "hello aurora");
    };
    probe("zlib wrapper", bytes(AURORA_ZLIB_HELLO));
    probe("raw stream", bytes(AURORA_RAW_HELLO));
}

AURORA_TEST_CASE(stream_enforces_the_decompressed_ceiling) {
    // 越限的是解压产物：detail 点名 `max_decompressed_bytes`，且宿主收到的段数停在那一层越限之前。
    Client client;
    client.max_decompressed_bytes(std::optional<std::uint64_t>{std::uint64_t{4}});
    const auto wire = bytes(AURORA_GZIP_HELLO);
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: gzip\r\n", wire)));
    AURORA_TEST_REQUIRE(peer.valid());

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(loopback_url(peer.port())), sink);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpBodyTooLarge);
    AURORA_TEST_CHECK(outcome.error().message.find("max_decompressed_bytes") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(sink.text.size() <= 4);
    AURORA_TEST_CHECK_EQ(sink.ends, 0);  // 越限是失败，不是收摊

    // 放宽到产物长度即放行：证明限的确实是解压产物而不是线上字节（线上 32 > 12）。
    client.max_decompressed_bytes(std::optional<std::uint64_t>{std::uint64_t{12}});
    RecordingSink loose;
    const auto passed = open_and_drive(client, Request::get(loopback_url(peer.port())), loose);
    AURORA_TEST_REQUIRE_MSG(bool(passed), passed ? std::string{} : passed.error().message);
    AURORA_TEST_CHECK_EQ(loose.text, "hello aurora");
}

#else

AURORA_TEST_CASE(stream_decompresses_a_gzip_body_with_two_byte_counters) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_deflate_locks_the_format_from_the_wrapper_header) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_enforces_the_decompressed_ceiling) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

#endif

AURORA_TEST_CASE(stream_replays_a_timeout_raised_before_any_head_is_handed) {
    // 流式路径的重放资格比物化路径窄，窄就窄在这一条：头还没交给宿主 ⇒ 可以重来。这里用「三档时限各拿一份
    // 新的钟」把重放数出来：`Timeouts::total` 50 ms × 3 次尝试 + 两次退避 ≥ 150 ms 虚拟时长，一次尝试最多 60 ms。
    //
    // 判据取虚拟时钟而不是 `peer.accepts()`：对端脚本线程是**串行**服务的，`silent()` 会一直占住它，后面的
    // 尝试因此连不上第 2 条连接——「先占住时限再放手让下一次成功」的剧本要把真实睡眠和虚拟时限赛跑，
    // 输赢取决于机器负载。重放本身已由 `utest_net_http_client.cpp` 的逐值判据端到端证过，本用例只补流式
    // 那一半独有的资格规则：没交过头的失败可以重来，且重来一次都没碰过宿主。
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{10}});
    RecordingSink sink;
    auto task = client.open_stream(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.total = std::chrono::milliseconds{50}}), sink);
    AURORA_TEST_REQUIRE(pump_stream(engine, task));

    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK(names_in(outcome.error().message, "total"));  // 逐尝试重新计时的正是这一档
    AURORA_TEST_CHECK_GE(engine.now().count(), 150);  // 三次尝试各用过一份 50 ms 窗口
    AURORA_TEST_CHECK_EQ(sink.heads, 0);  // 重放期间一次都没向宿主交过头
    AURORA_TEST_CHECK_EQ(sink.ends, 0);
}

AURORA_TEST_CASE(stream_does_not_replay_after_the_head_is_handed) {
    // 头一旦交出就耗尽资格：本库不能假装「回到交头之前」让宿主重写已写下那段，那是把契约违约当成重试收益。
    // 对端给出完整头部但按 `Content-Length` 欠着体不写，于是头交得出、交换收不完——同一枚时限旋钮下
    // 上一次用例走满 150 ms，这一枚只走满一份窗口。
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(write_then_hold("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"));  // 给头部不给体
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.retry_policy(RetryPolicy{.attempts = 3, .base = std::chrono::milliseconds{10}});
    RecordingSink sink;
    auto task = client.open_stream(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.total = std::chrono::milliseconds{50}}), sink);
    AURORA_TEST_REQUIRE(pump_stream(engine, task));

    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpTimeout);
    AURORA_TEST_CHECK_EQ(sink.heads, 1);  // 头确实交出去过：资格就此耗尽
    AURORA_TEST_CHECK_EQ(sink.ends, 0);  // 失败结局不调 `on_end`
    AURORA_TEST_CHECK_LE(engine.now().count(), 90);  // 只剩一份窗口：第二次尝试根本没发生
}

AURORA_TEST_CASE(stream_refuses_an_out_of_range_policy_before_any_connection) {
    // 形态闸在起手就过：越界的 `base` 不进入任何一次尝试，对端一条连接都不该被连上。
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, length_framed("", {'o'})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    RecordingSink sink;
    auto task = client.open_stream(
        Request::get(loopback_url(peer.port())).retry_policy(RetryPolicy{.base = std::chrono::milliseconds{-1}}), sink);
    AURORA_TEST_REQUIRE(aurora::test::net::drive_until(
                            "policy refused", [&task] { return task.is_done(); }, std::chrono::milliseconds{2000})
                            .satisfied);

    const auto outcome = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(outcome));
    AURORA_TEST_CHECK_EQ(outcome.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);
    AURORA_TEST_CHECK(names_in(outcome.error().message, "base"));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 0);
    AURORA_TEST_CHECK_EQ(sink.heads, 0);
}

AURORA_TEST_CASE(a_cache_hit_streams_the_stored_body_without_the_wire) {
    const aurora::test::net::HostDriven host;
    const auto body = long_body(40000);  // 远超一次读的窗口：命中时也必须仍是「逐段」而不是一次倾倒
    ScriptedPeer peer(reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", body)));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());

    // 表先由物化路径灌一份（流式入口不写表，见下一例），这份 40 000 字节的载荷就是命中时要逐段交出去的那段。
    AURORA_TEST_REQUIRE(bool(prime_with_send(client, Request::get(url))));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    // 命中缓存走的是同一个宿主、同一套次序：头部一次、体按段、收摊一次，只是线上一个字节也没来。
    RecordingSink sink;
    const auto hit = open_and_drive(client, Request::get(url), sink);
    AURORA_TEST_REQUIRE(bool(hit));
    AURORA_TEST_CHECK(hit.value().response.from_cache());
    AURORA_TEST_CHECK_EQ(hit.value().wire_bytes, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(hit.value().delivered_bytes, body.size());
    AURORA_TEST_CHECK_EQ(sink.text.size(), body.size());
    AURORA_TEST_CHECK_EQ(sink.text, std::string{body.begin(), body.end()});
    AURORA_TEST_CHECK_TRUE(sink.segments > 1);
    AURORA_TEST_CHECK_EQ(sink.heads, 1);
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_TRUE(sink.head_body_empty);
    AURORA_TEST_CHECK_EQ(sink.status, std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);  // 「同一 URL 二次请求不触网」在流式入口同样成立
}

AURORA_TEST_CASE(a_streamed_exchange_leaves_the_table_untouched) {
    const aurora::test::net::HostDriven host;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Cache-Control: max-age=600\r\n", {'a'})));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());

    RecordingSink first;
    const auto fetched = open_and_drive(client, Request::get(url), first);
    AURORA_TEST_REQUIRE(bool(fetched));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    // 流式入口只读不写：把载荷攒进表就意味着先把它攒进内存，那正是调用方选 `open_stream` 要避免的事。
    // 判据是「第二次仍然触网」——若这里写进了表，第二次就该是一次命中。
    RecordingSink second;
    const auto again = open_and_drive(client, Request::get(url), second);
    AURORA_TEST_REQUIRE(bool(again));
    AURORA_TEST_CHECK_FALSE(again.value().response.from_cache());
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(a_stale_entry_revalidates_and_streams_the_stored_body) {
    const aurora::test::net::HostDriven host;
    const std::vector<char> stored{'s', 't', 'o', 'r', 'e', 'd'};
    // 第一条给「抵达即过期 + 校验子」：`send` 把它灌进表；第二条只回一句 304。
    const auto initial = length_framed("Cache-Control: max-age=0\r\nETag: \"v1\"\r\n", stored);
    const auto *const not_modified = "HTTP/1.1 304 Not Modified\r\nCache-Control: max-age=600\r\nETag: \"v2\"\r\n\r\n";
    ScriptedPeer peer(per_connection({reply_after_reading(1, initial), reply_after_reading(1, not_modified)}));
    AURORA_TEST_REQUIRE(peer.valid());

    Client client;
    client.cache(CacheOptions{.enabled = true});
    const auto url = loopback_url(peer.port());
    AURORA_TEST_REQUIRE(bool(prime_with_send(client, Request::get(url))));
    AURORA_TEST_CHECK_EQ(peer.accepts(), 1);

    RecordingSink sink;
    const auto outcome = open_and_drive(client, Request::get(url), sink);
    AURORA_TEST_REQUIRE(bool(outcome));
    // 宿主看不见那枚中间状态：头部是已存那份 200，体是已存那 6 字节，`on_head` 恰一次、`on_end` 恰一次。
    AURORA_TEST_CHECK_EQ(sink.status, std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(sink.heads, 1);
    AURORA_TEST_CHECK_EQ(sink.ends, 1);
    AURORA_TEST_CHECK_EQ(sink.text, std::string{"stored"});
    AURORA_TEST_CHECK_EQ(outcome.value().delivered_bytes, std::uint64_t{6});
    AURORA_TEST_CHECK(outcome.value().response.from_cache());
    // 304 按定义没有体，故线上字节仍是 0（口径是「分帧解出的体字节」，与 HEAD 那一例同判据）。
    AURORA_TEST_CHECK_EQ(outcome.value().wire_bytes, std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(outcome.value().response.header("ETag").value_or(std::string{}), std::string{"\"v2\""});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);

    // 刚验证过的一份重新获得新鲜度：第三次不必再问对端，交出的仍是已存那份体。
    RecordingSink third;
    const auto again = open_and_drive(client, Request::get(url), third);
    AURORA_TEST_REQUIRE(bool(again));
    AURORA_TEST_CHECK(again.value().response.from_cache());
    AURORA_TEST_CHECK_EQ(third.text, std::string{"stored"});
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

}  // namespace aurora::test_cases::utest_net_http_stream

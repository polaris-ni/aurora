/// 测试类型: unit
/// 目标单元: include/aurora/net/http_client.h + src/aurora/net/http_client.cpp
/// 测试说明: HTTP 发起侧 send 核心闭环的离线回归（CHG-010 §6 / §12）。以 ScriptedPeer 在回环上按脚本
///           应答真实字节，覆盖 Content-Length 与 Chunked 两种分帧的物化、302 重定向跟随（跨到第二个对端）
///           与超限落 `http-redirect-limit`；整请求超时用 FakeEngine 脚本化时钟（连上但静默的对端 + 推进到
///           时限）验 `http-timeout`。内容编码接入（§12「内容编码」的 HTTP 侧）验 `Accept-Encoding` 由
///           缺省补齐（调用方已自带则原样保留、不覆盖不重复追加）、已声明环节的透明还原（gzip 与 `deflate`
///           的 zlib→裸流回落、`gzip, deflate` 链反序解）、`max_decompressed_bytes()` 越限落
///           `http-body-too-large` 且 detail 点名该旋钮、`br` 落 `http-content-encoding-unsupported`、HEAD
///           应答无体时不校验编码名；能力开关缺席的腿验「连宣告都不发」并把编码应答落 unsupported。
/// 全用例零真实远端、零 DNS（目标恒为数字字面量），故禁网 CI 无需豁免。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/fake_engine.h"
#include "framework/net_wait.h"
#include "framework/scripted_peer.h"

namespace aurora::test_cases::utest_net_http_client {
namespace {

using aurora::CoroTask;
using aurora::Result;
using aurora::net::http::Client;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::Timeouts;
using aurora::test::FakeEngine;
using aurora::test::FakeEngineScope;
using aurora::test::PeerScript;
using aurora::test::PeerStop;
using aurora::test::ScriptedPeer;
using aurora::test::scripts::reply_after_reading;
using aurora::test::scripts::silent;
using aurora::test::scripts::write_then_hold;

/// @brief 拼一个指向回环端口的 GET 目标。
[[nodiscard]] auto loopback_url(std::uint16_t port, std::string_view path = "/") -> std::string {
    return "http://127.0.0.1:" + std::to_string(port) + std::string{path};
}

/// @brief 取协程结果的内存活 Response 的 Result 层（协程本身不抛，外层恒成功）。
[[nodiscard]] auto inner(CoroTask<Result<Response>> &task) -> Result<Response> {
    const auto outer = task.result();
    AURORA_TEST_REQUIRE_MSG(bool(outer), outer ? std::string{} : outer.error().message);
    return outer.value();
}

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

// `hello aurora` 的金样包装：gzip（RFC 1952）、裸 DEFLATE（RFC 1951）、以及先 gzip 再 zlib 的两层链。
const unsigned char AURORA_GZIP_HELLO[] = {31, 139, 8,  0,   0,  0,  0, 0, 2,   255, 203, 72,  205, 201, 201, 87,
                                           72, 44,  45, 202, 47, 74, 4, 0, 214, 230, 105, 148, 12,  0,   0,   0};
#ifdef AURORA_ENABLE_COMPRESSION
const unsigned char AURORA_RAW_HELLO[] = {203, 72, 205, 201, 201, 87, 72, 44, 45, 202, 47, 74, 4, 0};
const unsigned char AURORA_GZIP_THEN_ZLIB[] = {120, 156, 147, 239, 230, 96,  0,   1,  166, 255, 167, 61,  206,
                                               158, 60,  25,  238, 161, 163, 123, 74, 223, 139, 133, 225, 218,
                                               179, 204, 41,  60,  12,  12,  12,  0,  162, 234, 10,  42};
#endif

/// @brief 拼一份 `Content-Length` 分帧的 200 应答：头部字段 + 原样字节体（压缩字节按长度分帧最省事）。
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

/// @brief 记录被写出的请求字节、随后按给定应答收摊的对端脚本。
///
/// 宣告 `Accept-Encoding` 与「不覆盖调用方自带字段」的判据在**写出的字节**上，光看应答侧结果证明不了；
/// 故这里把请求头部块留一份。脚本线程写、用例线程读，用互斥量交接（用例在 send 结束后才取，天然错开，
/// 锁只为把这件事写成可见的同步而不是默认成立）。
/// @param recorded 交接缓冲（脚本线程追加）。
/// @param mutex_for_recorded 交接缓冲的锁。
/// @param payload 读满一个头部块后写出的应答字节。
/// @return 对端脚本。
[[nodiscard]] auto record_then_reply(std::string &recorded, std::mutex &guard, std::string payload) -> PeerScript {
    return [&recorded, &guard, payload = std::move(payload)](aurora::net::blocking::StreamSocket &socket,
                                                             PeerStop &stop) -> void {
        (void)socket.set_io_timeout(std::chrono::milliseconds{200});
        std::vector<char> buffer(4096);
        std::string seen;
        while (!stop.load()) {
            const auto read = socket.read_some(std::span<char>{buffer});
            if (!read || read.value() == 0) {
                break;
            }
            seen.append(buffer.data(), read.value());
            if (seen.find("\r\n\r\n") != std::string::npos) {
                break;  // 头部块到位即应答（GET 无体，再读只会挂到超时）
            }
        }
        {
            const std::scoped_lock held(guard);
            recorded = seen;
        }
        (void)socket.write_all(std::span<const char>{payload});
    };
}

/// @brief 取记录到的请求字节（脚本线程写入，用例线程在 send 收尾后读取）。
/// @param recorded 交接缓冲。
/// @param guard 交接缓冲的锁。
/// @return 请求字节副本。
[[nodiscard]] auto recorded_request(const std::string &recorded, std::mutex &guard) -> std::string {
    const std::scoped_lock held(guard);
    return recorded;
}

/// @brief 统计子串出现次数：宣告字段「只补一次、不重复追加」的判据要用它。
/// @param haystack 待查文本。
/// @param needle 子串（大小写敏感，测试里的拼写都是发送侧原样）。
/// @return 出现次数。
[[nodiscard]] auto count_occurrences(std::string_view haystack, std::string_view needle) -> std::size_t {
    std::size_t hits = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++hits;
    }
    return hits;
}

/// @brief 把文本里的字节按 ASCII 小写化（比对头部字段名时不依赖大小写）。
/// @param text 原文。
/// @return 小写副本。
[[nodiscard]] auto lowered(std::string_view text) -> std::string {
    std::string out{text};
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(static_cast<unsigned char>(c) + 32);
        }
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(send_materializes_content_length) {
    Client client;
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello");
}

AURORA_TEST_CASE(send_materializes_chunked_body) {
    Client client;
    ScriptedPeer peer(reply_after_reading(
        1, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello world");
}

AURORA_TEST_CASE(send_follows_redirect_across_peers) {
    Client client;
    ScriptedPeer target(reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nZ"));
    AURORA_TEST_REQUIRE(target.valid());
    const std::string redirect =
        "HTTP/1.1 302 Found\r\nLocation: " + loopback_url(target.port()) + "\r\nContent-Length: 0\r\n\r\n";
    ScriptedPeer origin(reply_after_reading(1, redirect));
    AURORA_TEST_REQUIRE(origin.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(origin.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().status(), std::uint16_t{200});
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "Z");
    AURORA_TEST_CHECK_EQ(origin.accepts(), 1);
    AURORA_TEST_CHECK_EQ(target.accepts(), 1);
}

AURORA_TEST_CASE(send_stops_at_redirect_limit) {
    Client client;
    // 每跳都回 302 指向自身路径：跟随必然撞到上限而不是收敛。
    ScriptedPeer peer(reply_after_reading(1, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port(), "/loop")).max_redirects(2));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpRedirectLimit);
    // 初始一跳 + 两跳跟随 = 三次连接，其后因超限而止。
    AURORA_TEST_CHECK_EQ(peer.accepts(), 3);
}

AURORA_TEST_CASE(send_times_out_whole_request) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;

    Client client;
    ScriptedPeer peer(silent());  // 连得上，但永不回字节：读侧一直挂着。
    AURORA_TEST_REQUIRE(peer.valid());

    auto task = client.send(Request::get(loopback_url(peer.port())).timeout(std::chrono::milliseconds{50}));
    // 先泵到连接与写出落地（对端已 accept、读请求挂起）。
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    // 推进脚本化时钟到整请求时限：到期取消令牌，挂起的读以 net-cancelled 收尾，收尾据 timed_out 改道。
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

AURORA_TEST_CASE(send_times_out_read_idle) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    // 对端只写头部（声明 100 字节体却不再给）后静默：读体阶段挂起，read_idle 到点。
    ScriptedPeer peer(write_then_hold("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());

    auto task = client.send(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.read_idle = std::chrono::milliseconds{50}}));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("peer accepted", [&peer] { return peer.accepts() > 0; }).satisfied);
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

AURORA_TEST_CASE(send_times_out_connect) {
    FakeEngine engine;
    const FakeEngineScope scope(engine);
    const aurora::test::net::HostDriven host;
    Client client;
    ScriptedPeer peer(silent());
    AURORA_TEST_REQUIRE(peer.valid());

    // 不 pump：让 connect 始终不落地，直接推进时钟触发 connect 档。
    auto task = client.send(
        Request::get(loopback_url(peer.port())).timeouts(Timeouts{.connect = std::chrono::milliseconds{50}}));
    engine.advance(std::chrono::milliseconds{50});
    AURORA_TEST_CHECK(aurora::test::net::drive_until(
                          "send done", [&task] { return task.is_done(); }, std::chrono::milliseconds{500})
                          .satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpTimeout);
}

#ifdef AURORA_ENABLE_COMPRESSION

/// @brief 拼一份 chunked 分帧的 200 应答：单块容纳整个体，再跟终止块。
/// @param fields 头部字段行（每行以 `\\r\\n` 结尾，可空）。
/// @param body 消息体字节（分块解码后应交出的字节）。
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

AURORA_TEST_CASE(send_decompresses_a_gzip_body) {
    Client client;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
    // 头部按对端原样保留：体已是还原后的字节，但「对端声明了什么」仍是可审计的事实。
    AURORA_TEST_CHECK_EQ(response.value().header("Content-Encoding").value_or(""), "gzip");
}

AURORA_TEST_CASE(send_falls_back_to_a_raw_deflate_stream) {
    Client client;
    // 对端把裸 DEFLATE（RFC 1951）称作 `deflate`：zlib 包装那条路先拒，回落路径接手；分帧走 chunked。
    ScriptedPeer peer(reply_after_reading(1, chunked_framed("Content-Encoding: deflate\r\n", bytes(AURORA_RAW_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
}

AURORA_TEST_CASE(send_undoes_a_multi_stage_chain_in_reverse) {
    Client client;
    // 书写次序即应用次序：载荷先经 gzip 再经 zlib，故还原必须 deflate 先、gzip 后。
    ScriptedPeer peer(
        reply_after_reading(1, length_framed("Content-Encoding: gzip, deflate\r\n", bytes(AURORA_GZIP_THEN_ZLIB))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto text = response.value().text();
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
}

AURORA_TEST_CASE(send_enforces_the_decompressed_ceiling) {
    const auto wire = length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO));
    ScriptedPeer peer(reply_after_reading(1, wire));  // 线上 32 字节，还原后 12 字节
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    Client tight;
    tight.max_decompressed_bytes(std::optional<std::uint64_t>{std::uint64_t{4}});
    auto limited = tight.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("limited done", [&limited] { return limited.is_done(); }).satisfied);
    const auto refused = inner(limited);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::HttpBodyTooLarge);
    // detail 点名越的是哪一枚旋钮：指错地方，调用方就会去调不管用的那一枚。
    AURORA_TEST_CHECK(refused.error().message.find("max_decompressed_bytes") != std::string::npos);

    // 同一条应答在放宽后放行：证明判据管的是产物字节数而不是「压缩即拒绝」。
    Client loose;
    loose.max_decompressed_bytes(std::optional<std::uint64_t>{std::uint64_t{12}});
    auto allowed = loose.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(
        aurora::test::net::drive_until("allowed done", [&allowed] { return allowed.is_done(); }).satisfied);
    const auto delivered = inner(allowed);
    AURORA_TEST_REQUIRE_MSG(bool(delivered), delivered ? std::string{} : delivered.error().message);
    const auto text = delivered.value().text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "hello aurora");
    AURORA_TEST_CHECK_EQ(peer.accepts(), 2);
}

AURORA_TEST_CASE(send_advertises_the_supported_codings_by_default) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(record_then_reply(recorded, guard, length_framed("", {})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{1});
    AURORA_TEST_CHECK(request_text.find("accept-encoding: gzip, deflate\r\n") != std::string::npos);
}

AURORA_TEST_CASE(send_keeps_the_callers_accept_encoding) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(record_then_reply(recorded, guard, length_framed("", {})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    // 协商是调用方的表达：已自带该字段即不覆盖，也不重复补一条。
    auto task = client.send(Request::get(loopback_url(peer.port())).header("Accept-Encoding", "identity"));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{1});
    AURORA_TEST_CHECK(request_text.find("accept-encoding: identity\r\n") != std::string::npos);
}

AURORA_TEST_CASE(build_advertises_and_decodes_only_when_compression_is_compiled_in) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(
        record_then_reply(recorded, guard, length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    // 宣告与解码同源于「本构建真的带解码器」：这一腿里两者都必须在场。
    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{1});
    AURORA_TEST_CHECK(request_text.find("accept-encoding: gzip, deflate\r\n") != std::string::npos);
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    const auto &plain = response.value().body();
    AURORA_TEST_CHECK_EQ(std::string(plain.begin(), plain.end()), "hello aurora");
}

#else

// 能力缺席的腿按 TEST-R6 纪律把只在解码器在场时才成立的用例登记成 SKIP；两腿同名，用例数逐名可比。
AURORA_TEST_CASE(send_decompresses_a_gzip_body) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_falls_back_to_a_raw_deflate_stream) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_undoes_a_multi_stage_chain_in_reverse) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_enforces_the_decompressed_ceiling) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_advertises_the_supported_codings_by_default) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(send_keeps_the_callers_accept_encoding) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(build_advertises_and_decodes_only_when_compression_is_compiled_in) {
    std::mutex guard;
    std::string recorded;
    Client client;
    ScriptedPeer peer(
        record_then_reply(recorded, guard, length_framed("Content-Encoding: gzip\r\n", bytes(AURORA_GZIP_HELLO))));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    // 不发宣告，也不把压缩字节当明文交出：落 unsupported 而不是交出一段乱码。
    const auto request_text = lowered(recorded_request(recorded, guard));
    AURORA_TEST_CHECK_EQ(count_occurrences(request_text, "accept-encoding:"), std::size_t{0});
    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
}

#endif

AURORA_TEST_CASE(send_refuses_a_coding_outside_the_advertised_set) {
    Client client;
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: br\r\n", {'u', 'n', 'k'})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK(response.error().message.find("br") != std::string::npos);
}

AURORA_TEST_CASE(send_refuses_a_body_that_fails_its_declared_coding) {
    Client client;
    // 分帧干净（长度对得上）但字节过不了 gzip：判消息体损坏，而不是回吐压缩字节。
    ScriptedPeer peer(reply_after_reading(1, length_framed("Content-Encoding: gzip\r\n", {'N', 'O', 'T', '-'})));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::get(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_FALSE(bool(response));
#ifdef AURORA_ENABLE_COMPRESSION
    // 有解码器：分帧干净而字节不合流，成因是流损坏。
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
#else
    // 无解码器：同一份拒绝姿态、另一种成因——环节本身就解不了。两腿都绝不回吐压缩字节。
    AURORA_TEST_CHECK_EQ(response.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
#endif
}

AURORA_TEST_CASE(send_skips_coding_validation_for_a_head_response) {
    Client client;
    // HEAD 应答照抄 GET 的头部（这里连 `br` 都带上了），但本就没有可解的载荷：无体形态不校验编码名。
    ScriptedPeer peer(
        reply_after_reading(1, "HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 4096\r\n\r\n"));
    AURORA_TEST_REQUIRE(peer.valid());
    const aurora::test::net::HostDriven host;

    auto task = client.send(Request::head(loopback_url(peer.port())));
    AURORA_TEST_CHECK(aurora::test::net::drive_until("send done", [&task] { return task.is_done(); }).satisfied);

    const auto response = inner(task);
    AURORA_TEST_REQUIRE_MSG(bool(response), response ? std::string{} : response.error().message);
    AURORA_TEST_CHECK_EQ(response.value().body().size(), std::size_t{0});
}

}  // namespace aurora::test_cases::utest_net_http_client

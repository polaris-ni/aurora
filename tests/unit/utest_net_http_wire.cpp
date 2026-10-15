/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_wire.h + src/aurora/net/detail/http_wire.cpp
/// 测试说明: HTTP/1.1 报文字节层的逐值回归（CHG-018 §6 的报文编码 / 解析部分）。覆盖应答头部块解析
///           的三态分岔（还没读够 / 完整 / 不合文法或超头部上限）、同名头保序不折叠、obs-fold 续行合并、
///           CRLF 与裸 LF 两型换行、`header_bytes` 之后的字节归属消息体；分帧判定按规范性次序落形态
///           （`Content-Length` 与 `Transfer-Encoding` 并存判走私、非 chunked 的传输编码判不可实现、
///           1xx/204/304 与 HEAD 恒无体、多个不一致 `Content-Length` 判歧义）；请求序列化的三处校验两处
///           补齐（方法须为 token、字段名须为 token、字段值不得含 CR/LF 即头部注入入口，缺 `Host` 按
///           目标 URL 补齐含非缺省端口、空 path 补 `/`、IPv6 补方括号）；分块解码状态机的跨投递累积、
///           块扩展忽略、终止块与 trailer、长度字段溢出与越上限、缺 CRLF、终止后余字节的分岔；应答装配
///           状态机（`ResponseAssembler`）把上述三件按线上次序串起：跨投递物化 `Content-Length` 与
///           chunked 体、HEAD 应答恒无体、`UntilClose` 只在对端 FIN 后完整、体累计越限落
///           `http-body-too-large`、完整之后再喂字节判畸形（流水线不在本库面内）。
/// 全用例零 socket、零 DNS、不读环境变量，故三平台行为一致、wasm 腿同样可跑，无需 `AURORA_TEST_SKIP`。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/detail/http_wire.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_wire {
namespace {

using aurora::testing::require_value;
namespace wire = aurora::net::wire;

/// @brief 取三态解析产出的头部块：还没读够与畸形是两回事，此处缺失即致命失败。
/// @param parsed 解析结果（`Result<optional<ResponseHead>>`）。
/// @return 完整解析出的头部块副本。
[[nodiscard]] auto completed(const Result<std::optional<wire::ResponseHead>> &parsed) -> wire::ResponseHead {
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    return require_value(parsed.value());
}

/// @brief 由状态码与若干字段拼一个头部块，喂给 `framing_of`（只关心判据，不重跑解析）。
/// @param status 应答状态码。
/// @param fields 头部字段的「名字, 值」对（原始拼写，同名可多条）。
/// @return 组装好的头部块。
[[nodiscard]] auto make_head(std::uint16_t status, const std::vector<std::pair<std::string, std::string>> &fields)
    -> wire::ResponseHead {
    wire::ResponseHead head;
    head.status = status;
    for (const auto &[name, value] : fields) {
        head.headers.append(name, value);
    }
    return head;
}

/// @brief 取出已判定成功的请求字节；失败即致命失败并带上错误文本。
/// @param rendered `render_request_head` 的产出。
/// @return 序列化后的请求字节。
[[nodiscard]] auto unwrapped_render(const Result<std::string> &rendered) -> std::string {
    AURORA_TEST_REQUIRE_MSG(bool(rendered), rendered ? std::string{} : rendered.error().message);
    return rendered.value();
}

/// @brief 解出目标 URL：解析失败即致命失败（测试里的 URL 文本都是合法输入，不该走失败分支）。
/// @param text URL 文本。
/// @return 解析成功的 Url 副本。
[[nodiscard]] auto url_of(std::string_view text) -> Url {
    const auto parsed = Url::parse(text);
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    return parsed.value();
}

/// @brief 跑一遍分块解码：逐段 push 后一次性 drain，返回明文；任一 push 失败即致命失败。
/// @param segments 投递序列（模拟内核把一条消息切成多次读）。
/// @param decoder 输出：解码结束后的状态机，供调用方查 trailer 与 complete。
/// @return 解码出的明文字节串。
[[nodiscard]] auto run_decoder(const std::vector<std::string> &segments, wire::ChunkedDecoder &decoder) -> std::string {
    for (const auto &segment : segments) {
        const auto pushed = decoder.push(segment);
        AURORA_TEST_REQUIRE_MSG(bool(pushed), pushed ? std::string{} : pushed.error().message);
    }
    std::vector<char> out;
    decoder.drain(out);
    return std::string{out.begin(), out.end()};
}

}  // namespace

AURORA_TEST_CASE(response_head_is_three_state_and_preserves_fields) {
    // 还没读到空行：成功且值缺失，不是错误——调用方据此继续喂字节。
    const auto pending = wire::parse_response_head("HTTP/1.1 200 OK\r\nX-A: 1\r\n");
    AURORA_TEST_REQUIRE(bool(pending));
    AURORA_TEST_CHECK_TRUE(!pending.value().has_value());

    const std::string_view complete =
        "HTTP/1.1 200 OK\r\nServer: some/1.0\r\nX-A: 1\r\nx-a: 2\r\nSet-Cookie: a=1\r\n\r\nbody-bytes";
    const auto head = completed(wire::parse_response_head(complete));
    AURORA_TEST_CHECK_EQ(head.status, 200);
    AURORA_TEST_CHECK_EQ(head.reason, "OK");
    // 同名头保序不折叠（与 core::Headers 同口径）。
    AURORA_TEST_CHECK_EQ(head.headers.size(), std::size_t{4});
    AURORA_TEST_CHECK_EQ(head.headers.values("x-a").size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(head.headers.get("server").value_or(""), "some/1.0");
    // header_bytes 之后的字节属于消息体，绝不被头部解析吞掉。
    AURORA_TEST_CHECK_EQ(head.header_bytes, complete.size() - std::string_view{"body-bytes"}.size());
    AURORA_TEST_CHECK_EQ(complete.substr(head.header_bytes), "body-bytes");

    // 裸 LF 换行同样判完整；缺原因短语（只有状态码）也合文法。
    const auto lf_only = completed(wire::parse_response_head("HTTP/1.0 204\n\n"));
    AURORA_TEST_CHECK_EQ(lf_only.status, 204);
    AURORA_TEST_CHECK_TRUE(lf_only.headers.empty());
    AURORA_TEST_CHECK_EQ(completed(wire::parse_response_head("HTTP/1.1 200\r\n\r\n")).status, 200);

    // obs-fold 续行（前导空格 / 制表符）合并进上一条字段值，条目数不增。
    const auto folded = completed(wire::parse_response_head("HTTP/1.1 200 OK\r\nX-A: 1\r\n\t2\r\n\r\n"));
    AURORA_TEST_CHECK_EQ(folded.headers.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(folded.headers.get("x-a").value_or(""), "1 2");
}

AURORA_TEST_CASE(response_head_rejects_malformed_and_oversized) {
    for (const std::string_view bad :
         {"SOCKS reply is not HTTP\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n", "HTTP/1.1 2000 OK\r\n\r\n",
          "HTTP 200 OK\r\n\r\n", "HTTP/1.1\r\n\r\n", "HTTP/1.1 200 OK\r\ngarbage\r\n\r\n",
          "HTTP/1.1 200 OK\r\nX A: 1\r\n\r\n", "HTTP/1.1 200 OK\r\n: no-name\r\n\r\n",
          "HTTP/1.1 200 OK\r\n  folded-before-any-field\r\n\r\n"}) {
        const auto rejected = wire::parse_response_head(bad);
        AURORA_TEST_REQUIRE_FALSE(bool(rejected));
        AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::HttpProtocolError);
    }

    // 空行远在 32 KiB 上限之后：超限按畸形处理，绝不让读缓冲随对端节奏无界增长。
    std::string bloated = "HTTP/1.1 200 OK\r\n";
    while (bloated.size() < wire::AURORA_HTTP_MAX_HEAD_BYTES + 1) {
        bloated += "X-Padding: a great many bytes of nothing at all\r\n";
    }
    const auto capped = wire::parse_response_head(bloated);
    AURORA_TEST_REQUIRE_FALSE(bool(capped));
    AURORA_TEST_CHECK_EQ(capped.error().code_enum, ErrorCode::HttpProtocolError);

    // 未超上限且确实没读到空行 → 仍是「还没读够」。
    const auto still_pending_view = bloated.substr(0, 40);
    const auto still_pending = wire::parse_response_head(still_pending_view);
    AURORA_TEST_REQUIRE(bool(still_pending));
    AURORA_TEST_CHECK_TRUE(!still_pending.value().has_value());
}

AURORA_TEST_CASE(framing_rules_are_normative) {
    using F = wire::BodyFraming;
    const auto chunked = wire::framing_of(make_head(200, {{"Transfer-Encoding", "chunked"}}), false);
    AURORA_TEST_REQUIRE(bool(chunked));
    AURORA_TEST_CHECK_TRUE(chunked.value() == F::Chunked);

    const auto length = wire::framing_of(make_head(200, {{"Content-Length", "42"}}), false);
    AURORA_TEST_REQUIRE(bool(length));
    AURORA_TEST_CHECK_TRUE(length.value() == F::Length);

    const auto until_close = wire::framing_of(make_head(200, {}), false);
    AURORA_TEST_REQUIRE(bool(until_close));
    AURORA_TEST_CHECK_TRUE(until_close.value() == F::UntilClose);

    // 无体形态：1xx / 204 / 304，以及 HEAD 请求的应答恒无体（即便状态码带体）。
    for (const auto &no_body : {make_head(100, {{"Content-Length", "5"}}), make_head(204, {{"Content-Length", "5"}}),
                                make_head(304, {{"Transfer-Encoding", "chunked"}})}) {
        const auto framed = wire::framing_of(no_body, false);
        AURORA_TEST_REQUIRE(bool(framed));
        AURORA_TEST_CHECK_TRUE(framed.value() == F::None);
    }
    const auto head_reply = wire::framing_of(make_head(200, {{"Content-Length", "9"}}), true);
    AURORA_TEST_REQUIRE(bool(head_reply));
    AURORA_TEST_CHECK_TRUE(head_reply.value() == F::None);

    // 两条长度声明并存即走私；不一致的多个 Content-Length 即歧义；本库不实现的传输编码即畸形。
    for (const auto &ambiguous :
         {make_head(200, {{"Content-Length", "5"}, {"Transfer-Encoding", "chunked"}}),
          make_head(200, {{"Transfer-Encoding", "gzip"}}),
          make_head(200, {{"Content-Length", "3"}, {"Content-Length", "4"}}),
          make_head(200, {{"Content-Length", "0x10"}}), make_head(200, {{"Content-Length", "-1"}})}) {
        const auto framed = wire::framing_of(ambiguous, false);
        AURORA_TEST_REQUIRE_FALSE(bool(framed));
        AURORA_TEST_CHECK_EQ(framed.error().code_enum, ErrorCode::HttpProtocolError);
    }

    // 同名且取值一致的 Content-Length 重复不是歧义，仍是确切长度。
    const auto consistent = wire::framing_of(make_head(200, {{"Content-Length", "7"}, {"Content-Length", "7"}}), false);
    AURORA_TEST_REQUIRE(bool(consistent));
    AURORA_TEST_CHECK_TRUE(consistent.value() == F::Length);
}

AURORA_TEST_CASE(render_request_head_fills_and_validates) {
    const auto target = url_of("http://example.com/a/b?x=1&q=2");
    Headers fields;
    fields.set("Accept", "text/plain");
    const auto rendered = wire::render_request_head("GET", target, fields);
    AURORA_TEST_REQUIRE(bool(rendered));
    // 缺 Host 按目标补齐（缺省端口 80 不写出），请求目标带查询串，末尾空行收尾。
    AURORA_TEST_CHECK_EQ(rendered.value(),
                         "GET /a/b?x=1&q=2 HTTP/1.1\r\nHost: example.com\r\nAccept: text/plain\r\n\r\n");

    // 非缺省端口要写出；显式 Host 优先、不重复补；path 为空补 `/`。
    const auto ported = url_of("https://example.com:8443");
    AURORA_TEST_CHECK_EQ(unwrapped_render(wire::render_request_head("POST", ported, {})),
                         "POST / HTTP/1.1\r\nHost: example.com:8443\r\n\r\n");
    const auto explicit_def_port = url_of("https://example.com:443/");
    // 显式写了缺省端口仍按缺省处理，不重复写出 `:443`。
    AURORA_TEST_CHECK_EQ(unwrapped_render(wire::render_request_head("GET", explicit_def_port, {})),
                         "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    // IPv6 字面量补方括号（host 出口不含方括号）。
    const auto v6 = url_of("http://[::1]:8080/p");
    Headers with_host;
    with_host.set("host", "manual.example");  // 原始拼写照写，且不额外补 Host
    AURORA_TEST_CHECK_EQ(unwrapped_render(wire::render_request_head("DELETE", v6, with_host)),
                         "DELETE /p HTTP/1.1\r\nhost: manual.example\r\n\r\n");

    // 同名多条全部按登记次序写出。
    Headers cookies;
    cookies.append("Cookie", "a=1");
    cookies.append("Cookie", "b=2");
    AURORA_TEST_CHECK_EQ(unwrapped_render(wire::render_request_head("GET", target, cookies)),
                         "GET /a/b?x=1&q=2 HTTP/1.1\r\nHost: example.com\r\nCookie: a=1\r\nCookie: b=2\r\n\r\n");

    // 头部注入入口：字段值含 CR / LF 在拼出任何字节之前就拦住。
    Headers injected;
    injected.set("X-Evil", "ok\r\nInjected: yes");
    const auto rejected_value = wire::render_request_head("GET", target, injected);
    AURORA_TEST_REQUIRE_FALSE(bool(rejected_value));
    AURORA_TEST_CHECK_EQ(rejected_value.error().code_enum, ErrorCode::HttpRequestInvalid);

    // 方法 / 字段名不合 token 即拒。
    AURORA_TEST_CHECK_FALSE(bool(wire::render_request_head("GE T", target, {})));
    Headers bad_name;
    bad_name.set("Bad Name", "1");
    AURORA_TEST_CHECK_FALSE(bool(wire::render_request_head("GET", target, bad_name)));
}

AURORA_TEST_CASE(chunked_decoder_handles_split_and_trailers) {
    // 一次到位：两个数据块 + 终止块。
    wire::ChunkedDecoder whole;
    AURORA_TEST_CHECK_EQ(run_decoder({"5\r\nhello\r\n6\r\nworld!\r\n0\r\n\r\n"}, whole), "helloworld!");
    AURORA_TEST_CHECK_TRUE(whole.complete());

    // 跨多次投递：块长度行被切成两半、块数据与块尾 CRLF 落在下一次读里，都是常态。
    // 线上完整形态 `3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n`，切成六段投递。
    wire::ChunkedDecoder split;
    AURORA_TEST_CHECK_EQ(run_decoder({"3", "\r\nab", "c\r\n", "3\r\nde", "f\r\n0\r", "\n\r\n"}, split), "abcdef");
    AURORA_TEST_CHECK_TRUE(split.complete());

    // 块扩展参数按 RFC 忽略；十六进制严格解析（"10" 是 16 不是八进制的 8）。
    wire::ChunkedDecoder ext;
    AURORA_TEST_CHECK_EQ(run_decoder({"10;name=value\r\n0123456789abcdef\r\n0\r\n\r\n"}, ext), "0123456789abcdef");

    // 终止块之后的 trailer 经 trailers() 取回。
    wire::ChunkedDecoder trailed;
    AURORA_TEST_CHECK_EQ(run_decoder({"1\r\nx\r\n0\r\nChecksum: abc\r\n\r\n"}, trailed), "x");
    AURORA_TEST_CHECK_EQ(trailed.trailers().get("checksum").value_or(""), "abc");

    // 空体：只有终止块。
    wire::ChunkedDecoder empty_body;
    AURORA_TEST_CHECK_EQ(run_decoder({"0\r\n\r\n"}, empty_body), "");
    AURORA_TEST_CHECK_TRUE(empty_body.complete());

    // drain 只追加不清调用方缓冲，且取完即从内部移除（重复 drain 不重复产出）。
    wire::ChunkedDecoder partial;
    AURORA_TEST_REQUIRE(bool(partial.push("3\r\nabc")));
    std::vector<char> sink{'Z'};
    partial.drain(sink);
    partial.drain(sink);  // 第二次应为空
    const std::string drained{sink.begin(), sink.end()};
    AURORA_TEST_CHECK_EQ(drained, "Zabc");
}

AURORA_TEST_CASE(chunked_decoder_rejects_malformed) {
    // 长度字段非十六进制。
    wire::ChunkedDecoder not_hex;
    AURORA_TEST_CHECK_FALSE(bool(not_hex.push("XYZ\r\n")));

    // 块数据之后缺 CRLF。
    wire::ChunkedDecoder missing_crlf;
    AURORA_TEST_CHECK_FALSE(bool(missing_crlf.push("2\r\nabXY\r\n")));

    // 长度字段越单块上限（防巨大数被回绕成负数或极小值）。
    wire::ChunkedDecoder oversized;
    AURORA_TEST_CHECK_FALSE(bool(oversized.push("FFFFFFFFFFFFFFFF\r\n")));

    // trailer 字段畸形（缺冒号）。
    wire::ChunkedDecoder bad_trailer;
    AURORA_TEST_CHECK_FALSE(bool(bad_trailer.push("1\r\nx\r\n0\r\nnot-a-trailer\r\n\r\n")));

    // 终止块与 trailer 空行之后仍有字节 → 那是下一条消息的开头，本层不越界解释。
    wire::ChunkedDecoder trailing_bytes;
    const auto terminator = trailing_bytes.push("0\r\n\r\n");
    AURORA_TEST_REQUIRE_MSG(bool(terminator), terminator ? std::string{} : terminator.error().message);
    AURORA_TEST_CHECK_FALSE(bool(trailing_bytes.push("GET / HTTP/1.1")));
}

namespace {

/// @brief 取走装配器已解码的明文体并转成字符串。
/// @param assembler 已装配的应答装配器。
/// @return 明文体文本。
[[nodiscard]] auto assembled_body(wire::ResponseAssembler &assembler) -> std::string {
    std::vector<char> out;
    assembler.drain_body(out);
    return std::string{out.begin(), out.end()};
}

/// @brief 逐段送入字节，任一段失败即致命失败（把「不该畸形」写成断言前置）。
/// @param assembler 装配器。
/// @param segments 字节分段。
auto feed(wire::ResponseAssembler &assembler, const std::vector<std::string> &segments) -> void {
    for (const auto &segment : segments) {
        const auto pushed = assembler.push(segment);
        AURORA_TEST_REQUIRE_MSG(bool(pushed), pushed ? std::string{} : pushed.error().message);
    }
}

}  // namespace

AURORA_TEST_CASE(assembler_materializes_content_length_across_splits) {
    wire::ResponseAssembler assembler{std::nullopt, false};
    // 头部块与体首段同批抵达，余下体分两次到：装配必须跨投递累积。
    feed(assembler, {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe", "ll", "o"});
    AURORA_TEST_REQUIRE(assembler.head_ready());
    AURORA_TEST_CHECK_EQ(assembler.head().status, std::uint16_t{200});
    AURORA_TEST_CHECK(assembler.framing() == wire::BodyFraming::Length);
    AURORA_TEST_CHECK_TRUE(assembler.complete());
    AURORA_TEST_CHECK_EQ(assembled_body(assembler), "hello");
}

AURORA_TEST_CASE(assembler_materializes_chunked_body) {
    wire::ResponseAssembler assembler{std::nullopt, false};
    feed(assembler, {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab", "c\r\n2\r\nde\r\n", "0\r\n\r\n"});
    AURORA_TEST_CHECK(assembler.framing() == wire::BodyFraming::Chunked);
    AURORA_TEST_CHECK_TRUE(assembler.complete());
    AURORA_TEST_CHECK_EQ(assembled_body(assembler), "abcde");
}

AURORA_TEST_CASE(assembler_head_response_carries_no_body) {
    wire::ResponseAssembler assembler{std::nullopt, true};
    // HEAD 请求的应答即便带 Content-Length 也恒无体：整条应答在头部块到位即完整。
    feed(assembler, {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"});
    AURORA_TEST_CHECK(assembler.framing() == wire::BodyFraming::None);
    AURORA_TEST_CHECK_TRUE(assembler.complete());
    AURORA_TEST_CHECK_EQ(assembled_body(assembler), "");
}

AURORA_TEST_CASE(assembler_enforces_body_limit) {
    wire::ResponseAssembler assembler{std::uint64_t{4}, false};
    const auto pushed = assembler.push("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    AURORA_TEST_REQUIRE_FALSE(bool(pushed));
    AURORA_TEST_CHECK_EQ(pushed.error().code_enum, aurora::ErrorCode::HttpBodyTooLarge);
    AURORA_TEST_CHECK_TRUE(!assembler.complete());
}

AURORA_TEST_CASE(assembler_until_close_needs_peer_closed) {
    wire::ResponseAssembler assembler{std::nullopt, false};
    // 既无 Content-Length 又非 chunked 的 200：体到对端关闭为止。
    feed(assembler, {"HTTP/1.1 200 OK\r\n\r\nbo", "dy"});
    AURORA_TEST_CHECK(assembler.framing() == wire::BodyFraming::UntilClose);
    AURORA_TEST_CHECK_TRUE(!assembler.complete());
    const auto closed = assembler.peer_closed();
    AURORA_TEST_REQUIRE(bool(closed));
    AURORA_TEST_CHECK_TRUE(assembler.complete());
    AURORA_TEST_CHECK_EQ(assembled_body(assembler), "body");
}

AURORA_TEST_CASE(assembler_rejects_bytes_after_complete) {
    wire::ResponseAssembler assembler{std::nullopt, false};
    feed(assembler, {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nab"});
    AURORA_TEST_REQUIRE(assembler.complete());
    // 完整之后再喂字节属下一条消息（流水线不在本库面内），本层判畸形而不是悄悄吞下。
    AURORA_TEST_CHECK_FALSE(bool(assembler.push("HTTP/1.1 204 No Content\r\n\r\n")));
}

}  // namespace aurora::test_cases::utest_net_http_wire

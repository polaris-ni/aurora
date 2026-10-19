/// 测试类型: unit
/// 目标单元: include/aurora/net/http.h + src/aurora/net/http.cpp
/// 测试说明: HTTP 发起侧值类型的逐值回归。覆盖方法枚举与线上
///           文本的双向换算（往返一致、小写与未知拼写判 `http-request-invalid`）；工厂对目标 URL 的
///           当场解析与「解析失败延后到 render_head 才报错」的口径；请求序列化的两处补齐（有体且未声明
///           `Content-Length` / `Transfer-Encoding` 时按体长度补、缺 `Host` 由字节层按目标补）与 HEAD
///           请求丢弃消息体；链式字段追加保序、`set_header` 清同名后追加；应答值类型的状态码 / 原因 /
///           大小写不敏感取值 / 原始字节与文本形态；失败分型 `HttpErrorClass` 的逐码归档（以生成表做完整性
///           断言：每一枚 `net-*` / `http-*` 都必须被点名，非网络族码落兜底档）、档位与 `retryable` 的正交
///           关系、`error_class_of_status` 的 400 边界，以及 `error_class_text` 的九枚小写稳定令牌。请求侧的
///           `RetryPolicy` 槽位验两级语义：不设 = 跟随 `Client` 的缺省、设 `RetryPolicy{}` = 本条关闭重试
///           （两者必须可区分）、再设是整份替换而非逐字段合并、五枚字段逐个回读。上传/下载便捷面的值层
///           同样逐值断言：`ByteRange` 三型与线上文本的换算（含形态合法性）、`Range` / `If-Range` /
///           `Expect: 100-continue` 三枚旋钮在 `render_head()` 的现地补齐与「调用方写过的优先」、内存体与
///           `BodySource` 的互斥（两种交法后设者生效、`HEAD` 一律忽略）、源自报总长未知时改走分帧声明、
///           `Content-Range` 三形态解析与 `is_partial()` 只看状态码的口径。
/// 全用例零 socket、零 DNS、不读环境变量，故三平台行为一致、wasm 腿同样可跑，无需 `AURORA_TEST_SKIP`。

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/net/http.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http {
namespace {

using aurora::net::http::BodySource;
using aurora::net::http::ByteRange;
using aurora::net::http::error_class_of;
using aurora::net::http::error_class_of_status;
using aurora::net::http::error_class_text;
using aurora::net::http::HttpErrorClass;
using aurora::net::http::Method;
using aurora::net::http::Request;
using aurora::net::http::Response;
using aurora::net::http::RetryPolicy;
using aurora::net::http::Timeouts;
using aurora::testing::require_value;

/// @brief 断言请求序列化成功并返回其文本（畸形即致命失败）。
/// @param request 待序列化的请求。
/// @return 请求头部文本。
[[nodiscard]] auto head_text(const Request &request) -> std::string {
    const auto rendered = request.render_head();
    AURORA_TEST_REQUIRE_MSG(bool(rendered), rendered ? std::string{} : rendered.error().message);
    return rendered.value();
}

/// @brief 判断文本里是否出现某子串（把「补齐了没有」做成一条可读断言）。
/// @param haystack 序列化产出。
/// @param needle 期望出现的片段。
/// @return 出现为真。
[[nodiscard]] auto contains(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief 统计片段出现次数（把「只补齐了一句、没补成两句」做成可读断言）。
/// @param haystack 序列化产出。
/// @param needle 期望片段的精确文本。
/// @return 出现次数。
[[nodiscard]] auto count_of(std::string_view haystack, std::string_view needle) -> std::size_t {
    if (needle.empty()) {
        return 0;  // 空片段没有「出现次数」可言，也不该把循环钉在零长度上
    }
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos; pos = haystack.find(needle, pos)) {
        ++count;
        pos += needle.size();
    }
    return count;
}

/// @brief 一只可编排的流式体源：按给定序列吐出段，总长与可回退性都由测试指定。
///
/// 值层只需要它把三枚虚方法答出来（体泵与 100-continue 的行为在 `utest_net_http_client` 那条腿上验）；
/// 段消费进度在本对象里推进，故同一只源挂两次请求会接着上次吐——测试每例新建一只。
class ScriptedSource final : public BodySource {
  public:
    /// @brief 以段序列、总长与可回退性构造。
    /// @param chunks 各段明文；空段序列意为「一问就结束」。
    /// @param total `total_size()` 的回答。
    /// @param rewindable `restart()` 的回答。
    ScriptedSource(std::vector<std::string> chunks, std::optional<std::uint64_t> total, bool rewindable)
        : chunks_{std::move(chunks)}, total_{total}, rewindable_{rewindable} {}

    [[nodiscard]] auto next_chunk() -> Result<std::vector<char>> override {
        if (cursor_ >= chunks_.size()) {
            return Result<std::vector<char>>{std::vector<char>{}};
        }
        const auto &text = chunks_[cursor_++];
        const std::vector<char> bytes{text.begin(), text.end()};
        return Result<std::vector<char>>{bytes};
    }

    [[nodiscard]] auto total_size() const -> std::optional<std::uint64_t> override { return total_; }

    auto restart() -> bool override {
        if (!rewindable_) {
            return false;
        }
        cursor_ = 0;
        return true;
    }

  private:
    std::vector<std::string> chunks_;  ///< 段序列
    std::optional<std::uint64_t> total_;  ///< 自报总长
    bool rewindable_;  ///< 可否回退
    std::size_t cursor_{0};  ///< 下一段序号
};

/// @brief 判断 `render_head()` 是否以某枚错误码失败，并回读该码。
/// @param request 待序列化的请求。
/// @return 失败码；序列化成功时即致命失败。
[[nodiscard]] auto render_error_code(const Request &request) -> aurora::ErrorCode {
    const auto rendered = request.render_head();
    AURORA_TEST_REQUIRE_MSG(!bool(rendered), rendered ? std::string{"expected failure"} : std::string{});
    return rendered.error().code_enum;
}

/// @brief 一枚网络码的期望归因档（逐码列全：新增码没进本表就会被完整性用例拦下，而不是静默落兜底档）。
struct ClassExpectation {
    aurora::ErrorCode code;  ///< 生成表里的错误码
    HttpErrorClass expected;  ///< 期望归入的档
};

/// @brief `net-*` 与 `http-*` 两族全部 43 枚的期望归因档（按档分组，便于逐条读判定依据）。
constexpr std::array<ClassExpectation, 43> AURORA_HTTP_ERROR_CLASS_TABLE{{
    // 名字解析不了：还没走到链路。
    {.code = aurora::ErrorCode::NetDnsFailed, .expected = HttpErrorClass::Dns},
    // 链路断了且没有更具体的档可归：含服务端面三枚（发起侧不产出，但同样给出档而非「未知」）。
    {.code = aurora::ErrorCode::NetConnectFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetSocketFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetRuntimeFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetWaitFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetSendFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetRecvFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetBindFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetListenFailed, .expected = HttpErrorClass::Unreachable},
    {.code = aurora::ErrorCode::NetAcceptFailed, .expected = HttpErrorClass::Unreachable},
    // 代理这一跳给的答案：注意「配置读不到」归此档而「配置写错了」归兜底档——前者的动作在环境，后者在调用点。
    {.code = aurora::ErrorCode::NetProxyUnreachable, .expected = HttpErrorClass::Proxy},
    {.code = aurora::ErrorCode::NetProxyAuthRequired, .expected = HttpErrorClass::Proxy},
    {.code = aurora::ErrorCode::NetProxyProtocol, .expected = HttpErrorClass::Proxy},
    {.code = aurora::ErrorCode::NetProxyConfigUnreadable, .expected = HttpErrorClass::Proxy},
    // 安全通道本身（引擎不提供 TLS 属本构建能力，落兜底档）。
    {.code = aurora::ErrorCode::NetTlsHandshakeFailed, .expected = HttpErrorClass::Tls},
    {.code = aurora::ErrorCode::NetTlsCertRejected, .expected = HttpErrorClass::Tls},
    {.code = aurora::ErrorCode::NetTlsVersionMismatch, .expected = HttpErrorClass::Tls},
    {.code = aurora::ErrorCode::NetTlsPinMismatch, .expected = HttpErrorClass::Tls},
    // 时限到点：三档 HTTP 超时与裸传输空闲超时同归此档，档位不分阶段。
    {.code = aurora::ErrorCode::HttpTimeout, .expected = HttpErrorClass::Timeout},
    {.code = aurora::ErrorCode::NetConnectTimeout, .expected = HttpErrorClass::Timeout},
    {.code = aurora::ErrorCode::NetRecvTimeout, .expected = HttpErrorClass::Timeout},
    // 对端把报文写坏了：含「把重定向写成了环」。
    {.code = aurora::ErrorCode::HttpProtocolError, .expected = HttpErrorClass::Protocol},
    {.code = aurora::ErrorCode::HttpBodyCorrupt, .expected = HttpErrorClass::Protocol},
    {.code = aurora::ErrorCode::HttpRedirectLimit, .expected = HttpErrorClass::Protocol},
    // 调用方主动收摊优先于一切链路事实。
    {.code = aurora::ErrorCode::NetCancelled, .expected = HttpErrorClass::Cancelled},
    // 兜底档：调用点写错 / 本构建缺能力 / 本库旋钮设限——动作在代码与配置里，不在网络上。
    {.code = aurora::ErrorCode::NetUrlInvalid, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::HttpRequestInvalid, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetEndpointInvalid, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetPlatformUnavailable, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetTlsEngineUnavailable, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetOptionFailed, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetOptionConflict, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetTimeoutOptionFailed, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetProxyConfigInvalid, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetProxyUnsupported, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetFramerConfigInvalid, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetBufferLimit, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::NetFramerOverflow, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::HttpBodyNotReplayable, .expected = HttpErrorClass::Invalid},
    // 策略闸掐在发起之前：拒的是这份声明与这个目标的组合，不是链路。
    {.code = aurora::ErrorCode::NetPolicyViolated, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::HttpBodyTooLarge, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::HttpCharsetUnsupported, .expected = HttpErrorClass::Invalid},
    {.code = aurora::ErrorCode::HttpContentEncodingUnsupported, .expected = HttpErrorClass::Invalid},
}};

/// @brief 查期望表里某枚码的归档；不在表里回 `nullptr`。
/// @param code 待查的错误码。
/// @return 期望档的只读指针，未列即空。
[[nodiscard]] auto expected_class_of(aurora::ErrorCode code) -> const HttpErrorClass * {
    for (const auto &entry : AURORA_HTTP_ERROR_CLASS_TABLE) {
        if (entry.code == code) {
            return &entry.expected;
        }
    }
    return nullptr;
}

/// @brief 判断 slug 属于网络两族（`net-*` / `http-*`）。
/// @param slug 生成表里的冻结 slug。
/// @return 属于两族为真。
[[nodiscard]] auto is_network_slug(std::string_view slug) -> bool {
    return slug.starts_with("net-") || slug.starts_with("http-");
}

}  // namespace

AURORA_TEST_CASE(method_text_and_parse_are_inverse) {
    for (const Method method : {Method::Get, Method::Head, Method::Post, Method::Put, Method::Patch, Method::Delete}) {
        const auto text = aurora::net::http::method_text(method);
        AURORA_TEST_CHECK_TRUE(!text.empty());
        const auto parsed = aurora::net::http::method_from_text(text);
        AURORA_TEST_REQUIRE(bool(parsed));
        AURORA_TEST_CHECK_EQ(parsed.value(), method);
    }

    // 大小写敏感：HTTP 方法是区分大小写的令牌，小写拼写不得被「善意地」当成对应方法。
    const auto lower = aurora::net::http::method_from_text("get");
    AURORA_TEST_REQUIRE_FALSE(bool(lower));
    AURORA_TEST_CHECK_EQ(lower.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);

    // 自定义方法本批次不提供：未知拼写报错而不是接受下来让 Content-Length 的取舍失据。
    const auto unknown = aurora::net::http::method_from_text("PROPFIND");
    AURORA_TEST_REQUIRE_FALSE(bool(unknown));
    AURORA_TEST_CHECK_EQ(unknown.error().code_enum, aurora::ErrorCode::HttpRequestInvalid);
}

AURORA_TEST_CASE(factory_parses_url_and_defers_error_to_render) {
    const auto good = Request::get("https://example.com/a");
    AURORA_TEST_CHECK_TRUE(good.url().has_value());
    AURORA_TEST_CHECK_EQ(good.method(), Method::Get);
    AURORA_TEST_CHECK_TRUE(contains(head_text(good), "GET /a HTTP/1.1"));

    // 目标解析失败不在工厂报错（工厂返回 Request 而非 Result），而是把解析码原样延后到 render_head。
    const auto bad = Request::post("http://[::1");
    AURORA_TEST_CHECK_TRUE(!bad.url().has_value());
    const auto rendered = bad.render_head();
    AURORA_TEST_REQUIRE_FALSE(bool(rendered));
    AURORA_TEST_CHECK_EQ(rendered.error().code_enum, aurora::ErrorCode::NetUrlInvalid);
}

AURORA_TEST_CASE(render_head_fills_host_and_content_length) {
    std::vector<char> body{'h', 'e', 'l', 'l', 'o'};
    const auto request = Request::post("https://example.com:8443/api?q=1").body(std::move(body));
    const auto text = head_text(request);
    AURORA_TEST_CHECK_TRUE(contains(text, "POST /api?q=1 HTTP/1.1"));
    // 非缺省端口写进 Host；体长度自动补齐。
    AURORA_TEST_CHECK_TRUE(contains(text, "Host: example.com:8443"));
    AURORA_TEST_CHECK_TRUE(contains(text, "Content-Length: 5"));

    // 调用方已自报长度时不覆盖；GET 无体时不补长度。
    const auto declared = Request::post("https://example.com/").set_header("Content-Length", "42");
    AURORA_TEST_CHECK_TRUE(contains(head_text(declared), "Content-Length: 42"));
    const auto plain = Request::get("https://example.com/");
    AURORA_TEST_CHECK_TRUE(!contains(head_text(plain), "Content-Length"));
}

AURORA_TEST_CASE(head_request_drops_body_and_setter_chains) {
    const auto head = Request::head("https://example.com/").body(std::vector<char>{'x'});
    AURORA_TEST_CHECK_TRUE(head.is_head());
    AURORA_TEST_CHECK_TRUE(head.body().empty());

    // header 追加保序、set_header 清同名后追加到末尾。
    auto request = Request::get("https://example.com/");
    request.header("Accept", "text/plain").header("Accept", "application/json");
    AURORA_TEST_CHECK_EQ(request.headers().values("Accept").size(), std::size_t{2});
    request.set_header("Accept", "image/png");
    AURORA_TEST_CHECK_EQ(request.headers().values("Accept").size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(request.headers().get("accept").value_or(""), "image/png");

    // 时限与重定向上限是链式选项，未设时 timeout 为空、重定向缺省 10。
    AURORA_TEST_CHECK_TRUE(!request.timeout().has_value());
    AURORA_TEST_CHECK_EQ(request.max_redirects(), 10);
    request.timeout(std::chrono::milliseconds{2500}).max_redirects(0);
    const auto pending_timeout = request.timeout();
    AURORA_TEST_CHECK_TRUE(pending_timeout.has_value());
    AURORA_TEST_CHECK_EQ(pending_timeout.value_or(std::chrono::milliseconds{}).count(), std::int64_t{2500});
    AURORA_TEST_CHECK_EQ(request.max_redirects(), 0);
}

AURORA_TEST_CASE(response_value_accessors) {
    Headers headers;
    headers.set("Content-Type", "application/json");
    headers.append("Set-Cookie", "a=1");
    headers.append("Set-Cookie", "b=2");
    const Response response{200, "OK", headers, std::vector<char>{'A', 'B'}};

    AURORA_TEST_CHECK_EQ(response.status(), std::uint16_t{200});
    AURORA_TEST_CHECK_EQ(response.reason(), "OK");
    // 大小写不敏感取值，与 Headers 同口径。
    AURORA_TEST_CHECK_EQ(response.header("content-type").value_or(""), "application/json");
    AURORA_TEST_CHECK_TRUE(!response.header("X-Missing").has_value());
    AURORA_TEST_CHECK_EQ(response.headers().values("set-cookie").size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(response.body().size(), std::size_t{2});
    // 无 charset 的 Content-Type（application/json）按 UTF-8 缺省解码；text() 现返回 Result。
    const auto text = response.text();
    AURORA_TEST_REQUIRE(bool(text));
    AURORA_TEST_CHECK_EQ(text.value(), "AB");
}

AURORA_TEST_CASE(response_text_decodes_by_charset) {
    // UTF-8：剥去前导 BOM 后原样直传。
    Headers utf8;
    utf8.set("Content-Type", "text/plain; charset=UTF-8");
    const Response with_bom{200, "OK", utf8, std::vector<char>{'\xEF', '\xBB', '\xBF', 'A', 'B'}};
    const auto bom = with_bom.text();
    AURORA_TEST_REQUIRE(bool(bom));
    AURORA_TEST_CHECK_EQ(bom.value(), "AB");

    // 非法 UTF-8 序列不报错也不替换：字节原样抵达（§12「绝不静默替换非法序列」）。
    const Response raw_illegal{200, "OK", utf8, std::vector<char>{'\xFF', 'X'}};
    const auto illegal = raw_illegal.text();
    AURORA_TEST_REQUIRE(bool(illegal));
    const std::string expected_illegal{'\xFF', 'X'};
    AURORA_TEST_CHECK_EQ(illegal.value(), expected_illegal);

    // Latin-1：0xE9（é）逐字节转码成 UTF-8 两字节序列。
    Headers latin;
    latin.set("Content-Type", "text/html; charset=iso-8859-1");
    const Response l1{200, "OK", latin, std::vector<char>{'\xE9'}};
    const auto l1_text = l1.text();
    AURORA_TEST_REQUIRE(bool(l1_text));
    AURORA_TEST_CHECK_EQ(l1_text.value(), "\xC3\xA9");

    // charset 令牌大小写不敏感、可带双引号、可省连字符：latin1 / "latin1" 都能识别。
    Headers quoted;
    quoted.set("Content-Type", "text/plain; charset=\"latin1\"");
    const Response q{200, "OK", quoted, std::vector<char>{'\xE9'}};
    const auto quoted_text = q.text();
    AURORA_TEST_REQUIRE(bool(quoted_text));
    AURORA_TEST_CHECK_EQ(quoted_text.value(), "\xC3\xA9");

    // 无 Content-Type：按 UTF-8 缺省。
    const Response no_ct{200, "OK", Headers{}, std::vector<char>{'A', 'B'}};
    const auto no_ct_text = no_ct.text();
    AURORA_TEST_REQUIRE(bool(no_ct_text));
    AURORA_TEST_CHECK_EQ(no_ct_text.value(), "AB");

    // 不支持的 charset：报 http-charset-unsupported，原始字节仍可经 body() 取回。
    Headers gbk;
    gbk.set("Content-Type", "text/plain; charset=gbk");
    const Response chinese{200, "OK", gbk, std::vector<char>{'\xC4', '\xE3'}};
    const auto failed = chinese.text();
    AURORA_TEST_REQUIRE_FALSE(bool(failed));
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, aurora::ErrorCode::HttpCharsetUnsupported);
    AURORA_TEST_CHECK_EQ(chinese.body().size(), std::size_t{2});
}

AURORA_TEST_CASE(byte_range_three_forms_convert_to_their_wire_texts) {
    // 三型的字段值文本各一条，逐字节定案（工厂拼错偏移会在这一例就现形）。
    const auto open = ByteRange::from(500);
    AURORA_TEST_CHECK_TRUE(open.is_valid());
    AURORA_TEST_CHECK_EQ(open.to_header_value(), "bytes=500-");
    const auto closed = ByteRange::between(0, 499);
    AURORA_TEST_CHECK_TRUE(closed.is_valid());
    AURORA_TEST_CHECK_EQ(closed.to_header_value(), "bytes=0-499");
    const auto tail = ByteRange::suffix(30);
    AURORA_TEST_CHECK_TRUE(tail.is_valid());
    AURORA_TEST_CHECK_EQ(tail.to_header_value(), "bytes=-30");
    // 后缀形态把长度存在 last 里：唯一一处字段名与语义不同名的地方，由换算承担。
    AURORA_TEST_CHECK_TRUE(!tail.first.has_value());
    AURORA_TEST_CHECK_EQ(tail.last.value_or(0), std::uint64_t{30});

    // 三种非法形态：两端皆空、终点在起点之前、零长后缀。
    const ByteRange nothing{};
    AURORA_TEST_CHECK_TRUE(!nothing.is_valid());
    AURORA_TEST_CHECK_EQ(nothing.to_header_value(), "");
    const auto reversed = ByteRange::between(900, 800);
    AURORA_TEST_CHECK_TRUE(!reversed.is_valid());
    const auto empty_tail = ByteRange::suffix(0);
    AURORA_TEST_CHECK_TRUE(!empty_tail.is_valid());
    AURORA_TEST_CHECK_EQ(empty_tail.to_header_value(), "");

    // 越界一律不裁剪：那是对端用 416 说话的事，本层不替它改数值。
    const auto far = ByteRange::from(std::numeric_limits<std::uint64_t>::max());
    AURORA_TEST_CHECK_TRUE(far.is_valid());
    AURORA_TEST_CHECK_EQ(far.to_header_value(), "bytes=18446744073709551615-");
}

AURORA_TEST_CASE(render_head_fills_range_and_if_range_from_the_knobs) {
    const auto segment = Request::get("https://example.com/part").range(ByteRange::between(0, 499));
    const auto text = head_text(segment);
    AURORA_TEST_CHECK_TRUE(contains(text, "Range: bytes=0-499"));
    // 三枚旋钮都不进字段表：读 headers() 不该以为这次没分段。
    AURORA_TEST_CHECK_TRUE(!segment.headers().contains("Range"));

    // `If-Range` 是分段请求的附属：只设它时不上线，与 `range()` 同设时紧随其后。
    const auto alone = Request::get("https://example.com/part").if_range("\"v1\"");
    AURORA_TEST_CHECK_TRUE(!contains(head_text(alone), "If-Range"));
    const auto conditioned = Request::get("https://example.com/part").range(ByteRange::from(1000)).if_range("\"v1\"");
    const auto conditioned_text = head_text(conditioned);
    AURORA_TEST_CHECK_TRUE(contains(conditioned_text, "Range: bytes=1000-"));
    AURORA_TEST_CHECK_TRUE(contains(conditioned_text, "If-Range: \"v1\""));
    // 空串与未设同义。
    AURORA_TEST_CHECK_TRUE(!Request::get("https://example.com/").if_range("").if_range().has_value());

    // 非法形态在起手就拒：一句要上线的话不带着它去问对端。
    const auto malformed = Request::get("https://example.com/").range(ByteRange::between(900, 800));
    AURORA_TEST_CHECK_EQ(render_error_code(malformed), aurora::ErrorCode::HttpRequestInvalid);
}

AURORA_TEST_CASE(streaming_body_switches_the_declared_framing) {
    // 总长未知的源 → 分帧声明；已知 → 确切长度。判定与体泵同源（detail::plan_request_body），故这里说什么
    // 线上就写什么。
    ScriptedSource unknown{std::vector<std::string>{"ab", "cd"}, std::nullopt, true};
    const auto chunked = Request::post("https://example.com/upload").body(unknown);
    const auto chunked_text = head_text(chunked);
    AURORA_TEST_CHECK_TRUE(contains(chunked_text, "Transfer-Encoding: chunked"));
    AURORA_TEST_CHECK_TRUE(!contains(chunked_text, "Content-Length"));

    ScriptedSource known{std::vector<std::string>{"abc"}, std::uint64_t{3}, true};
    const auto measured = Request::post("https://example.com/upload").body(known);
    AURORA_TEST_CHECK_TRUE(contains(head_text(measured), "Content-Length: 3"));
    AURORA_TEST_CHECK_TRUE(!contains(head_text(measured), "Transfer-Encoding"));

    // 调用方自报的长度优先：流式体 + 已声明长度是「我知道多大但不想整份进内存」的正当写法。
    const auto declared =
        Request::post("https://example.com/upload").body(unknown).set_header("Content-Length", "1048576");
    const auto declared_text = head_text(declared);
    AURORA_TEST_CHECK_TRUE(contains(declared_text, "Content-Length: 1048576"));
    AURORA_TEST_CHECK_TRUE(!contains(declared_text, "Transfer-Encoding"));
}

AURORA_TEST_CASE(body_source_and_memory_body_are_exclusive) {
    ScriptedSource source{std::vector<std::string>{"xy"}, std::nullopt, false};

    // 后设者生效，且前者被清掉——同时挂着会让形态判定有两个输入。
    auto swapped = Request::post("https://example.com/").body(std::vector<char>{'a', 'b', 'c'});
    AURORA_TEST_CHECK_EQ(swapped.body().size(), std::size_t{3});
    swapped.body(source);
    AURORA_TEST_CHECK_TRUE(swapped.body().empty());
    AURORA_TEST_CHECK_TRUE(swapped.body_source() == &source);
    swapped.body(std::vector<char>{'z'});
    AURORA_TEST_CHECK_TRUE(swapped.body_source() == nullptr);
    AURORA_TEST_CHECK_EQ(swapped.body().size(), std::size_t{1});

    // `HEAD` 两种交法都忽略：请求不得带体。
    const auto head = Request::head("https://example.com/").body(std::vector<char>{'q'}).body(source);
    AURORA_TEST_CHECK_TRUE(head.body().empty());
    AURORA_TEST_CHECK_TRUE(head.body_source() == nullptr);
    AURORA_TEST_CHECK_TRUE(!contains(head_text(head), "Content-Length"));
    AURORA_TEST_CHECK_TRUE(!contains(head_text(head), "Transfer-Encoding"));
}

AURORA_TEST_CASE(conflicting_body_declarations_are_refused_at_render) {
    // 两枚字段并存：线上只有一句该作数，本库不替调用方挑。
    const auto both = Request::post("https://example.com/")
                          .body(std::vector<char>{'a'})
                          .set_header("Content-Length", "1")
                          .set_header("Transfer-Encoding", "chunked");
    AURORA_TEST_CHECK_EQ(render_error_code(both), aurora::ErrorCode::HttpRequestInvalid);

    // 本库只按 chunked 发请求体：其余传输编码写上线就等于声明了一段没人负责编码的字节。
    const auto gzip =
        Request::post("https://example.com/").body(std::vector<char>{'a'}).set_header("Transfer-Encoding", "gzip");
    AURORA_TEST_CHECK_EQ(render_error_code(gzip), aurora::ErrorCode::HttpRequestInvalid);

    // 长度值非纯十进制（含带符号与空值）同样起手即拒。
    const auto bogus =
        Request::post("https://example.com/").body(std::vector<char>{'a'}).set_header("Content-Length", "12x");
    AURORA_TEST_CHECK_EQ(render_error_code(bogus), aurora::ErrorCode::HttpRequestInvalid);
}

AURORA_TEST_CASE(expect_continue_waits_only_when_a_body_is_waiting) {
    // 有体才问：无体的请求设了也不写 `Expect`（问了也没有待发的体）。
    const auto without_body = Request::get("https://example.com/").expect_continue(true);
    AURORA_TEST_CHECK_TRUE(without_body.expect_continue());
    AURORA_TEST_CHECK_TRUE(!contains(head_text(without_body), "Expect"));

    const auto with_body = Request::post("https://example.com/").body(std::vector<char>{'a'}).expect_continue(true);
    AURORA_TEST_CHECK_TRUE(contains(head_text(with_body), "Expect: 100-continue"));

    // 缺省关：不设时逐字节与「本功能从未存在」同型。
    AURORA_TEST_CHECK_TRUE(!Request::post("https://example.com/").body(std::vector<char>{'a'}).expect_continue());
    AURORA_TEST_CHECK_TRUE(
        !contains(head_text(Request::post("https://example.com/").body(std::vector<char>{'a'})), "Expect"));

    // 调用方自己写过的 `Expect` 永远优先（与本件的其余补齐同一通例）：线上一句，不是两句。
    const auto declared = Request::post("https://example.com/")
                              .body(std::vector<char>{'a'})
                              .set_header("Expect", "100-continue")
                              .expect_continue(true);
    AURORA_TEST_CHECK_EQ(declared.headers().values("Expect").size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(declared.headers().get("Expect").value_or(""), "100-continue");
    AURORA_TEST_CHECK_EQ(count_of(head_text(declared), "Expect: 100-continue"), std::size_t{1});
}

AURORA_TEST_CASE(zero_length_memory_body_writes_no_framing_field) {
    // 「没有体」与「体长为零」在线上等价，后者不必多出一句声明。
    const auto empty = Request::post("https://example.com/").body(std::vector<char>{});
    AURORA_TEST_CHECK_TRUE(!contains(head_text(empty), "Content-Length"));
    AURORA_TEST_CHECK_TRUE(!contains(head_text(empty), "Transfer-Encoding"));
    // 零内存体算不上「有待发的体」，故 100-continue 也不上线。
    const auto asked = Request::post("https://example.com/").body(std::vector<char>{}).expect_continue(true);
    AURORA_TEST_CHECK_TRUE(!contains(head_text(asked), "Expect"));
}

AURORA_TEST_CASE(content_range_parses_the_three_forms) {
    // 三种合法形态：闭合区间带总长、区间带未知总长、范围未知只有总长。
    Headers partial;
    partial.set("Content-Range", "bytes 0-499/1234");
    const Response first{206, "Partial Content", partial, std::vector<char>{'a'}};
    const auto parsed = require_value(first.content_range());
    AURORA_TEST_CHECK_EQ(parsed.first.value_or(0), std::uint64_t{0});
    AURORA_TEST_CHECK_EQ(parsed.last.value_or(0), std::uint64_t{499});
    AURORA_TEST_CHECK_EQ(parsed.total.value_or(0), std::uint64_t{1234});
    AURORA_TEST_CHECK_TRUE(first.is_partial());

    Headers unknown_total;
    unknown_total.set("Content-Range", "bytes 500-1234/*");
    const Response second{206, "Partial Content", unknown_total, {}};
    const auto second_range = require_value(second.content_range());
    AURORA_TEST_CHECK_EQ(second_range.first.value_or(0), std::uint64_t{500});
    AURORA_TEST_CHECK_TRUE(!second_range.total.has_value());

    Headers incomplete;
    incomplete.set("Content-Range", "bytes */1234");
    const Response third{416, "Range Not Satisfiable", incomplete, {}};
    const auto third_range = require_value(third.content_range());
    AURORA_TEST_CHECK_TRUE(!third_range.first.has_value());
    AURORA_TEST_CHECK_TRUE(!third_range.last.has_value());
    AURORA_TEST_CHECK_EQ(third_range.total.value_or(0), std::uint64_t{1234});
    // 416 说了「满足不了」而不是「这是一段」：`is_partial()` 只认 206。
    AURORA_TEST_CHECK_TRUE(!third.is_partial());

    // 解不出的一律回空而不报错：坏字段附着的仍是一份要交出的应答。
    for (const std::string_view bogus :
         {std::string_view{"bytes 0-499"}, std::string_view{"items 0-1/2"}, std::string_view{"bytes abc-def/12"},
          std::string_view{"bytes 0-499/xyz"}, std::string_view{""}}) {
        Headers headers;
        headers.set("Content-Range", bogus);
        const Response response{206, "Partial Content", headers, {}};
        AURORA_TEST_CHECK_TRUE(!bool(response.content_range()));
        AURORA_TEST_CHECK_TRUE(response.is_partial());  // 判定只看状态码，不被字段的可解性牵着走
    }

    // 无字段即无范围，且 200 恒不部分。
    const Response whole{200, "OK", Headers{}, std::vector<char>{'a'}};
    AURORA_TEST_CHECK_TRUE(!whole.content_range().has_value());
    AURORA_TEST_CHECK_TRUE(!whole.is_partial());
}

AURORA_TEST_CASE(request_timeouts_have_three_stages) {
    const auto zero = std::chrono::milliseconds{0};

    // 旧 timeout() 只写 total 档，connect / read_idle 保持不设。
    const auto legacy = Request::get("https://example.com/").timeout(std::chrono::milliseconds{1500});
    AURORA_TEST_CHECK_TRUE(legacy.timeout().has_value());
    AURORA_TEST_CHECK_EQ(legacy.timeout().value_or(zero).count(), std::int64_t{1500});
    AURORA_TEST_CHECK_EQ(legacy.timeouts().total.value_or(zero).count(), std::int64_t{1500});
    AURORA_TEST_CHECK_TRUE(!legacy.timeouts().connect.has_value());
    AURORA_TEST_CHECK_TRUE(!legacy.timeouts().read_idle.has_value());

    // timeouts() 三档独立；total 仍可从旧 timeout() 读到（向后兼容）。
    const auto staged = Request::get("https://example.com/")
                            .timeouts(Timeouts{
                                .connect = std::chrono::milliseconds{500},
                                .read_idle = std::chrono::milliseconds{800},
                                .total = std::chrono::milliseconds{3000},
                            });
    AURORA_TEST_CHECK_EQ(staged.timeouts().connect.value_or(zero).count(), std::int64_t{500});
    AURORA_TEST_CHECK_EQ(staged.timeouts().read_idle.value_or(zero).count(), std::int64_t{800});
    AURORA_TEST_CHECK_EQ(staged.timeouts().total.value_or(zero).count(), std::int64_t{3000});
    AURORA_TEST_CHECK_EQ(staged.timeout().value_or(zero).count(), std::int64_t{3000});

    // 缺省三档全不设。
    const auto bare = Request::get("https://example.com/");
    AURORA_TEST_CHECK_TRUE(!bare.timeouts().connect.has_value());
    AURORA_TEST_CHECK_TRUE(!bare.timeouts().read_idle.has_value());
    AURORA_TEST_CHECK_TRUE(!bare.timeouts().total.has_value());
}

AURORA_TEST_CASE(request_retry_policy_slot_follows_or_overrides_the_client) {
    const auto zero = std::chrono::milliseconds{0};

    // 槽位只回答「本请求要不要覆盖 `Client` 的缺省」：不设即 `nullopt`，两级合成发生在客户端起手时。
    const auto follow = Request::get("https://example.com/");
    AURORA_TEST_CHECK_TRUE(!follow.retry_policy().has_value());

    // 「这一条不许重试」= 设一份 `attempts` 为缺省值的策略，而不是传一枚空值——空值的意思是「跟随缺省」，
    // 而缺省可能正是开着退避的客户端。关掉与没想过必须可区分，否则「按请求关重试」无从表达。
    const auto off = Request::get("https://example.com/").retry_policy(RetryPolicy{});
    AURORA_TEST_CHECK_EQ(require_value(off.retry_policy()).attempts, std::uint32_t{1});

    // 逐字段回读：五枚各有其形，`budget` 不设仍是「没有墙」而不是「零时长墙」（非正值与不设同义）。
    const auto full = Request::get("https://example.com/")
                          .retry_policy(RetryPolicy{
                              .attempts = 5,
                              .base = std::chrono::milliseconds{30},
                              .max_backoff = std::chrono::milliseconds{900},
                              .jitter = 0.5,
                              .budget = std::chrono::milliseconds{2000},
                          });
    const RetryPolicy policy = require_value(full.retry_policy());
    AURORA_TEST_CHECK_EQ(policy.attempts, std::uint32_t{5});
    AURORA_TEST_CHECK_EQ(policy.base.count(), std::int64_t{30});
    AURORA_TEST_CHECK_EQ(policy.max_backoff.count(), std::int64_t{900});
    AURORA_TEST_CHECK_NEAR(policy.jitter, 0.5, 1e-12);
    AURORA_TEST_CHECK_EQ(policy.budget.value_or(zero).count(), std::int64_t{2000});

    // 再设一次是整份替换而非逐字段合并：`budget` 由有值回到不设，本请求的墙随之消失。
    auto replaced =
        Request::get("https://example.com/").retry_policy(RetryPolicy{.budget = std::chrono::milliseconds{2000}});
    replaced.retry_policy(RetryPolicy{.attempts = 2});
    const RetryPolicy current = require_value(replaced.retry_policy());
    AURORA_TEST_CHECK_EQ(current.attempts, std::uint32_t{2});
    AURORA_TEST_CHECK_TRUE(!current.budget.has_value());
}

AURORA_TEST_CASE(error_class_maps_every_net_and_http_code) {
    // 正向：期望表里逐码比对实现给出的档。
    for (const auto &entry : AURORA_HTTP_ERROR_CLASS_TABLE) {
        AURORA_TEST_CHECK_EQ(error_class_of(entry.code), entry.expected);
    }

    // 完整性：生成表里每一枚 `net-*` / `http-*` 都必须在期望表里被点名。新增一枚网络码若忘了归档，
    // 它会静默落进兜底档、让「该不该给重试按钮」的判定悄悄变形——这条断言把那种漂移挡在测试里。
    std::size_t seen = 0;
    for (const auto &meta : AURORA_ERROR_TABLE) {
        if (!is_network_slug(meta.slug)) {
            continue;
        }
        ++seen;
        const auto *expected = expected_class_of(meta.code);
        AURORA_TEST_REQUIRE_MSG(expected != nullptr, std::string{meta.slug} + " is an unclassified network code");
        AURORA_TEST_CHECK_MSG(error_class_of(meta.code) == *expected,
                              std::string{"class drift for: "} + std::string{meta.slug});
    }
    AURORA_TEST_CHECK_EQ(seen, AURORA_HTTP_ERROR_CLASS_TABLE.size());
    AURORA_TEST_CHECK_EQ(seen, std::size_t{43});

    // `Status` 档没有码可映射（非 2xx 是成功 Result，状态码是数据不是错误），它的唯一产出点是下面那枚
    // 按状态码折算的函数——本断言把「码表里不该出现 Status」钉成一条事实，免得日后误加。
    for (const auto &entry : AURORA_HTTP_ERROR_CLASS_TABLE) {
        AURORA_TEST_CHECK_TRUE(entry.expected != HttpErrorClass::Status);
    }
}

AURORA_TEST_CASE(error_class_of_a_non_network_code_falls_back_to_invalid) {
    // 分型是观测口而非校验口：不属于网络交换的码照样给出兜底档，调用方无需先判「这枚码该不该问」。
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::GeneralUnknown), HttpErrorClass::Invalid);
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::LayoutDepthExceeded), HttpErrorClass::Invalid);
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::WidgetUnknownType), HttpErrorClass::Invalid);
}

AURORA_TEST_CASE(error_class_is_orthogonal_to_retryable) {
    // 同一档里可重试与不可重试并存是刻意的：档位回答「往哪儿排查」，`retryable` 回答「要不要再来一次」。
    // 若两者折成一枚，`net-send-failed`（链路断了但重发无益）与 `net-connect-failed`（换一次就可能通）
    // 就得各占一档，档位数量会被重试口径牵着走。
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::NetConnectFailed), HttpErrorClass::Unreachable);
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::NetSendFailed), HttpErrorClass::Unreachable);
    AURORA_TEST_CHECK_TRUE(aurora::retryable(aurora::ErrorCode::NetConnectFailed));
    AURORA_TEST_CHECK_FALSE(aurora::retryable(aurora::ErrorCode::NetSendFailed));

    // 反方向同样成立：同一枚 `retryable` 值散落在不同档里。
    AURORA_TEST_CHECK_TRUE(aurora::retryable(aurora::ErrorCode::NetDnsFailed));
    AURORA_TEST_CHECK_TRUE(aurora::retryable(aurora::ErrorCode::NetProxyConfigUnreadable));
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::NetDnsFailed), HttpErrorClass::Dns);
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::NetProxyConfigUnreadable), HttpErrorClass::Proxy);

    // 「读不到系统代理」与「代理配置写错了」的排查方向相反：前者问环境，后者改调用点。
    AURORA_TEST_CHECK_EQ(error_class_of(aurora::ErrorCode::NetProxyConfigInvalid), HttpErrorClass::Invalid);
}

AURORA_TEST_CASE(error_class_of_status_folds_only_4xx_and_5xx) {
    for (const std::uint16_t status :
         {std::uint16_t{400}, std::uint16_t{404}, std::uint16_t{429}, std::uint16_t{500}, std::uint16_t{503}}) {
        AURORA_TEST_CHECK_EQ(error_class_of_status(status), HttpErrorClass::Status);
    }
    // 1xx / 2xx / 3xx 没有「状态码即失败」这回事：兜底档在此的含义退化为「无可归因的状态码类失败」。
    for (const std::uint16_t status :
         {std::uint16_t{100}, std::uint16_t{200}, std::uint16_t{204}, std::uint16_t{302}, std::uint16_t{399}}) {
        AURORA_TEST_CHECK_EQ(error_class_of_status(status), HttpErrorClass::Invalid);
    }
    // 边界钉在 400：399 与 400 之间不许有第三种解释。
    AURORA_TEST_CHECK_EQ(error_class_of_status(std::uint16_t{399}), HttpErrorClass::Invalid);
    AURORA_TEST_CHECK_EQ(error_class_of_status(std::uint16_t{400}), HttpErrorClass::Status);
}

AURORA_TEST_CASE(error_class_text_is_a_stable_lowercase_token) {
    constexpr std::array<HttpErrorClass, 9> all_classes{
        HttpErrorClass::Invalid,  HttpErrorClass::Dns,    HttpErrorClass::Unreachable,
        HttpErrorClass::Proxy,    HttpErrorClass::Tls,    HttpErrorClass::Timeout,
        HttpErrorClass::Protocol, HttpErrorClass::Status, HttpErrorClass::Cancelled,
    };
    constexpr std::array<std::string_view, 9> expected_texts{"invalid", "dns",      "unreachable", "proxy",    "tls",
                                                             "timeout", "protocol", "status",      "cancelled"};

    for (std::size_t index = 0; index < all_classes.size(); ++index) {
        const auto text = error_class_text(all_classes.at(index));
        AURORA_TEST_CHECK_EQ(text, expected_texts.at(index));
        AURORA_TEST_CHECK_TRUE(!text.empty());
        // 令牌恒小写下划线族：日志与 Inspector 只认这枚文本，大小写漂了就成两条真值。
        for (const char c : text) {
            AURORA_TEST_CHECK_TRUE(c >= 'a' && c <= 'z');
        }
        // 九档令牌两两不同（可枚举性：进程外消费者靠它区分档位）。
        for (std::size_t other = 0; other < all_classes.size(); ++other) {
            if (other != index) {
                AURORA_TEST_CHECK_TRUE(text != expected_texts.at(other));
            }
        }
    }
    // 兜底档是枚举的零值：默认构造出来的档位与 `error_class_of` 的兜底口径同为 `Invalid`。
    AURORA_TEST_CHECK_EQ(static_cast<int>(HttpErrorClass{}), static_cast<int>(HttpErrorClass::Invalid));
    AURORA_TEST_CHECK_EQ(error_class_text(HttpErrorClass{}), "invalid");
}

}  // namespace aurora::test_cases::utest_net_http

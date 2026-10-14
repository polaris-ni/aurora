/// 测试类型: unit
/// 目标单元: include/aurora/net/http.h + src/aurora/net/http.cpp
/// 测试说明: HTTP 发起侧值类型的逐值回归（CHG-010 §6 / §12 的请求与应答对象）。覆盖方法枚举与线上
///           文本的双向换算（往返一致、小写与未知拼写判 `http-request-invalid`）；工厂对目标 URL 的
///           当场解析与「解析失败延后到 render_head 才报错」的口径；请求序列化的两处补齐（有体且未声明
///           `Content-Length` / `Transfer-Encoding` 时按体长度补、缺 `Host` 由字节层按目标补）与 HEAD
///           请求丢弃消息体；链式字段追加保序、`set_header` 清同名后追加；应答值类型的状态码 / 原因 /
///           大小写不敏感取值 / 原始字节与文本形态。
/// 全用例零 socket、零 DNS、不读环境变量，故三平台行为一致、wasm 腿同样可跑，无需 `AURORA_TEST_SKIP`。

#include <chrono>
#include <cstdint>
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

using aurora::net::http::Method;
using aurora::net::http::Request;
using aurora::net::http::Response;

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
    AURORA_TEST_CHECK_TRUE(request.timeout().has_value());
    AURORA_TEST_CHECK_EQ(request.timeout()->count(), std::int64_t{2500});
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
    AURORA_TEST_CHECK_EQ(response.text(), "AB");
}

}  // namespace aurora::test_cases::utest_net_http

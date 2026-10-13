/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/proxy_wire.h + src/aurora/net/detail/proxy_wire.cpp
/// 测试说明: 代理握手字节层的逐值回归（CHG-010 §11 代理族的握手部分）。覆盖 CONNECT 请求行的
///           `authority-form`（IPv6 补方括号）、`Proxy-Authorization: Basic` 的 base64 形态与
///           「无用户名即不发该头」，应答头部块解析的三态分岔（还没读够 / 完整 / 不合文法或超
///           上限）、同名头保序不折叠、CRLF 与裸 LF 两型换行；SOCKS5 侧覆盖方法协商请求（有凭据
///           才多 offer 方法 2）、子协商的 255 字节上限、`ATYPE` 由「域名是否交给代理解析」与目标
///           形态共同决定（非 `h` 形态给名字即拒、带作用域号即拒）、变长应答的按需读取与绑定端点
///           还原，以及状态字节 → 错误码的分岔口径（407 与方法 / 子协商的认证失败落
///           `net-proxy-auth-required`、形态不支持落 `net-proxy-unsupported`、其余隧道失败落
///           `net-proxy-unreachable`、报文不合文法落 `net-proxy-protocol`）。
/// 全用例零 socket、零 DNS、不读环境变量，故三平台行为一致、wasm 腿同样可跑，无需 `AURORA_TEST_SKIP`。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/detail/proxy_wire.h"
#include "aurora/net/proxy.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_proxy_wire {
namespace {

using aurora::net::Endpoint;
using aurora::net::ProxyCredentials;
using aurora::testing::require_value;
namespace wire = aurora::net::wire;

/// @brief 把字节写成空格分隔的大写十六进制串，失败时能一眼看出差在哪一字节。
/// @param bytes 待渲染字节。
/// @return 十六进制文本。
auto hex(const std::vector<std::uint8_t> &bytes) -> std::string {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string out;
    for (const auto byte : bytes) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.push_back(digits.at(byte >> 4U));
        out.push_back(digits.at(byte & 0x0FU));
    }
    return out;
}

/// @brief 把字节向量变成文本，喂给只收 `string_view` 的解析入口（含零字节也原样保留）。
/// @param bytes 握手中读到的字节。
/// @return 等长文本，不做任何转码。
auto raw(std::vector<std::uint8_t> bytes) -> std::string {
    return std::string{bytes.begin(), bytes.end()};
}

/// @brief 取三态解析产出的报文：还没读够与畸形是两回事，此处缺失即致命失败。
/// @param parsed 解析结果（`Result<optional<T>>`）。
/// @return 完整解析出的报文副本。
template <typename T>
[[nodiscard]] auto completed(const Result<std::optional<T>> &parsed) -> T {
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    return require_value(parsed.value());
}

/// @brief 取出已判定成功的结果值（调用点已确认成功，此处只避免重复解引用）。
/// @param result 待取出的结果。
/// @return 成功值的副本。
template <typename T>
[[nodiscard]] auto unwrapped(const Result<T> &result) -> T {
    AURORA_TEST_REQUIRE(bool(result));
    return result.value();
}

/// @brief 把应答里的绑定端点渲染成文本：文本即含地址族与端口，比逐字段断言更省一眼。
/// @param reply 已完整解析的 SOCKS5 应答。
/// @return 端点的规范化文本；域名型回报不还原成端点，此处给 `<missing>`。
[[nodiscard]] auto bound_text(const wire::Socks5Reply &reply) -> std::string {
    if (!reply.bound.has_value()) {
        return "<missing>";
    }
    return reply.bound->to_string();
}

}  // namespace

AURORA_TEST_CASE(connect_request_uses_authority_form_and_basic_credentials) {
    const auto plain = wire::make_connect_request("example.com", 8080, ProxyCredentials{});
    AURORA_TEST_CHECK_EQ(plain, "CONNECT example.com:8080 HTTP/1.1\r\nHost: example.com:8080\r\n\r\n");

    const auto bracketed = wire::make_connect_request("::1", 443, ProxyCredentials{});
    AURORA_TEST_CHECK_EQ(bracketed, "CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]:443\r\n\r\n");

    const ProxyCredentials credentials{.username = "bob", .password = "s3cret"};
    const auto authenticated = wire::make_connect_request("example.com", 8080, credentials);
    // 期望的 base64 取自 RFC 4648 §10 的编码规则，独立于本实现手算，不是「跑一遍抄下来」。
    AURORA_TEST_CHECK_EQ(authenticated,
                         "CONNECT example.com:8080 HTTP/1.1\r\nHost: example.com:8080\r\n"
                         "Proxy-Authorization: Basic Ym9iOnMzY3JldA==\r\n\r\n");
    AURORA_TEST_CHECK_TRUE(authenticated.find("s3cret") == std::string::npos);

    // 只有密码没有用户名即「没有凭据」：宁可少发一条头，也不发一条必然被拒的空账号 Basic。
    const ProxyCredentials half_empty{.username = "", .password = "orphan"};
    AURORA_TEST_CHECK_EQ(wire::make_connect_request("example.com", 8080, half_empty), plain);
}

AURORA_TEST_CASE(connect_response_head_is_three_state) {
    const auto pending = wire::parse_connect_response("HTTP/1.1 200 OK\r\nX-Pro: 1\r\n");
    AURORA_TEST_REQUIRE(bool(pending));
    AURORA_TEST_CHECK_TRUE(!pending.value().has_value());

    const std::string_view complete =
        "HTTP/1.1 200 Connection established\r\nProxy-Agent: some proxy/1.0\r\nX-A: 1\r\nx-a: 2\r\n\r\ntunnel";
    const auto head = completed(wire::parse_connect_response(complete));
    AURORA_TEST_CHECK_EQ(head.status, 200);
    AURORA_TEST_CHECK_EQ(head.reason, "Connection established");
    // 同名头保序不折叠（与 core::Headers 同口径），值里的内部空格原样保留。
    AURORA_TEST_CHECK_EQ(head.headers.size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(head.headers.values("x-a").size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(head.headers.get("proxy-agent").value_or(""), "some proxy/1.0");
    // header_bytes 之后的字节属于隧道，不能被头部解析吞掉。
    AURORA_TEST_CHECK_EQ(head.header_bytes, complete.size() - std::string_view{"tunnel"}.size());
    AURORA_TEST_CHECK_EQ(complete.substr(head.header_bytes), "tunnel");

    // 裸 LF 换行同样判完整：这里只读不回写，为一条换行差异把可用代理判成畸形不值得。
    const auto lf_only = completed(wire::parse_connect_response("HTTP/1.0 204\n\n"));
    AURORA_TEST_CHECK_EQ(lf_only.status, 204);
    AURORA_TEST_CHECK_EQ(lf_only.header_bytes, std::size_t{14});
    AURORA_TEST_CHECK_TRUE(lf_only.headers.empty());

    // 缺原因短语（只有状态码）合文法：HTTP/1.0 代理与测试替身常这么写。
    AURORA_TEST_CHECK_EQ(completed(wire::parse_connect_response("HTTP/1.1 200\r\n\r\n")).status, 200);
}

AURORA_TEST_CASE(connect_response_rejects_non_http_malformed_and_oversized) {
    for (const std::string_view bad : {"SOCKS5 reply is not HTTP\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n",
                                       "HTTP/1.1 2000 OK\r\n\r\n", "HTTP 200 OK\r\n\r\n", "HTTP/1.1\r\n\r\n",
                                       "HTTP/1.1 200 OK\r\ngarbage\r\n\r\n", "HTTP/1.1 200 OK\r\nX A: 1\r\n\r\n",
                                       "HTTP/1.1 200 OK\r\n: no-name\r\n\r\n",
                                       "HTTP/1.1 200 OK\r\nX-A: 1\r\n  folded\r\n\r\n"}) {
        const auto rejected = wire::parse_connect_response(bad);
        AURORA_TEST_REQUIRE_FALSE(bool(rejected));
        AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::NetProxyProtocol);
    }

    // 预算只有 64 字节而空行远在其后：超上限按畸形处理，绝不让读缓冲随对端节奏无界增长。
    std::string bloated = "HTTP/1.1 200 OK\r\n";
    while (bloated.size() < 200) {
        bloated += "X-Padding: a great many bytes of nothing at all\r\n";
    }
    const auto capped = wire::parse_connect_response(bloated, 64);
    AURORA_TEST_REQUIRE_FALSE(bool(capped));
    AURORA_TEST_CHECK_EQ(capped.error().code_enum, ErrorCode::NetProxyProtocol);
    AURORA_TEST_CHECK_TRUE(capped.error().message.find("64") != std::string::npos);

    // 未超上限且确实没读到空行 → 仍是「还没读够」，不是错误。
    const auto still_pending = wire::parse_connect_response(bloated.substr(0, 40), 64);
    AURORA_TEST_REQUIRE(bool(still_pending));
    AURORA_TEST_CHECK_TRUE(!still_pending.value().has_value());
}

AURORA_TEST_CASE(socks5_greeting_and_subnegotiation_follow_the_rfc) {
    AURORA_TEST_CHECK_EQ(hex(wire::make_socks5_greeting(false)), "05 01 00");
    AURORA_TEST_CHECK_EQ(hex(wire::make_socks5_greeting(true)), "05 02 00 02");

    const auto pending_method = wire::parse_socks5_method("x");
    AURORA_TEST_REQUIRE(bool(pending_method));
    AURORA_TEST_CHECK_TRUE(!pending_method.value().has_value());
    AURORA_TEST_CHECK_EQ(completed(wire::parse_socks5_method(raw({0x05, 0x02}))), std::uint8_t{2});
    const auto wrong_version = wire::parse_socks5_method(raw({0x04, 0x00}));
    AURORA_TEST_REQUIRE_FALSE(bool(wrong_version));
    AURORA_TEST_CHECK_EQ(wrong_version.error().code_enum, ErrorCode::NetProxyProtocol);

    const ProxyCredentials credentials{.username = "u", .password = "pw"};
    AURORA_TEST_CHECK_EQ(hex(unwrapped(wire::make_socks5_auth_request(credentials))), "01 01 75 02 70 77");
    AURORA_TEST_CHECK_EQ(completed(wire::parse_socks5_auth(raw({0x01, 0x00}))), std::uint8_t{0});
    // 子协商回报的版本字段不携带信息，但文法只认 1：放宽会把「对端根本不是 SOCKS5」读成认证失败。
    const auto wrong_auth_version = wire::parse_socks5_auth(raw({0x00, 0x01}));
    AURORA_TEST_REQUIRE_FALSE(bool(wrong_auth_version));
    AURORA_TEST_CHECK_EQ(wrong_auth_version.error().code_enum, ErrorCode::NetProxyProtocol);

    for (const ProxyCredentials &bad : {ProxyCredentials{.username = "", .password = "x"},
                                        ProxyCredentials{.username = std::string(256, 'u'), .password = "x"},
                                        ProxyCredentials{.username = "u", .password = std::string(256, 'p')}}) {
        const auto rejected = wire::make_socks5_auth_request(bad);
        AURORA_TEST_REQUIRE_FALSE(bool(rejected));
        AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::NetProxyConfigInvalid);
    }
}

AURORA_TEST_CASE(socks5_connect_picks_atype_by_resolution_authority) {
    std::vector<std::uint8_t> expected_domain{0x05, 0x01, 0x00, 0x03, 0x0B};
    for (const auto character : std::string_view{"example.com"}) {
        expected_domain.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    expected_domain.insert(expected_domain.end(), {0x01, 0xBB});
    const auto domain = wire::make_socks5_connect("example.com", 443, true);
    AURORA_TEST_REQUIRE(bool(domain));
    AURORA_TEST_CHECK_EQ(hex(domain.value()), hex(expected_domain));

    const auto v4 = wire::make_socks5_connect("192.0.2.1", 8080, false);
    AURORA_TEST_REQUIRE(bool(v4));
    AURORA_TEST_CHECK_EQ(hex(v4.value()), "05 01 00 01 C0 00 02 01 1F 90");

    const auto v6 = wire::make_socks5_connect("2001:db8::1", 80, false);
    AURORA_TEST_REQUIRE(bool(v6));
    AURORA_TEST_CHECK_EQ(hex(v6.value()), "05 01 00 04 20 01 0D B8 00 00 00 00 00 00 00 00 00 00 00 01 00 50");

    // 未授权代理解析时给名字：本层不做 DNS，也不静默把名字塞进 ATYPE=3——那是行为差异，不是兼容。
    const auto named = wire::make_socks5_connect("example.com", 443, false);
    AURORA_TEST_REQUIRE_FALSE(bool(named));
    AURORA_TEST_CHECK_EQ(named.error().code_enum, ErrorCode::NetEndpointInvalid);
    AURORA_TEST_CHECK_TRUE(named.error().message.find("example.com") != std::string::npos);

    // 域名形态的长度前缀只有一个字节，256 字节的名字放不下。
    AURORA_TEST_CHECK_FALSE(bool(wire::make_socks5_connect(std::string(256, 'a'), 443, true)));

    // 作用域号在 SOCKS5 地址编码里没有字段可放：宁可报错，也不悄悄丢掉它连到别的链路上去。
    const auto scoped_text = Endpoint::from_ip("fe80::1%2", 443);
    AURORA_TEST_REQUIRE(bool(scoped_text));
    AURORA_TEST_CHECK_EQ(scoped_text.value().zone(), "2");
    const auto rejected_scope = wire::make_socks5_connect("fe80::1%2", 443, false);
    AURORA_TEST_REQUIRE_FALSE(bool(rejected_scope));
    AURORA_TEST_CHECK_EQ(rejected_scope.error().code_enum, ErrorCode::NetEndpointInvalid);
    AURORA_TEST_CHECK_TRUE(rejected_scope.error().message.find("scope id") != std::string::npos);
}

AURORA_TEST_CASE(socks5_reply_parsing_tracks_variable_address_length) {
    const auto pending = wire::parse_socks5_reply(raw({0x05, 0x00, 0x00, 0x01, 0xCB, 0x00}));
    AURORA_TEST_REQUIRE(bool(pending));
    AURORA_TEST_CHECK_TRUE(!pending.value().has_value());

    // 应答后面紧跟隧道首字节：解析只吃掉 10 字节，其余留给传输切片。
    const auto v4 = completed(wire::parse_socks5_reply(
        raw({0x05, 0x00, 0x00, 0x01, 203, 0, 113, 1, 0x1F, 0x90, 't', 'u'})));
    AURORA_TEST_CHECK_EQ(v4.status, 0);
    AURORA_TEST_CHECK_EQ(v4.consumed, std::size_t{10});
    AURORA_TEST_CHECK_EQ(bound_text(v4), "203.0.113.1:8080");

    const auto v6 = completed(wire::parse_socks5_reply(
        raw({0x05, 0x00, 0x00, 0x04, 0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x1F, 0x90})));
    AURORA_TEST_CHECK_EQ(v6.consumed, std::size_t{22});
    AURORA_TEST_CHECK_EQ(bound_text(v6), "[2001:db8::1]:8080");

    // 域名型绑定地址不还原成端点：那需要一个未经解析的假端点。
    const auto domain =
        completed(wire::parse_socks5_reply(raw({0x05, 0x00, 0x00, 0x03, 0x03, 'a', 'b', 'c', 0x1F, 0x90})));
    AURORA_TEST_CHECK_EQ(domain.consumed, std::size_t{10});
    AURORA_TEST_CHECK_TRUE(!domain.bound.has_value());
    // 长度前缀本身还没读到，仍是「还没读够」。
    const auto short_domain = wire::parse_socks5_reply(raw({0x05, 0x00, 0x00, 0x03}));
    AURORA_TEST_REQUIRE(bool(short_domain));
    AURORA_TEST_CHECK_TRUE(!short_domain.value().has_value());

    const auto unknown_atype = wire::parse_socks5_reply(raw({0x05, 0x00, 0x00, 0x09, 0x01, 0x02}));
    AURORA_TEST_REQUIRE_FALSE(bool(unknown_atype));
    AURORA_TEST_CHECK_EQ(unknown_atype.error().code_enum, ErrorCode::NetProxyProtocol);
    const auto wrong_version = wire::parse_socks5_reply(raw({0x04, 0x00, 0x00, 0x01, 1, 2, 3, 4, 0, 80}));
    AURORA_TEST_REQUIRE_FALSE(bool(wrong_version));
    AURORA_TEST_CHECK_EQ(wrong_version.error().code_enum, ErrorCode::NetProxyProtocol);

    // 非零 REP 照样解析成功：状态字节到错误码的换算是另一件事（见下面的分岔判据）。
    AURORA_TEST_CHECK_EQ(
        completed(wire::parse_socks5_reply(raw({0x05, 0x05, 0x00, 0x01, 0, 0, 0, 0, 0, 0}))).status, std::uint8_t{5});
}

AURORA_TEST_CASE(handshake_status_maps_to_the_documented_error_codes) {
    AURORA_TEST_CHECK_EQ(wire::connect_tunnel_error(407, "proxy.internal:3128", "a.example:443").code_enum,
                         ErrorCode::NetProxyAuthRequired);
    AURORA_TEST_CHECK_EQ(wire::connect_tunnel_error(502, "proxy.internal:3128", "a.example:443").code_enum,
                         ErrorCode::NetProxyUnreachable);
    // 隧道失败的诊断说清「经哪个代理连哪个目标」，且永不含凭据。
    const auto refused = wire::connect_tunnel_error(403, "proxy.internal:3128", "a.example:443");
    AURORA_TEST_CHECK_EQ(refused.code_enum, ErrorCode::NetProxyUnreachable);
    AURORA_TEST_CHECK_TRUE(refused.message.find("proxy.internal:3128") != std::string::npos);
    AURORA_TEST_CHECK_TRUE(refused.message.find("a.example:443") != std::string::npos);

    AURORA_TEST_CHECK_EQ(wire::socks5_error_for(0xFF, wire::Socks5Phase::MethodSelection, "p:1080").code_enum,
                         ErrorCode::NetProxyAuthRequired);
    AURORA_TEST_CHECK_EQ(wire::socks5_error_for(0x03, wire::Socks5Phase::MethodSelection, "p:1080").code_enum,
                         ErrorCode::NetProxyUnsupported);
    AURORA_TEST_CHECK_EQ(wire::socks5_error_for(0x01, wire::Socks5Phase::Authentication, "p:1080").code_enum,
                         ErrorCode::NetProxyAuthRequired);
    AURORA_TEST_CHECK_EQ(wire::socks5_error_for(0x08, wire::Socks5Phase::Tunnel, "p:1080").code_enum,
                         ErrorCode::NetProxyUnsupported);
    AURORA_TEST_CHECK_EQ(wire::socks5_error_for(0x09, wire::Socks5Phase::Tunnel, "p:1080").code_enum,
                         ErrorCode::NetProxyUnsupported);
    // 其余 REP 一律可达性失败：本库不静默回退直连，调用方据此报错即可。
    for (const std::uint8_t rep : {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}) {
        AURORA_TEST_CHECK_EQ(wire::socks5_error_for(rep, wire::Socks5Phase::Tunnel, "p:1080").code_enum,
                             ErrorCode::NetProxyUnreachable);
    }
}

}  // namespace aurora::test_cases::utest_net_proxy_wire

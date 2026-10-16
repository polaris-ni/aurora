/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_auth_wire.h + src/aurora/net/detail/http_auth_wire.cpp
/// 测试说明: 认证字段文法层与凭据编码的逐值回归（CHG-018 §12 认证切片的解析半边）。覆盖方案名归一
///           （`Basic` / `Bearer` 认档位、其余归 `Unsupported` 且原文拼写留在 `name`）、`realm` 单列成字段
///           且不重复进参数表（同名多次只取首个）、其余参数的键 ASCII 小写而值保留原文、quoted-string 里
///           的逗号不当分隔符、反斜杠转义（`\"` 与 `\\`，以及跟在反斜杠后的任意字符）、裸 token 参数、
///           参数表半途断裂只丢尾部残段而保留已解析部分、引号没闭合判整条不可解析、没有方案令牌判不可
///           解析、超过 8 KiB 上限判不可解析（含恰在上限一侧仍要解析）、多条字段值按线上次序各成一枚方案
///           且不可解析的那条只被丢弃。base64 侧验 `RFC 4648` §4 的全组填充向量、UTF-8 多字节逐字节编码，
///           以及内嵌 NUL 与高位字节不被当成终止。
/// 全用例零 socket、零真实域控，故三平台行为一致、禁网 CI 无需豁免。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/net/detail/http_auth_wire.h"
#include "aurora/net/http_auth.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_auth_wire {
namespace {

using aurora::net::detail::AURORA_AUTH_FIELD_MAX_BYTES;
using aurora::net::detail::base64_encode;
using aurora::net::detail::parse_auth_mechanism;
using aurora::net::detail::parse_auth_mechanisms;
using aurora::net::http::AuthMechanism;
using aurora::net::http::AuthScheme;
using aurora::testing::require_value;

/// @brief 把参数表压成 `键=值` 序列（按登记次序），失败时能一眼看出差在哪一枚。
/// @param mechanism 解析出的方案。
/// @return 逗号分隔的文本。
auto parameter_text(const AuthMechanism &mechanism) -> std::string {
    std::string out;
    for (const auto &[name, value] : mechanism.parameters) {
        if (!out.empty()) {
            out += ',';
        }
        out += name;
        out += '=';
        out += value;
    }
    return out;
}

/// @brief 取解析成功的方案；没解析出来即致命失败，后续断言不在空值上空跑。
/// @param value 一条字段值的原文。
/// @return 解析产物。
auto parsed(std::string_view value) -> AuthMechanism {
    const auto result = parse_auth_mechanism(value);
    // 消息留在这一句（`require_value` 只会报「没持有值」，看不出是哪条原文没解析出来）；取值交给它——
    // `AURORA_TEST_REQUIRE` 的宏展开对路径分析不透明，紧随其后的 `value()` 会被误报成未检查访问。
    AURORA_TEST_REQUIRE_MSG(result.has_value(), "the challenge is not parsable: " + std::string{value});
    return require_value(result);
}

}  // namespace

AURORA_TEST_CASE(scheme_name_is_folded_for_the_slot_and_kept_verbatim_in_the_name) {
    const auto plain = parsed(R"(Basic realm="api")");
    AURORA_TEST_CHECK(plain.scheme == AuthScheme::Basic);
    AURORA_TEST_CHECK_EQ(plain.name, std::string{"Basic"});
    // 档位判定折叠大小写，`name` 不折叠：只归一档位就把「对端到底点名了什么」丢了，而未实现方案的自定义
    // 实现方需要的恰是那一行原文。
    const auto shouted = parsed("BASIC realm=api");
    AURORA_TEST_CHECK(shouted.scheme == AuthScheme::Basic);
    AURORA_TEST_CHECK_EQ(shouted.name, std::string{"BASIC"});
    const auto unknown = parsed("Negotiate");
    AURORA_TEST_CHECK(unknown.scheme == AuthScheme::Unsupported);
    AURORA_TEST_CHECK_EQ(unknown.name, std::string{"Negotiate"});
    AURORA_TEST_CHECK_EQ(unknown.realm, std::string{});  // 一条参数都没给：空串，不是「缺这个字段」
    const auto bearer = parsed("bearer");
    AURORA_TEST_CHECK(bearer.scheme == AuthScheme::Bearer);  // 折叠不需要参数在场
}

AURORA_TEST_CASE(realm_is_lifted_out_while_other_parameters_keep_their_text) {
    const auto mechanism = parsed(R"(Basic charset=UTF-8, realm="api", QOP="auth, auth-int")");
    AURORA_TEST_CHECK_EQ(mechanism.realm, std::string{"api"});
    // 键小写（`QOP` 与 `qop` 是同一枚参数），值原样（`realm` 是给人看的域，改一个字节就换了一个域）。
    AURORA_TEST_CHECK_EQ(parameter_text(mechanism), std::string{"charset=UTF-8,qop=auth, auth-int"});
    const auto twice = parsed(R"(Basic realm="one", realm="two")");
    AURORA_TEST_CHECK_EQ(twice.realm, std::string{"one"});  // 同名多次：取首个
    // 第二枚既不顶掉第一枚，也不降级成一枚普通参数——公共面明说 `parameter("realm")` 不是它的去处。
    AURORA_TEST_CHECK(twice.parameters.empty());
    AURORA_TEST_CHECK(!twice.parameter("realm").has_value());
}

AURORA_TEST_CASE(a_quoted_comma_is_not_a_separator_and_escapes_are_resolved) {
    const auto described = parsed(R"(Bearer error="invalid_token", error_description="use \"this\", not that")");
    AURORA_TEST_CHECK_EQ(parameter_text(described),
                         std::string{R"(error=invalid_token,error_description=use "this", not that)"});
    AURORA_TEST_CHECK_EQ(parsed(R"(Bearer realm="a,b", scope=read)").realm, std::string{"a,b"});
    AURORA_TEST_CHECK_EQ(parameter_text(parsed(R"(Bearer realm="a,b", scope=read)")), std::string{"scope=read"});
    // 反斜杠只表达「下一枚字符按字面读」，本身不入值——包括它后面跟的不是引号或反斜杠的那种形态。
    AURORA_TEST_CHECK_EQ(parsed(R"(Bearer realm="a\\b\qc")").realm, std::string{R"(a\bqc)"});
    AURORA_TEST_CHECK_EQ(parsed("Bearer realm=\"tab\there\"").realm, std::string{"tab\there"});  // 制表符在引号内是值
}

AURORA_TEST_CASE(a_broken_parameter_table_only_costs_the_trailing_fragment) {
    const auto tail = parsed(R"(Basic realm="api", charset)");
    AURORA_TEST_CHECK_EQ(tail.realm, std::string{"api"});  // 已解析的参数照旧有效
    AURORA_TEST_CHECK(tail.parameters.empty());  // 没有 `=` 的那一枚不是参数，尾部残段整段丢弃
    const auto no_value = parsed(R"(Basic realm="api", charset=)");
    AURORA_TEST_CHECK_EQ(no_value.realm, std::string{"api"});
    AURORA_TEST_CHECK(no_value.parameters.empty());
}

AURORA_TEST_CASE(an_unterminated_quote_makes_the_whole_challenge_unparsable) {
    // 那不是「少了几个参数」而是「读不成」：当成参数值吞下会把后面的 `realm` 一路吃进一枚不存在的键里。
    AURORA_TEST_CHECK(!parse_auth_mechanism(R"(Basic realm="api)").has_value());
    AURORA_TEST_CHECK(!parse_auth_mechanism(R"(Bearer token="abc)").has_value());
    // 与「引号成对、只是内容里有引号」区分开：后者是可解析的转义，不是没闭合。
    AURORA_TEST_REQUIRE(parse_auth_mechanism(R"(Bearer token="a\"b")").has_value());
}

AURORA_TEST_CASE(a_value_without_a_scheme_token_is_unparsable) {
    AURORA_TEST_CHECK(!parse_auth_mechanism("").has_value());
    AURORA_TEST_CHECK(!parse_auth_mechanism("   ").has_value());
    AURORA_TEST_CHECK(!parse_auth_mechanism(R"("quoted")").has_value());  // 首字符不是 token
    AURORA_TEST_CHECK(!parse_auth_mechanism("=realm=api").has_value());
}

AURORA_TEST_CASE(the_field_length_cap_binds_just_past_the_limit) {
    const auto at_the_cap = "Basic realm=\"" + std::string(AURORA_AUTH_FIELD_MAX_BYTES - 14U, 'x') + "\"";
    AURORA_TEST_REQUIRE_EQ(at_the_cap.size(), AURORA_AUTH_FIELD_MAX_BYTES);
    AURORA_TEST_REQUIRE(parse_auth_mechanism(at_the_cap).has_value());  // 恰在上限仍解析，拒的是越界那一侧
    AURORA_TEST_CHECK(!parse_auth_mechanism(at_the_cap + "x").has_value());
}

AURORA_TEST_CASE(multiple_field_values_become_mechanisms_in_online_order) {
    const auto mechanisms =
        parse_auth_mechanisms({R"(Bearer realm="api")", R"("legacy=Basic")", R"(Basic realm="legacy")"});
    AURORA_TEST_REQUIRE_EQ(mechanisms.size(), std::size_t{2});  // 不可解析那条只被丢弃，不牵连其余
    AURORA_TEST_CHECK(mechanisms.at(0).scheme == AuthScheme::Bearer);  // 线上次序即偏好次序
    AURORA_TEST_CHECK(mechanisms.at(1).scheme == AuthScheme::Basic);
    AURORA_TEST_CHECK(parse_auth_mechanisms({}).empty());
    AURORA_TEST_REQUIRE_EQ(parse_auth_mechanisms({"Basic", "Bearer"}).size(), std::size_t{2});
    AURORA_TEST_CHECK(mechanisms.at(1).realm == "legacy");
}

AURORA_TEST_CASE(base64_matches_the_rfc_4648_vectors_for_every_tail_length) {
    struct Case {
        std::string_view input;
        std::string_view expected;
    };
    const std::vector<Case> cases{
        Case{.input = "", .expected = ""},
        Case{.input = "f", .expected = "Zg=="},
        Case{.input = "fo", .expected = "Zm8="},
        Case{.input = "foo", .expected = "Zm9v"},
        Case{.input = "foob", .expected = "Zm9vYg=="},
        Case{.input = "fooba", .expected = "Zm9vYmE="},
        Case{.input = "foobar", .expected = "Zm9vYmFy"},
    };
    for (const auto &item : cases) {
        AURORA_TEST_CHECK_EQ(base64_encode(item.input), std::string{item.expected});
    }
}

AURORA_TEST_CASE(base64_is_byte_exact_for_utf8_high_bytes_and_embedded_nul) {
    // UTF-8 逐字节进码表（`RFC 7617 §2.1` 的缺省字符集）：任何归一都会与对端的逐字节比对分岔。
    // 写成字节转义而非源文件里的非 ASCII 字符：源编码与终端代码页都不受本库控制，而这里要断言的正是字节。
    AURORA_TEST_CHECK_EQ(base64_encode(std::string_view{"\xc3\xa9", 2}), std::string{"w6k="});
    AURORA_TEST_CHECK_EQ(base64_encode(std::string_view{"\xff\x00", 2}), std::string{"/wA="});
    AURORA_TEST_CHECK_EQ(base64_encode(std::string_view{"a\0b", 3}), std::string{"YQBi"});
    // `:` 是用户名与口令的分隔符，它两侧的空格与引号都是凭据的一部分，编码层一律不动。
    AURORA_TEST_CHECK_EQ(base64_encode(R"(user name":"pa:ss)"), std::string{"dXNlciBuYW1lIjoicGE6c3M="});
}

}  // namespace aurora::test_cases::utest_net_http_auth_wire

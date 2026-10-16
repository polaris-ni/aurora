/// 测试类型: unit
/// 目标单元: include/aurora/net/proxy.h
/// 测试说明: 代理配置层的文本契约与决策契约回归：形态与缺省端口（裸
///           `host[:port]` 按 HTTP CONNECT、SOCKS5 缺省 1080、`socks5h` 把解析权交给代理）、
///           userinfo 的百分号解码与 `to_string()` 的密码掩码（凭据不得从可打印形态泄漏）、
///           已知但不支持的 scheme 走独立错误码而非退化成明文跳（`net-proxy-unsupported`）、
///           畸形值一律报错不静默直连。`NO_PROXY` 文法覆盖 `*`、域名本体与任意深层子域、
///           数字字面量全等、CIDR 前缀与端口限定，以及前缀越界 / 主机位非零 / 未加括号 IPv6
///           三类拒收。环境侧覆盖大小写变量优先级、错误里带上变量名，以及「安全目标绝不回退到
///           `HTTP_PROXY`」这条选择次序——它是企业网里被误配得最多的一条。
/// 全用例为纯文本/数值逻辑，不开 socket、不读真实环境变量（读取器就地注入），故三平台行为一致、
/// wasm 腿同样可跑，无需 `AURORA_TEST_SKIP`。

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/proxy.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_proxy {
namespace {

using aurora::net::NoProxy;
using aurora::net::ProxyConfig;
using aurora::net::ProxyCredentials;
using aurora::net::ProxyEnvironment;
using aurora::net::ProxyKind;
using aurora::testing::require_value;

/// @brief 由固定表构造环境读取器：未列出的变量视为未设置。
/// @param values 变量名到值的映射（测试数据，规模恒为个位数）。
/// @return 可注入 `ProxyEnvironment::from_environment` 的读取器。
auto make_reader(std::initializer_list<std::pair<std::string_view, std::string_view>> values)
    -> ProxyEnvironment::Reader {
    std::vector<std::pair<std::string, std::string>> table;
    for (const auto &[name, value] : values) {
        table.emplace_back(std::string(name), std::string(value));
    }
    return [table = std::move(table)](std::string_view name) -> std::optional<std::string> {
        for (const auto &[candidate, value] : table) {
            if (candidate == name) {
                return value;
            }
        }
        return std::nullopt;
    };
}

}  // namespace

AURORA_TEST_CASE(parse_accepts_forms_and_fills_default_ports) {
    const auto explicit_http = ProxyConfig::parse("http://Proxy.Example.COM:3128");
    AURORA_TEST_REQUIRE(bool(explicit_http));
    AURORA_TEST_CHECK_EQ(explicit_http.value().kind(), ProxyKind::HttpConnect);
    AURORA_TEST_CHECK_EQ(explicit_http.value().host(), "proxy.example.com");
    AURORA_TEST_CHECK_EQ(explicit_http.value().port(), 3128);
    AURORA_TEST_CHECK_FALSE(explicit_http.value().resolves_via_proxy());
    AURORA_TEST_CHECK_EQ(explicit_http.value().source(), "");

    // 裸 host 形态是真实配置里最常见的一种（HTTP_PROXY=proxy.internal），缺省端口按 curl 口径取 80。
    const auto bare = ProxyConfig::parse("proxy.internal");
    AURORA_TEST_REQUIRE(bool(bare));
    AURORA_TEST_CHECK_EQ(bare.value().kind(), ProxyKind::HttpConnect);
    AURORA_TEST_CHECK_EQ(bare.value().port(), 80);

    const auto socks = ProxyConfig::parse("socks5://10.0.0.1");
    AURORA_TEST_REQUIRE(bool(socks));
    AURORA_TEST_CHECK_EQ(socks.value().kind(), ProxyKind::Socks5);
    AURORA_TEST_CHECK_EQ(socks.value().port(), 1080);
    AURORA_TEST_CHECK_FALSE(socks.value().resolves_via_proxy());

    const auto socks_h = ProxyConfig::parse("  socks5h://[fd00::2]:1080  ");
    AURORA_TEST_REQUIRE(bool(socks_h));
    AURORA_TEST_CHECK_EQ(socks_h.value().host(), "fd00::2");
    AURORA_TEST_CHECK_TRUE(socks_h.value().resolves_via_proxy());

    const auto off = ProxyConfig::direct();
    AURORA_TEST_CHECK_TRUE(off.is_direct());
    AURORA_TEST_CHECK_EQ(off.to_string(), "direct");
}

AURORA_TEST_CASE(parse_rejects_malformed_and_reports_code) {
    const auto empty = ProxyConfig::parse("   ");
    AURORA_TEST_REQUIRE_FALSE(bool(empty));
    AURORA_TEST_CHECK_EQ(empty.error().code_enum, ErrorCode::NetProxyConfigInvalid);

    const auto unknown = ProxyConfig::parse("ftp://proxy:21");
    AURORA_TEST_REQUIRE_FALSE(bool(unknown));
    AURORA_TEST_CHECK_EQ(unknown.error().code_enum, ErrorCode::NetProxyConfigInvalid);

    // 认得但本批次不实现：必须是独立码，调用方才能区分「写错了」与「本库不支持」。
    const auto tls_proxy = ProxyConfig::parse("https://proxy:3128");
    AURORA_TEST_REQUIRE_FALSE(bool(tls_proxy));
    AURORA_TEST_CHECK_EQ(tls_proxy.error().code_enum, ErrorCode::NetProxyUnsupported);
    const auto socks4 = ProxyConfig::parse("socks4://proxy:1080");
    AURORA_TEST_REQUIRE_FALSE(bool(socks4));
    AURORA_TEST_CHECK_EQ(socks4.error().code_enum, ErrorCode::NetProxyUnsupported);

    for (const std::string_view bad : {"http://proxy:3128/upstream", "http://proxy:3128/?x=1", "http://proxy:3128#frag",
                                       "http://:3128", "http://proxy:0", "http://proxy:99999", "socks5://[fd00::2"}) {
        const auto rejected = ProxyConfig::parse(bad);
        AURORA_TEST_REQUIRE_FALSE(bool(rejected));
        AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::NetProxyConfigInvalid);
    }
}

AURORA_TEST_CASE(credentials_are_decoded_and_masked_on_print) {
    // 密码里带 `:`（`%3A`）与用户名里带 `@`（`%40`）都是真实凭据的常见形态：拆分只认第一个冒号，
    // 其余部分必须原样落在密码里。
    const auto with_auth = ProxyConfig::parse("http://bob%40x:p%3Ass@proxy:3128");
    AURORA_TEST_REQUIRE(bool(with_auth));
    AURORA_TEST_CHECK_TRUE(with_auth.value().has_credentials());
    AURORA_TEST_CHECK_EQ(with_auth.value().credentials().username, "bob@x");
    AURORA_TEST_CHECK_EQ(with_auth.value().credentials().password, "p:ss");

    const std::string printed = with_auth.value().to_string();
    AURORA_TEST_CHECK_EQ(printed, "http://bob%40x:***@proxy:3128");
    AURORA_TEST_CHECK_FALSE(printed.find("p%3Ass") != std::string::npos);
    AURORA_TEST_CHECK_FALSE(printed.find("p:ss") != std::string::npos);

    // 只有用户名没有密码：合法形态，序列化不带 `:***`。
    const auto name_only = ProxyConfig::parse("socks5://bob@proxy:1080");
    AURORA_TEST_REQUIRE(bool(name_only));
    AURORA_TEST_CHECK_EQ(name_only.value().to_string(), "socks5://bob@proxy:1080");
    AURORA_TEST_CHECK_TRUE(name_only.value().credentials().password.empty());

    const auto no_user = ProxyConfig::parse("http://@proxy:3128");
    AURORA_TEST_REQUIRE_FALSE(bool(no_user));
    AURORA_TEST_CHECK_EQ(no_user.error().code_enum, ErrorCode::NetProxyConfigInvalid);

    // IPv6 代理在可打印形态里必须重新带括号，否则渲染结果无法回解析。
    const auto v6 = ProxyConfig::parse("socks5h://[fd00::2]");
    AURORA_TEST_REQUIRE(bool(v6));
    AURORA_TEST_CHECK_EQ(v6.value().to_string(), "socks5h://[fd00::2]:1080");
}

AURORA_TEST_CASE(structural_factories_validate_host_and_port) {
    const auto manual = ProxyConfig::http_connect("10.0.0.1", 3128, ProxyCredentials{.username = "bob"});
    AURORA_TEST_REQUIRE(bool(manual));
    AURORA_TEST_CHECK_EQ(manual.value().kind(), ProxyKind::HttpConnect);
    AURORA_TEST_CHECK_EQ(manual.value().host(), "10.0.0.1");
    AURORA_TEST_CHECK_TRUE(manual.value().has_credentials());

    // 端口 0 的语境是「取该形态缺省端口」，不是「用 0 号端口连接」。
    const auto defaulted = ProxyConfig::socks5("::1", 0, true);
    AURORA_TEST_REQUIRE(bool(defaulted));
    AURORA_TEST_CHECK_EQ(defaulted.value().port(), 1080);
    AURORA_TEST_CHECK_EQ(defaulted.value().host(), "::1");
    AURORA_TEST_CHECK_TRUE(defaulted.value().resolves_via_proxy());

    const auto no_host = ProxyConfig::http_connect("", 3128);
    AURORA_TEST_REQUIRE_FALSE(bool(no_host));
    AURORA_TEST_CHECK_EQ(no_host.error().code_enum, ErrorCode::NetProxyConfigInvalid);

    // 主机位不能混入端口或 scheme：结构化入参就没有「猜形态」的余地。
    const auto dirty_host = ProxyConfig::http_connect("proxy:3128", 80);
    AURORA_TEST_REQUIRE_FALSE(bool(dirty_host));
    const auto spaced_host = ProxyConfig::socks5("bad host", 1080, false);
    AURORA_TEST_REQUIRE_FALSE(bool(spaced_host));
    AURORA_TEST_CHECK_EQ(spaced_host.error().code_enum, ErrorCode::NetProxyConfigInvalid);
}

AURORA_TEST_CASE(no_proxy_matches_domains_and_port_scoped_rules) {
    const auto parsed = NoProxy::parse("example.com; .internal:8080 ,  localhost");
    AURORA_TEST_REQUIRE(bool(parsed));
    AURORA_TEST_CHECK_EQ(parsed.value().size(), 3);
    const NoProxy &list = parsed.value();

    AURORA_TEST_CHECK_TRUE(list.matches("example.com", 443));
    AURORA_TEST_CHECK_TRUE(list.matches("EXAMPLE.COM", 443));
    AURORA_TEST_CHECK_TRUE(list.matches("api.example.com", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("example.com.", 443));
    // 后缀匹配必须落在点边界上：以规则名结尾但不同域的宿主不算命中。
    AURORA_TEST_CHECK_FALSE(list.matches("notexample.com", 443));
    AURORA_TEST_CHECK_FALSE(list.matches("exapl.com", 443));

    AURORA_TEST_CHECK_TRUE(list.matches("internal", 8080));
    AURORA_TEST_CHECK_TRUE(list.matches("a.b.internal", 8080));
    AURORA_TEST_CHECK_FALSE(list.matches("internal", 443));

    AURORA_TEST_CHECK_TRUE(list.matches("localhost", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("LOCALHOST", 80));

    const auto all = NoProxy::match_all();
    AURORA_TEST_CHECK_TRUE(all.matches("anything", 1));
    const auto star = NoProxy::parse("*");
    AURORA_TEST_REQUIRE(bool(star));
    AURORA_TEST_CHECK_TRUE(star.value().matches("anything", 1));

    AURORA_TEST_CHECK_TRUE(NoProxy::parse("").value().empty());
    AURORA_TEST_CHECK_TRUE(NoProxy::parse(" , ; ,").value().empty());
}

AURORA_TEST_CASE(no_proxy_matches_literals_exactly_and_cidr_by_prefix) {
    const auto parsed = NoProxy::parse("127.0.0.1, [::1]:8080, 10.0.0.0/8, 0.0.0.0/0, fd00::/8, *.corp.example");
    AURORA_TEST_REQUIRE(bool(parsed));
    const NoProxy &list = parsed.value();

    // 数字字面量走全等，绝不做后缀匹配（否则 `.0.0.1` 会放行无关宿主）。
    AURORA_TEST_CHECK_TRUE(list.matches("127.0.0.1", 443));
    AURORA_TEST_CHECK_FALSE(list.matches("1.127.0.0.1", 443));

    // 未加括号的 IPv6 与 `host:port` 无法区分，故规则里必须带括号；命中时端口限定生效。
    AURORA_TEST_CHECK_TRUE(list.matches("::1", 8080));
    AURORA_TEST_CHECK_FALSE(list.matches("::1", 443));

    AURORA_TEST_CHECK_TRUE(list.matches("10.255.0.1", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("fd00::9", 80));
    // `fd00::/8` 覆盖 fd00–fdff 首字节，故 fe00:: 才是越界样本。
    AURORA_TEST_CHECK_FALSE(list.matches("fe00::1", 80));
    // 名字型目标在本层不做解析，故永不被 CIDR 命中——与 Go / curl 同口径。
    AURORA_TEST_CHECK_FALSE(list.matches("host.internal", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("corp.example", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("a.corp.example", 80));
    AURORA_TEST_CHECK_FALSE(list.matches("xcorp.example", 80));
}

AURORA_TEST_CASE(no_proxy_rejects_entries_outside_the_grammar) {
    for (const std::string_view bad : {"10.0.0.0/33", "10.0.1.5/8", "fd00::/129", "::1", "[::1", "[::1]x",
                                       "example.com:0", "example.com:99999", "example.com:", "10.0.0.0/8:3128", "/8"}) {
        const auto rejected = NoProxy::parse(bad);
        AURORA_TEST_REQUIRE_FALSE(bool(rejected));
        AURORA_TEST_CHECK_EQ(rejected.error().code_enum, ErrorCode::NetProxyConfigInvalid);
    }
    // 出错时带上第几条，运维才能定位到具体那一项。
    const auto located = NoProxy::parse("ok.example, 10.0.1.5/8");
    AURORA_TEST_REQUIRE_FALSE(bool(located));
    AURORA_TEST_CHECK_TRUE(located.error().message.find("entry #2") != std::string::npos);
}

AURORA_TEST_CASE(environment_prefers_uppercase_and_names_the_variable) {
    const auto both = ProxyEnvironment::from_environment(
        make_reader({{"HTTP_PROXY", "http://upper:1"}, {"http_proxy", "http://lower:2"}}));
    AURORA_TEST_REQUIRE(bool(both));
    const auto upper_hit = require_value(both.value().http);
    AURORA_TEST_CHECK_EQ(upper_hit.host(), "upper");
    AURORA_TEST_CHECK_EQ(upper_hit.source(), "HTTP_PROXY");

    const auto only_lower = ProxyEnvironment::from_environment(make_reader({{"http_proxy", "socks5://lower"}}));
    AURORA_TEST_REQUIRE(bool(only_lower));
    const auto lower_hit = require_value(only_lower.value().http);
    AURORA_TEST_CHECK_EQ(lower_hit.source(), "http_proxy");
    AURORA_TEST_CHECK_EQ(lower_hit.port(), 1080);

    // 空串视为未设置：这是 CI 与容器里最常见的「显式清空」写法。
    const auto cleared = ProxyEnvironment::from_environment(make_reader({{"HTTP_PROXY", ""}, {"NO_PROXY", "  "}}));
    AURORA_TEST_REQUIRE(bool(cleared));
    AURORA_TEST_CHECK_TRUE(!cleared.value().http.has_value());
    AURORA_TEST_CHECK_TRUE(cleared.value().exceptions.empty());

    const auto broken = ProxyEnvironment::from_environment(make_reader({{"HTTPS_PROXY", "http://bad host"}}));
    AURORA_TEST_REQUIRE_FALSE(bool(broken));
    AURORA_TEST_CHECK_EQ(broken.error().code_enum, ErrorCode::NetProxyConfigInvalid);
    AURORA_TEST_CHECK_TRUE(broken.error().message.find("HTTPS_PROXY") != std::string::npos);

    const auto bad_exception = ProxyEnvironment::from_environment(make_reader({{"NO_PROXY", "10.0.0.0/33"}}));
    AURORA_TEST_REQUIRE_FALSE(bool(bad_exception));
    AURORA_TEST_CHECK_TRUE(bad_exception.error().message.find("NO_PROXY") != std::string::npos);
}

AURORA_TEST_CASE(for_target_follows_the_documented_order) {
    const auto both = ProxyEnvironment::from_environment(
        make_reader({{"HTTP_PROXY", "http://plain:3128"}, {"HTTPS_PROXY", "socks5h://secure:1080"}}));
    AURORA_TEST_REQUIRE(bool(both));
    const ProxyEnvironment &env = both.value();

    const auto plain = env.for_target("a.example", 80, false);
    AURORA_TEST_CHECK_EQ(plain.host(), "plain");
    const auto secure = env.for_target("a.example", 443, true);
    AURORA_TEST_CHECK_EQ(secure.host(), "secure");
    AURORA_TEST_CHECK_EQ(secure.kind(), ProxyKind::Socks5);

    // 只配 HTTP_PROXY 时，安全目标直连而不是「借道明文代理」——两变量之间的隔离是运维的契约。
    const auto only_http = ProxyEnvironment::from_environment(make_reader({{"HTTP_PROXY", "http://plain:3128"}}));
    AURORA_TEST_REQUIRE(bool(only_http));
    AURORA_TEST_CHECK_TRUE(only_http.value().for_target("a.example", 80, false).kind() == ProxyKind::HttpConnect);
    AURORA_TEST_CHECK_TRUE(only_http.value().for_target("a.example", 443, true).is_direct());

    // ALL_PROXY 是两者的共同回退，明文与安全都能命中。
    const auto only_all = ProxyEnvironment::from_environment(make_reader({{"ALL_PROXY", "socks5://any:1080"}}));
    AURORA_TEST_REQUIRE(bool(only_all));
    AURORA_TEST_CHECK_EQ(only_all.value().for_target("a.example", 80, false).host(), "any");
    AURORA_TEST_CHECK_EQ(only_all.value().for_target("a.example", 443, true).host(), "any");

    // 例外优先于一切代理选择。
    const auto exempted = ProxyEnvironment::from_environment(
        make_reader({{"ALL_PROXY", "socks5://any:1080"}, {"NO_PROXY", "*.example"}}));
    AURORA_TEST_REQUIRE(bool(exempted));
    AURORA_TEST_CHECK_TRUE(exempted.value().for_target("a.example", 443, true).is_direct());
    AURORA_TEST_CHECK_EQ(exempted.value().for_target("other.test", 443, true).host(), "any");

    const auto nothing = ProxyEnvironment::from_environment(make_reader({}));
    AURORA_TEST_REQUIRE(bool(nothing));
    AURORA_TEST_CHECK_TRUE(nothing.value().for_target("a.example", 80, false).is_direct());
    AURORA_TEST_CHECK_TRUE(nothing.value() == ProxyEnvironment::disabled());
}

AURORA_TEST_CASE(config_equality_ignores_the_diagnostic_source) {
    const auto from_env = ProxyConfig::parse("http://proxy:3128", "HTTP_PROXY");
    const auto explicit_config = ProxyConfig::parse("http://proxy:3128");
    AURORA_TEST_REQUIRE(bool(from_env));
    AURORA_TEST_REQUIRE(bool(explicit_config));
    AURORA_TEST_CHECK_EQ(from_env.value().source(), "HTTP_PROXY");
    AURORA_TEST_CHECK_TRUE(from_env.value() == explicit_config.value());

    const auto other_port = ProxyConfig::parse("http://proxy:3129");
    AURORA_TEST_REQUIRE(bool(other_port));
    AURORA_TEST_CHECK_FALSE(from_env.value() == other_port.value());

    const auto with_auth = ProxyConfig::parse("http://bob@proxy:3128");
    AURORA_TEST_REQUIRE(bool(with_auth));
    AURORA_TEST_CHECK_FALSE(with_auth.value() == explicit_config.value());
}

}  // namespace aurora::test_cases::utest_net_proxy

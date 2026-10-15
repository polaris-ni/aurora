/// 测试类型: unit
/// 目标单元: include/aurora/net/policy.h + src/aurora/net/policy.cpp
/// 测试说明: 声明式安全策略的三维判据回归（CHG-018 §12 策略件）。全用例为纯判定逻辑，不开 socket、
///           不查解析器，故三平台与 wasm 腿行为一致、无需 `AURORA_TEST_SKIP`。覆盖四件事：
///           - **明文主机清单的两种写法**：不带前导点＝注册名全等，带一枚前导点＝该域及其任意深层子域；
///             这一维与 `NoProxy` 的域名规则**刻意不同**（白名单里 `example.com` 静默放行 `sub.example.com`
///             是这类配置最常见的失误），故后缀匹配必须由那枚点显式选出来。大小写与根区尾点按 DNS 口径
///             归一；`*` 放开整维；空条目与空清单不放开任何东西（漏配一行不该变成放行一切）。
///           - **TLS 版本下限只判不改**：`TlsContext` 能谈到下限以下即冲突、直接拒，detail 同时点名两侧
///             取值；判定不写回上下文——「看起来生效了其实被改了」是这类声明最坏的失效形态，用例以
///             const 引用传参后再逐值比对原字段证明调用方那份没被动过。明文那侧无版本可谈，下限不参与。
///           - **`require_proxy` 的字面含义**：任何判定为直连的跳都被拒，`NO_PROXY` 例外命中的目标因此
///             同样被拒（那是该条声明的直接后果，不是例外）。
///           - **三维问的次序与进程级默认值**：三条都不合时交出的是第一条（明文）的原因；
///             `set_default_policy()` / `default_policy()` 往返、清除回「未设」，且交出的是副本。
/// 进程级存储是共享状态：用例用 RAII 守卫在退出时清回未设，绝不把策略留给同 runner 的其它套件。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/policy.h"
#include "aurora/net/proxy.h"
#include "aurora/net/tls.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_policy {
namespace {

using aurora::net::NetPolicy;
using aurora::net::ProxyConfig;
using aurora::net::TlsContext;
using aurora::net::TlsVersion;
using aurora::testing::require_value;

/// @brief 解析一条目标 URL；写死在测试里的字面量，解析失败就是用例本身写错了。
/// @param text URL 文本。
/// @return 解析好的 URL。
[[nodiscard]] auto url(std::string_view text) -> aurora::Url {
    auto parsed = aurora::Url::parse(text);
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    return parsed.value();
}

/// @brief 进程级默认策略的作用域守卫：进入时设定、退出时清回未设。
///
/// 本文件的用例顺序不是契约，同 runner 的其它套件也不该看到这里设过的策略，故退出必清。
struct DefaultPolicyScope {
    /// @brief 装入一份进程级默认策略。
    /// @param policy 要装入的策略。
    explicit DefaultPolicyScope(NetPolicy policy) { aurora::net::set_default_policy(std::move(policy)); }
    ~DefaultPolicyScope() { aurora::net::set_default_policy(std::nullopt); }
    DefaultPolicyScope(const DefaultPolicyScope &) = delete;
    auto operator=(const DefaultPolicyScope &) -> DefaultPolicyScope & = delete;
    DefaultPolicyScope(DefaultPolicyScope &&) = delete;
    auto operator=(DefaultPolicyScope &&) -> DefaultPolicyScope & = delete;
};

/// @brief 一份直连代理决策（`require_proxy` 那一维读的就是它）。
/// @return `Direct` 形态的配置。
[[nodiscard]] auto direct_hop() -> ProxyConfig { return ProxyConfig::direct(); }

/// @brief 一条经 HTTP 代理的决策（只用于「经代理则放行」的正向断言）。
/// @return 解析 `http://relay.test:3128` 得到的配置。
[[nodiscard]] auto proxied_hop() -> ProxyConfig {
    auto parsed = ProxyConfig::parse("http://relay.test:3128");
    AURORA_TEST_REQUIRE(bool(parsed));
    return parsed.value();
}

/// @brief 把判定结果收成人可读的一条文本（红时靠它指认是哪一维、报的什么细节）。
/// @param verdict 判定结果。
/// @return `slug / message` 文本；放行时为空。
[[nodiscard]] auto reason_of(const aurora::Result<void> &verdict) -> std::string {
    return verdict ? std::string{} : verdict.error().code + " / " + verdict.error().message;
}

}  // namespace

AURORA_TEST_CASE(construction_is_the_conservative_default) {
    const NetPolicy policy;
    AURORA_TEST_CHECK_TRUE(policy.allow_cleartext_hosts.empty());
    AURORA_TEST_CHECK_EQ(policy.min_tls_version, TlsVersion::Tls12);
    AURORA_TEST_CHECK_FALSE(policy.require_proxy);

    // 缺省构造就把明文全拒：这不是「功能没做完」的占位，而是本库对凭据与用户数据中转的默认回答。
    const auto refused = policy.evaluate(url("http://example.com/"), direct_hop(), TlsContext{});
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK(refused.error().message.find("allow_cleartext_hosts") != std::string::npos);
    AURORA_TEST_CHECK(refused.error().message.find("http://example.com") != std::string::npos);
}

AURORA_TEST_CASE(a_bare_list_entry_matches_only_that_exact_name) {
    const NetPolicy policy{.allow_cleartext_hosts = {"example.com"}};
    AURORA_TEST_CHECK_TRUE(policy.allows_cleartext_host("example.com"));
    // 比较形态归一：大小写与一枚根区尾点都不该让一条写对的清单失效。
    AURORA_TEST_CHECK_TRUE(policy.allows_cleartext_host("EXAMPLE.COM"));
    AURORA_TEST_CHECK_TRUE(policy.allows_cleartext_host("example.com."));

    // 子域必须**不**命中：这是与 `NoProxy` 的域名规则分岔的那一条。
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("sub.example.com"));
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("a.b.example.com"));
    // 后缀形态的陷阱：只差一个分隔符的邻居域名不该被顺带放行。
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("evil-example.com"));
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("example.org"));
}

AURORA_TEST_CASE(a_leading_dot_entry_opens_that_domain_and_every_subdomain) {
    const NetPolicy policy{.allow_cleartext_hosts = {".example.com"}};
    AURORA_TEST_CHECK_TRUE(policy.allows_cleartext_host("example.com"));  // 域本体算命中
    AURORA_TEST_CHECK_TRUE(policy.allows_cleartext_host("a.example.com"));
    AURORA_TEST_CHECK_TRUE(policy.allows_cleartext_host("a.b.c.example.com"));  // 任意深度
    // 半截后缀与差一个分隔符的邻居都不该命中——比较始终带着那枚点。
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("xample.com"));
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("notexample.com"));
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("example.com.evil.test"));
}

AURORA_TEST_CASE(the_star_opens_the_dimension_while_empty_entries_open_nothing) {
    const NetPolicy wide{.allow_cleartext_hosts = {"*"}};
    AURORA_TEST_CHECK_TRUE(wide.allows_cleartext_host("anything.test"));
    AURORA_TEST_CHECK_TRUE(wide.allows_cleartext_host("127.0.0.1"));

    // 空清单、空条目、以及「只有空条目」三种形态一律不放行：漏配一行不该变成放行一切。
    const NetPolicy none{.allow_cleartext_hosts = {}};
    AURORA_TEST_CHECK_FALSE(none.allows_cleartext_host("example.com"));
    const NetPolicy blank{.allow_cleartext_hosts = {""}};
    AURORA_TEST_CHECK_FALSE(blank.allows_cleartext_host("example.com"));
    AURORA_TEST_CHECK_FALSE(blank.allows_cleartext_host(""));

    // 清单里混一条空条目也不影响其余条目照常生效。
    const NetPolicy mixed{.allow_cleartext_hosts = {"", "example.com"}};
    AURORA_TEST_CHECK_TRUE(mixed.allows_cleartext_host("example.com"));
    AURORA_TEST_CHECK_FALSE(mixed.allows_cleartext_host("other.test"));
}

AURORA_TEST_CASE(no_implicit_loopback_exception) {
    // 回环与任何其它主机一样受明文维度约束：例外藏在库里就等于让两种配置长得一样。
    const NetPolicy policy;
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("127.0.0.1"));
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("::1"));
    AURORA_TEST_CHECK_FALSE(policy.allows_cleartext_host("localhost"));

    const auto refused = policy.evaluate(url("http://127.0.0.1:8080/"), direct_hop(), TlsContext{});
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPolicyViolated);

    // 点名回环即放行：清单就是这条决定的可见记录。
    const NetPolicy listed{.allow_cleartext_hosts = {"127.0.0.1"}};
    AURORA_TEST_CHECK_TRUE(bool(listed.evaluate(url("http://127.0.0.1:8080/"), direct_hop(), TlsContext{})));
}

AURORA_TEST_CASE(the_tls_floor_refuses_a_context_that_can_talk_below_it) {
    const NetPolicy policy{.allow_cleartext_hosts = {}, .min_tls_version = TlsVersion::Tls13};

    TlsContext loose;
    loose.min_version = TlsVersion::Tls12;  // 下界比策略低：谈到的版本取决于对端
    const auto refused = policy.evaluate(url("https://example.com/"), direct_hop(), loose);
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    // detail 同时点名两侧取值：只报「策略拒绝」的现场要让人去猜是哪一枚配置低了。
    AURORA_TEST_CHECK(reason_of(refused).find("TLS 1.2") != std::string::npos);
    AURORA_TEST_CHECK(reason_of(refused).find("TLS 1.3") != std::string::npos);

    // 上界低于下限同样是冲突：那一档版本根本谈不到。
    TlsContext capped;
    capped.min_version = TlsVersion::Tls13;
    capped.max_version = TlsVersion::Tls11;
    const auto capped_refusal = policy.evaluate(url("https://example.com/"), direct_hop(), capped);
    AURORA_TEST_REQUIRE_FALSE(bool(capped_refusal));
    AURORA_TEST_CHECK(reason_of(capped_refusal).find("TLS 1.1") != std::string::npos);

    TlsContext strict;
    strict.min_version = TlsVersion::Tls13;
    AURORA_TEST_CHECK_TRUE(bool(policy.evaluate(url("https://example.com/"), direct_hop(), strict)));

    // 只判不改：判定之后调用方那份上下文的版本界逐值如故（策略从不替它抬高）。
    AURORA_TEST_CHECK_EQ(loose.min_version, TlsVersion::Tls12);
    AURORA_TEST_CHECK_FALSE(loose.max_version.has_value());
}

AURORA_TEST_CASE(the_tls_floor_is_silent_for_cleartext_targets) {
    // 明文没有版本可谈，下限这一维不参与；放行明文的那一维由清单单独决定。
    const NetPolicy policy{.allow_cleartext_hosts = {"example.com"}, .min_tls_version = TlsVersion::Tls13};
    TlsContext loose;
    loose.min_version = TlsVersion::Tls10;
    const auto verdict = policy.evaluate(url("http://example.com/"), direct_hop(), loose);
    AURORA_TEST_CHECK_MSG(bool(verdict), reason_of(verdict));
}

AURORA_TEST_CASE(require_proxy_refuses_every_direct_hop) {
    const NetPolicy policy{.allow_cleartext_hosts = {"*"}, .min_tls_version = TlsVersion::Tls12, .require_proxy = true};

    const auto refused = policy.evaluate(url("http://example.com/"), direct_hop(), TlsContext{});
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, aurora::ErrorCode::NetPolicyViolated);
    AURORA_TEST_CHECK(reason_of(refused).find("proxy") != std::string::npos);

    const auto relayed = policy.evaluate(url("http://example.com/"), proxied_hop(), TlsContext{});
    AURORA_TEST_CHECK_MSG(bool(relayed), reason_of(relayed));

    // 关掉这一维，直连照常放行：被拒的从来只有「要求了代理而本跳直连」这一种组合。
    const NetPolicy relaxed{.allow_cleartext_hosts = {"*"}, .min_tls_version = TlsVersion::Tls12};
    AURORA_TEST_CHECK_FALSE(relaxed.require_proxy);
    AURORA_TEST_CHECK_TRUE(bool(relaxed.evaluate(url("http://example.com/"), direct_hop(), TlsContext{})));
}

AURORA_TEST_CASE(the_three_dimensions_are_asked_in_order) {
    // 三条都不合时交出的是明文那一维：次序固定，调用方据此可以「修一条就少一条」。
    const NetPolicy policy{.allow_cleartext_hosts = {}, .min_tls_version = TlsVersion::Tls13, .require_proxy = true};
    TlsContext loose;
    loose.min_version = TlsVersion::Tls10;

    const auto cleartext = policy.evaluate(url("http://example.com/"), direct_hop(), loose);
    AURORA_TEST_REQUIRE_FALSE(bool(cleartext));
    AURORA_TEST_CHECK(cleartext.error().message.find("allow_cleartext_hosts") != std::string::npos);

    // 明文放行后，下一维才是版本下限（同一份策略、同一个上下文，只是 scheme 换成 https）。
    const NetPolicy cleartext_open{.allow_cleartext_hosts = {"example.com"}, .min_tls_version = TlsVersion::Tls13};
    const auto version = cleartext_open.evaluate(url("https://example.com/"), direct_hop(), loose);
    AURORA_TEST_REQUIRE_FALSE(bool(version));
    AURORA_TEST_CHECK(version.error().message.find("TLS 1.0") != std::string::npos);

    // 版本也谈妥后，最后一维才问代理。
    const NetPolicy need_proxy{
        .allow_cleartext_hosts = {"example.com"}, .min_tls_version = TlsVersion::Tls12, .require_proxy = true};
    const auto routing = need_proxy.evaluate(url("https://example.com/"), direct_hop(), TlsContext{});
    AURORA_TEST_REQUIRE_FALSE(bool(routing));
    AURORA_TEST_CHECK(routing.error().message.find("proxy") != std::string::npos);
}

AURORA_TEST_CASE(evaluation_does_not_look_at_method_path_or_port) {
    // 判据只看 scheme、主机与本跳决策：路径与端口不参与其中，方法更是压根不给它。
    const NetPolicy policy{.allow_cleartext_hosts = {".example.com"}};
    AURORA_TEST_CHECK_TRUE(
        bool(policy.evaluate(url("http://a.example.com/deep/path?q=1#f"), direct_hop(), TlsContext{})));
    AURORA_TEST_CHECK_TRUE(bool(policy.evaluate(url("http://example.com:8080/"), direct_hop(), TlsContext{})));
    AURORA_TEST_CHECK_FALSE(bool(policy.evaluate(url("http://example.org/"), direct_hop(), TlsContext{})));
}

AURORA_TEST_CASE(the_process_default_round_trips_and_clears) {
    AURORA_TEST_CHECK_FALSE(aurora::net::default_policy().has_value());  // 未设＝回到保守缺省，不是关掉策略

    {
        NetPolicy policy;
        policy.allow_cleartext_hosts = {"intra.example"};
        policy.min_tls_version = TlsVersion::Tls13;
        const DefaultPolicyScope scope{policy};

        const NetPolicy seen = require_value(aurora::net::default_policy());
        AURORA_TEST_CHECK_EQ(seen.min_tls_version, TlsVersion::Tls13);
        AURORA_TEST_CHECK_TRUE(seen.allows_cleartext_host("intra.example"));

        // 交出的是副本：改了拿到的那份，不该回头改写进程级存储。
        NetPolicy mutated = seen;
        mutated.allow_cleartext_hosts.emplace_back("other.test");
        (void)mutated;
        const NetPolicy again = require_value(aurora::net::default_policy());
        AURORA_TEST_CHECK_EQ(again.allow_cleartext_hosts.size(), std::size_t{1});
    }

    AURORA_TEST_CHECK_FALSE(aurora::net::default_policy().has_value());  // 守卫退出即清回未设
}

}  // namespace aurora::test_cases::utest_net_policy

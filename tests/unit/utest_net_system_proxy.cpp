/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/system_proxy.cpp
/// 测试说明: 系统代理快照到决策集合的换算回归（CHG-018 §11「系统代理读取」）：PAC 与 WPAD 一律
///           报 `net-proxy-unsupported` 而不回空集合（配了自动配置的机器意图就是「须经代理」，
///           静默当成没配会让办公网流量直连出去且无痕）；系统里没配代理是正常状态，回空集合；
///           服务器表按 scheme 令牌分槽（`http=` / `https=` / `socks5=` / 裸项归 `all`），
///           `ftp=` 与 `gopher=` 略过、`socks=` 与未知令牌分别报「认得但不实现」与「畸形」，
///           同槽重复项不再静默覆盖。例外清单里的 `<local>` 与 `<-loopback>` 两枚系统令牌必须
///           建模而非丢弃。合并侧覆盖三条裁决：环境变量任一代理变量存在即接管（系统腿根本不被
///           询问）、本腿没有通道等价于只有环境变量、通道在而读失败原样上抛。
/// 全用例经内部快照接缝与注入读取器完成，零平台 API、零真实环境变量，故三平台与 wasm 一致，
/// 无需 `AURORA_TEST_SKIP`。

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/detail/system_proxy.h"
#include "aurora/net/proxy.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_system_proxy {
namespace {

using aurora::net::NoProxy;
using aurora::net::ProxyConfig;
using aurora::net::ProxyEnvironment;
using aurora::net::ProxyKind;
using aurora::testing::require_value;
namespace sys = aurora::net::detail;

/// @brief 用例结束时清除采集钩子：钩子是进程级槽位，留着会让同 runner 的后续用例看到上一条的快照。
struct HookScope {
    HookScope() = default;
    ~HookScope() { sys::set_system_proxy_hook({}); }
    HookScope(const HookScope &) = delete;
    auto operator=(const HookScope &) -> HookScope & = delete;
    HookScope(HookScope &&) = delete;
    auto operator=(HookScope &&) -> HookScope & = delete;
};

/// @brief 造一份手工代理快照。
/// @param servers 服务器表原文。
/// @param bypass 例外清单原文。
/// @return 快照（`manual_enabled` 随服务器表非空而置真）。
auto manual(std::string_view servers, std::string_view bypass = {}) -> sys::SystemProxySnapshot {
    sys::SystemProxySnapshot snapshot;
    snapshot.manual_enabled = !servers.empty();
    snapshot.server_list = std::string(servers);
    snapshot.bypass_list = std::string(bypass);
    return snapshot;
}

/// @brief 由固定表构造环境读取器：未列出的变量视为未设置。
/// @param values 变量名到值的映射（测试数据，规模恒为个位数）。
/// @return 可注入 `from_environment_or_system` 的读取器。
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

AURORA_TEST_CASE(pac_and_wpad_are_reported_instead_of_dropped) {
    sys::SystemProxySnapshot with_pac;
    with_pac.pac_url = "http://ad.internal/proxy.pac";
    const auto pac = sys::system_proxy_from_snapshot(with_pac);
    AURORA_TEST_REQUIRE_FALSE(bool(pac));
    AURORA_TEST_CHECK_EQ(pac.error().code_enum, ErrorCode::NetProxyUnsupported);
    // 诊断文本要指得出是哪个脚本：运维要知道「这台机器靠 PAC 决定路由」这件事本身。
    AURORA_TEST_CHECK_TRUE(pac.error().message.find("proxy.pac") != std::string::npos);

    sys::SystemProxySnapshot with_wpad;
    with_wpad.auto_detect = true;
    const auto wpad = sys::system_proxy_from_snapshot(with_wpad);
    AURORA_TEST_REQUIRE_FALSE(bool(wpad));
    AURORA_TEST_CHECK_EQ(wpad.error().code_enum, ErrorCode::NetProxyUnsupported);

    // 两条判据都先于「手工代理是否存在」：PAC 与手工项并存时同样不能假装手工项就是全部意图。
    const auto both = sys::system_proxy_from_snapshot([] {
        auto snapshot = manual("proxy.internal:3128");
        snapshot.pac_url = "http://ad/internal.pac";
        return snapshot;
    }());
    AURORA_TEST_REQUIRE_FALSE(bool(both));
    AURORA_TEST_CHECK_EQ(both.error().code_enum, ErrorCode::NetProxyUnsupported);
}

AURORA_TEST_CASE(absent_manual_proxy_is_a_successful_empty_set) {
    const sys::SystemProxySnapshot none;
    const auto empty = sys::system_proxy_from_snapshot(none);
    AURORA_TEST_REQUIRE(bool(empty));
    const auto target = empty.value().for_target("api.example.com", 443, true);
    AURORA_TEST_CHECK_TRUE(target.is_direct());

    // 没配代理但写了例外清单：例外照样解析，它此刻是无害的，也不该被当成畸形配置。
    const auto bypass_only = sys::system_proxy_from_snapshot([] {
        sys::SystemProxySnapshot snapshot;
        snapshot.bypass_list = "10.0.0.0/8, .internal";
        return snapshot;
    }());
    AURORA_TEST_REQUIRE(bool(bypass_only));
    AURORA_TEST_CHECK_TRUE(bypass_only.value().exceptions.matches("host.internal", 80));
}

AURORA_TEST_CASE(server_list_fills_slots_by_scheme_token) {
    const auto parsed = sys::system_proxy_from_snapshot(
        manual("http=plain.internal:3128; HTTPS=secure.internal:8443; socks5=gw.internal:1080", "*.lab.internal"));
    AURORA_TEST_REQUIRE(bool(parsed));
    const ProxyEnvironment &env = parsed.value();

    const auto &http = require_value(env.http);
    AURORA_TEST_CHECK_EQ(http.kind(), ProxyKind::HttpConnect);
    AURORA_TEST_CHECK_EQ(http.host(), "plain.internal");
    AURORA_TEST_CHECK_EQ(http.port(), static_cast<std::uint16_t>(3128));
    // `source()` 带槽位令牌：诊断里要能看出这条来自系统设置的哪个槽，而不是环境变量。
    AURORA_TEST_CHECK_EQ(http.source(), "system:http");

    const auto &https = require_value(env.https);
    AURORA_TEST_CHECK_EQ(https.host(), "secure.internal");
    AURORA_TEST_CHECK_EQ(https.port(), static_cast<std::uint16_t>(8443));

    const auto &all = require_value(env.all);
    AURORA_TEST_CHECK_EQ(all.kind(), ProxyKind::Socks5);
    // 系统设置里的 SOCKS 由服务端口径代理解析目标名，故映射为 `socks5h` 语义。
    AURORA_TEST_CHECK_TRUE(all.resolves_via_proxy());

    // 决策口径与配置侧完全复用：安全目标走 https 槽，明文走 http 槽，例外命中即直连。
    AURORA_TEST_CHECK_EQ(env.for_target("api.example.com", 443, true).host(), "secure.internal");
    AURORA_TEST_CHECK_EQ(env.for_target("api.example.com", 80, false).host(), "plain.internal");
    AURORA_TEST_CHECK_TRUE(env.for_target("box.lab.internal", 80, false).is_direct());
}

AURORA_TEST_CASE(bare_entry_and_skipped_protocols) {
    // 裸项（无 `=`）归 `all` 槽；`ftp=` / `gopher=` 略过——本库不发起这两类连接，它们不影响判定。
    const auto parsed =
        sys::system_proxy_from_snapshot(manual("ftp=ftp.internal:21, bare.internal:3128, gopher=go:70"));
    AURORA_TEST_REQUIRE(bool(parsed));
    AURORA_TEST_CHECK_TRUE(!parsed.value().http.has_value());
    AURORA_TEST_CHECK_TRUE(!parsed.value().https.has_value());
    const auto &all = require_value(parsed.value().all);
    AURORA_TEST_CHECK_EQ(all.host(), "bare.internal");
    AURORA_TEST_CHECK_EQ(all.source(), "system:all");

    // 缺省端口与配置侧同口径：CONNECT 80、SOCKS5 1080。
    const auto defaulted = sys::system_proxy_from_snapshot(manual("http=proxy.internal"));
    AURORA_TEST_REQUIRE(bool(defaulted));
    AURORA_TEST_CHECK_EQ(require_value(defaulted.value().http).port(), static_cast<std::uint16_t>(80));
}

AURORA_TEST_CASE(ambiguous_and_malformed_entries_fail_loud) {
    struct Probe {
        std::string_view servers;
        ErrorCode expected;
        std::string_view needle;
    };
    for (const Probe probe : {
             Probe{.servers = "socks=legacy.internal:1080",
                   .expected = ErrorCode::NetProxyUnsupported,
                   .needle = "SOCKS4"},
             Probe{.servers = "smb=file.internal",
                   .expected = ErrorCode::NetProxyConfigInvalid,
                   .needle = "unrecognized scheme token"},
             Probe{.servers = "http=proxy.internal;http=other.internal",
                   .expected = ErrorCode::NetProxyConfigInvalid,
                   .needle = "duplicates"},
             Probe{
                 .servers = "http=", .expected = ErrorCode::NetProxyConfigInvalid, .needle = "empty proxy server part"},
             Probe{.servers = "=proxy.internal",
                   .expected = ErrorCode::NetProxyConfigInvalid,
                   .needle = "not a scheme token"},
             // 代理自身走 TLS：内层的「认得但不实现」必须原样透出，折算成配置畸形会把排查方向带偏。
             Probe{.servers = "http=https://proxy.internal:8443",
                   .expected = ErrorCode::NetProxyUnsupported,
                   .needle = "not implemented"},
             Probe{.servers = "proxy bad host", .expected = ErrorCode::NetProxyConfigInvalid, .needle = "entry #1"},
         }) {
        const auto rejected = sys::system_proxy_from_snapshot(manual(probe.servers));
        AURORA_TEST_REQUIRE_FALSE(bool(rejected));
        AURORA_TEST_CHECK_EQ(rejected.error().code_enum, probe.expected);
        AURORA_TEST_CHECK_TRUE(rejected.error().message.find(probe.needle) != std::string::npos);
    }

    // 开了手工却没写服务器表：这是配置残缺，不能当成「没配」。
    sys::SystemProxySnapshot enabled;
    enabled.manual_enabled = true;
    const auto missing = sys::system_proxy_from_snapshot(enabled);
    AURORA_TEST_REQUIRE_FALSE(bool(missing));
    AURORA_TEST_CHECK_EQ(missing.error().code_enum, ErrorCode::NetProxyConfigInvalid);
}

AURORA_TEST_CASE(system_exception_tokens_are_modeled) {
    const auto parsed = sys::system_proxy_from_snapshot(manual("proxy.internal:3128", "<local>, <-LOOPBACK>, .corp"));
    AURORA_TEST_REQUIRE(bool(parsed));
    const NoProxy &list = parsed.value().exceptions;

    // `<local>`：无限定域主机名例外；带点的主机与数字字面量都不算。
    AURORA_TEST_CHECK_TRUE(list.matches("printer", 80));
    AURORA_TEST_CHECK_FALSE(list.matches("printer.lab", 80));
    AURORA_TEST_CHECK_FALSE(list.matches("10.1.2.3", 80));

    // `<-loopback>`：折算成 v4/v6 两条回环前缀，端口限定不参与。
    AURORA_TEST_CHECK_TRUE(list.matches("127.0.0.1", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("127.9.9.9", 3128));
    AURORA_TEST_CHECK_TRUE(list.matches("::1", 443));
    AURORA_TEST_CHECK_FALSE(list.matches("128.0.0.1", 80));
    AURORA_TEST_CHECK_TRUE(list.matches("host.corp", 80));
}

AURORA_TEST_CASE(from_system_uses_the_hook_and_names_a_missing_channel) {
    HookScope scope;

    sys::set_system_proxy_hook([]() -> Result<std::optional<sys::SystemProxySnapshot>> {
        return std::optional<sys::SystemProxySnapshot>{manual("hooked.internal:3128")};
    });
    const auto hooked = ProxyEnvironment::from_system();
    AURORA_TEST_REQUIRE(bool(hooked));
    AURORA_TEST_CHECK_EQ(require_value(hooked.value().all).host(), "hooked.internal");

    // 钩子里的畸形快照按原码上抛，不在这里二次折算：换算层的判据只有一处。
    sys::set_system_proxy_hook([]() -> Result<std::optional<sys::SystemProxySnapshot>> {
        return std::optional<sys::SystemProxySnapshot>{manual("smb=file.internal")};
    });
    const auto malformed = ProxyEnvironment::from_system();
    AURORA_TEST_REQUIRE_FALSE(bool(malformed));
    AURORA_TEST_CHECK_EQ(malformed.error().code_enum, ErrorCode::NetProxyConfigInvalid);

    // 空快照 = 本腿没有系统代理通道，报 `net-platform-unavailable` 而不是回空集合。
    sys::set_system_proxy_hook(
        []() -> Result<std::optional<sys::SystemProxySnapshot>> { return std::optional<sys::SystemProxySnapshot>{}; });
    const auto unavailable = ProxyEnvironment::from_system();
    AURORA_TEST_REQUIRE_FALSE(bool(unavailable));
    AURORA_TEST_CHECK_EQ(unavailable.error().code_enum, ErrorCode::NetPlatformUnavailable);
}

AURORA_TEST_CASE(environment_takes_over_before_the_system_is_asked) {
    HookScope scope;
    int system_calls = 0;
    sys::set_system_proxy_hook([&system_calls]() -> Result<std::optional<sys::SystemProxySnapshot>> {
        ++system_calls;
        return std::optional<sys::SystemProxySnapshot>{manual("system.internal:3128")};
    });

    // 设过任一代理变量 = 接管：系统腿连问都不问，合并只会让「设了却没生效」更难归因。
    const auto taken =
        ProxyEnvironment::from_environment_or_system(make_reader({{"HTTP_PROXY", "http://env.internal:8080"}}));
    AURORA_TEST_REQUIRE(bool(taken));
    AURORA_TEST_CHECK_EQ(require_value(taken.value().http).host(), "env.internal");
    AURORA_TEST_CHECK_EQ(system_calls, 0);

    const auto merged = ProxyEnvironment::from_environment_or_system(make_reader({}));
    AURORA_TEST_REQUIRE(bool(merged));
    AURORA_TEST_CHECK_EQ(require_value(merged.value().all).host(), "system.internal");
    AURORA_TEST_CHECK_EQ(system_calls, 1);

    // 本腿没有通道时，合并入口等价于一次环境变量读取（Linux 上这就是全部语义）。
    sys::set_system_proxy_hook(
        []() -> Result<std::optional<sys::SystemProxySnapshot>> { return std::optional<sys::SystemProxySnapshot>{}; });
    const auto env_only =
        ProxyEnvironment::from_environment_or_system(make_reader({{"ALL_PROXY", "socks5://only:1080"}}));
    AURORA_TEST_REQUIRE(bool(env_only));
    AURORA_TEST_CHECK_EQ(require_value(env_only.value().all).host(), "only");

    const auto nothing = ProxyEnvironment::from_environment_or_system(make_reader({}));
    AURORA_TEST_REQUIRE(bool(nothing));
    AURORA_TEST_CHECK_TRUE(nothing.value().for_target("api.example.com", 443, true).is_direct());

    // 通道在而读失败：原样上抛，绝不退化成「按环境变量算了」。
    sys::set_system_proxy_hook([]() -> Result<std::optional<sys::SystemProxySnapshot>> {
        return make_error(ErrorCode::NetProxyConfigUnreadable, "detail=system proxy: registry access denied");
    });
    const auto refused = ProxyEnvironment::from_environment_or_system(make_reader({}));
    AURORA_TEST_REQUIRE_FALSE(bool(refused));
    AURORA_TEST_CHECK_EQ(refused.error().code_enum, ErrorCode::NetProxyConfigUnreadable);
}

}  // namespace aurora::test_cases::utest_net_system_proxy

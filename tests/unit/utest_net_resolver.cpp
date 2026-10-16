/// 测试类型: unit
/// 目标单元: include/aurora/net/resolver.h
/// 测试说明: 主机名解析的短路、规范化与投递契约回归（CHG-018 双栈与竞速切片）：数字字面量在
///           钩子之前短路（故能证明「本机字面量永不查网」）、空主机名是用法错误而不是网络故障、
///           产出恒去重且保持首次出现次序（不同端口算不同条目）、平台腿失败与「查到了但零条可用
///           记录」都落 `net-dns-failed`、异步完成恰一次且发生在调用线程之外、空回调不发起任何查表。
///           另有一例走真实平台腿：`*.invalid` 是 RFC 6761 保留域，合规解析器必须回 NXDOMAIN，
///           离线机器的失败同样是失败，故该判据不依赖网络可达性。
/// 除真实平台腿一例，其余用例经注入接缝完成，不依赖 DNS、不读环境变量，三平台与 wasm 口径一致。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/detail/lookup.h"
#include "aurora/net/engine.h"
#include "aurora/net/resolver.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_resolver {
namespace {

using aurora::net::Endpoint;
using aurora::net::Resolver;

/// @brief 装入 / 清除查表钩子的作用域（离开即清除，避免污染后续用例）。
class HookScope {
  public:
    /// @brief 构造即装入钩子。
    /// @param hook 钩子本体。
    explicit HookScope(aurora::net::detail::LookupFn hook) { aurora::net::detail::set_lookup_hook(std::move(hook)); }
    /// @brief 析构即清除钩子（回落平台腿）。
    ~HookScope() { aurora::net::detail::set_lookup_hook(nullptr); }

    HookScope(const HookScope &) = delete;
    auto operator=(const HookScope &) -> HookScope & = delete;
    HookScope(HookScope &&) = delete;
    auto operator=(HookScope &&) -> HookScope & = delete;
};

/// @brief 由数字字面量构造测试数据里的端点。
/// @param text 地址文本。
/// @param port 端口。
/// @return 端点；构造失败即用例致命失败（测试数据本身写错）。
auto literal(std::string_view text, std::uint16_t port) -> Endpoint {
    auto built = Endpoint::from_ip(text, port);
    AURORA_TEST_REQUIRE(bool(built));
    return std::move(built.value());
}

/// @brief 轮询等待一个异步完成（引擎线程与主线程投递器的组合不保证时序）。
/// @param flag 完成标志。
/// @return 在预算内完成为 true。
auto wait_until(const std::atomic_flag &flag) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.test()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

/// @brief 用例结束时收敛引擎：`resolve` 会惰性启动常驻线程，留它在跑会让同 runner 的后续用例
///        既看到「capabilities 未启动线程」判据失真，又与自己的事件源驱动方重复等待同一注册口。
struct EngineConverge {
    EngineConverge() = default;
    ~EngineConverge() { aurora::net::Engine::instance().shutdown(); }
    EngineConverge(const EngineConverge &) = delete;
    auto operator=(const EngineConverge &) -> EngineConverge & = delete;
    EngineConverge(EngineConverge &&) = delete;
    auto operator=(EngineConverge &&) -> EngineConverge & = delete;
};

}  // namespace

AURORA_TEST_CASE(numeric_literal_short_circuits_before_the_hook) {
    std::atomic_int calls{0};
    HookScope scope([&calls](std::string_view, std::uint16_t) -> Result<std::vector<Endpoint>> {
        ++calls;
        return make_error(ErrorCode::NetDnsFailed, "hook must not be reached");
    });

    const auto v4 = Resolver::lookup("127.0.0.1", 80);
    AURORA_TEST_REQUIRE(bool(v4));
    AURORA_TEST_CHECK_EQ(v4.value().size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(v4.value().at(0).host(), "127.0.0.1");
    AURORA_TEST_CHECK_EQ(v4.value().at(0).port(), std::uint16_t{80});

    const auto v6 = Resolver::lookup("[::1]", 443);
    AURORA_TEST_REQUIRE(bool(v6));
    AURORA_TEST_CHECK_EQ(v6.value().at(0).host(), "::1");

    // 作用域号也是字面量的一部分：带它的地址同样不该走 DNS（DNS 回报里没有 zone 这个字段）。
    const auto scoped = Resolver::lookup("fe80::1%2", 80);
    AURORA_TEST_REQUIRE(bool(scoped));
    AURORA_TEST_CHECK_EQ(scoped.value().at(0).zone(), "2");

    AURORA_TEST_CHECK_EQ(calls.load(), 0);
}

AURORA_TEST_CASE(empty_host_is_a_usage_error_not_a_dns_failure) {
    const auto result = Resolver::lookup("", 80);
    AURORA_TEST_REQUIRE_FALSE(bool(result));
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::NetEndpointInvalid);
}

AURORA_TEST_CASE(answers_are_deduplicated_in_first_seen_order) {
    HookScope scope([](std::string_view, std::uint16_t) -> Result<std::vector<Endpoint>> {
        return std::vector<Endpoint>{literal("203.0.113.1", 80), literal("2001:db8::1", 80),
                                     literal("203.0.113.1", 80),  // 同族同址同端口：重复
                                     literal("203.0.113.1", 8080)};  // 同址不同端口：独立条目
    });

    const auto result = Resolver::lookup("example.com", 80);
    AURORA_TEST_REQUIRE(bool(result));
    AURORA_TEST_REQUIRE_EQ(result.value().size(), std::size_t{3});
    AURORA_TEST_CHECK_EQ(result.value().at(0).to_string(), "203.0.113.1:80");
    AURORA_TEST_CHECK_EQ(result.value().at(1).to_string(), "[2001:db8::1]:80");
    AURORA_TEST_CHECK_EQ(result.value().at(2).to_string(), "203.0.113.1:8080");
}

AURORA_TEST_CASE(resolver_failure_and_empty_answer_both_fail_as_dns) {
    HookScope failure([](std::string_view, std::uint16_t) -> Result<std::vector<Endpoint>> {
        return make_error(ErrorCode::NetDnsFailed, "host=svc.example, resolver code=8");
    });
    const auto failed = Resolver::lookup("svc.example", 443);
    AURORA_TEST_REQUIRE_FALSE(bool(failed));
    AURORA_TEST_CHECK_EQ(failed.error().code_enum, ErrorCode::NetDnsFailed);

    HookScope empty(
        [](std::string_view, std::uint16_t) -> Result<std::vector<Endpoint>> { return std::vector<Endpoint>{}; });
    const auto no_answer = Resolver::lookup("gone.example", 443);
    AURORA_TEST_REQUIRE_FALSE(bool(no_answer));
    AURORA_TEST_CHECK_EQ(no_answer.error().code_enum, ErrorCode::NetDnsFailed);
    // 「查表成功但零条可用记录」等同不可用：诊断必须带上主机名，否则调用方只能靠猜。
    AURORA_TEST_CHECK_TRUE(no_answer.error().message.find("gone.example") != std::string::npos);
}

AURORA_TEST_CASE(platform_leg_fails_for_the_reserved_invalid_tld) {
#ifdef AURORA_PLATFORM_WASM
    // 浏览器运行时没有 BSD socket 语义（解析经 shim 而非真 DNS），`.invalid` 的保留判定
    // 在该腿上不成立；本腿由 NullEngine 一律回不可用，见其余 net 套件的同款跳过口径。
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    // 真实平台腿：`.invalid` 由 RFC 6761 保留，合规解析器必须回 NXDOMAIN；完全没有网络的机器
    // 同样回失败。两条路径的判据一致，故此用例不依赖 DNS 可达性。
    const auto result = Resolver::lookup("aurora.invalid", 443);
    AURORA_TEST_REQUIRE_FALSE(bool(result));
    AURORA_TEST_CHECK_EQ(result.error().code_enum, ErrorCode::NetDnsFailed);
    AURORA_TEST_CHECK_TRUE(result.error().message.find("aurora.invalid") != std::string::npos);
#endif
}

AURORA_TEST_CASE(async_resolve_delivers_exactly_once_off_the_calling_thread) {
#ifdef AURORA_PLATFORM_WASM
    // 浏览器运行时的异步解析没有引擎交付路径（无引擎腿一律回不可用，钩子根本没机会跑），
    // 本用例「钩子在另一线程执行、回调在另一线程抵达」的前提在该腿不成立——与兄弟用例
    // 同一口径整体跳过，而非把平台的不可用伪装成一次失败的查表。
    AURORA_TEST_SKIP("the browser runtime has no BSD socket semantics; aurora::net is not built for wasm");
#else
    AURORA_TEST_REQUIRE_THREADS();
    const auto main_thread = std::this_thread::get_id();
    std::atomic_int hook_calls{0};
    std::atomic_int callback_calls{0};
    std::atomic_int lookup_thread_is_not_caller{0};
    std::atomic_int callback_thread_is_not_caller{0};
    std::atomic_int answer_size{0};
    std::atomic_flag done = ATOMIC_FLAG_INIT;

    HookScope scope([&](std::string_view, std::uint16_t) -> Result<std::vector<Endpoint>> {
        ++hook_calls;
        lookup_thread_is_not_caller = std::this_thread::get_id() == main_thread ? 0 : 1;
        return std::vector<Endpoint>{literal("203.0.113.7", 8080)};
    });
    // 声明在钩子之后，故析构在先：先把引擎线程收敛掉，再摘钩子，不留「线程还在读槽」的窗口。
    EngineConverge converge;

    // 回调在另一条线程执行，故此处只记录事实、断言留回本用例线程做（测试框架的失败登记不做跨线程同步）。
    Resolver::resolve("api.example", 8080, [&](Result<std::vector<Endpoint>> result) {
        ++callback_calls;
        callback_thread_is_not_caller = std::this_thread::get_id() == main_thread ? 0 : 1;
        answer_size = result && result.value().size() == 1 ? 1 : -1;
        done.test_and_set();
    });

    AURORA_TEST_CHECK_TRUE(wait_until(done));
    AURORA_TEST_CHECK_EQ(callback_calls.load(), 1);
    AURORA_TEST_CHECK_EQ(hook_calls.load(), 1);
    AURORA_TEST_CHECK_EQ(answer_size.load(), 1);
    // 阻塞的查表腿与完成回调都不占用调用线程——这是「异步」的全部意义。
    AURORA_TEST_CHECK_EQ(lookup_thread_is_not_caller.load(), 1);
    AURORA_TEST_CHECK_EQ(callback_thread_is_not_caller.load(), 1);
#endif
}

AURORA_TEST_CASE(empty_callback_starts_no_lookup) {
    std::atomic_int calls{0};
    HookScope scope([&calls](std::string_view, std::uint16_t) -> Result<std::vector<Endpoint>> {
        ++calls;
        return std::vector<Endpoint>{literal("203.0.113.9", 80)};
    });

    Resolver::resolve("quiet.example", 80, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    AURORA_TEST_CHECK_EQ(calls.load(), 0);
}

}  // namespace aurora::test_cases::utest_net_resolver

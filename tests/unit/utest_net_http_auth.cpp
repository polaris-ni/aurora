/// 测试类型: unit
/// 目标单元: include/aurora/net/http_auth.h + src/aurora/net/http_auth.cpp
/// 测试说明: 认证值层与两枚内置提供方的形态回归（§12 认证切片的值半边）。覆盖 `AuthChallenge::mechanism()`
///           按档位命中/落空、`AuthMechanism::parameter()` 的大小写不敏感与 `realm` 不在此处、`AuthDecision::empty()`
///           即「放弃」这一唯一表达、`basic_credentials()` 的 UTF-8 组串逐字节（含 `RFC 7617 §2.1` 的示例向量、
///           口令里的冒号不做转义）、内置件只在挑战点名自己那枚方案时答复（只给 `Digest` 时答空而不是拒答）、
///           答复字段按挑战类型落位（源站 `Authorization` / 代理 `Proxy-Authorization`）、令牌原样上线、
///           以及派生方以协程出口跨挂起点取凭据（含「换取失败即上抛错误」那条通路）。内置件全程不挂起，故其
///           返回任务当场 `is_done()`，这条也一并钉住——发起侧靠它决定是否多花一次轮询。
/// 全用例零 socket；仅派生方那一条用到后台线程池，故带线程要求。

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/net/http.h"
#include "aurora/net/http_auth.h"
#include "aurora/state/coroutine.h"
#include "framework/async_wait.h"
#include "framework/aurora_test.h"
#include "framework/net_wait.h"

namespace aurora::test_cases::utest_net_http_auth {
namespace {

using aurora::net::http::AuthChallenge;
using aurora::net::http::AuthDecision;
using aurora::net::http::Authenticator;
using aurora::net::http::AuthMechanism;
using aurora::net::http::AuthScheme;
using aurora::net::http::basic_credentials;
using aurora::net::http::BasicAuthenticator;
using aurora::net::http::BearerAuthenticator;
using aurora::net::http::Method;

/// @brief 提供方出口的协程任务类型（`authenticate()` 的返回形态）。
using AuthTask = aurora::CoroTask<aurora::Result<AuthDecision>>;

/// @brief 造一枚方案条目（值层用例按形态直接装配，不经文法层——那条路有自己的回归文件）。
/// @param slot 归一后的档位。
/// @param spelling 线上原文的方案名。
/// @param realm `realm` 参数原文，可为空。
/// @param extras 其余参数，按线上次序。
/// @return 方案条目。
auto mechanism(AuthScheme slot, std::string_view spelling, std::string_view realm,
               std::vector<std::pair<std::string, std::string>> extras = {}) -> AuthMechanism {
    auto entry = AuthMechanism{};
    entry.scheme = slot;
    entry.name = std::string{spelling};
    entry.realm = std::string{realm};
    entry.parameters = std::move(extras);
    return entry;
}

/// @brief 造一次挑战：源站 401 或代理 407，带给出的那几枚方案。
/// @param proxy 真是代理在问（407）时为 true。
/// @param mechanisms 对端给出的方案，按线上次序。
/// @return 挑战。
auto challenge(bool proxy, std::vector<AuthMechanism> mechanisms) -> AuthChallenge {
    auto asked = AuthChallenge{};
    asked.proxy = proxy;
    asked.status = proxy ? 407 : 401;
    asked.host = proxy ? "proxy.example.net" : "api.example.org";
    asked.method = Method::Post;
    asked.url = proxy ? "http://api.example.org/v1/items" : "https://api.example.org/v1/items";
    asked.mechanisms = std::move(mechanisms);
    return asked;
}

/// @brief 有界泵到协程收尾：续体经进程级唯一队列回投，等待方须自己领值班并排水。
///
/// 内置件从不挂起（帧在创建表达式内即收尾），谓词首判即中，替身只领一拍空租约、不留孤儿项。
/// @tparam Task 协程任务（`is_done()` 问法）。
/// @param subject 等待主题，进超时文本。
/// @param task 待收尾的协程。
/// @return 收尾事实（预算内命中为真）。
template <typename Task>
[[nodiscard]] auto drive_done(std::string_view subject, const Task &task) -> aurora::test::WaitOutcome {
    const aurora::test::net::MainQueueLoop duty;
    return aurora::test::net::drive_until(subject, [&task] { return task.is_done(); });
}

/// @brief 按双层解包取出已收尾帧的答复。
///
/// 双层是本层的固定形态（`CoroTask<Result<T>>::result()` 求得 `Result<Result<T>>`）：外层是「这帧协程有没有
/// 跑完」、内层才是「提供方有没有给出凭据」，两枚失败各有各的说法，不在这里合流。
/// @param task 提供方的协程任务（未收尾时有界等待；谓词先判后睡，故内置件零睡眠）。
/// @return 答复；任一层失败即致命失败。
[[nodiscard]] auto settled(const AuthTask &task) -> AuthDecision {
    const auto outcome = drive_done("auth provider frame", task);
    AURORA_TEST_REQUIRE_MSG(outcome.satisfied, outcome.report());
    const auto outer = task.result();
    // 消息实参在宏里无条件求值，故 `error()` 只在失败分支取——成功态取它是 `bad_variant_access`。
    AURORA_TEST_REQUIRE_MSG(outer.ok(), outer ? std::string{"outer frame"} : outer.error().message);
    const auto &inner = outer.value();
    AURORA_TEST_REQUIRE_MSG(inner.ok(), inner ? std::string{"provider answer"} : inner.error().message);
    return inner.value();
}

/// @brief 问一次内置件并取出答复：内置件从不挂起，故先当场断言帧已收尾。
/// @param provider 凭据提供方。
/// @param asked 对端说出的挑战。
/// @return 答复；提供方报错时致命失败。
auto answer(const Authenticator &provider, const AuthChallenge &asked) -> AuthDecision {
    const auto task = provider.authenticate(asked);
    AURORA_TEST_REQUIRE_MSG(task.is_done(), "the provider did not finish on the spot");
    return settled(task);
}

/// @brief 派生方：跨挂起点换取令牌，只带走挑战的副本。
///
/// 存在的理由是钉住出口形态本身——「取凭据的动作可以是一次 `co_await`」是本层选择协程抽象类而非凭据池的
/// 唯一理由，故它必须有真挂起、真恢复、真答复的用例。
class DeferredTokenAuthenticator final : public Authenticator {
  public:
    /// @brief 挂起一次后台动作，回来后按 `realm` 副本决定往哪一侧投凭据。
    /// @param asked 对端说出的挑战。
    /// @return 协程任务：成功携答复，后台动作失败时携那份错误。
    /// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): 挑战由发起侧持有，帧只借用
    [[nodiscard]] auto authenticate(const AuthChallenge &asked) const -> CoroTask<Result<AuthDecision>> override {
        // 跨挂起点只带副本：存活期契约（`net/http_auth.h` 的 `authenticate()`）不承诺挑战活过 `co_await`。
        const auto *offered = asked.mechanism(AuthScheme::Bearer);
        const std::string realm = offered == nullptr ? std::string{} : offered->realm;
        const bool proxy = asked.proxy;

        const auto exchanged = co_await co_async([]() -> Result<int> { return Result<int>{7}; });
        if (!exchanged) {
            co_return Result<AuthDecision>{exchanged.error()};
        }
        auto headers = aurora::Headers{};
        const auto field = proxy ? std::string_view{"Proxy-Authorization"} : std::string_view{"Authorization"};
        headers.set(field, "Bearer " + realm + "-" + std::to_string(exchanged.value()));
        co_return Result<AuthDecision>{AuthDecision{std::move(headers)}};
    }
};

/// @brief 派生方：换取动作本身失败，把那份错误原样交出（本库不拿一份没到手的凭据去重发）。
///
/// 与上一枚的区别是「错误从挂起点之后回来」——发起侧据此上抛时，帧已经让出过，路径与内置件的即时答复
/// 不是同一条。
class FailingAuthenticator final : public Authenticator {
  public:
    /// @brief 挂起一次注定失败的换取，随后交出那次失败。
    /// @return 协程任务：恒失败，载荷是后台动作的错误。
    ///
    /// 本件不读挑战，故形参不留名；挑战由发起侧存在交换作用域上、帧只借用（存活期契约见 `net/http_auth.h`）。
    /// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters, readability-named-parameter)
    [[nodiscard]] auto authenticate(const AuthChallenge &) const -> CoroTask<Result<AuthDecision>> override {
        const auto exchanged = co_await co_async([]() -> Result<int> { throw std::runtime_error{"token endpoint"}; });
        if (!exchanged) {
            co_return Result<AuthDecision>{exchanged.error()};
        }
        co_return Result<AuthDecision>{AuthDecision{}};
    }
};

}  // namespace

AURORA_TEST_CASE(mechanism_lookup_hits_the_named_slot_only) {
    const auto asked = challenge(false, {
                                            mechanism(AuthScheme::Unsupported, "Digest", "api"),
                                            mechanism(AuthScheme::Basic, "basic", "root"),
                                        });
    const auto *basic = asked.mechanism(AuthScheme::Basic);
    AURORA_TEST_REQUIRE(basic != nullptr);
    AURORA_TEST_CHECK_EQ(basic->name, std::string{"basic"});  // 档位归一，原文拼写留在 name
    AURORA_TEST_CHECK_EQ(basic->realm, std::string{"root"});
    AURORA_TEST_CHECK(asked.mechanism(AuthScheme::Bearer) == nullptr);  // 没点名的档位取不到
    // 未列名的方案照样在列表里，只是档位是 `Unsupported`：自定义实现方按 `name` 认领它。
    const auto *digest = asked.mechanism(AuthScheme::Unsupported);
    AURORA_TEST_REQUIRE(digest != nullptr);
    AURORA_TEST_CHECK_EQ(digest->name, std::string{"Digest"});

    const auto empty = challenge(true, {});
    AURORA_TEST_CHECK(empty.mechanism(AuthScheme::Basic) == nullptr);
    AURORA_TEST_CHECK_EQ(empty.status, std::uint16_t{407});
}

AURORA_TEST_CASE(parameter_lookup_is_case_insensitive_and_excludes_realm) {
    // 登记侧的 `QOP` 是大写原文（自定义实现方手工装配时的形态），查询侧四种拼写都要能命中同一枚值。
    const auto entry = mechanism(AuthScheme::Basic, "Basic", "api",
                                 {
                                     {"charset", "UTF-8"},
                                     {"QOP", "auth,auth-int"},
                                 });
    AURORA_TEST_CHECK_EQ(entry.parameter("charset").value_or(std::string{}), std::string{"UTF-8"});
    AURORA_TEST_CHECK_EQ(entry.parameter("qop").value_or(std::string{}), std::string{"auth,auth-int"});
    AURORA_TEST_CHECK_EQ(entry.parameter("QOP").value_or(std::string{}), std::string{"auth,auth-int"});
    AURORA_TEST_CHECK_EQ(entry.parameter("CharSeT").value_or(std::string{}), std::string{"UTF-8"});
    AURORA_TEST_CHECK(!entry.parameter("nonce").has_value());
    // `realm` 有单列字段，因此不是参数表里的一项；`parameter("realm")` 取不到它。
    AURORA_TEST_CHECK(!entry.parameter("realm").has_value());
    AURORA_TEST_CHECK_EQ(entry.realm, std::string{"api"});
}

AURORA_TEST_CASE(an_empty_decision_is_the_only_way_to_decline) {
    const auto declined = AuthDecision{};
    AURORA_TEST_CHECK(declined.empty());
    auto answered = AuthDecision{};
    answered.headers.set("Authorization", "Basic QQ==");
    AURORA_TEST_CHECK(!answered.empty());
    AURORA_TEST_CHECK_EQ(answered.headers.get("Authorization").value_or(std::string{}), std::string{"Basic QQ=="});
}

AURORA_TEST_CASE(basic_credentials_join_first_then_encode_over_utf8) {
    // `RFC 7617 §2.1` 的示例向量：先组 `user ":" password` 整串，再逐字节 base64。
    AURORA_TEST_CHECK_EQ(basic_credentials("Aladdin", "open sesame"),
                         std::string{"Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ=="});
    AURORA_TEST_CHECK_EQ(basic_credentials("", ""), std::string{"Basic Og=="});  // 冒号是分隔符，空凭据也要留着它
    // 口令里的冒号不参与分隔：对端按第一枚冒号切分，故后半串整段是口令。
    AURORA_TEST_CHECK_EQ(basic_credentials("u", "a:b:c"), std::string{"Basic dTphOmI6Yw=="});
    // UTF-8 逐字节、不做百分号编码、不做归一：任何一步都会与对端的逐字节比对分岔。
    AURORA_TEST_CHECK_EQ(basic_credentials(std::string_view{"\xc3\xa9", 2}, "pass"), std::string{"Basic w6k6cGFzcw=="});
    AURORA_TEST_CHECK_EQ(basic_credentials("user", "p\xc3\xa9ss"), std::string{"Basic dXNlcjpww6lzcw=="});
}

AURORA_TEST_CASE(basic_provider_answers_only_when_the_challenge_names_basic) {
    const auto provider = BasicAuthenticator{"Aladdin", "open sesame"};

    const auto asked = challenge(false, {mechanism(AuthScheme::Basic, "Basic", "api")});
    const auto answered = answer(provider, asked);
    AURORA_TEST_CHECK_EQ(answered.headers.get("Authorization").value_or(std::string{}),
                         basic_credentials("Aladdin", "open sesame"));
    // 只答被点名的那枚方案：其余方案一律答空，把对端那份 401 原样交出，而不是送一份无人索取的凭据。
    AURORA_TEST_CHECK(!answered.headers.contains("Proxy-Authorization"));

    const auto digest = challenge(false, {mechanism(AuthScheme::Unsupported, "Digest", "api")});
    AURORA_TEST_CHECK(answer(provider, digest).empty());
}

AURORA_TEST_CASE(the_credential_field_follows_who_asked) {
    const auto provider = BasicAuthenticator{"lyn", "pw"};
    const auto asked = challenge(true, {mechanism(AuthScheme::Basic, "Basic", "squid")});
    const auto answered = answer(provider, asked);
    // 源站要 `Authorization`、代理要 `Proxy-Authorization`：写错一处就是把凭据送到没资格看它的那一侧。
    AURORA_TEST_CHECK_EQ(answered.headers.get("Proxy-Authorization").value_or(std::string{}),
                         basic_credentials("lyn", "pw"));
    AURORA_TEST_CHECK(!answered.headers.contains("Authorization"));

    const auto origin = challenge(false, {mechanism(AuthScheme::Basic, "Basic", "api")});
    const auto back = answer(provider, origin);
    AURORA_TEST_CHECK(back.headers.contains("Authorization"));
    AURORA_TEST_CHECK(!back.headers.contains("Proxy-Authorization"));
}

AURORA_TEST_CASE(bearer_provider_writes_the_token_verbatim) {
    const auto provider = BearerAuthenticator{"ey.abc-123~_"};
    const auto asked = challenge(false, {mechanism(AuthScheme::Bearer, "bearer", "api")});
    const auto answered = answer(provider, asked);
    // 方案名用本层的规范拼写（首字母大写），令牌原样上线、不校验形状、不做编码。
    AURORA_TEST_CHECK_EQ(answered.headers.get("Authorization").value_or(std::string{}),
                         std::string{"Bearer ey.abc-123~_"});

    const auto basic_only = challenge(true, {mechanism(AuthScheme::Basic, "Basic", "squid")});
    AURORA_TEST_CHECK(answer(provider, basic_only).empty());
}

AURORA_TEST_CASE(built_in_providers_never_suspend) {
    // 发起侧据此决定是否多花一次轮询：内置件在调用点当场即已完成。
    const auto asked = challenge(false, {mechanism(AuthScheme::Basic, "Basic", "api")});
    const auto basic = BasicAuthenticator{"a", "b"};
    AURORA_TEST_CHECK(basic.authenticate(asked).is_done());
    const auto bearer = BearerAuthenticator{"t"};
    AURORA_TEST_CHECK(bearer.authenticate(asked).is_done());
}

AURORA_TEST_CASE(a_derived_provider_can_await_the_credential_exchange) {
    AURORA_TEST_REQUIRE_THREADS();
    const auto provider = DeferredTokenAuthenticator{};

    // 挑战在帧还在挂起时就越出作用域：协程体只能用让出前抄下的副本，这正是存活期契约要钉的那一条。
    const auto task = [&provider]() -> AuthTask {
        const auto asked = challenge(false, {mechanism(AuthScheme::Bearer, "Bearer", "offline")});
        return provider.authenticate(asked);
    }();
    const auto answered = settled(task);
    // 挂起点之后用的仍是 `realm` 与 `proxy` 的副本：两者都早在让出前抄进了协程帧。
    AURORA_TEST_CHECK_EQ(answered.headers.get("Authorization").value_or(std::string{}),
                         std::string{"Bearer offline-7"});
    AURORA_TEST_CHECK(!answered.empty());

    const auto proxied = challenge(true, {mechanism(AuthScheme::Bearer, "Bearer", "squid")});
    const auto back = settled(provider.authenticate(proxied));
    // 同一枚派生方按挑战类型落位：代理在问就只补 `Proxy-Authorization`。
    AURORA_TEST_CHECK_EQ(back.headers.get("Proxy-Authorization").value_or(std::string{}),
                         std::string{"Bearer squid-7"});
    AURORA_TEST_CHECK(!back.headers.contains("Authorization"));
}

AURORA_TEST_CASE(a_failed_exchange_propagates_the_provider_error) {
    AURORA_TEST_REQUIRE_THREADS();
    const auto provider = FailingAuthenticator{};
    const auto asked = challenge(false, {mechanism(AuthScheme::Basic, "Basic", "api")});

    const auto task = provider.authenticate(asked);
    const auto outcome = drive_done("failing provider frame", task);
    AURORA_TEST_REQUIRE_MSG(outcome.satisfied, outcome.report());
    const auto outer = task.result();
    AURORA_TEST_REQUIRE(outer.ok());  // 外层成功：帧跑完了
    const auto &inner = outer.value();
    // 内层失败：换取没换成，那份错误就是本次交换的结局，本库不拿一份没到手的凭据去重发。
    AURORA_TEST_CHECK(!inner.ok());
    AURORA_TEST_CHECK_EQ(inner.error().code_enum, aurora::ErrorCode::RuntimeAsyncException);
}

}  // namespace aurora::test_cases::utest_net_http_auth

// http_auth.cpp —— 认证值层与两枚内置提供方（口径见 `net/http_auth.h`）。
#include "aurora/net/http_auth.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/http_auth_wire.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {
namespace {

/// @brief ASCII 单字节小写化；非大写字母原样返回（规避 locale 与负值 `char` 相关的 UB）。
/// @param unit 原始字节。
/// @return 折叠后的字节。
constexpr auto to_lower_ascii(char unit) -> char {
    const auto raw = static_cast<unsigned char>(unit);
    if (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')) {
        return static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'));
    }
    return unit;
}

/// @brief 按挑战的类型取该写的那枚字段名：源站要 `Authorization`，代理要 `Proxy-Authorization`。
///
/// 两者是同一次交换里两处不同的听众：前者归目标源站，后者归代理。写错一处不只是「对端不认识」，而是
/// 把凭据送到了没资格看它的那一侧。
/// @param challenge 对端说出的挑战。
/// @return 字段名原文。
[[nodiscard]] auto credential_field(const AuthChallenge &challenge) -> std::string_view {
    return challenge.proxy ? std::string_view{"Proxy-Authorization"} : std::string_view{"Authorization"};
}

/// @brief 组装一枚只含该字段的答复。
/// @param challenge 对端说出的挑战（决定字段名）。
/// @param value 字段值原文。
/// @return 答复。
[[nodiscard]] auto single_field_decision(const AuthChallenge &challenge, std::string_view value) -> AuthDecision {
    auto headers = aurora::Headers{};
    headers.set(credential_field(challenge), value);
    auto decision = AuthDecision{};
    decision.headers = std::move(headers);
    return decision;
}

}  // namespace

auto AuthMechanism::parameter(std::string_view key) const -> std::optional<std::string> {
    auto lowered = std::string{key};
    std::ranges::transform(lowered, lowered.begin(), to_lower_ascii);
    for (const auto &[name, value] : parameters) {
        // 两侧都折叠：文法层交回的键已是小写，但公共面的承诺是「大小写不敏感」，而自定义实现方可能手工
        // 装配一枚机制（合成一份挑战、或把自研方案的参数表原样搬进来）。只折叠查询侧会让那种形态静默取不到
        // 值，现场表现为「参数明明在列表里，`parameter()` 却说没有」。
        auto folded = std::string{name};
        std::ranges::transform(folded, folded.begin(), to_lower_ascii);
        if (folded == lowered) {
            return value;
        }
    }
    return std::nullopt;
}

auto AuthChallenge::mechanism(AuthScheme scheme) const -> const AuthMechanism * {
    const auto found =
        std::ranges::find_if(mechanisms, [scheme](const AuthMechanism &entry) { return entry.scheme == scheme; });
    return found == mechanisms.end() ? nullptr : &*found;
}

BasicAuthenticator::BasicAuthenticator(std::string user, std::string password)
    : user_(std::move(user)), password_(std::move(password)) {}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): 挑战由发起侧持有，帧只借用
auto BasicAuthenticator::authenticate(const AuthChallenge &challenge) const -> CoroTask<Result<AuthDecision>> {
    // 只在挑战点名 `Basic` 时答复：对端没给这一枚方案就送上一份用户名口令，等于把凭据投向一个未知协议，
    // 而「未知」在这里的现场表现是对端收下之后仍然拒你，你无从分辨它拒的是凭据还是格式。
    const bool offered = challenge.mechanism(AuthScheme::Basic) != nullptr;
    if (!offered) {
        co_return Result<AuthDecision>{AuthDecision{}};
    }
    co_return Result<AuthDecision>{single_field_decision(challenge, basic_credentials(user_, password_))};
}

BearerAuthenticator::BearerAuthenticator(std::string token) : token_(std::move(token)) {}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): 挑战由发起侧持有，帧只借用
auto BearerAuthenticator::authenticate(const AuthChallenge &challenge) const -> CoroTask<Result<AuthDecision>> {
    const bool offered = challenge.mechanism(AuthScheme::Bearer) != nullptr;
    if (!offered) {
        co_return Result<AuthDecision>{AuthDecision{}};
    }
    co_return Result<AuthDecision>{single_field_decision(challenge, "Bearer " + token_)};
}

auto basic_credentials(std::string_view user, std::string_view password) -> std::string {
    // 组串在前、编码在后：base64 的输入是 `user ":" password` 这一整串 UTF-8 字节（`RFC 7617 §2.1`）。
    // 先编码再拼接、或对某一段做百分号编码，都会与对端的逐字节比对分岔。
    const auto joined = std::string{user} + ':' + std::string{password};
    return "Basic " + detail::base64_encode(joined);
}

}  // namespace aurora::net::http

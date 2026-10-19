// http_auth_wire.cpp —— 认证字段的文法层与凭据编码的实现（口径见 `detail/http_auth_wire.h`）。
#include "aurora/net/detail/http_auth_wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::net::detail {
namespace {

/// @brief 单字节 ASCII 小写化；非大写字母原样返回（避开 locale 与负值 `char` 相关的 UB）。
/// @param c 原始字节。
/// @return 折叠后的字节。
constexpr auto to_lower(char c) -> char {
    const auto raw = static_cast<unsigned char>(c);
    if (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')) {
        return static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'));
    }
    return c;
}

/// @brief 整串 ASCII 小写（只用于方案名与参数键的比较，值一律不折叠）。
/// @param text 原始文本。
/// @return 折叠后的副本。
[[nodiscard]] auto to_lower(std::string_view text) -> std::string {
    std::string out{text};
    std::ranges::transform(out, out.begin(), [](char c) { return to_lower(c); });
    return out;
}

/// @brief `RFC 7230 §3.2.6` 里除字母与数字之外的 token 字符集（`is_tchar()` 的查表）。
constexpr std::string_view AURORA_TCHAR_SPECIALS = "!#$%&'*+-.^_`|~";

/// @brief 是否为 `RFC 7230 §3.2.6` 的 token 字符（参数名与裸参数值的唯一合法构成）。
/// @param c 原始字节。
/// @return 属于 token 集时 true。
constexpr auto is_tchar(char c) -> bool {
    const auto raw = static_cast<unsigned char>(c);
    const auto letter = (raw >= static_cast<unsigned char>('a') && raw <= static_cast<unsigned char>('z')) ||
                        (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z'));
    const auto digit = raw >= static_cast<unsigned char>('0') && raw <= static_cast<unsigned char>('9');
    return letter || digit || AURORA_TCHAR_SPECIALS.find(c) != std::string_view::npos;
}

/// @brief 去掉两端的 OWS（空格与水平制表，`RFC 9110 §5.6.3`）；CR/LF 由头部层剥掉，这里不必再认。
/// @param text 原始文本。
/// @return 去壳后的片段。
constexpr auto trim(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 从 `at` 起取一段 token（连续 tchar）。
/// @param text 剩余文本。
/// @param at 游标；成功时前移到 token 之后。
/// @return token 文本；首字符即非 tchar 时为空。
[[nodiscard]] auto take_token(std::string_view text, std::size_t &at) -> std::string_view {
    const std::size_t begin = at;
    while (at < text.size() && is_tchar(text.at(at))) {
        ++at;
    }
    return text.substr(begin, at - begin);
}

/// @brief 从 `at` 起取一段 quoted-string 并就地反转义（`\"` / `\\` 之外的反斜杠也按转义读）。
///
/// 引号没闭合即交回失败——那是「整条挑战读不成」的形态，与「尾部残段丢掉」是两回事：前者若当成参数值
/// 吞下，会把后面的 `realm` 一路吃进一个不存在的键里。
/// @param text 剩余文本。
/// @param at 游标；成功时前移到闭合引号之后。
/// @param value 输出：反转义后的参数值。
/// @return 以 `"` 起、以 `"` 止时为 true。
[[nodiscard]] auto take_quoted(std::string_view text, std::size_t &at, std::string &value) -> bool {
    if (at >= text.size() || text.at(at) != '"') {
        return false;
    }
    ++at;
    value.clear();
    while (at < text.size()) {
        const char c = text.at(at);
        if (c == '\\' && at + 1 < text.size()) {
            value.push_back(text.at(at + 1));  // 反斜杠只表达「下一枚字符按字面读」，本身不入值
            at += 2;
            continue;
        }
        if (c == '"') {
            ++at;
            return true;
        }
        value.push_back(c);
        ++at;
    }
    return false;
}

/// @brief 把方案名归一到档位（ASCII 大小写不敏感，`RFC 7235 §2.1` 的 auth-scheme 形态）。
/// @param name 线上原文的方案名。
/// @return 命中本库实现的两枚之一，或 `Unsupported`。
[[nodiscard]] auto scheme_of(std::string_view name) -> aurora::net::http::AuthScheme {
    const auto lowered = to_lower(name);
    if (lowered == "basic") {
        return aurora::net::http::AuthScheme::Basic;
    }
    if (lowered == "bearer") {
        return aurora::net::http::AuthScheme::Bearer;
    }
    return aurora::net::http::AuthScheme::Unsupported;
}

}  // namespace

auto parse_auth_mechanism(std::string_view value) -> std::optional<aurora::net::http::AuthMechanism> {
    if (value.size() > AURORA_AUTH_FIELD_MAX_BYTES) {
        return std::nullopt;
    }
    const std::string_view body = trim(value);
    if (body.empty()) {
        return std::nullopt;
    }
    std::size_t at = 0;
    const auto name = take_token(body, at);
    if (name.empty()) {
        return std::nullopt;  // 首字符不是 token：整条读不成
    }
    aurora::net::http::AuthMechanism mechanism;
    mechanism.scheme = scheme_of(name);
    mechanism.name = std::string{name};

    while (at < body.size()) {
        at = body.find_first_not_of(" \t,", at);
        if (at == std::string_view::npos) {
            break;
        }
        const auto key = take_token(body, at);
        at = body.find_first_not_of(" \t", at);
        if (key.empty() || at == std::string_view::npos || body.at(at) != '=') {
            break;  // 参数表读到这里就断了：尾部残段丢弃，已解析的参数照旧有效
        }
        ++at;
        at = body.find_first_not_of(" \t", at);
        if (at == std::string_view::npos) {
            break;
        }
        auto text = std::string{};
        if (body.at(at) == '"') {
            if (!take_quoted(body, at, text)) {
                return std::nullopt;  // 引号没闭合：这条挑战不可解析，不是「少了几个参数」
            }
        } else {
            text = std::string{take_token(body, at)};
            if (text.empty()) {
                break;
            }
        }
        const std::string lowered = to_lower(key);
        if (lowered == "realm") {
            // `realm` 只在它单列的那一枚字段上说话：第二枚既不改写第一枚（「取首个」），也不降级成一枚
            // 普通参数——公共面明说 `parameter("realm")` 不是它的去处，留进参数表就是给调用方多造一条
            // 与事实不同的读法。
            if (mechanism.realm.empty()) {
                mechanism.realm = std::move(text);
            }
            continue;
        }
        const auto duplicate =
            std::ranges::any_of(mechanism.parameters, [&lowered](const auto &entry) { return entry.first == lowered; });
        if (!duplicate) {
            mechanism.parameters.emplace_back(lowered, std::move(text));
        }
    }
    return mechanism;
}

auto parse_auth_mechanisms(const std::vector<std::string> &values) -> std::vector<aurora::net::http::AuthMechanism> {
    std::vector<aurora::net::http::AuthMechanism> mechanisms;
    mechanisms.reserve(values.size());
    for (const auto &value : values) {
        if (auto parsed = parse_auth_mechanism(value); parsed) {
            mechanisms.push_back(std::move(*parsed));
        }
    }
    return mechanisms;
}

auto base64_encode(std::string_view input) -> std::string {
    constexpr std::string_view table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(input.at(index)));
        const std::size_t available = input.size() - index;
        const auto second = available > 1 ? static_cast<std::uint32_t>(static_cast<unsigned char>(input.at(index + 1)))
                                          : static_cast<std::uint32_t>(0);
        const auto third = available > 2 ? static_cast<std::uint32_t>(static_cast<unsigned char>(input.at(index + 2)))
                                         : static_cast<std::uint32_t>(0);
        const auto group = (first << 16) | (second << 8) | third;
        out.push_back(table.at((group >> 18) & 0x3FU));
        out.push_back(table.at((group >> 12) & 0x3FU));
        out.push_back(available > 1 ? table.at((group >> 6) & 0x3FU) : '=');
        out.push_back(available > 2 ? table.at(group & 0x3FU) : '=');
    }
    return out;
}

}  // namespace aurora::net::detail

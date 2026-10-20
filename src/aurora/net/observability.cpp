/// @file observability.cpp
/// @brief 凭据分级与进程级导出策略的实现：字段呈现与 URL 折叠。
///
/// 分档由库裁定：键名字里能认出凭据的，无论调用方声明哪一档都上调为 `Secret`；否则沿用调用方
/// 声明的档位（调用方只会是库内导出点，声明即已按字段表评估过）。`public` 档在 `present()` 抛出
/// 时仍回原值——它本就没有「折叠形态」可回退；`Sensitive` 与 `Secret` 回退到 `"***"` 与各自的
/// 结构化折叠。

#include "aurora/net/observability.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/url.h"

namespace aurora::net {
namespace {

constexpr std::string_view AURORA_FOLDED = "***";
constexpr std::string_view AURORA_UNPARSABLE_URL = "<unparsable-url>";

/// @brief 进程级策略槽；读写都持这把锁，调用策略本体时已释放。
std::mutex &policy_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<const ExportPolicy> &policy_slot() {
    static std::shared_ptr<const ExportPolicy> policy;
    return policy;
}

/// @brief 取当前策略的共享持有（一段临时强引用，调用期间策略不会析构）。
auto current_policy() -> std::shared_ptr<const ExportPolicy> {
    const std::scoped_lock guard(policy_mutex());
    return policy_slot();
}

auto ascii_lower(std::string_view text) -> std::string {
    std::string out{text};
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

auto contains_ci(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

/// @brief 头部名是否属凭据类：四枚固定名，或名字含 token / secret / password / api-key。
auto is_secret_header(std::string_view lower_name) -> bool {
    if (lower_name == "authorization" || lower_name == "proxy-authorization" || lower_name == "cookie" ||
        lower_name == "set-cookie") {
        return true;
    }
    return contains_ci(lower_name, "token") || contains_ci(lower_name, "secret") ||
           contains_ci(lower_name, "password") || contains_ci(lower_name, "api-key") ||
           contains_ci(lower_name, "api_key");
}

/// @brief 查询参数名（解码态、小写、`-` 归一为 `_`）是否属令牌名单。
auto is_token_name(std::string_view lower_name) -> bool {
    std::string normalized{lower_name};
    std::replace(normalized.begin(), normalized.end(), '-', '_');
    return normalized == "token" || normalized == "access_token" || normalized == "refresh_token" ||
           normalized == "id_token" || normalized == "api_key" || normalized == "key" || normalized == "secret" ||
           normalized == "signature" || normalized == "sig" || normalized == "session" || normalized == "password" ||
           normalized == "code";
}

/// @brief 库侧分级：键名字能认出凭据的恒为 Secret，其余沿用调用方声明。
auto effective_class(std::string_view key, FieldClass declared) -> FieldClass {
    if (key == "userinfo") {
        return FieldClass::Secret;
    }
    if (key.starts_with("header.")) {
        if (is_secret_header(ascii_lower(key.substr(7)))) {
            return FieldClass::Secret;
        }
        return declared;
    }
    if (key.starts_with("query.")) {
        const std::string_view raw_name = key.substr(6);
        const auto decoded = percent_decode(raw_name);
        if (!decoded) {
            // 名字解不开就无法证明它不是令牌名：按最保守的一档处理。
            return FieldClass::Secret;
        }
        return is_token_name(ascii_lower(decoded.value())) ? FieldClass::Secret : declared;
    }
    return declared;
}

/// @brief 认证类头部保留方案与凭据长度；其余 Secret 与用户信息段一律 `"***"`。
auto fold_secret(std::string_view key, std::string_view value) -> std::string {
    if (key.starts_with("header.")) {
        const std::string name = ascii_lower(key.substr(7));
        if (name == "authorization" || name == "proxy-authorization") {
            const std::size_t space = value.find(' ');
            if (space == std::string_view::npos || space == 0) {
                return std::string{AURORA_FOLDED};
            }
            std::string out{value.substr(0, space)};
            out += " len=";
            out += std::to_string(value.size() - space - 1);
            return out;
        }
        return std::string{AURORA_FOLDED};
    }
    if (key.starts_with("query.")) {
        std::string out{key.substr(6)};
        out += "=***";
        return out;
    }
    return std::string{AURORA_FOLDED};
}

/// @brief 折叠一段查询串：令牌参数 `name=***`、其余参数交 `present_field()` 决定，段序与定界符原样。
auto present_query(ExportSurface surface, std::string_view query) -> std::string {
    std::string out;
    std::size_t begin = 0;
    while (begin <= query.size()) {
        const std::size_t amp = query.find('&', begin);
        const std::size_t end = amp == std::string_view::npos ? query.size() : amp;
        const std::string_view segment = query.substr(begin, end - begin);
        if (!segment.empty()) {
            const std::size_t eq = segment.find('=');
            if (eq == std::string_view::npos) {
                // 无值的裸参数没有可泄漏的内容，原样保留形态。
                out += segment;
            } else {
                const std::string name{segment.substr(0, eq)};
                const std::string key = "query." + name;
                if (effective_class(key, FieldClass::Sensitive) == FieldClass::Secret) {
                    out += fold_secret(key, segment.substr(eq + 1));
                } else {
                    out += name;
                    out.push_back('=');
                    out += present_field(surface, key, FieldClass::Sensitive, segment.substr(eq + 1));
                }
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        out.push_back('&');
        begin = amp + 1;
    }
    return out;
}

}  // namespace

auto install_export_policy(std::shared_ptr<const ExportPolicy> policy) -> void {
    const std::scoped_lock guard(policy_mutex());
    policy_slot() = std::move(policy);
}

auto export_policy() -> std::shared_ptr<const ExportPolicy> { return current_policy(); }

auto present_field(ExportSurface surface, std::string_view key, FieldClass field_class, std::string_view value)
    -> std::string {
    const auto policy = current_policy();
    const FieldClass resolved = effective_class(key, field_class);
    std::string fallback;
    std::string base;
    switch (resolved) {
        case FieldClass::Public:
            base.assign(value);
            fallback = base;
            break;
        case FieldClass::Sensitive: {
            fallback = AURORA_FOLDED;
            bool allowed = false;
            if (policy) {
                try {
                    allowed = policy->allows(surface, key, resolved);
                } catch (...) {
                    allowed = false;
                }
            }
            base = allowed ? std::string{value} : fallback;
            break;
        }
        case FieldClass::Secret:
            fallback = fold_secret(key, value);
            base = fallback;
            break;
    }
    if (!policy) {
        return base;
    }
    try {
        return policy->present(surface, key, resolved, base);
    } catch (...) {
        return fallback;
    }
}

auto present_url(ExportSurface surface, std::string_view url) -> std::string {
    auto parsed = Url::parse(url);
    if (!parsed) {
        return std::string{AURORA_UNPARSABLE_URL};
    }
    const Url &target = parsed.value();
    std::string out;
    if (target.is_absolute()) {
        out += target.scheme();
        out += "://";
        if (target.host_kind() == UrlHostKind::Ipv6) {
            out.push_back('[');
            out += target.host();
            out.push_back(']');
        } else {
            out += target.host();
        }
        if (target.port()) {
            out.push_back(':');
            out += std::to_string(*target.port());
        }
    }
    out += target.path();
    if (target.has_query()) {
        out.push_back('?');
        out += present_query(surface, target.query());
    }
    return out;
}

}  // namespace aurora::net

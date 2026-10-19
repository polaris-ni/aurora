#include "http_cookie_jar.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "aurora/net/detail/http_retry.h"

namespace aurora::net::detail {
namespace {

/// @brief ASCII 大小写折叠（域与属性名的比较基准；只折字母，其余字节不动）。
auto ascii_lower(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// @brief 剥 OWS（`RFC 6265 §5.2` 的 trim 口径）。
auto trim_ows(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 主机是否数字/十六进制/冒号形态（近似 IP 判定，供 domain-match 的「排除 IP」半边用）。
///
/// `RFC 6265 §5.1.3` 要求「字符串等价或以 `.域` 结尾且主机不是 IP」。完整 IP 文法解析不必要：一个
/// 只含数字、点、冒号与十六进制字符的主机必然不是注册名，按 IP 处理即关闭后缀匹配——宁可少匹配
/// （表现为主机只有全等）也不会错匹配（`1.2.3.4` 与 `2.3.4` 那类）。
auto looks_like_ip_literal(std::string_view host) -> bool {
    if (host.empty()) {
        return false;
    }
    const auto allowed = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':' ||
               c == '%';
    };
    return std::ranges::all_of(host, allowed);
}

/// @brief `RFC 6265 §5.1.3` 的 domain-match：字符串全等，或主机以 `.域` 结尾且主机不是 IP 形态。
auto domain_matches(std::string_view host, std::string_view domain) -> bool {
    if (host == domain) {
        return true;
    }
    if (looks_like_ip_literal(host)) {
        return false;
    }
    if (host.size() <= domain.size() || host[host.size() - domain.size() - 1] != '.') {
        return false;
    }
    return host.substr(host.size() - domain.size()) == domain;
}

/// @brief `RFC 6265 §5.1.4` 的 path-match：全等，或路径以 cookie 路径为前缀且边界是 `/`。
auto path_matches(std::string_view request_path, std::string_view cookie_path) -> bool {
    if (request_path == cookie_path) {
        return true;
    }
    if (request_path.size() < cookie_path.size() || !request_path.starts_with(cookie_path)) {
        return false;
    }
    return cookie_path.back() == '/' || request_path[cookie_path.size()] == '/';
}

/// @brief `RFC 6265 §5.1.4` 的 default-path 算法：取请求路径去掉最右一段（含那段之前的 `/`）。
auto default_path_of(std::string_view uri_path) -> std::string {
    if (uri_path.empty() || uri_path.front() != '/') {
        return "/";
    }
    const auto last_slash = uri_path.rfind('/');
    if (last_slash == 0) {
        return "/";
    }
    return std::string{uri_path.substr(0, last_slash)};
}

}  // namespace

/// @brief 解析一枚 `Set-Cookie` 值（`RFC 6265 §5.2`；`Expires` / `Max-Age` 并给时 `Max-Age` 优先）。
/// @param set_cookie_value 单枚字段值（调用方逐条给，不经折叠）。
/// @param target 本跳目标（域与缺省路径的基准）。
/// @param now 现在的时刻（`Expires` 的比较基准）。
/// @return 解析产物；畸形即 `nullopt`（整条丢弃，不给调用方造一次失败）。
auto parse_set_cookie(std::string_view set_cookie_value, const aurora::Url &target,
                      std::chrono::system_clock::time_point now) -> std::optional<HttpCookie> {
    const auto semicolon = set_cookie_value.find(';');
    const auto pair = semicolon == std::string_view::npos ? set_cookie_value : set_cookie_value.substr(0, semicolon);
    const auto equals = pair.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return std::nullopt;  // 无 `=` 或空名：整条丢弃
    }
    HttpCookie cookie;
    cookie.name = std::string{trim_ows(pair.substr(0, equals))};
    cookie.value = std::string{trim_ows(pair.substr(equals + 1))};
    if (cookie.name.empty()) {
        return std::nullopt;
    }

    bool has_max_age = false;
    long long max_age_seconds = 0;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::string_view rest =
        semicolon == std::string_view::npos ? std::string_view{} : set_cookie_value.substr(semicolon + 1);
    while (!rest.empty()) {
        const auto next = rest.find(';');
        const auto attribute = next == std::string_view::npos ? rest : rest.substr(0, next);
        rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
        const auto split = attribute.find('=');
        const auto attribute_name =
            ascii_lower(trim_ows(split == std::string_view::npos ? attribute : attribute.substr(0, split)));
        const auto attribute_value =
            trim_ows(split == std::string_view::npos ? std::string_view{} : attribute.substr(split + 1));
        if (attribute_name == "secure") {
            cookie.secure = true;
        } else if (attribute_name == "httponly") {
            cookie.http_only = true;
        } else if (attribute_name == "domain") {
            auto domain = ascii_lower(attribute_value);
            if (!domain.empty() && domain.front() == '.') {
                domain.erase(0, 1);  // 前导点按 `§5.2.3` 置空
            }
            if (domain.empty()) {
                continue;  // 空域：按「属性没给」处理
            }
            // `§5.2.3`：属性域必须与规范化请求主机 domain-match，否则整条 cookie 丢弃。
            const std::string request_host = ascii_lower(target.host());
            if (!domain_matches(request_host, domain)) {
                return std::nullopt;
            }
            cookie.domain = std::move(domain);
            cookie.host_only = false;
        } else if (attribute_name == "path") {
            // `§5.2.4`：空值或不以 `/` 开头按 default-path 处理。
            if (!attribute_value.empty() && attribute_value.front() == '/') {
                cookie.path = std::string{attribute_value};
            }
        } else if (attribute_name == "expires") {
            expires_at = parse_http_date(attribute_value);  // 解析失败按「属性没给」（§5.2.1）
        } else if (attribute_name == "max-age") {
            // `§5.2.2`：首个字符必须是数字或 `-`，其余必须是数字；不合文法忽略本属性。
            if (!attribute_value.empty()) {
                const std::string_view digits =
                    attribute_value.front() == '-' ? attribute_value.substr(1) : attribute_value;
                if (!digits.empty() && std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; })) {
                    max_age_seconds = std::stoll(std::string{attribute_value});
                    has_max_age = true;
                }
            }
        }
        // 其余属性（含 `SameSite`）忽略：本 Jar 不做跨站模型。
    }

    // 过期序：`Max-Age` 优先于 `Expires`（§5.3 第 3 条）。两者都无 = 会话 cookie（内存 Jar 的会话即 Jar 生）。
    if (has_max_age) {
        if (max_age_seconds <= 0) {
            return std::nullopt;  // `Max-Age <= 0` 即「立即过期」：直接丢弃与存进去等价且更省
        }
        cookie.expiry = now + std::chrono::seconds{max_age_seconds};
    } else if (expires_at.has_value()) {
        if (*expires_at <= now) {
            return std::nullopt;  // 已过期：同理直接丢弃
        }
        cookie.expiry = *expires_at;
    }
    if (cookie.path.empty()) {
        cookie.path = default_path_of(target.path());
    }
    if (cookie.domain.empty()) {
        cookie.domain = ascii_lower(target.host());
    }
    return cookie;
}

auto HttpCookieJar::store_from_response(const aurora::Url &target, const aurora::Headers &response_fields,
                                        bool secure_channel, std::chrono::system_clock::time_point now) -> void {
    const auto fields = response_fields.values("Set-Cookie");
    if (fields.empty()) {
        return;
    }
    const std::scoped_lock guard{mutex_};
    for (const auto &field : fields) {
        auto parsed = parse_set_cookie(field, target, now);
        if (!parsed.has_value()) {
            continue;  // 畸形丢弃：不给调用方造一次失败
        }
        if (parsed->secure && !secure_channel) {
            continue;  // `Secure` cookie 不上明文信道（存侧半边）
        }
        parsed->created = now;
        // 去重替换（§5.3 第 11 条）：同 (名, 域, host-only, 路径) 视为同一枚，替换内容、保留创建时刻。
        auto existing = std::ranges::find_if(cookies_, [&](const HttpCookie &entry) {
            return entry.name == parsed->name && entry.domain == parsed->domain &&
                   entry.host_only == parsed->host_only && entry.path == parsed->path;
        });
        if (existing != cookies_.end()) {
            parsed->created = existing->created;
            *existing = std::move(*parsed);
            continue;
        }
        // 上限逐出：先清过期，仍满再按创建时刻逐出最老的。
        std::erase_if(cookies_,
                      [&](const HttpCookie &entry) { return entry.expiry.has_value() && *entry.expiry <= now; });
        if (cookies_.size() >= AURORA_COOKIE_JAR_MAX) {
            auto oldest = std::ranges::min_element(
                cookies_, [](const HttpCookie &a, const HttpCookie &b) { return a.created < b.created; });
            cookies_.erase(oldest);
        }
        cookies_.push_back(std::move(*parsed));
    }
}

auto HttpCookieJar::cookies_for(const aurora::Url &target, std::chrono::system_clock::time_point now) const
    -> std::string {
    const std::scoped_lock guard{mutex_};
    const std::string host = ascii_lower(target.host());
    const std::string_view request_path = target.path();
    std::vector<const HttpCookie *> matched;
    for (const auto &entry : cookies_) {
        if (entry.expiry.has_value() && *entry.expiry <= now) {
            continue;  // 过期条目取侧跳过（清除留给写侧顺手做）
        }
        if (entry.secure && !target.is_secure()) {
            continue;  // `Secure` cookie 不上明文目标（取侧半边）
        }
        const bool domain_ok = entry.host_only ? host == entry.domain : domain_matches(host, entry.domain);
        if (!domain_ok || !path_matches(request_path, entry.path)) {
            continue;
        }
        matched.push_back(&entry);
    }
    // `§5.4`：路径长者在前；同长按创建时刻早者在前。
    std::ranges::sort(matched, [](const HttpCookie *a, const HttpCookie *b) {
        if (a->path.size() != b->path.size()) {
            return a->path.size() > b->path.size();
        }
        return a->created < b->created;
    });
    std::string header;
    for (const auto *pick : matched) {
        if (!header.empty()) {
            header += "; ";
        }
        header += pick->name;
        header += '=';
        header += pick->value;
    }
    return header;
}

auto HttpCookieJar::size() const -> std::size_t {
    const std::scoped_lock guard{mutex_};
    return cookies_.size();
}

auto HttpCookieJar::clear() -> void {
    const std::scoped_lock guard{mutex_};
    cookies_.clear();
}

}  // namespace aurora::net::detail

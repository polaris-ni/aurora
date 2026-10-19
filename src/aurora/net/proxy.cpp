/// @brief 代理配置解析、`NO_PROXY` 例外匹配与环境变量选择次序的实现。
/// @file src/aurora/net/proxy.cpp
///
/// 全文件是纯文本/数值逻辑，零平台头、零 socket、零 DNS：本层唯一职责是把「配置文本」变成
/// 「可判定的规则集合」，握手字节不在这里。三条落码纪律：
///
/// - **不猜语义**：文法之外的形态（`10.0.0.0/33`、没加括号的 IPv6 例外项、带路径的代理 URL）
///   一律报错，错误 message 里带上是哪个变量或第几条规则。宽松解析在代理配置上的代价是
///   「本该直连的内网流量翻了出去的代理」，这类故障在日志里几乎不可见。
/// - **规范化只有一份**：主机文本一律经 `Url` / `Endpoint` 的既有口径规范化（小写、去尾点、
///   IPv6 去括号），避免同一台代理因拼写不同而判成两台。
/// - **凭据只在内存里**：解析出的 userinfo 立即百分号解码，`to_string()` 出口恒掩码，
///   本层不做任何 base64 或头组装。

#include "aurora/net/proxy.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/address.h"
#include "aurora/net/detail/system_proxy.h"

namespace aurora::net {
namespace {

/// @brief 构造 `net-proxy-config-invalid`（与 `net-endpoint-invalid` 同一 message 形态）。
/// @param detail 被拒的原因片段（ASCII，可含原始输入回显）。
/// @return 表驱动 Error。
auto config_error(std::string detail) -> Error {
    return make_error(ErrorCode::NetProxyConfigInvalid, "detail=" + std::move(detail));
}

/// @brief 构造 `net-proxy-unsupported`：scheme 认得但本批次不实现。
/// @param detail 说明哪个形态不支持。
/// @return 表驱动 Error。
auto unsupported_error(std::string detail) -> Error {
    return make_error(ErrorCode::NetProxyUnsupported, "detail=" + std::move(detail));
}

/// @brief 去掉内层错误 message 的 `detail=` 前缀，便于拼上层语境。
/// @param message 内层错误 message（本层与 `Url` / `Endpoint` 的错误都是这一形态）。
/// @return 去掉前缀后的原因文本。
auto strip_detail(std::string_view message) -> std::string {
    constexpr std::string_view prefix = "detail=";
    return std::string(message.starts_with(prefix) ? message.substr(prefix.size()) : message);
}

/// @brief 把内层失败（`Url` / `Endpoint` 的解析错误）折算成本层错误码并加语境。
///
/// 折算而非透传是刻意的：调用方要知道的是「这条代理配置不能用」，而内层码只是本层内部
/// 拿 URL 文法当校验工具的结果。原因文本原样保留在 `detail=` 里，不损失诊断力。
/// @param inner 内层错误。
/// @param context 语境前缀（变量名、第几条规则之类）；非空时以 `": "` 连接原因。
/// @return `net-proxy-config-invalid` 的 Error。
auto config_error_from(const Error &inner, std::string_view context) -> Error {
    std::string detail(context);
    if (!detail.empty()) {
        detail += ": ";
    }
    detail += strip_detail(inner.message);
    return config_error(std::move(detail));
}

/// @brief 单字节 ASCII 空白判定（不依赖 locale）。
/// @param c 原始字节。
/// @return 空格、制表、换行、垂直制表、换页或回车时为 true。
constexpr auto is_space(unsigned char c) -> bool {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/// @brief 去掉首尾 ASCII 空白。
/// @param text 原始文本。
/// @return 修剪后的视图。
auto trim(std::string_view text) -> std::string_view {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// @brief ASCII 小写化副本（非大写字母原样保留）。
/// @param text 原始文本。
/// @return 折叠后的副本。
auto lower_copy(std::string_view text) -> std::string {
    std::string out(text);
    for (char &ch : out) {
        const auto raw = static_cast<unsigned char>(ch);
        if (raw >= 'A' && raw <= 'Z') {
            ch = static_cast<char>(raw - 'A' + 'a');
        }
    }
    return out;
}

/// @brief 纯十进制解析：整段必须是数字，且值落在 `[0, limit]`。
/// @param text 候选文本。
/// @param limit 上界（含）。
/// @param value 输出解析值。
/// @return 全部字符为十进制数字且不越界时为 true（值为 `0` 也算成功，由调用方判语境）。
auto parse_decimal(std::string_view text, std::uint32_t limit, std::uint32_t &value) -> bool {
    if (text.empty()) {
        return false;
    }
    std::uint64_t acc = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        acc = (acc * 10U) + static_cast<std::uint64_t>(ch - '0');
        if (acc > limit) {
            return false;
        }
    }
    value = static_cast<std::uint32_t>(acc);
    return true;
}

/// @brief 取该形态的缺省端口（curl / Go 口径）。
/// @param kind 代理形态。
/// @return `HttpConnect` 80、`Socks5` 1080、`Direct` 0。
constexpr auto default_port(ProxyKind kind) -> std::uint16_t {
    switch (kind) {
        case ProxyKind::HttpConnect:
            return 80;
        case ProxyKind::Socks5:
            return 1080;
        case ProxyKind::Direct:
            break;
    }
    return 0;
}

/// @brief 序列化用的 scheme 前缀（与 `parse()` 接受的形态互逆）。
/// @param kind 代理形态。
/// @param resolve_via_proxy SOCKS5 是否交代理解析。
/// @return 不带 `://` 的 scheme 文本；`Direct` 返回 `direct`。
auto scheme_prefix(ProxyKind kind, bool resolve_via_proxy) -> std::string_view {
    switch (kind) {
        case ProxyKind::Direct:
            return "direct";
        case ProxyKind::HttpConnect:
            return "http";
        case ProxyKind::Socks5:
            return resolve_via_proxy ? "socks5h" : "socks5";
    }
    return "direct";
}

/// @brief 校验并规范化代理主机文本（借用 `Url` 的既有口径，避免第二套主机文法）。
/// @param host 候选主机：注册名、IPv4，或 IPv6 字面量（裸写或带括号都可）。
/// @param context 错误原因前缀。
/// @return 成功为规范化后的主机（ASCII 小写、IPv6 不含括号）；空主机或文法畸形为错误。
auto normalize_proxy_host(std::string_view host, std::string_view context) -> Result<std::string> {
    if (host.empty()) {
        return config_error(std::string(context) + ": proxy host is empty");
    }
    const auto url = Url::from_parts("http", host, std::nullopt, {});
    if (!url) {
        return config_error_from(url.error(), context);
    }
    if (url.value().host().empty()) {
        return config_error(std::string(context) + ": proxy host is empty");
    }
    return std::string(url.value().host());
}

/// @brief 按第一个 `:` 拆 userinfo 为解码态凭据。
/// @param userinfo URL 的 userinfo 段（编码态，不含 `@`）。
/// @param context 错误原因前缀。
/// @return 成功为凭据（有 userinfo 段时用户名非空）；编码畸形或用户名为空为对应错误。
auto credentials_from_userinfo(std::string_view userinfo, std::string_view context) -> Result<ProxyCredentials> {
    if (userinfo.empty()) {
        return ProxyCredentials{};
    }
    const std::size_t colon = userinfo.find(':');
    const auto raw_name = userinfo.substr(0, colon);
    const auto raw_password = colon == std::string_view::npos ? std::string_view{} : userinfo.substr(colon + 1);
    const auto name = percent_decode(raw_name);
    if (!name) {
        return config_error_from(name.error(), context);
    }
    const auto password = percent_decode(raw_password);
    if (!password) {
        return config_error_from(password.error(), context);
    }
    if (name.value().empty()) {
        return config_error(std::string(context) + ": proxy userinfo has an empty username");
    }
    return ProxyCredentials{.username = name.value(), .password = password.value()};
}

/// @brief 判断主机文本是否为数字字面量，并给出规范化写法。
/// @param host 候选主机文本（可带括号）。
/// @param normalized 输出规范化后的字面量（仅成功时写）。
/// @return 是 IPv4 / IPv6 字面量时为 true。
auto as_numeric_literal(std::string_view host, std::string &normalized) -> bool {
    const auto endpoint = Endpoint::from_ip(host, 0);
    if (!endpoint || endpoint.value().is_local_path()) {
        return false;
    }
    normalized = endpoint.value().host();
    return true;
}

/// @brief 规范化例外规则里的主机文本：小写、去 `*.` 或 `.` 前缀、去尾点。
/// @param host 候选主机文本（不含端口与括号）。
/// @return 用于比较的规范化文本；可能为空（调用方判空）。
auto normalize_domain_rule(std::string_view host) -> std::string {
    std::string out = lower_copy(trim(host));
    if (out.starts_with("*.")) {
        out.erase(0, 2);
    } else if (out.starts_with('.')) {
        out.erase(0, 1);
    }
    while (out.ends_with('.')) {
        out.pop_back();
    }
    return out;
}

/// @brief 规范化待匹配目标主机：小写、去 IPv6 括号、去尾点。
/// @param host 目标主机文本。
/// @return 规范化副本。
auto normalize_target_host(std::string_view host) -> std::string {
    std::string out = lower_copy(trim(host));
    if (out.starts_with('[')) {
        const std::size_t close = out.find(']');
        if (close != std::string::npos && close >= 1U) {
            out.erase(0, 1);
            if (out.ends_with(']')) {
                out.pop_back();
            }
        }
    }
    while (out.ends_with('.')) {
        out.pop_back();
    }
    return out;
}

/// @brief 算出第 `index` 字节在该前缀长度下的网络位掩码。
/// @param prefix_bits 前缀长度。
/// @param index 字节下标（从 0 起）。
/// @return 该字节里属于网络前缀的比特为 1。
constexpr auto prefix_mask(std::uint8_t prefix_bits, std::size_t index) -> std::uint8_t {
    const std::uint32_t bits = prefix_bits;
    const std::uint32_t high =
        bits >= (index + 1U) * 8U ? 8U : (bits <= index * 8U ? 0U : bits - static_cast<std::uint32_t>(index * 8U));
    return high == 0U ? static_cast<std::uint8_t>(0x00) : static_cast<std::uint8_t>(~(0xFFU >> high));
}

/// @brief 拆出例外列表一项目的主机与端口限定。
/// @param entry 单项文本（已 trim、非空、不含 `/`）。
/// @param host_part 输出主机文本（IPv6 已去括号）。
/// @param port 输出端口限定；`0` = 未限定。
/// @return 成功为 true；括号不配对、端口畸形或裸 IPv6 带冒号时为 false。
auto split_exception_entry(std::string_view entry, std::string_view &host_part, std::uint16_t &port) -> bool {
    port = 0;
    std::uint32_t value = 0;
    if (entry.starts_with('[')) {
        const std::size_t close = entry.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host_part = entry.substr(1, close - 1);
        const std::string_view rest = entry.substr(close + 1);
        if (host_part.empty()) {
            return false;
        }
        if (rest.empty()) {
            return true;
        }
        if (rest.front() != ':' || !parse_decimal(rest.substr(1), 65535U, value) || value == 0U) {
            return false;
        }
        port = static_cast<std::uint16_t>(value);
        return true;
    }
    const std::size_t colon = entry.find(':');
    if (colon == std::string_view::npos) {
        host_part = entry;
        return true;
    }
    host_part = entry.substr(0, colon);
    // 第二段冒号只可能是没加括号的 IPv6 字面量——本层不猜，直接拒。
    if (entry.substr(colon + 1).find(':') != std::string_view::npos || host_part.empty() ||
        !parse_decimal(entry.substr(colon + 1), 65535U, value) || value == 0U) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

/// @brief 解析 CIDR 形态的例外项（`10.0.0.0/8`、`fd00::/8`、`0.0.0.0/0`）。
///
/// 只给原因片段，不重复回显项文本——上层 `NoProxy::parse` 统一加上「第几条 + 原文」语境。
/// @param entry 单项文本（含 `/`）。
/// @return 成功为 `Cidr` 规则；前缀越界、主机位非零或地址畸形为 `net-proxy-config-invalid`。
auto parse_cidr_entry(std::string_view entry) -> Result<NoProxy::Rule> {
    const std::size_t slash = entry.find('/');
    std::uint32_t bits = 0;
    if (!parse_decimal(entry.substr(slash + 1), 128U, bits)) {
        return config_error("invalid CIDR prefix length");
    }
    const auto endpoint = Endpoint::from_ip(entry.substr(0, slash), 0);
    if (!endpoint) {
        return config_error_from(endpoint.error(), "invalid CIDR address");
    }
    const Endpoint &value = endpoint.value();
    const std::uint32_t limit = value.family() == AddressFamily::IPv4 ? 32U : 128U;
    if (bits > limit) {
        return config_error("invalid CIDR: prefix is wider than the address family");
    }
    const auto &bytes = value.address_bytes();
    const std::size_t width = value.family() == AddressFamily::IPv4 ? 4U : 16U;
    const auto rule_bits = static_cast<std::uint8_t>(bits);
    for (std::size_t index = 0; index < width; ++index) {
        if ((bytes.at(index) & static_cast<std::uint8_t>(~prefix_mask(rule_bits, index))) != 0U) {
            return config_error("invalid CIDR: host bits are set");
        }
    }
    NoProxy::Rule rule;
    rule.kind = NoProxy::Rule::Kind::Cidr;
    rule.family = value.family();
    rule.network = bytes;
    rule.prefix_bits = rule_bits;
    return rule;
}

/// @brief 判断 CIDR 规则是否覆盖目标地址。
/// @param rule CIDR 规则。
/// @param host 目标主机文本（已规范化）。
/// @return 目标是同族数字字面量且落在前缀内时为 true。
auto cidr_covers(const NoProxy::Rule &rule, std::string_view host) -> bool {
    std::string literal;
    if (!as_numeric_literal(host, literal)) {
        return false;
    }
    const auto endpoint = Endpoint::from_ip(literal, 0);
    if (!endpoint || endpoint.value().family() != rule.family) {
        return false;
    }
    const auto &bytes = endpoint.value().address_bytes();
    const std::size_t width = rule.family == AddressFamily::IPv4 ? 4U : 16U;
    for (std::size_t index = 0; index < width; ++index) {
        const std::uint8_t mask = prefix_mask(rule.prefix_bits, index);
        if ((bytes.at(index) & mask) != (rule.network.at(index) & mask)) {
            return false;
        }
    }
    return true;
}

/// @brief 判断主机是否落在域名规则的域内（本体或任意深层子域）。
/// @param domain 规则域名（已规范化，不含前导点）。
/// @param host 目标主机（已规范化）。
/// @return 全等或以 `.domain` 结尾时为 true。
auto domain_contains(std::string_view domain, std::string_view host) -> bool {
    if (host == domain) {
        return true;
    }
    if (host.size() <= domain.size() + 1U) {
        return false;
    }
    const std::size_t offset = host.size() - domain.size() - 1U;
    return host[offset] == '.' && host.substr(offset + 1) == domain;
}

/// @brief 读取一个环境变量的有效值（大写形态优先，空值视为未设置）。
/// @param reader 注入的读取器。
/// @param upper 大写变量名。
/// @param lower 小写变量名。
/// @param name 输出实际命中的变量名。
/// @return 非空值；两者都未设置或都为空时为 `nullopt`。
auto read_variable(const ProxyEnvironment::Reader &reader, std::string_view upper, std::string_view lower,
                   std::string &name) -> std::optional<std::string> {
    for (const std::string_view candidate : {upper, lower}) {
        const auto raw = reader(candidate);
        if (raw && !trim(*raw).empty()) {
            name = std::string(candidate);
            return raw;
        }
    }
    return std::nullopt;
}

/// @brief 读取并解析一个代理类环境变量。
/// @param reader 注入的读取器。
/// @param upper 大写变量名。
/// @param lower 小写变量名。
/// @return 成功为「未设置（`nullopt`）或已解析配置」，其 `source()` 填命中的变量名；
///         变量存在但畸形为对应错误。
auto load_proxy_variable(const ProxyEnvironment::Reader &reader, std::string_view upper, std::string_view lower)
    -> Result<std::optional<ProxyConfig>> {
    std::string name;
    const auto raw = read_variable(reader, upper, lower, name);
    if (!raw) {
        return std::optional<ProxyConfig>{};
    }
    const auto parsed = ProxyConfig::parse(*raw, name);
    if (!parsed) {
        // 变量存在但写错：错误里必须带上是哪个变量，否则运维无从定位。
        return config_error_from(parsed.error(), name);
    }
    return std::optional<ProxyConfig>{parsed.value()};
}

/// @brief 进程环境变量的默认读取器。
/// @return 读取器：变量不存在或值为空时返回 `nullopt`（空串按未设置处理是 curl / Go 的共同口径）。
auto default_reader() -> ProxyEnvironment::Reader {
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv 在 MSVC/clang-cl 下被标为"不安全"，但它是标准可移植接口
#endif
    return [](std::string_view name) -> std::optional<std::string> {
        const auto *raw = std::getenv(std::string(name).c_str());
        if (raw == nullptr) {
            return std::nullopt;
        }
        return std::string(raw);
    };
#if defined(AURORA_COMPILER_MSVC) || defined(AURORA_COMPILER_CLANG_CL)
#pragma warning(pop)
#endif
}

/// @brief 判断集合是否已经由环境变量表达过代理意图（任一代理类变量存在即为「接管」）。
/// @param env 环境变量读出的集合。
/// @return `HTTP_PROXY` / `HTTPS_PROXY` / `ALL_PROXY` 任一有值为 true。
auto takes_over(const ProxyEnvironment &env) -> bool {
    return env.http.has_value() || env.https.has_value() || env.all.has_value();
}

}  // namespace

auto ProxyCredentials::operator==(const ProxyCredentials &other) const -> bool {
    return username == other.username && password == other.password;
}

auto ProxyConfig::direct() -> ProxyConfig { return ProxyConfig{}; }

auto ProxyConfig::parse(std::string_view text, std::string_view source) -> Result<ProxyConfig> {
    const auto trimmed = trim(text);
    if (trimmed.empty()) {
        return config_error("proxy value is empty");
    }
    ProxyKind kind = ProxyKind::HttpConnect;
    bool resolve_via_proxy = false;
    std::string normalized;
    const std::size_t separator = trimmed.find("://");
    if (separator == std::string_view::npos) {
        normalized = "http://" + std::string(trimmed);  // 裸 host[:port] 按 HTTP CONNECT 处理
    } else {
        const auto scheme = lower_copy(trimmed.substr(0, separator));
        if (scheme == "http") {
            kind = ProxyKind::HttpConnect;
        } else if (scheme == "socks5" || scheme == "socks5h") {
            kind = ProxyKind::Socks5;
            resolve_via_proxy = scheme == "socks5h";
        } else if (scheme == "https" || scheme == "socks4" || scheme == "socks4a") {
            return unsupported_error("scheme '" + scheme + "' is recognized but not implemented");
        } else {
            return config_error("unknown proxy scheme '" + scheme + "'");
        }
        normalized = std::string(trimmed);
    }

    const auto url = Url::parse(normalized);
    if (!url) {
        return config_error_from(url.error(), "proxy value '" + std::string(trimmed) + "'");
    }
    const Url &parsed = url.value();
    if (parsed.host().empty()) {
        return config_error("proxy value '" + std::string(trimmed) + "' has no host");
    }
    if (!parsed.path().empty() && parsed.path() != "/") {
        return config_error("proxy value '" + std::string(trimmed) + "' must not carry a path");
    }
    if (parsed.has_query() || parsed.has_fragment()) {
        return config_error("proxy value '" + std::string(trimmed) + "' must not carry a query or fragment");
    }
    const std::uint16_t port = parsed.port().value_or(default_port(kind));
    if (port == 0) {
        return config_error("proxy value '" + std::string(trimmed) + "' has port 0");
    }
    // `@` 在场但 userinfo 为空，是「以为填了凭据其实没填」的写法，不能当成无凭据放过。
    if (parsed.userinfo().empty() && normalized.find('@') != std::string::npos) {
        return config_error("proxy value '" + std::string(trimmed) + "' has an empty username");
    }
    const auto credentials = credentials_from_userinfo(parsed.userinfo(), "proxy userinfo");
    if (!credentials) {
        return credentials.error();
    }
    ProxyConfig config;
    config.kind_ = kind;
    config.resolve_via_proxy_ = resolve_via_proxy;
    config.port_ = port;
    config.host_ = std::string(parsed.host());
    config.credentials_ = credentials.value();
    config.source_ = std::string(trim(source));
    return config;
}

auto ProxyConfig::http_connect(std::string_view host, std::uint16_t port, const ProxyCredentials &credentials)
    -> Result<ProxyConfig> {
    return build(ProxyKind::HttpConnect, false, host, port, credentials);
}

auto ProxyConfig::socks5(std::string_view host, std::uint16_t port, bool resolve_via_proxy,
                         const ProxyCredentials &credentials) -> Result<ProxyConfig> {
    return build(ProxyKind::Socks5, resolve_via_proxy, host, port, credentials);
}

auto ProxyConfig::build(ProxyKind kind, bool resolve_via_proxy, std::string_view host, std::uint16_t port,
                        const ProxyCredentials &credentials) -> Result<ProxyConfig> {
    const auto normalized = normalize_proxy_host(host, "proxy host '" + std::string(host) + "'");
    if (!normalized) {
        return normalized.error();
    }
    ProxyConfig config;
    config.kind_ = kind;
    config.resolve_via_proxy_ = resolve_via_proxy;
    config.port_ = port == 0 ? default_port(kind) : port;
    config.host_ = normalized.value();
    config.credentials_ = credentials;
    return config;
}

auto ProxyConfig::to_string() const -> std::string {
    if (is_direct()) {
        return "direct";
    }
    std::string out(scheme_prefix(kind_, resolve_via_proxy_));
    out += "://";
    if (has_credentials()) {
        out += percent_encode(credentials_.username, PercentSet::Unreserved);
        if (!credentials_.password.empty()) {
            out += ":***";
        }
        out += "@";
    }
    if (host_.find(':') != std::string::npos) {
        out += "[" + host_ + "]";  // IPv6 字面量在 URL 形态里必须带括号
    } else {
        out += host_;
    }
    out += ":" + std::to_string(port_);
    return out;
}

auto ProxyConfig::operator==(const ProxyConfig &other) const -> bool {
    return kind_ == other.kind_ && resolve_via_proxy_ == other.resolve_via_proxy_ && port_ == other.port_ &&
           host_ == other.host_ && credentials_ == other.credentials_;
}

auto NoProxy::parse(std::string_view list) -> Result<NoProxy> {
    std::vector<Rule> rules;
    std::size_t ordinal = 0;
    // 逗号、分号与空白是等价分隔符：三种写法都在真实配置里出现，分隔符本身不携带语义故可宽容。
    const auto consume = [&rules, &ordinal](std::string_view field) -> Result<void> {
        const auto entry = trim(field);
        if (entry.empty()) {
            return Result<void>{};  // 空项忽略：`,` 收尾与 `A,,B` 都是合法书写
        }
        ++ordinal;
        const std::string context = "exception entry #" + std::to_string(ordinal) + " '" + std::string(entry) + "'";
        if (entry == "*") {
            Rule rule;
            rule.kind = Rule::Kind::Any;
            rules.push_back(rule);
            return Result<void>{};
        }
        // 两枚尖括号令牌来自系统代理的例外列表（IE / WinHTTP 与 macOS 系统配置共用这套写法），
        // 它们不是主机名，必须在本层建模——丢了 `<local>` 等于让无边局域网主机静默走代理。
        const auto token = lower_copy(entry);
        if (token == "<local>") {
            Rule rule;
            rule.kind = Rule::Kind::SingleLabel;
            rules.push_back(rule);
            return Result<void>{};
        }
        if (token == "<-loopback>") {
            for (const std::string_view cidr : {"127.0.0.0/8", "::1/128"}) {
                auto rule = parse_cidr_entry(cidr);
                if (!rule) {
                    return config_error_from(rule.error(), context);
                }
                rules.push_back(rule.value());
            }
            return Result<void>{};
        }
        if (entry.find('/') != std::string_view::npos) {
            auto cidr = parse_cidr_entry(entry);
            if (!cidr) {
                return config_error_from(cidr.error(), context);
            }
            rules.push_back(cidr.value());
            return Result<void>{};
        }
        std::string_view host_part;
        std::uint16_t port = 0;
        if (!split_exception_entry(entry, host_part, port)) {
            return config_error(context + " is neither a domain, a bracketed IPv6, nor a host:port pair");
        }
        Rule rule;
        rule.port = port;
        std::string literal;
        if (as_numeric_literal(host_part, literal)) {
            rule.kind = Rule::Kind::Literal;
            rule.domain = std::move(literal);
        } else {
            rule.kind = Rule::Kind::Domain;
            rule.domain = normalize_domain_rule(host_part);
            if (rule.domain.empty()) {
                return config_error(context + " has no host part");
            }
        }
        rules.push_back(std::move(rule));
        return Result<void>{};
    };
    std::string field;
    for (const char ch : list) {
        const auto raw = static_cast<unsigned char>(ch);
        if (raw == ',' || raw == ';' || is_space(raw)) {
            auto consumed = consume(field);
            if (!consumed) {
                return consumed.error();
            }
            field.clear();
            continue;
        }
        field.push_back(ch);
    }
    auto consumed = consume(field);
    if (!consumed) {
        return consumed.error();
    }
    NoProxy result;
    result.rules_ = std::move(rules);
    return result;
}

auto NoProxy::match_all() -> NoProxy {
    NoProxy result;
    Rule rule;
    rule.kind = Rule::Kind::Any;
    result.rules_.push_back(rule);
    return result;
}

auto NoProxy::matches(std::string_view host, std::uint16_t port) const -> bool {
    const std::string target = normalize_target_host(host);
    if (target.empty()) {
        return false;
    }
    std::string scratch;  // 数字字面量判定的复用缓冲（循环内反复问同一目标，没必要反复分配）
    const bool single_label = target.find('.') == std::string::npos && !as_numeric_literal(target, scratch);
    for (const Rule &rule : rules_) {
        const bool port_open = rule.port == 0 || rule.port == port;
        switch (rule.kind) {
            case Rule::Kind::Any:
                return true;
            case Rule::Kind::Domain:
                if (port_open && domain_contains(rule.domain, target)) {
                    return true;
                }
                break;
            case Rule::Kind::Literal:
                if (port_open && rule.domain == target) {
                    return true;
                }
                break;
            case Rule::Kind::SingleLabel:
                // 无限定域主机名：名字里既没有点、又不是数字字面量。`<local>` 的意图是
                // 「这台机器所在的无边局域网」，而无限定域名只在局域网里解析得开。
                if (port_open && single_label) {
                    return true;
                }
                break;
            case Rule::Kind::Cidr:
                if (cidr_covers(rule, target)) {
                    return true;
                }
                break;
        }
    }
    return false;
}

auto NoProxy::operator==(const NoProxy &other) const -> bool {
    if (rules_.size() != other.rules_.size()) {
        return false;
    }
    for (std::size_t index = 0; index < rules_.size(); ++index) {
        const Rule &mine = rules_[index];
        const Rule &theirs = other.rules_[index];
        if (mine.kind != theirs.kind || mine.domain != theirs.domain || mine.family != theirs.family ||
            mine.network != theirs.network || mine.prefix_bits != theirs.prefix_bits || mine.port != theirs.port) {
            return false;
        }
    }
    return true;
}

auto ProxyEnvironment::from_environment() -> Result<ProxyEnvironment> { return from_environment(default_reader()); }

auto ProxyEnvironment::from_environment(const Reader &reader) -> Result<ProxyEnvironment> {
    auto http = load_proxy_variable(reader, "HTTP_PROXY", "http_proxy");
    if (!http) {
        return http.error();
    }
    auto https = load_proxy_variable(reader, "HTTPS_PROXY", "https_proxy");
    if (!https) {
        return https.error();
    }
    auto all = load_proxy_variable(reader, "ALL_PROXY", "all_proxy");
    if (!all) {
        return all.error();
    }
    NoProxy exceptions;
    std::string name;
    const auto raw = read_variable(reader, "NO_PROXY", "no_proxy", name);
    if (raw) {
        auto parsed = NoProxy::parse(trim(*raw));
        if (!parsed) {
            return config_error_from(parsed.error(), name);
        }
        exceptions = std::move(parsed.value());
    }
    ProxyEnvironment env;
    env.http = std::move(http.value());
    env.https = std::move(https.value());
    env.all = std::move(all.value());
    env.exceptions = std::move(exceptions);
    return env;
}

auto ProxyEnvironment::disabled() -> ProxyEnvironment { return ProxyEnvironment{}; }

auto ProxyEnvironment::from_system() -> Result<ProxyEnvironment> {
    // 钩子优先：系统采集在单测里不可控（它读的是当前用户/机器的真实配置），装钩子后本入口的
    // 全部判据都能逐值断言，口径同 `Resolver` 的解析钩子。
    const auto &hook = detail::system_proxy_hook();
    Result<std::optional<detail::SystemProxySnapshot>> snapshot = hook ? hook() : detail::platform_system_proxy();
    if (!snapshot) {
        return snapshot.error();
    }
    const auto &raw = snapshot.value();
    if (!raw) {
        // 「本腿没有系统代理通道」与「通道在但机器没配」是两件事：前者该由调用方回退到
        // 环境变量口径，后者就是一次成功的空读取。压成同一回报会让 Linux 腿看起来在报错。
        return make_error(ErrorCode::NetPlatformUnavailable,
                          "detail=this platform leg exposes no system proxy setting; read the environment instead");
    }
    return detail::system_proxy_from_snapshot(*raw);
}

auto ProxyEnvironment::from_environment_or_system() -> Result<ProxyEnvironment> {
    return from_environment_or_system(default_reader());
}

auto ProxyEnvironment::from_environment_or_system(const Reader &reader) -> Result<ProxyEnvironment> {
    auto env = from_environment(reader);
    if (!env) {
        return env.error();
    }
    if (takes_over(env.value())) {
        // 显式设过任一代理变量 = 接管：系统设置不参与，也不做逐槽位合并。容器 / CI / 代理调试
        // 现场全靠这条覆盖机器级配置，合并会让「设了却没生效」变成新的排查黑洞。
        return env;
    }
    auto system = from_system();
    if (!system) {
        if (system.error().code_enum == ErrorCode::NetPlatformUnavailable) {
            return env;  // 本腿没有系统通道：这次读取的全部含义就是环境变量，不是故障
        }
        return system.error();
    }
    return system;
}

auto ProxyEnvironment::for_target(std::string_view host, std::uint16_t port, bool secure) const -> ProxyConfig {
    if (exceptions.matches(host, port)) {
        return ProxyConfig::direct();
    }
    const std::optional<ProxyConfig> &primary = secure ? https : http;
    if (primary) {
        return *primary;
    }
    if (all) {
        return *all;
    }
    return ProxyConfig::direct();
}

auto ProxyEnvironment::operator==(const ProxyEnvironment &other) const -> bool {
    return http == other.http && https == other.https && all == other.all && exceptions == other.exceptions;
}

}  // namespace aurora::net

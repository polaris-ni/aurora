// Aurora 网络 — 端点值类型实现（CHG-010 底座切片：地址文本解析、规范化与校验）。
//
// 本文件**不引入任何平台 socket 头**：IPv4 / IPv6 的解析与规范化全部手写，目的是让同一输入
// 在 Win32 / POSIX / Emscripten 上得到同一结果与同一条诊断（跨平台一致行为判据见
// codespec/specification/06-app-platform.md §12.2）。交给 inet_pton 就等于把「哪些畸形串被
// 谁容忍」交给 libc 实现细节，那是三套行为的起点。
//
// 缓冲区一律 `std::array` + `.at()`：地址解析要在不定长位置反复落字节，越界即写坏栈，
// 检查边界不是洁癖而是这里的主要故障模式。

#include "aurora/net/address.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <utility>

namespace aurora::net {

namespace {

/// @brief 构造 `net-endpoint-invalid`，把被拒原因写进 message。
/// @param detail 人类可读的原因片段（ASCII，随输入文本回显）。
/// @return 表驱动 Error。
auto endpoint_error(const std::string &detail) -> Error {
    return make_error(ErrorCode::NetEndpointInvalid, "detail=" + detail);
}

/// @brief 单个 IPv4 段是否可接受（禁前导零，避开与八进制形态的歧义）。
/// @param token 一段文本。
/// @param out 求值结果（0-255）。
/// @return 1-3 位纯十进制、无前导零且不超过 255 时为 true。
auto parse_octet(std::string_view token, std::uint8_t &out) -> bool {
    if (token.empty() || token.size() > 3) {
        return false;
    }
    for (const char c : token) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    if (token.size() > 1 && token.front() == '0') {
        return false;
    }
    int value = 0;
    for (const char c : token) {
        value = (value * 10) + (c - '0');
    }
    if (value > 255) {
        return false;
    }
    out = static_cast<std::uint8_t>(value);
    return true;
}

/// @brief 解析点分十进制 IPv4。
/// @param text 主机文本（不含端口、不含方括号）。
/// @param out 四字节结果（网络序即书写序）。
/// @return 恰好四段且逐段合法时为 true。
auto parse_ipv4(std::string_view text, std::array<std::uint8_t, 4> &out) -> bool {
    std::array<std::uint8_t, 4> octets{};
    std::size_t index = 0;
    std::size_t pos = 0;
    while (true) {
        const std::size_t dot = text.find('.', pos);
        const std::size_t end = (dot == std::string_view::npos) ? text.size() : dot;
        const std::string_view token = text.substr(pos, end - pos);
        if (index >= octets.size() || !parse_octet(token, octets.at(index))) {
            return false;
        }
        ++index;
        if (dot == std::string_view::npos) {
            break;
        }
        pos = dot + 1;
    }
    if (index != octets.size()) {
        return false;
    }
    out = octets;
    return true;
}

/// @brief 单个 16 位十六进制段。
/// @param token 1-4 位十六进制文本。
/// @param out 求值结果。
/// @return 纯十六进制且长度合规时为 true。
auto parse_hextet(std::string_view token, std::uint16_t &out) -> bool {
    if (token.empty() || token.size() > 4) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char raw : token) {
        std::uint32_t digit = 0;
        if (raw >= '0' && raw <= '9') {
            digit = static_cast<std::uint32_t>(raw - '0');
        } else if (raw >= 'a' && raw <= 'f') {
            digit = static_cast<std::uint32_t>(raw - 'a') + 10U;
        } else if (raw >= 'A' && raw <= 'F') {
            digit = static_cast<std::uint32_t>(raw - 'A') + 10U;
        } else {
            return false;
        }
        value = (value * 16U) + digit;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

/// @brief 把一组冒分段写入 `groups`，末段允许内嵌 IPv4（`::ffff:192.0.2.1` 形态）。
/// @param part 冒号分隔的段文本（可为空，表示压缩边界的一侧无段）。
/// @param groups 累积的 16 位段缓冲。
/// @param count 已写入段数（就地累加）。
/// @param allows_embedded_v4 是否允许在本段尾部出现内嵌 IPv4（只有整串最后一段允许）。
/// @return 全部段合法且未溢出 8 段时为 true。
auto parse_ipv6_part(std::string_view part, std::array<std::uint16_t, 8> &groups, std::size_t &count,
                     bool allows_embedded_v4) -> bool {
    if (part.empty()) {
        return true;
    }
    if (part.front() == ':' || part.back() == ':') {
        return false;  // 压缩边界已由调用方切开，剩余冒号必是畸形
    }
    std::size_t pos = 0;
    while (pos < part.size()) {
        std::size_t colon = part.find(':', pos);
        if (colon == std::string_view::npos) {
            colon = part.size();
        }
        const std::string_view token = part.substr(pos, colon - pos);
        if (token.find('.') != std::string_view::npos) {
            // 内嵌 IPv4 尾段：占两个 16 位段，且必须是整段文本的最后一段。
            if (!allows_embedded_v4 || colon != part.size() || count + 2 > groups.size()) {
                return false;
            }
            std::array<std::uint8_t, 4> octets{};
            if (!parse_ipv4(token, octets)) {
                return false;
            }
            groups.at(count) = static_cast<std::uint16_t>((octets.at(0) << 8U) | octets.at(1));
            ++count;
            groups.at(count) = static_cast<std::uint16_t>((octets.at(2) << 8U) | octets.at(3));
            ++count;
            return true;
        }
        std::uint16_t hextet = 0;
        if (count >= groups.size() || !parse_hextet(token, hextet)) {
            return false;
        }
        groups.at(count) = hextet;
        ++count;
        pos = colon + 1;
    }
    return true;
}

/// @brief 解析冒分十六进制 IPv6（含 `::` 压缩与内嵌 IPv4 尾段）。
/// @param text 主机文本（不含方括号与 zone id）。
/// @param out 16 字节网络序结果。
/// @return 结构合法且恰好填满 128 位时为 true。
auto parse_ipv6(std::string_view text, std::array<std::uint8_t, 16> &out) -> bool {
    if (text.empty()) {
        return false;
    }
    const std::size_t first = text.find("::");
    const bool compressed = (first != std::string_view::npos);
    if (compressed && text.find("::", first + 1) != std::string_view::npos) {
        return false;  // 至多一处 `::`
    }
    std::array<std::uint16_t, 8> head{};
    std::array<std::uint16_t, 8> tail{};
    std::size_t head_len = 0;
    std::size_t tail_len = 0;
    if (compressed) {
        const bool head_ok = parse_ipv6_part(text.substr(0, first), head, head_len, false);
        const bool tail_ok = parse_ipv6_part(text.substr(first + 2), tail, tail_len, true);
        if (!head_ok || !tail_ok) {
            return false;
        }
        if (head_len + tail_len >= head.size()) {
            return false;  // `::` 必须至少压缩一个段
        }
    } else {
        if (!parse_ipv6_part(text, head, head_len, true) || head_len != head.size()) {
            return false;
        }
    }

    std::array<std::uint16_t, 8> groups{};
    std::size_t written = 0;
    for (std::size_t i = 0; i < head_len; ++i) {
        groups.at(written) = head.at(i);
        ++written;
    }
    if (compressed) {
        const std::size_t zeros = head.size() - head_len - tail_len;
        for (std::size_t i = 0; i < zeros; ++i) {
            groups.at(written) = 0;
            ++written;
        }
        for (std::size_t i = 0; i < tail_len; ++i) {
            groups.at(written) = tail.at(i);
            ++written;
        }
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        out.at(i * 2) = static_cast<std::uint8_t>(groups.at(i) >> 8U);
        out.at((i * 2) + 1) = static_cast<std::uint8_t>(groups.at(i) & 0xFFU);
    }
    return written == groups.size();
}

/// @brief 判断前 `count` 个字节是否全零。
/// @param bytes 地址字节缓冲。
/// @param count 检查的字节数（不得超过缓冲长度）。
/// @return 全零为 true。
auto first_bytes_zero(const std::array<std::uint8_t, 16> &bytes, std::size_t count) -> bool {
    return std::ranges::all_of(bytes | std::views::take(count), [](std::uint8_t byte) -> bool { return byte == 0; });
}

/// @brief 判断 16 字节是否全零。
/// @param bytes 16 字节网络序地址。
/// @return 全零为 true。
auto all_zero(const std::array<std::uint8_t, 16> &bytes) -> bool { return first_bytes_zero(bytes, bytes.size()); }

/// @brief 判断地址是否落在 link-local 前缀 `fe80::/10`（zone id 唯一有意义处）。
/// @param bytes 16 字节网络序地址。
/// @return 高 10 位符合 `fe80::/10` 时为 true。
auto is_link_local(const std::array<std::uint8_t, 16> &bytes) -> bool {
    return bytes.at(0) == 0xFE && (bytes.at(1) & 0xC0) == 0x80;
}

/// @brief 渲染 IPv4 点分十进制。
/// @param octets 四字节地址。
/// @return 形如 `127.0.0.1`。
auto format_ipv4(const std::array<std::uint8_t, 4> &octets) -> std::string {
    std::string text = std::to_string(octets.at(0));
    for (std::size_t i = 1; i < octets.size(); ++i) {
        text += '.';
        text += std::to_string(octets.at(i));
    }
    return text;
}

/// @brief 规范化渲染 IPv6（RFC 5952：小写、去前导零、最左最长零游程压成 `::`）。
/// @param bytes 16 字节网络序地址。
/// @return 不含方括号的规范化文本，如 `::1`、`fe80::1`、`2001:db8::`。
auto format_ipv6(const std::array<std::uint8_t, 16> &bytes) -> std::string {
    std::array<std::uint16_t, 8> groups{};
    std::array<std::string, 8> parts{};
    for (std::size_t i = 0; i < groups.size(); ++i) {
        groups.at(i) = static_cast<std::uint16_t>((bytes.at(i * 2) << 8U) | bytes.at((i * 2) + 1));
        std::string hex;
        for (int shift = 12; shift >= 0; shift -= 4) {
            const auto digit = static_cast<std::uint16_t>((groups.at(i) >> shift) & 0xFU);
            hex.push_back(digit < 10 ? static_cast<char>('0' + digit) : static_cast<char>('a' + (digit - 10)));
        }
        const std::size_t first_non_zero = hex.find_first_not_of('0');
        parts.at(i) = (first_non_zero == std::string::npos) ? "0" : hex.substr(first_non_zero);
    }

    std::size_t best_start = 0;
    std::size_t best_len = 0;
    std::size_t run_start = 0;
    bool in_run = false;
    for (std::size_t i = 0; i <= groups.size(); ++i) {
        const bool zero = (i < groups.size()) && (groups.at(i) == 0);
        if (zero && !in_run) {
            in_run = true;
            run_start = i;
        } else if (!zero && in_run) {
            if ((i - run_start) > best_len) {
                best_len = i - run_start;
                best_start = run_start;
            }
            in_run = false;
        }
    }
    auto join = [&parts](std::size_t from, std::size_t to) -> std::string {
        std::string text;
        for (std::size_t i = from; i < to; ++i) {
            if (i != from) {
                text += ':';
            }
            text += parts.at(i);
        }
        return text;
    };
    if (best_len < 2) {
        return join(0, groups.size());
    }
    const std::string head_text = join(0, best_start);
    const std::string tail_text = join(best_start + best_len, groups.size());
    if (head_text.empty()) {
        return tail_text.empty() ? "::" : "::" + tail_text;
    }
    if (tail_text.empty()) {
        return head_text + "::";
    }
    return head_text + "::" + tail_text;
}

/// @brief 判断 zone id 文本是否可接受（标识符字符集、非空、有长度上限）。
/// @param zone `%` 之后的文本。
/// @return 全部字符属于 `[A-Za-z0-9._-]` 且长度 ≤ 64 时为 true。
auto is_valid_zone(std::string_view zone) -> bool {
    if (zone.empty() || zone.size() > 64) {
        return false;
    }
    return std::ranges::all_of(zone, [](char c) -> bool {
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        return alnum || c == '.' || c == '_' || c == '-';
    });
}

}  // namespace

auto Endpoint::from_ip(std::string_view address, std::uint16_t port) -> Result<Endpoint> {
    if (address.empty()) {
        return endpoint_error("empty host text");
    }
    const bool has_space =
        std::ranges::any_of(address, [](char c) -> bool { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
    if (has_space) {
        return endpoint_error("host text must not contain whitespace");
    }

    std::string_view host = address;
    bool bracketed = false;
    if (host.front() == '[') {
        if (host.size() < 2 || host.back() != ']') {
            return endpoint_error("bracketed IPv6 must not carry an inline port");
        }
        host = host.substr(1, host.size() - 2);
        bracketed = true;
    }

    std::string zone;
    if (const std::size_t percent = host.find('%'); percent != std::string_view::npos) {
        zone = std::string(host.substr(percent + 1));
        host = host.substr(0, percent);
        if (!is_valid_zone(zone)) {
            return endpoint_error("zone id is empty or uses unsupported characters");
        }
    }

    std::array<std::uint8_t, 16> bytes{};
    Endpoint endpoint;
    if (std::array<std::uint8_t, 4> octets{}; parse_ipv4(host, octets)) {
        if (bracketed) {
            return endpoint_error("brackets are only valid around IPv6 literals");
        }
        if (!zone.empty()) {
            return endpoint_error("zone id is not meaningful for IPv4");
        }
        endpoint.family_ = AddressFamily::IPv4;
        for (std::size_t i = 0; i < octets.size(); ++i) {
            bytes.at(i) = octets.at(i);
        }
        endpoint.host_ = format_ipv4(octets);
    } else if (parse_ipv6(host, bytes)) {
        if (!zone.empty() && !is_link_local(bytes)) {
            return endpoint_error("zone id is only meaningful on link-local IPv6 (fe80::/10)");
        }
        endpoint.family_ = AddressFamily::IPv6;
        endpoint.host_ = format_ipv6(bytes);
    } else {
        return endpoint_error("not a valid IPv4 or IPv6 address");
    }
    endpoint.zone_ = std::move(zone);
    endpoint.port_ = port;
    endpoint.bytes_ = bytes;
    endpoint.valid_ = true;
    return endpoint;
}

auto Endpoint::loopback(std::uint16_t port) -> Endpoint {
    auto built = from_ip("127.0.0.1", port);
    return built.ok() ? built.value() : Endpoint{};
}

auto Endpoint::loopback_v6(std::uint16_t port) -> Endpoint {
    auto built = from_ip("::1", port);
    return built.ok() ? built.value() : Endpoint{};
}

auto Endpoint::any(bool any_v6, std::uint16_t port) -> Endpoint {
    auto built = from_ip(any_v6 ? "::" : "0.0.0.0", port);
    return built.ok() ? built.value() : Endpoint{};
}

auto Endpoint::local(std::string_view path) -> Result<Endpoint> {
    if (path.empty()) {
        return endpoint_error("local endpoint path is empty");
    }
    if (path.find('\0') != std::string_view::npos) {
        return endpoint_error("local endpoint path must not contain a NUL byte");
    }
    if (path.size() > AURORA_NET_MAX_LOCAL_PATH) {
        return endpoint_error("local endpoint path exceeds the sun_path limit");
    }
    Endpoint endpoint;
    endpoint.family_ = AddressFamily::LocalPath;
    endpoint.host_ = std::string(path);
    endpoint.valid_ = true;
    return endpoint;
}

auto Endpoint::is_loopback() const -> bool {
    if (!valid_) {
        return false;  // 无效占位不是地址，全零字节不代表任何语义
    }
    if (family_ == AddressFamily::IPv4) {
        return bytes_.at(0) == 127;
    }
    if (family_ == AddressFamily::IPv6) {
        return first_bytes_zero(bytes_, bytes_.size() - 1) && bytes_.at(bytes_.size() - 1) == 1;
    }
    return false;
}

auto Endpoint::is_unspecified() const -> bool {
    if (!valid_) {
        return false;
    }
    if (family_ == AddressFamily::IPv4) {
        return first_bytes_zero(bytes_, 4);
    }
    if (family_ == AddressFamily::IPv6) {
        return all_zero(bytes_);
    }
    return false;
}

auto Endpoint::to_string() const -> std::string {
    if (!valid_) {
        return {};
    }
    if (family_ == AddressFamily::LocalPath) {
        return "unix:" + host_;
    }
    std::string text;
    if (family_ == AddressFamily::IPv6) {
        text.push_back('[');
        text += host_;
        if (!zone_.empty()) {
            text += '%';
            text += zone_;
        }
        text.push_back(']');
    } else {
        text = host_;
    }
    text += ':';
    text += std::to_string(port_);
    return text;
}

auto Endpoint::operator==(const Endpoint &other) const -> bool {
    if (valid_ != other.valid_ || family_ != other.family_ || port_ != other.port_ || host_ != other.host_) {
        return false;
    }
    if (family_ == AddressFamily::LocalPath) {
        return true;
    }
    return zone_ == other.zone_;
}

}  // namespace aurora::net

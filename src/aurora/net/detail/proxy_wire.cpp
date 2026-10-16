// proxy_wire.cpp —— 代理握手字节层的实现（见 `detail/proxy_wire.h` 的口径条款）。
//
// 本文件只做两件事：把要发给代理的字节按文法拼出来，把代理回给我们的字节按文法读回来。
// 因此这里不出现任何平台 socket 头（CHG-006 的「平台头唯一落点在 socket.cpp」不变量继续成立），
// 也不发起任何 I/O——「写到套接字 / 从套接字读」属传输切片，本层给它可逐值断言的纯函数。

#include "aurora/net/detail/proxy_wire.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/detail/http_auth_wire.h"
#include "aurora/net/proxy.h"

namespace aurora::net::wire {
namespace {

/// 协议常量（`RFC 1928` 的字段取值）：命名空间级常量按门禁须 `AURORA_` 前缀 + 全大写。
constexpr std::uint8_t AURORA_SOCKS_VERSION = 5;
constexpr std::uint8_t AURORA_SOCKS_COMMAND_CONNECT = 1;
constexpr std::uint8_t AURORA_SOCKS_ATYPE_IPV4 = 1;
constexpr std::uint8_t AURORA_SOCKS_ATYPE_DOMAIN = 3;
constexpr std::uint8_t AURORA_SOCKS_ATYPE_IPV6 = 4;
constexpr std::uint8_t AURORA_SOCKS_METHOD_NO_AUTHENTICATION = 0;
constexpr std::uint8_t AURORA_SOCKS_METHOD_USERNAME_PASSWORD = 2;
constexpr std::uint8_t AURORA_SOCKS_METHOD_NO_ACCEPTABLE = 0xFF;
constexpr std::uint8_t AURORA_SOCKS_METHOD_CHALLENGE_HANDSHAKE = 3;
constexpr std::uint8_t AURORA_SOCKS_AUTH_VERSION = 1;
constexpr std::uint8_t AURORA_SOCKS_MAX_ID_OCTETS = 0xFF;

/// @brief SOCKS5 的 REP 里属「代理自身形态不支持」的两值（`RFC 1928` §6 逐值枚举：`X'07'` 命令不支持、
///        `X'08'` 地址类型不支持；其余非零值是对端/链路的瞬时失败，归可达性码）。
constexpr std::uint8_t AURORA_SOCKS_REP_COMMAND_NOT_SUPPORTED = 7;
constexpr std::uint8_t AURORA_SOCKS_REP_ADDRESS_TYPE_NOT_SUPPORTED = 8;

/// @brief 构造「报文不合文法」错误（本层的畸形一律落此码，不与可达性失败混同）。
/// @param detail 现场细节（含阶段与实际字节 / 文本）。
/// @return `net-proxy-protocol` 错误。
auto protocol_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetProxyProtocol, std::string{detail});
}

/// @brief 构造「端点不合 SOCKS5 地址编码」错误。
/// @param detail 现场细节。
/// @return `net-endpoint-invalid` 错误。
auto endpoint_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetEndpointInvalid, std::string{detail});
}

/// @brief 百分号编码之外的凭据长度越界错误（凭据没处安放，属配置问题）。
/// @param detail 现场细节。
/// @return `net-proxy-config-invalid` 错误。
auto config_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetProxyConfigInvalid, std::string{detail});
}

/// @brief 读一个无符号八位组。
///
/// `string_view` 的元素是带符号 `char`，直接转 `uint8_t` 会把 0x80 以上的字节当成符号扩展值——
/// 端口、长度前缀与方法号都是这类字节，所以本文件所有字节读取一律走此处。
/// @param bytes 报文字节。
/// @param index 偏移（调用方保证在界内）。
/// @return 该字节的无符号值。
constexpr auto octet(std::string_view bytes, std::size_t index) -> std::uint8_t {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(bytes.at(index)));
}

/// @brief 把主机与端口拼成 HTTP 的 `authority-form`。
/// @param host 主机文本；含 `:` 即按 IPv6 字面量补方括号。
/// @param port 端口（主机序）。
/// @return 形如 `example.com:8080` 或 `[::1]:8080`。
auto authority(std::string_view host, std::uint16_t port) -> std::string {
    std::string out;
    out.reserve(host.size() + 8);
    if (host.find(':') != std::string_view::npos) {
        out.push_back('[');
        out.append(host);
        out.push_back(']');
    } else {
        out.append(host);
    }
    out.push_back(':');
    out.append(std::to_string(port));
    return out;
}

/// @brief 取一行（到 `'\n'` 为止，去掉行尾 `'\r'`）。
/// @param bytes 剩余字节。
/// @param line_end 输出：本行连同换行的长度；无换行时为 `npos`。
/// @return 去掉换行符后的行文本。
auto take_line(std::string_view bytes, std::size_t &line_end) -> std::string_view {
    const auto newline = bytes.find('\n');
    if (newline == std::string_view::npos) {
        line_end = std::string_view::npos;
        return {};
    }
    std::size_t content = newline;
    if (content > 0 && bytes.at(content - 1) == '\r') {
        --content;
    }
    line_end = newline + 1;
    return bytes.substr(0, content);
}

/// @brief 找头部块的终止空行。
/// @param bytes 已读到的字节。
/// @return 空行之后的偏移（即头部块长度）；未出现时 `npos`。
auto head_block_end(std::string_view bytes) -> std::size_t {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t line_end = 0;
        const auto line = take_line(bytes.substr(offset), line_end);
        if (line_end == std::string_view::npos) {
            return std::string_view::npos;
        }
        offset += line_end;
        if (line.empty()) {
            return offset;
        }
    }
    return std::string_view::npos;
}

/// @brief 解析状态行 `HTTP/<major>.<minor> <NNN> [reason]`。
/// @param line 状态行文本（已去换行）。
/// @param head 输出：状态码与原因短语。
/// @return 成功为 true；不合文法为 false（错误由调用方统一落码，避免两处文案漂移）。
auto parse_status_line(std::string_view line, ConnectHead &head) -> bool {
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto all_digits = [&](std::string_view text) { return !text.empty() && std::ranges::all_of(text, is_digit); };
    if (!line.starts_with("HTTP/")) {
        return false;
    }
    const auto version_end = line.find(' ', 5);
    if (version_end == std::string_view::npos) {
        return false;
    }
    const auto version = line.substr(5, version_end - 5);
    const auto dot = version.find('.');
    if (dot == std::string_view::npos || !all_digits(version.substr(0, dot)) || !all_digits(version.substr(dot + 1))) {
        return false;
    }
    const auto status_start = version_end + 1;
    const auto status_end = line.find(' ', status_start);
    const auto status_text = status_end == std::string_view::npos
                                 ? line.substr(status_start)
                                 : line.substr(status_start, status_end - status_start);
    if (status_text.size() != 3 || !all_digits(status_text)) {
        return false;
    }
    head.status = static_cast<std::uint16_t>(((status_text.at(0) - '0') * 100) + ((status_text.at(1) - '0') * 10) +
                                             (status_text.at(2) - '0'));
    head.reason = status_end == std::string_view::npos ? std::string{} : std::string{line.substr(status_end + 1)};
    return true;
}

/// @brief 解析头部字段行（同名不折叠、原始拼写交给 `Headers` 保序）。
/// @param block 状态行之后的头部块（含终止空行）。
/// @param head 输出：字段表。
/// @return 全部字段合文法为 true；任一行缺冒号或以空白续折（obs-fold）为 false。
auto parse_header_fields(std::string_view block, ConnectHead &head) -> bool {
    std::size_t offset = 0;
    while (offset < block.size()) {
        std::size_t line_end = 0;
        const auto line = take_line(block.substr(offset), line_end);
        offset += line_end;
        if (line.empty()) {
            return true;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return false;
        }
        const auto name = line.substr(0, colon);
        if (name.find(' ') != std::string_view::npos || name.find('\t') != std::string_view::npos) {
            return false;
        }
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        head.headers.append(name, value);
    }
    return true;
}

/// @brief 把地址字节装成 `Endpoint` 需要的 16 字节数组。
/// @param bytes 应答字节。
/// @param start 地址首字节偏移。
/// @param length 地址长度（4 或 16）。
/// @return 补齐后的数组（非地址段为零）。
auto copy_address(std::string_view bytes, std::size_t start, std::size_t length) -> std::array<std::uint8_t, 16> {
    std::array<std::uint8_t, 16> out{};
    for (std::size_t index = 0; index < length; ++index) {
        out.at(index) = octet(bytes, start + index);
    }
    return out;
}

/// @brief 读两字节大端端口（SOCKS5 的地址端口字段固定网络序 = 大端）。
/// @param bytes 应答字节。
/// @param start 端口首字节偏移。
/// @return 主机序端口。
auto read_port(std::string_view bytes, std::size_t start) -> std::uint16_t {
    const auto high = static_cast<std::uint16_t>(octet(bytes, start));
    const auto low = static_cast<std::uint16_t>(octet(bytes, start + 1));
    return static_cast<std::uint16_t>((high << 8) | low);
}

/// @brief 把主机文本按 SOCKS5 地址编码写进请求尾部。
/// @param host 数字字面量文本。
/// @param port 端口（主机序）。
/// @param request 输出：追加 ATYPE、地址与端口。
/// @return 成功为 true；非数字或带作用域号为 false（细节写进 out_detail）。
/// @param out_detail 输出：失败时的现场细节。
auto append_numeric_address(std::string_view host, std::uint16_t port, std::vector<std::uint8_t> &request,
                            std::string &out_detail) -> bool {
    const auto parsed = Endpoint::from_ip(host, port);
    if (!parsed) {
        out_detail =
            "SOCKS5 without proxy-side resolution needs a numeric target address, got '" + std::string{host} + "'";
        return false;
    }
    const auto &endpoint = parsed.value();
    if (!endpoint.zone().empty()) {
        out_detail = "SOCKS5 has no field for an IPv6 scope id, so '%" + endpoint.zone() + "' cannot be sent";
        return false;
    }
    const auto &bytes = endpoint.address_bytes();
    switch (endpoint.family()) {
        case AddressFamily::IPv4:
            request.push_back(AURORA_SOCKS_ATYPE_IPV4);
            for (std::size_t index = 0; index < 4; ++index) {
                request.push_back(bytes.at(index));
            }
            break;
        case AddressFamily::IPv6:
            request.push_back(AURORA_SOCKS_ATYPE_IPV6);
            for (std::size_t index = 0; index < 16; ++index) {
                request.push_back(bytes.at(index));
            }
            break;
        default:
            out_detail = "SOCKS5 targets are IPv4 or IPv6, not family " +
                         std::to_string(static_cast<unsigned>(endpoint.family()));
            return false;
    }
    request.push_back(static_cast<std::uint8_t>(port >> 8U));
    request.push_back(static_cast<std::uint8_t>(port & 0xFFU));
    return true;
}

}  // namespace

auto make_connect_request(std::string_view host, std::uint16_t port, const ProxyCredentials &credentials,
                          const std::optional<std::string> &proxy_authorization) -> std::string {
    const auto target = authority(host, port);
    std::string out = "CONNECT " + target + " HTTP/1.1\r\n";
    out += "Host: " + target + "\r\n";
    // 挑战-答复那一条路给的字段值原样写上线（`Digest` 之类的答案不是 `user:pass`，压不进预防性形态）；
    // 没给答复时仍按凭据做预防性 Basic。两处都不覆盖调用方已经写在配置里的凭据。
    if (proxy_authorization.has_value() && !proxy_authorization->empty()) {
        out += "Proxy-Authorization: " + *proxy_authorization + "\r\n";
    } else if (!credentials.empty()) {
        out += "Proxy-Authorization: Basic " +
               detail::base64_encode(credentials.username + ":" + credentials.password) + "\r\n";
    }
    out += "\r\n";
    return out;
}

auto parse_connect_response(std::string_view bytes, std::size_t max_head_bytes) -> Result<std::optional<ConnectHead>> {
    const auto end = head_block_end(bytes);
    if (end == std::string_view::npos) {
        if (bytes.size() > max_head_bytes) {
            return protocol_error("CONNECT response has no blank line closing its headers within " +
                                  std::to_string(max_head_bytes) + " bytes");
        }
        return std::optional<ConnectHead>{};
    }
    if (end > max_head_bytes) {
        return protocol_error("CONNECT response header block is " + std::to_string(end) +
                              " bytes, above the configured " + std::to_string(max_head_bytes) + " byte limit");
    }
    const auto block = bytes.substr(0, end);
    const auto status_end = block.find('\n');
    auto status_line = block.substr(0, status_end);
    if (!status_line.empty() && status_line.back() == '\r') {
        status_line.remove_suffix(1);
    }
    ConnectHead head;
    if (!parse_status_line(status_line, head)) {
        return protocol_error("CONNECT response status line is not \"HTTP/<major>.<minor> <NNN> [reason]\": '" +
                              std::string{status_line} + "'");
    }
    if (!parse_header_fields(block.substr(status_end + 1), head)) {
        return protocol_error("CONNECT response header field is malformed (missing colon, or obs-fold)");
    }
    head.header_bytes = end;
    return std::optional<ConnectHead>{std::move(head)};
}

auto make_socks5_greeting(bool with_credentials) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> out;
    out.push_back(AURORA_SOCKS_VERSION);
    out.push_back(static_cast<std::uint8_t>(with_credentials ? 2 : 1));
    out.push_back(AURORA_SOCKS_METHOD_NO_AUTHENTICATION);
    if (with_credentials) {
        out.push_back(AURORA_SOCKS_METHOD_USERNAME_PASSWORD);
    }
    return out;
}

auto parse_socks5_method(std::string_view bytes) -> Result<std::optional<std::uint8_t>> {
    if (bytes.size() < 2) {
        return std::optional<std::uint8_t>{};
    }
    if (octet(bytes, 0) != AURORA_SOCKS_VERSION) {
        return protocol_error("SOCKS5 method selection reply is not version 5 (got " +
                              std::to_string(static_cast<unsigned>(octet(bytes, 0))) + ")");
    }
    return std::optional<std::uint8_t>{octet(bytes, 1)};
}

auto make_socks5_auth_request(const ProxyCredentials &credentials) -> Result<std::vector<std::uint8_t>> {
    if (credentials.empty()) {
        return config_error("SOCKS5 username/password subnegotiation needs a non-empty username");
    }
    if (credentials.username.size() > AURORA_SOCKS_MAX_ID_OCTETS ||
        credentials.password.size() > AURORA_SOCKS_MAX_ID_OCTETS) {
        return config_error("SOCKS5 subnegotiation carries at most 255 octets per field (username is " +
                            std::to_string(credentials.username.size()) + ", password is " +
                            std::to_string(credentials.password.size()) + ")");
    }
    std::vector<std::uint8_t> out;
    out.push_back(AURORA_SOCKS_AUTH_VERSION);
    out.push_back(static_cast<std::uint8_t>(credentials.username.size()));
    for (const auto character : credentials.username) {
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    out.push_back(static_cast<std::uint8_t>(credentials.password.size()));
    for (const auto character : credentials.password) {
        out.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    return out;
}

auto parse_socks5_auth(std::string_view bytes) -> Result<std::optional<std::uint8_t>> {
    if (bytes.size() < 2) {
        return std::optional<std::uint8_t>{};
    }
    // 该版本字段不携带信息，但文法只认 1：放宽会让「对端根本不是 SOCKS5」被读成一次认证失败。
    if (octet(bytes, 0) != AURORA_SOCKS_AUTH_VERSION) {
        return protocol_error("SOCKS5 subnegotiation reply is not version 1 (got " +
                              std::to_string(static_cast<unsigned>(octet(bytes, 0))) + ")");
    }
    return std::optional<std::uint8_t>{octet(bytes, 1)};
}

auto make_socks5_connect(std::string_view host, std::uint16_t port, bool resolve_via_proxy)
    -> Result<std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> request;
    request.push_back(AURORA_SOCKS_VERSION);
    request.push_back(AURORA_SOCKS_COMMAND_CONNECT);
    request.push_back(0);
    if (!resolve_via_proxy) {
        std::string detail;
        if (!append_numeric_address(host, port, request, detail)) {
            return endpoint_error(std::move(detail));
        }
        return request;
    }
    if (host.empty() || host.size() > AURORA_SOCKS_MAX_ID_OCTETS) {
        return endpoint_error("SOCKS5 domain targets are length-prefixed with one octet, so a name of " +
                              std::to_string(host.size()) + " octets cannot be sent");
    }
    request.push_back(AURORA_SOCKS_ATYPE_DOMAIN);
    request.push_back(static_cast<std::uint8_t>(host.size()));
    for (const auto character : host) {
        request.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    request.push_back(static_cast<std::uint8_t>(port >> 8U));
    request.push_back(static_cast<std::uint8_t>(port & 0xFFU));
    return request;
}

auto parse_socks5_reply(std::string_view bytes, std::size_t max_head_bytes) -> Result<std::optional<Socks5Reply>> {
    if (bytes.size() > max_head_bytes) {
        return protocol_error("SOCKS5 CONNECT reply is above the configured " + std::to_string(max_head_bytes) +
                              " byte limit");
    }
    if (bytes.size() < 4) {
        return std::optional<Socks5Reply>{};
    }
    if (octet(bytes, 0) != AURORA_SOCKS_VERSION) {
        return protocol_error("SOCKS5 CONNECT reply is not version 5 (got " +
                              std::to_string(static_cast<unsigned>(octet(bytes, 0))) + ")");
    }
    const auto atype = octet(bytes, 3);
    std::size_t address_length = 0;
    switch (atype) {
        case AURORA_SOCKS_ATYPE_IPV4:
            address_length = 4;
            break;
        case AURORA_SOCKS_ATYPE_IPV6:
            address_length = 16;
            break;
        case AURORA_SOCKS_ATYPE_DOMAIN: {
            if (bytes.size() < 5) {
                return std::optional<Socks5Reply>{};
            }
            address_length = octet(bytes, 4);
            break;
        }
        default:
            return protocol_error("SOCKS5 CONNECT reply carries unknown ATYPE " +
                                  std::to_string(static_cast<unsigned>(atype)));
    }
    const auto address_start = atype == AURORA_SOCKS_ATYPE_DOMAIN ? std::size_t{5} : std::size_t{4};
    const std::size_t total = address_start + address_length + 2;
    if (bytes.size() < total) {
        return std::optional<Socks5Reply>{};
    }
    Socks5Reply reply;
    reply.status = octet(bytes, 1);
    reply.consumed = total;
    if (atype == AURORA_SOCKS_ATYPE_DOMAIN) {
        // 绑定地址回的是文本域名，而本层不把域名变成端点（那需要一个未经解析的假端点）。
        return std::optional<Socks5Reply>{reply};
    }
    const auto family = atype == AURORA_SOCKS_ATYPE_IPV4 ? AddressFamily::IPv4 : AddressFamily::IPv6;
    auto bound =
        Endpoint::from_bytes(family, copy_address(bytes, address_start, address_length), read_port(bytes, total - 2));
    if (!bound) {
        return protocol_error("SOCKS5 CONNECT reply carries a bound address that is not a valid endpoint");
    }
    reply.bound = bound.value();
    return std::optional<Socks5Reply>{reply};
}

auto connect_tunnel_error(std::uint16_t status, std::string_view proxy, std::string_view target) -> Error {
    const auto context = "CONNECT " + std::string{target} + " via proxy " + std::string{proxy};
    if (status == 407) {
        return make_error(ErrorCode::NetProxyAuthRequired, context + " answered 407 (proxy authentication required)");
    }
    return make_error(ErrorCode::NetProxyUnreachable, context + " answered " + std::to_string(status));
}

auto socks5_error_for(std::uint8_t status, Socks5Phase phase, std::string_view proxy) -> Error {
    const auto code = static_cast<unsigned>(status);
    const auto context = "proxy " + std::string{proxy} + " (SOCKS5 status " + std::to_string(code) + ")";
    switch (phase) {
        case Socks5Phase::MethodSelection:
            if (status == AURORA_SOCKS_METHOD_NO_ACCEPTABLE) {
                return make_error(ErrorCode::NetProxyAuthRequired,
                                  "no offered SOCKS5 method was accepted by " + context +
                                      " - this build offers only no-auth and username/password");
            }
            if (status == AURORA_SOCKS_METHOD_CHALLENGE_HANDSHAKE) {
                return make_error(
                    ErrorCode::NetProxyUnsupported,
                    "SOCKS5 method selection picked the IETF reserved challenge-handshake method for " + context);
            }
            return make_error(ErrorCode::NetProxyUnreachable,
                              "unexpected SOCKS5 method selection reply from " + context);
        case Socks5Phase::Authentication:
            return make_error(ErrorCode::NetProxyAuthRequired,
                              "SOCKS5 username/password subnegotiation was refused by " + context);
        case Socks5Phase::Tunnel:
            if (status == AURORA_SOCKS_REP_COMMAND_NOT_SUPPORTED ||
                status == AURORA_SOCKS_REP_ADDRESS_TYPE_NOT_SUPPORTED) {
                return make_error(ErrorCode::NetProxyUnsupported, "SOCKS5 CONNECT cannot be served by " + context);
            }
            return make_error(ErrorCode::NetProxyUnreachable, "SOCKS5 CONNECT was refused by " + context);
    }
    return make_error(ErrorCode::NetProxyUnreachable, "SOCKS5 handshake failed for " + context);
}

}  // namespace aurora::net::wire

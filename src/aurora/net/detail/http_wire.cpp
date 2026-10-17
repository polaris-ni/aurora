// http_wire.cpp —— HTTP/1.1 报文字节层的实现（见 `detail/http_wire.h` 的口径条款）。
//
// 本文件只做两件事：把请求行与请求字段按文法拼出来，把应答的状态行、头部块与分块体按文法读回来。
// 因此这里不出现任何平台 socket 头（的「平台头唯一落点在 socket.cpp」不变量继续成立），
// 也不发起任何 I/O——「写到套接字 / 从套接字读」属传输切片，本层给它可逐值断言的纯函数。

#include "aurora/net/detail/http_wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"

namespace aurora::net::wire {
namespace {

/// 头部块解析上限之外的第二道闸：单条块长度行 / trailer 行超过此字节数即判畸形（防无界增长）。
/// 与 `AURORA_HTTP_MAX_HEAD_BYTES` 同量级——现实里最长的头部行也远不到此值，越过它就不可能是兼容。
constexpr std::size_t AURORA_HTTP_MAX_LINE_BYTES = std::size_t{8} * 1024;

/// 1xx 信息应答的状态码下界（`1xx` 到 `199` 恒无消息体，`RFC 9112 §6.4`）。
constexpr std::uint16_t AURORA_HTTP_STATUS_1XX_MIN = 100;
constexpr std::uint16_t AURORA_HTTP_STATUS_1XX_MAX = 199;
/// 协议升级（`101 Switching Protocols`）：唯一**不**按「非最终应答」处理的 1xx——它宣告这条连接此后不再说
/// HTTP/1.1，而「换成说什么」属 WebSocket 切片，HTTP 客户端收下它只会让本层拿着两套文法读一条流。
constexpr std::uint16_t AURORA_HTTP_STATUS_SWITCHING_PROTOCOLS = 101;
/// 无体状态码（204 No Content 与 304 Not Modified）；304 的应答头描述的是被缓存的表示，不带体。
constexpr std::uint16_t AURORA_HTTP_STATUS_NO_CONTENT = 204;
/// 分块长度行的十六进制字符表（小写线上形态；文件作用域而非函数内，是为了让 `AURORA_` 常量命名口径统一）。
constexpr std::string_view AURORA_HEX_DIGITS = "0123456789abcdef";
constexpr std::uint16_t AURORA_HTTP_STATUS_NOT_MODIFIED = 304;

/// @brief 该状态码是否为「还没说完话」的信息应答（`RFC 9110 §15.2`：1xx 恒无体，其后紧跟最终应答）。
/// @param status 应答状态码。
/// @return 属 1xx 且不是 `101` 时为 true。
constexpr auto is_informational(std::uint16_t status) -> bool {
    return status >= AURORA_HTTP_STATUS_1XX_MIN && status <= AURORA_HTTP_STATUS_1XX_MAX &&
           status != AURORA_HTTP_STATUS_SWITCHING_PROTOCOLS;
}

/// @brief 把无符号整数写成小写十六进制（分块长度行的线上形态；不用 `<format>`，与本仓其余产字节处同型）。
/// @param value 待写数值。
/// @return 至少一位、无前导零的小写十六进制文本。
auto to_lower_hex(std::uint64_t value) -> std::string {
    std::string text;
    do {
        // 逐位从低位起取，末尾整体反转：`substr().front()` 而非下标取值，是为了不在非常量下标上开口子。
        text.push_back(AURORA_HEX_DIGITS.substr(static_cast<std::size_t>(value % 16U), 1).front());
        value /= 16U;
    } while (value != 0);
    std::ranges::reverse(text);
    return text;
}

/// @brief 构造「报文不合文法」错误（本层的畸形一律落此码，不与体上限 / 请求写法错误混同）。
/// @param detail 现场细节（含实际字节文本与所在阶段）。
/// @return `http-protocol-error` 错误。
auto protocol_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::HttpProtocolError, std::string{detail});
}

/// @brief 构造「请求写法在发出之前即不合文法」错误。
/// @param detail 现场细节（含越界的字段名 / 值或非法方法文本）。
/// @return `http-request-invalid` 错误。
auto request_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::HttpRequestInvalid, std::string{detail});
}

/// @brief 判断字符是否 HTTP token 字符（`RFC 7230 §3.2.6` 的 tchar）。
/// @param c 待判字符。
/// @return token 字符为真。
constexpr auto is_token_char(char c) -> bool {
    const auto raw = static_cast<unsigned char>(c);
    const bool alphanumeric = (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z') || (raw >= '0' && raw <= '9');
    if (alphanumeric) {
        return true;
    }
    constexpr std::string_view specials = "!#$%&'*+-.^_`|~";
    return specials.find(c) != std::string_view::npos;
}

/// @brief 文本是否整体为合法 token（非空且逐字符合 tchar）。
/// @param text 待判文本。
/// @return 合法 token 为真。
constexpr auto is_token(std::string_view text) -> bool {
    if (text.empty()) {
        return false;
    }
    return std::ranges::all_of(text, [](char c) { return is_token_char(c); });
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
/// @return 空行之后的偏移（即头部块长度，含空行本身）；未出现时 `npos`。
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

/// @brief 去掉首尾的可选空白（OWS：空格与制表符，`RFC 9110 §5.5`）。
/// @param text 原始文本。
/// @return 修剪后的视图。
constexpr auto trim_ows(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 解析状态行 `HTTP/<major>.<minor> SP 3DIGIT [SP reason]`。
/// @param line 状态行文本（已去换行）。
/// @param head 输出：状态码与原因短语。
/// @return 成功为 true；不合文法为 false（错误由调用方统一落码，避免两处文案漂移）。
auto parse_status_line(std::string_view line, ResponseHead &head) -> bool {
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
    // 主版本恒为 1（本库不谈 HTTP/2 以上），次版本记下来供连接池的「HTTP/1.1 才可复用」判据用——
    // 1.0 缺省 `close` 语义，逐跳判断比从字段反推可靠。
    head.version_minor = static_cast<std::uint8_t>(version.substr(dot + 1).front() - '0');
    return true;
}

/// @brief 解析头部字段行，允许 obs-fold 续行（`RFC 9112 §5.2`，已废弃但仍会出现）。
/// @param block 状态行之后的头部块（含终止空行）。
/// @param out 输出：字段条目（原始名字拼写保留，obs-fold 已合并进上一条值）。
/// @return 全部字段合文法为 true；缺冒号、名字含非法字符或首行即续折为 false。
auto parse_header_fields(std::string_view block, std::vector<Headers::Entry> &out) -> bool {
    std::size_t offset = 0;
    while (offset < block.size()) {
        std::size_t line_end = 0;
        const auto line = take_line(block.substr(offset), line_end);
        offset += line_end;
        if (line.empty()) {
            return true;
        }
        if (line.front() == ' ' || line.front() == '\t') {
            if (out.empty()) {
                return false;  // obs-fold 续行出现在任何字段之前
            }
            out.back().value.push_back(' ');
            out.back().value.append(trim_ows(line));
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return false;
        }
        const auto name = line.substr(0, colon);
        if (!is_token(name)) {
            return false;
        }
        out.push_back(
            Headers::Entry{.name = std::string{name}, .value = std::string{trim_ows(line.substr(colon + 1))}});
    }
    return true;
}

/// @brief 逐条比对 `Content-Length` 值是否一致（多枚同名头取值不同即报文歧义）。
/// @param values 全部 `Content-Length` 值（已 trim）。
/// @return 空或全部相等为 true。
auto content_length_consistent(const std::vector<std::string> &values) -> bool {
    if (values.size() <= 1) {
        return true;
    }
    return std::ranges::all_of(values, [&](const std::string &v) { return v == values.front(); });
}

/// @brief 解析十进制 `Content-Length`（只允许非负整数）。
/// @param text 已 trim 的值文本。
/// @return 合文法为 true；空、含非数字或越 `size_t` 上界为 false。
auto is_valid_content_length(std::string_view text) -> bool {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        value = (value * 10U) + digit;
    }
    return true;
}

/// @brief 解析分块长度（十六进制，`RFC 9112 §4.1`）。
/// @param text 块长度行去掉块扩展后的文本。
/// @param out 输出：数值（溢出时置为 `uint64` 上界）。
/// @return 逐字符合十六进制为 true；含非十六进制字符或为空为 false。
auto parse_chunk_size(std::string_view text, std::uint64_t &out) -> bool {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    bool overflowed = false;
    for (const char c : text) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10U;
        } else {
            return false;
        }
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 16U) {
            overflowed = true;
        }
        value = (value * 16U) + digit;
    }
    out = overflowed ? std::numeric_limits<std::uint64_t>::max() : value;
    return true;
}

/// @brief 把 `Transfer-Encoding` 逗号表拆成小写 token 集合，判定是否含 chunked 及其它编码。
/// @param values 全部 `Transfer-Encoding` 头值。
/// @param out_chunked 输出：是否出现 chunked。
/// @param out_other 输出：是否出现本库不实现的传输编码。
auto scan_transfer_encoding(const std::vector<std::string> &values, bool &out_chunked, bool &out_other) -> void {
    out_chunked = false;
    out_other = false;
    for (const auto &raw : values) {
        // 切段一律从 `raw` 的视图上做：`raw.substr()` 产出的是临时 `std::string`，而 `trim_ows` 返回视图，
        // 直接套上去会得到一枚指向已析构临时量的悬空视图（同一份逻辑在此处曾在两腿给出不同裁决）。
        const std::string_view text{raw};
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t stop = text.find(',', start);
            const auto token =
                trim_ows(text.substr(start, stop == std::string_view::npos ? std::string_view::npos : stop - start));
            if (!token.empty()) {
                std::string lowered;
                lowered.reserve(token.size());
                for (const char c : token) {
                    const auto u = static_cast<unsigned char>(c);
                    lowered.push_back(u >= 'A' && u <= 'Z' ? static_cast<char>(u + ('a' - 'A')) : c);
                }
                if (lowered == "chunked") {
                    out_chunked = true;
                } else {
                    out_other = true;
                }
            }
            if (stop == std::string_view::npos) {
                break;
            }
            start = stop + 1;
        }
    }
}

/// @brief 该 scheme 的注册默认端口（仅覆盖本库会走 HTTP 的四种；未知返回 0 表示无默认）。
/// @param scheme 小写协议名。
/// @return 默认端口；无注册默认时 0。
auto default_port(std::string_view scheme) -> std::uint16_t {
    if (scheme == "http" || scheme == "ws") {
        return 80;
    }
    if (scheme == "https" || scheme == "wss") {
        return 443;
    }
    return 0;
}

/// @brief 该目标在线上要显式写出的端口（authority 的两处用法——绝对形态请求目标与 `Host`——共用这一枚取舍）。
/// @param target 已解析的目标 URL。
/// @return 需要写出的端口；走缺省端口（或未声明端口）时 0，表示不写。
auto displayed_port(const Url &target) -> std::uint16_t {
    const auto explicit_port = target.port();
    if (!explicit_port.has_value()) {
        return 0;
    }
    const auto def = default_port(target.scheme());
    return (def == 0 || explicit_port.value() != def) ? explicit_port.value() : 0;
}

/// @brief 把主机与（可选）端口拼成请求目标与 `Host` 用的 authority。
/// @param host 主机文本；含 `:` 即按 IPv6 字面量补方括号。
/// @param port 需要显式写出的端口；为 0 表示走缺省端口、不写出。
/// @return 形如 `example.com` 或 `[::1]:8080`。
std::string authority(std::string_view host, std::uint16_t port) {
    std::string out;
    out.reserve(host.size() + 8);
    if (host.find(':') != std::string_view::npos) {
        out.push_back('[');
        out.append(host);
        out.push_back(']');
    } else {
        out.append(host);
    }
    if (port != 0) {
        out.push_back(':');
        out.append(std::to_string(port));
    }
    return out;
}

}  // namespace

auto parse_response_head(std::string_view buffered) -> Result<std::optional<ResponseHead>> {
    const std::string_view bytes = buffered;
    const auto end = head_block_end(bytes);
    if (end == std::string_view::npos) {
        if (bytes.size() > AURORA_HTTP_MAX_HEAD_BYTES) {
            return protocol_error("response header block has no blank line closing it within " +
                                  std::to_string(AURORA_HTTP_MAX_HEAD_BYTES) + " bytes");
        }
        return std::optional<ResponseHead>{};
    }
    if (end > AURORA_HTTP_MAX_HEAD_BYTES) {
        return protocol_error("response header block is " + std::to_string(end) + " bytes, above the " +
                              std::to_string(AURORA_HTTP_MAX_HEAD_BYTES) + " byte limit");
    }
    const auto block = bytes.substr(0, end);
    const auto status_end = block.find('\n');
    auto status_line = block.substr(0, status_end);
    if (!status_line.empty() && status_line.back() == '\r') {
        status_line.remove_suffix(1);
    }
    ResponseHead head;
    if (!parse_status_line(status_line, head)) {
        return protocol_error("response status line is not \"HTTP/<major>.<minor> <NNN> [reason]\": '" +
                              std::string{status_line} + "'");
    }
    std::vector<Headers::Entry> fields;
    if (!parse_header_fields(block.substr(status_end + 1), fields)) {
        return protocol_error("response header field is malformed (missing colon, illegal name, or leading obs-fold)");
    }
    for (const auto &field : fields) {
        head.headers.append(field.name, field.value);
    }
    head.header_bytes = end;
    return std::optional<ResponseHead>{std::move(head)};
}

auto framing_of(const ResponseHead &head, bool request_is_head) -> Result<BodyFraming> {
    const auto te_values = head.headers.values("Transfer-Encoding");
    const auto cl_values = head.headers.values("Content-Length");
    bool chunked = false;
    bool other_te = false;
    scan_transfer_encoding(te_values, chunked, other_te);

    // `Content-Length` 与 `Transfer-Encoding` 并存即报文走私：中间代理各自采信哪条不可知，本库不猜。
    if (!cl_values.empty() && !te_values.empty()) {
        return protocol_error(
            "response declares both Content-Length and Transfer-Encoding; framing is ambiguous (request smuggling "
            "vector), aborting");
    }
    for (const auto &length : cl_values) {
        if (!is_valid_content_length(length)) {
            return protocol_error("Content-Length value '" + length + "' is not a non-negative decimal integer");
        }
    }
    if (!content_length_consistent(cl_values)) {
        return protocol_error("response carries multiple Content-Length headers with differing values");
    }
    const bool status_no_body =
        (head.status >= AURORA_HTTP_STATUS_1XX_MIN && head.status <= AURORA_HTTP_STATUS_1XX_MAX) ||
        head.status == AURORA_HTTP_STATUS_NO_CONTENT || head.status == AURORA_HTTP_STATUS_NOT_MODIFIED;
    // HEAD 的应答与无体状态码恒无体：此时传输编码是否可实现无关紧要，不必报错。
    if (request_is_head || status_no_body) {
        return BodyFraming::None;
    }
    if (other_te) {
        return protocol_error(
            "response uses a Transfer-Encoding this build does not decode (only \"chunked\" is supported)");
    }
    if (chunked) {
        return BodyFraming::Chunked;
    }
    if (!cl_values.empty()) {
        return BodyFraming::Length;
    }
    return BodyFraming::UntilClose;
}

auto render_request_head(std::string_view method_text, const Url &target, const Headers &fields, bool absolute_form)
    -> Result<std::string> {
    if (!is_token(method_text)) {
        return request_error("request method '" + std::string{method_text} +
                             "' is not an HTTP token (empty or contains separators)");
    }
    // 请求目标（origin-form）：path 为空补 '/'，非空但不以 '/' 开头时补前导 '/'，随后接查询串；不含片段。
    std::string request_target;
    if (auto path = target.path(); path.empty()) {
        request_target = "/";
    } else if (path.front() != '/') {
        request_target.push_back('/');
        request_target.append(path);
    } else {
        request_target = path;
    }
    if (target.has_query()) {
        request_target.push_back('?');
        request_target.append(target.query());
    }
    // 绝对形态在 origin-form 之前冠上 `scheme://authority`（`RFC 9112 §3.2.2`）。authority 用与 `Host` 同一套
    // 端口取舍（缺省端口不写），且刻意不含 userinfo 与片段：前者是凭据、不该出现在线上（与代理凭据同一条
    // 纪律），后者按 `RFC 9110 §7.2` 只属客户端，从来不发往对端。
    if (absolute_form) {
        if (target.host().empty()) {
            return request_error("cannot render an absolute-form request target: target has an empty host");
        }
        std::string absolute;
        absolute.reserve(target.scheme().size() + request_target.size() + 24);
        absolute.append(target.scheme());
        absolute.append("://");
        absolute.append(authority(target.host(), displayed_port(target)));
        absolute.append(request_target);
        request_target = std::move(absolute);
    }

    // 字段名与字段值先行校验：CR / LF 是头部注入的唯一入口，必须在拼出任何字节之前拦住。
    for (const auto &field : fields.entries()) {
        if (!is_token(field.name)) {
            return request_error("header name '" + field.name + "' is not an HTTP token");
        }
        if (field.value.find_first_of("\r\n") != std::string::npos) {
            return request_error("header '" + field.name +
                                 "' value contains a CR or LF, which would inject an extra header line");
        }
    }

    std::string out;
    out.reserve(64 + request_target.size() + (fields.size() * 32));
    out.append(method_text);
    out.push_back(' ');
    out.append(request_target);
    out.append(" HTTP/1.1\r\n");

    if (!fields.contains("Host")) {
        if (target.host().empty()) {
            return request_error(
                "cannot derive a Host header: target has an empty authority and no explicit Host field");
        }
        out.append("Host: ").append(authority(target.host(), displayed_port(target))).append("\r\n");
    }

    for (const auto &field : fields.entries()) {
        out.append(field.name).append(": ").append(field.value).append("\r\n");
    }
    out.append("\r\n");
    return out;
}

auto render_request_chunk(std::string_view data) -> std::string {
    // 长度行 + 数据 + 收尾 CRLF；空段即终止块，其后不再挂 trailer 行（本库发出侧不产 trailer）。
    std::string out = to_lower_hex(static_cast<std::uint64_t>(data.size()));
    out.append("\r\n");
    out.append(data);
    out.append("\r\n");
    return out;
}

auto ChunkedDecoder::push(std::string_view bytes) -> Result<void> {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    while (true) {
        switch (phase_) {
            case Phase::SizeLine: {
                const std::string_view view{buffer_.data(), buffer_.size()};
                std::size_t line_end = 0;
                const auto line = take_line(view, line_end);
                if (line_end == std::string_view::npos) {
                    if (buffer_.size() > AURORA_HTTP_MAX_LINE_BYTES) {
                        return protocol_error("chunk size line has no CRLF within " +
                                              std::to_string(AURORA_HTTP_MAX_LINE_BYTES) + " bytes");
                    }
                    return Result<void>{};
                }
                const auto semi = line.find(';');  // 块扩展参数按 RFC 忽略
                std::uint64_t size = 0;
                if (!parse_chunk_size(semi == std::string_view::npos ? line : line.substr(0, semi), size)) {
                    return protocol_error("chunk size '" + std::string{line.substr(0, semi)} +
                                          "' is not a hexadecimal length");
                }
                if (size > AURORA_HTTP_MAX_CHUNK_BYTES) {
                    return protocol_error("chunk size " + std::to_string(size) + " is above the " +
                                          std::to_string(AURORA_HTTP_MAX_CHUNK_BYTES) + " byte per-chunk limit");
                }
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(line_end));
                if (size == 0) {
                    done_ = true;
                    phase_ = Phase::TrailerLine;
                } else {
                    remaining_ = size;
                    phase_ = Phase::Data;
                }
                break;
            }
            case Phase::Data: {
                const std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(remaining_), buffer_.size());
                pending_.insert(pending_.end(), buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(take));
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(take));
                remaining_ -= take;
                if (remaining_ > 0) {
                    return Result<void>{};  // 当前块还没读满
                }
                phase_ = Phase::DataCrlf;
                break;
            }
            case Phase::DataCrlf: {
                if (buffer_.size() < 2) {
                    return Result<void>{};  // 块尾 CRLF 落在下一次读里
                }
                if (buffer_[0] != '\r' || buffer_[1] != '\n') {
                    return protocol_error("chunk data is not followed by the required CRLF");
                }
                buffer_.erase(buffer_.begin(), buffer_.begin() + 2);
                phase_ = Phase::SizeLine;
                break;
            }
            case Phase::TrailerLine: {
                const std::string_view view{buffer_.data(), buffer_.size()};
                std::size_t line_end = 0;
                const auto line = take_line(view, line_end);
                if (line_end == std::string_view::npos) {
                    if (buffer_.size() > AURORA_HTTP_MAX_HEAD_BYTES) {
                        return protocol_error("chunked trailer block has no blank line within " +
                                              std::to_string(AURORA_HTTP_MAX_HEAD_BYTES) + " bytes");
                    }
                    return Result<void>{};
                }
                // `line` 是指向 `buffer_` 的视图，erase 会让其内容前移，故必须在 erase 之前把名字与值抄出来。
                if (line.empty()) {
                    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(line_end));
                    phase_ = Phase::Ended;  // trailer 由空行收尾
                    break;
                }
                const auto colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0 || !is_token(line.substr(0, colon))) {
                    return protocol_error("chunked trailer field '" + std::string{line} + "' is malformed");
                }
                std::string trailer_name{line.substr(0, colon)};
                std::string trailer_value{trim_ows(line.substr(colon + 1))};
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(line_end));
                trailers_.append(trailer_name, trailer_value);
                break;
            }
            case Phase::Ended: {
                if (!buffer_.empty()) {
                    if (retain_leftover_) {
                        // 容忍模式（连接池复用的存侧半边）：富余字节存下来交调用方裁决，不判畸形。
                        leftover_.insert(leftover_.end(), buffer_.begin(), buffer_.end());
                        buffer_.clear();
                        return Result<void>{};
                    }
                    return protocol_error(
                        "received bytes after the terminating chunk and its trailer block; that is the start of the "
                        "next message, which this decoder does not consume");
                }
                return Result<void>{};
            }
        }
    }
}

auto ChunkedDecoder::drain(std::vector<char> &out) -> void {
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

namespace {

/// @brief 构造「物化正文越限」错误（本层唯一的资源型拒绝，不与文法 / 请求写法错误混同）。
/// @param limit 生效的字节上限。
/// @param actual 越限时的已累计明文字节数。
/// @return `http-body-too-large` 错误。
auto body_too_large_error(std::uint64_t limit, std::uint64_t actual) -> Error {
    return make_error(ErrorCode::HttpBodyTooLarge, "response body of " + std::to_string(actual) +
                                                       " bytes exceeds the configured limit of " +
                                                       std::to_string(limit) + " bytes");
}

/// @brief 把已判定合法的 `Content-Length` 十进制文本转成整数（`framing_of` 已保证非负且可解析）。
/// @param text 十进制数字文本。
/// @return 转换值；溢出夹到 `UINT64_MAX`（越限判定随后由上限核算完成）。
auto parse_decimal(std::string_view text) -> std::uint64_t {
    std::uint64_t value = 0;
    for (const char c : text) {
        if (value > (std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(c - '0')) / 10U) {
            return std::numeric_limits<std::uint64_t>::max();  // 夹住，交给后续上限核算拒绝
        }
        value = (value * 10U) + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

}  // namespace

ResponseAssembler::ResponseAssembler(std::optional<std::uint64_t> max_body_bytes, bool request_is_head,
                                     bool retain_leftover)
    : max_body_bytes_{max_body_bytes}, request_is_head_{request_is_head}, retain_leftover_{retain_leftover},
      chunked_{retain_leftover} {}

auto ResponseAssembler::absorb(std::string_view bytes) -> Result<void> {
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    produced_ += bytes.size();
    if (max_body_bytes_.has_value() && produced_ > *max_body_bytes_) {
        return Result<void>{body_too_large_error(*max_body_bytes_, produced_)};
    }
    return Result<void>{};
}

auto ResponseAssembler::enter_body_phase() -> Result<void> {
    switch (framing_) {
        case BodyFraming::None:
            complete_ = true;
            phase_ = Phase::Ended;
            return Result<void>{};
        case BodyFraming::Length: {
            const auto length = head_.headers.get("Content-Length");
            if (!length.has_value()) {
                return Result<void>{protocol_error("framing is Content-Length but the header is absent")};
            }
            content_remaining_ = parse_decimal(*length);
            if (content_remaining_ == 0) {
                complete_ = true;
                phase_ = Phase::Ended;
            }
            return Result<void>{};
        }
        case BodyFraming::Chunked:
        case BodyFraming::UntilClose:
            return Result<void>{};
    }
    return Result<void>{};
}

auto ResponseAssembler::push(std::string_view bytes) -> Result<void> {
    if (phase_ == Phase::Ended) {
        if (!bytes.empty()) {
            if (retain_leftover_) {
                // 容忍模式（连接池复用的存侧半边）：完整之后的字节属下一条消息，存下来交调用方裁决
                // （富余非空即放弃复用），而不是当场判畸形。
                leftover_.append(bytes);
                return Result<void>{};
            }
            return Result<void>{protocol_error(
                "bytes after a complete response belong to the next message, which this assembler does not consume")};
        }
        return Result<void>{};
    }

    buffer_.append(bytes);

    if (phase_ == Phase::Head) {
        // 一轮投递里可能同时装着「一条信息应答 + 最终应答」（`100 Continue` 与它后面的 `200 OK` 常在
        // 同一批字节里到），故这里是一个循环而不是单次解析：1xx 收进待取队列、吃掉它自己的头部字节、
        // 留在 Head 阶段接着看下一段；只有非 1xx 才把本条消息推到体阶段。
        for (;;) {
            const std::string_view view{buffer_.data(), buffer_.size()};
            auto parsed = parse_response_head(view);
            if (!parsed) {
                return Result<void>{parsed.error()};
            }
            if (!parsed.value().has_value()) {
                return Result<void>{};  // 头部块还没读够
            }
            ResponseHead candidate = std::move(*parsed.value());
            if (candidate.status == AURORA_HTTP_STATUS_SWITCHING_PROTOCOLS) {
                return Result<void>{protocol_error(
                    "response is 101 Switching Protocols: this client does not change protocol mid-connection "
                    "(protocol upgrade belongs to the WebSocket surface)")};
            }
            if (is_informational(candidate.status)) {
                const std::size_t consumed = candidate.header_bytes;
                informationals_.push_back(std::move(candidate));
                buffer_.erase(0, consumed);
                continue;
            }
            head_ = std::move(candidate);
            head_ready_ = true;
            auto framing = framing_of(head_, request_is_head_);
            if (!framing) {
                return Result<void>{framing.error()};
            }
            framing_ = framing.value();
            // 头部之后（若有）的字节属于消息体，留在缓冲里交给本轮的体处理。
            buffer_.erase(0, head_.header_bytes);
            phase_ = Phase::Body;
            auto entered = enter_body_phase();
            if (!entered || phase_ != Phase::Body) {
                return entered;
            }
            break;
        }
    }

    // 到这里 phase_ == Body：按分帧形态消费缓冲里的体字节。
    switch (framing_) {
        case BodyFraming::Length: {
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(content_remaining_, static_cast<std::uint64_t>(buffer_.size())));
            if (take > 0) {
                auto ok = absorb(std::string_view{buffer_.data(), take});
                if (!ok) {
                    return ok;
                }
                buffer_.erase(0, take);
                content_remaining_ -= take;
            }
            if (content_remaining_ == 0) {
                complete_ = true;
                phase_ = Phase::Ended;
                if (retain_leftover_ && !buffer_.empty()) {
                    // 同批抵达的字节越过了本条消息的边界（声明长度之后还有富余）：容忍模式下存下来交裁决，
                    // 不留在缓冲里无人认领（关池时那只是无关紧要的尾巴，复用时它就是下一条的前缀）。
                    leftover_.append(buffer_);
                    buffer_.clear();
                }
            }
            return Result<void>{};
        }
        case BodyFraming::Chunked: {
            const std::string_view body{buffer_.data(), buffer_.size()};
            auto pushed = chunked_.push(body);
            buffer_.clear();  // 分块解码器自持缓冲，本层不再留原件
            if (!pushed) {
                return pushed;
            }
            std::vector<char> decoded;
            chunked_.drain(decoded);
            auto ok = absorb(std::string_view{decoded.data(), decoded.size()});
            if (!ok) {
                return ok;
            }
            if (chunked_.complete()) {
                complete_ = true;
                phase_ = Phase::Ended;
                // 终止块（及 trailer）之后的富余由解码器收在它自己的 leftover 里，转交到装配器统一出口。
                auto extra = chunked_.take_leftover();
                leftover_.append(extra.begin(), extra.end());
            }
            return Result<void>{};
        }
        case BodyFraming::UntilClose: {
            if (!buffer_.empty()) {
                auto ok = absorb(std::string_view{buffer_.data(), buffer_.size()});
                buffer_.clear();
                if (!ok) {
                    return ok;
                }
            }
            return Result<void>{};  // 只在对端 FIN 后才算完整
        }
        case BodyFraming::None:
            return Result<void>{};  // enter_body_phase 已置完整
    }
    return Result<void>{};
}

auto ResponseAssembler::peer_closed() -> Result<void> {
    if (phase_ == Phase::Body && framing_ == BodyFraming::UntilClose) {
        complete_ = true;
        phase_ = Phase::Ended;
    }
    return Result<void>{};
}

auto ResponseAssembler::take_informational() -> std::optional<ResponseHead> {
    if (informationals_.empty()) {
        return std::optional<ResponseHead>{};
    }
    std::optional<ResponseHead> taken{std::move(informationals_.front())};
    informationals_.erase(informationals_.begin());
    return taken;
}

auto ResponseAssembler::drain_body(std::vector<char> &out) -> void {
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

auto ResponseAssembler::take_leftover() -> std::string {
    auto taken = std::move(leftover_);
    leftover_.clear();
    return taken;
}

}  // namespace aurora::net::wire

// http.cpp —— HTTP 请求 / 应答值类型与方法的线上文本（CHG-010 §6、§12 的发起侧输入输出）。
//
// 落在这里的都是「不需要套接字就能定案」的部分：方法令牌的双向换算、目标 URL 的当场解析、
// 请求头部序列化前对 `Content-Length` 的补齐。真正的收发在发起侧（Client），它复用字节层
// `net/detail/http_wire.h`，本文件同样经它把请求行与字段落成字节。
#include "aurora/net/http.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/core/utf8.h"
#include "aurora/net/detail/http_wire.h"

namespace aurora::net::http {

namespace {

/// @brief 方法枚举 ↔ 线上文本的唯一换算表（顺序即 `Method` 声明顺序）。
constexpr std::array<std::pair<Method, std::string_view>, 6> AURORA_HTTP_METHOD_TABLE{
    {
        {Method::Get, "GET"},
        {Method::Head, "HEAD"},
        {Method::Post, "POST"},
        {Method::Put, "PUT"},
        {Method::Patch, "PATCH"},
        {Method::Delete, "DELETE"},
    },
};

/// @brief ASCII 单字节小写化；非大写字母原样返回（规避 locale 与负值 `char` 相关的 UB）。
auto to_lower_ascii(char c) -> char {
    const auto raw = static_cast<unsigned char>(c);
    if (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')) {
        return static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'));
    }
    return c;
}

/// @brief 整串 ASCII 小写化（用于比较参数名一类应当大小写不敏感的令牌）。
[[nodiscard]] auto to_lower_ascii_str(std::string_view s) -> std::string {
    std::string out{s};
    for (char &c : out) {
        c = to_lower_ascii(c);
    }
    return out;
}

/// @brief 裁去首尾 OWS 空白（空格 / 水平制表）。
[[nodiscard]] auto trim_ows(std::string_view s) -> std::string_view {
    const auto is_space = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// @brief 归一化 charset 令牌：转小写并丢弃 `-` 与 `_`，让 `UTF-8` / `utf_8` / `utf8` 收敛到同一判定。
[[nodiscard]] auto normalize_charset(std::string_view raw) -> std::string {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        if (c != '-' && c != '_') {
            out.push_back(to_lower_ascii(c));
        }
    }
    return out;
}

/// @brief 从 `Content-Type` 值中抠出 `charset` 参数并归一化。
///
/// 只认 `charset=` 参数（键 ASCII 大小写不敏感），其值可带双引号；参数以 `;` 分隔。找不到时返回空串，
/// 交由调用方按「缺省即 UTF-8」处理。这里只做提取与归一化，不判定 charset 是否受支持。
[[nodiscard]] auto charset_of(std::string_view content_type) -> std::string {
    std::size_t pos = 0;
    while (pos <= content_type.size()) {
        const auto semi = content_type.find(';', pos);
        const auto segment =
            (semi == std::string_view::npos) ? content_type.substr(pos) : content_type.substr(pos, semi - pos);
        const auto eq = segment.find('=');
        if (eq != std::string_view::npos && to_lower_ascii_str(trim_ows(segment.substr(0, eq))) == "charset") {
            auto value = trim_ows(segment.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return normalize_charset(value);
        }
        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }
    return {};
}

/// @brief UTF-8 字节序标记（BOM）：`text()` 在 UTF-8 路径上剥去前导三字节。
constexpr std::string_view AURORA_UTF8_BOM{"\xEF\xBB\xBF", 3};

}  // namespace

auto method_text(Method method) -> std::string_view {
    for (const auto &[candidate, text] : AURORA_HTTP_METHOD_TABLE) {
        if (candidate == method) {
            return text;
        }
    }
    return {};
}

auto method_from_text(std::string_view text) -> Result<Method> {
    for (const auto &[candidate, candidate_text] : AURORA_HTTP_METHOD_TABLE) {
        if (candidate_text == text) {
            return Result<Method>{candidate};
        }
    }
    return Result<Method>{
        make_error(ErrorCode::HttpRequestInvalid, "unknown HTTP method text: '" + std::string{text} + "'")};
}

Request::Request(Method method, std::string_view url) : method_{method} {
    if (auto parsed = Url::parse(url); parsed) {
        url_.emplace(parsed.value());
    } else {
        url_error_.emplace(parsed.error());
    }
}

auto Request::get(std::string_view url) -> Request { return Request{Method::Get, url}; }
auto Request::head(std::string_view url) -> Request { return Request{Method::Head, url}; }
auto Request::post(std::string_view url) -> Request { return Request{Method::Post, url}; }
auto Request::put(std::string_view url) -> Request { return Request{Method::Put, url}; }
auto Request::patch(std::string_view url) -> Request { return Request{Method::Patch, url}; }
auto Request::delete_(std::string_view url) -> Request { return Request{Method::Delete, url}; }

auto Request::header(std::string_view name, std::string_view value) -> Request & {
    headers_.append(name, value);
    return *this;
}

auto Request::set_header(std::string_view name, std::string_view value) -> Request & {
    headers_.set(name, value);
    return *this;
}

auto Request::body(std::vector<char> body) -> Request & {
    // HEAD 请求不得携带消息体：忽略之，而不是让字节层去拒绝一条本不该存在的请求。
    if (!is_head()) {
        body_ = std::move(body);
    }
    return *this;
}

auto Request::timeout(std::chrono::milliseconds value) -> Request & {
    timeouts_.total = value;
    return *this;
}

auto Request::timeouts(Timeouts value) -> Request & {
    timeouts_ = value;
    return *this;
}

auto Request::retry_policy(RetryPolicy value) -> Request & {
    retry_policy_ = value;
    return *this;
}

auto Request::no_cache(bool value) -> Request & {
    no_cache_ = value;
    return *this;
}

auto Request::max_redirects(int value) -> Request & {
    max_redirects_ = value;
    return *this;
}

auto Request::on_progress(ProgressHandler handler) -> Request & {
    on_progress_ = std::move(handler);
    return *this;
}

auto Request::render_head() const -> Result<std::string> {
    if (!url_.has_value()) {
        // 目标解析失败在工厂就已定案，这里只是把那条错误原样交出去，不再另编一句。
        return Result<std::string>{*url_error_};
    }
    Headers fields = headers_;
    // 有体且调用方未自行声明长度或分帧时，按体长度补 `Content-Length`；HEAD 恒无体，不补。
    if (!body_.empty() && !fields.contains("Content-Length") && !fields.contains("Transfer-Encoding")) {
        fields.set("Content-Length", std::to_string(body_.size()));
    }
    return wire::render_request_head(method_text(method_), *url_, fields);
}

Response::Response(std::uint16_t status, std::string reason, Headers headers, std::vector<char> body, bool from_cache)
    : status_{status}, reason_{std::move(reason)}, headers_{std::move(headers)}, body_{std::move(body)},
      from_cache_{from_cache} {}

auto Response::header(std::string_view name) const -> std::optional<std::string> { return headers_.get(name); }

auto Response::text() const -> Result<std::string> {
    std::string charset;
    if (const auto content_type = headers_.get("Content-Type"); content_type.has_value()) {
        charset = charset_of(content_type.value());
    }

    // 无 `Content-Type` 或未标 `charset`：按现代缺省视作 UTF-8（RFC 9231 口径）。
    if (charset.empty() || charset == "utf8") {
        std::string_view bytes{body_.data(), body_.size()};
        if (bytes.starts_with(AURORA_UTF8_BOM)) {
            bytes.remove_prefix(AURORA_UTF8_BOM.size());  // 剥 UTF-8 BOM，勿把 EF BB BF 混进文本
        }
        // 非法 UTF-8 序列一律原样直传、绝不替换（§12③）：std::string 本就是字节容器，交由调用方校验。
        return Result<std::string>{std::string{bytes}};
    }

    if (charset == "iso88591" || charset == "latin1") {
        std::string out;
        out.reserve(body_.size());
        for (const char byte : body_) {
            out += utf8_encode(static_cast<unsigned char>(byte));  // Latin-1 每字节恰为码点 U+0000..U+00FF
        }
        return Result<std::string>{std::move(out)};
    }

    return Result<std::string>{
        make_error(ErrorCode::HttpCharsetUnsupported, "unsupported response charset: '" + charset + "'")};
}

auto error_class_of(ErrorCode code) -> HttpErrorClass {
    // 归因档按「失败发生在哪一跳」定，动作（能不能重试）留给 `Error::retryable`：同一档里可重试与
    // 不可重试的码并存是刻意的，把动作折进档位就得为每枚码再造一枚枚举，反而没人敢改。
    switch (code) {
        case ErrorCode::NetCancelled:  // 调用方主动收摊：链路事实一律让位
            return HttpErrorClass::Cancelled;
        case ErrorCode::HttpTimeout:
        case ErrorCode::NetRecvTimeout:
        case ErrorCode::NetConnectTimeout:
            return HttpErrorClass::Timeout;
        case ErrorCode::NetDnsFailed:  // 名字解析不了，还没走到链路
            return HttpErrorClass::Dns;
        case ErrorCode::NetTlsHandshakeFailed:
        case ErrorCode::NetTlsCertRejected:
        case ErrorCode::NetTlsVersionMismatch:
        case ErrorCode::NetTlsPinMismatch:
            return HttpErrorClass::Tls;
        case ErrorCode::NetProxyUnreachable:
        case ErrorCode::NetProxyAuthRequired:
        case ErrorCode::NetProxyProtocol:
        case ErrorCode::NetProxyConfigUnreadable:  // 「读不到系统代理」是代理这一跳的事，不是调用点写错
            return HttpErrorClass::Proxy;
        case ErrorCode::NetConnectFailed:
        case ErrorCode::NetSocketFailed:
        case ErrorCode::NetRuntimeFailed:
        case ErrorCode::NetWaitFailed:
        case ErrorCode::NetSendFailed:
        case ErrorCode::NetRecvFailed:
        case ErrorCode::NetBindFailed:  // 服务端面：发起侧不产出，仍给出档而非「未知」
        case ErrorCode::NetListenFailed:  // 同上
        case ErrorCode::NetAcceptFailed:  // 同上
            return HttpErrorClass::Unreachable;
        case ErrorCode::HttpProtocolError:
        case ErrorCode::HttpBodyCorrupt:
        case ErrorCode::HttpRedirectLimit:  // 对端把重定向写成了环
            return HttpErrorClass::Protocol;
        default:
            // 兜底档覆盖两类：本族里「动作在代码或配置里而不在网络上」的 16 枚（`net-url-invalid` /
            // `http-request-invalid` / `net-endpoint-invalid` / `net-platform-unavailable` /
            // `net-tls-engine-unavailable` / `net-option-failed` / `net-option-conflict` /
            // `net-timeout-option-failed` / `net-proxy-config-invalid` / `net-proxy-unsupported` /
            // `net-framer-config-invalid` / `net-buffer-limit` / `net-framer-overflow` /
            // `http-body-too-large` / `http-charset-unsupported` / `http-content-encoding-unsupported`），
            // 以及压根不属于网络交换的其它族码。逐码清单以 `specification/06-app-platform.md` §8.8.22 与
            // `utest_net_http` 的期望表为准（本 switch 与那两处同批改）；分型是观测口而非校验口，故不报错、只归档。
            return HttpErrorClass::Invalid;
    }
}

auto error_class_of_status(std::uint16_t status) -> HttpErrorClass {
    // 400 起才算「状态码本身即失败」：3xx 的重定向由发起侧按 Location 处理完才交出最终应答，2xx 与
    // 1xx 走到这里时没有失败可归因。
    return status >= 400 ? HttpErrorClass::Status : HttpErrorClass::Invalid;
}

auto error_class_text(HttpErrorClass error_class) -> std::string_view {
    switch (error_class) {
        case HttpErrorClass::Invalid:
            return "invalid";
        case HttpErrorClass::Dns:
            return "dns";
        case HttpErrorClass::Unreachable:
            return "unreachable";
        case HttpErrorClass::Proxy:
            return "proxy";
        case HttpErrorClass::Tls:
            return "tls";
        case HttpErrorClass::Timeout:
            return "timeout";
        case HttpErrorClass::Protocol:
            return "protocol";
        case HttpErrorClass::Status:
            return "status";
        case HttpErrorClass::Cancelled:
            return "cancelled";
    }
    return "invalid";  // 越界值（非本枚举的合法档）折回兜底档，与 error_class_of 的 default 同一口径
}

}  // namespace aurora::net::http

// http.cpp —— HTTP 请求 / 应答值类型与方法的线上文本。
//
// 落在这里的都是「不需要套接字就能定案」的部分：方法令牌的双向换算、目标 URL 的当场解析、字节范围与
// `Content-Range` 的双向折算、请求头部序列化时对体形态与 `Range` / `If-Range` / `Expect` 的补齐。体形态
// 的判定不在本文件自成一个分支——它跟「写完头部之后到底往外泵什么字节」是同一件事，故收在
// `net/detail/http_body.h` 里由 `plan_request_body()` 单点裁决，本文件与发起侧的体泵各问一次。真正的收发
// 在发起侧（Client），它复用字节层 `net/detail/http_wire.h`，本文件同样经它把请求行与字段落成字节。
#include "aurora/net/http.h"

#include <array>
#include <charconv>
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
#include "aurora/net/detail/http_body.h"
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

/// @brief 解析一段纯十进制非负整数（首尾空白已由 `trim_ows` 裁过；任一字符非数字即整体失败）。
///
/// 溢出与带符号都归「解不出」：字节偏移在线上是没有符号也没有进位的十进制串，能解析出别的东西只说明
/// 那句话本就不是一个偏移。范围两端的数字都可以任意大（`RFC 9110 §14.1.2` 明确要求接收方预备大数），
/// 故超出 `std::uint64_t` 时同样回空而非裁到上限——裁出来的偏移比一句「看不懂」更容易被当成事实。
/// @param text 待解析文本。
/// @return 解析出的值；文法不合或超出可表示范围为 `nullopt`。
[[nodiscard]] auto parse_decimal(std::string_view text) -> std::optional<std::uint64_t> {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char *const first = text.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 要 [first, last) 两端
    const char *const last = first + text.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// @brief ASCII 大小写不敏感地判断前缀（用于 `bytes` 这类单位令牌）。
/// @param text 待判文本。
/// @param prefix 前缀；本身应全是小写字母。
/// @return 以该前缀开头为真。
[[nodiscard]] auto starts_with_ci(std::string_view text, std::string_view prefix) -> bool {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (to_lower_ascii(text[i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

/// @brief 一句「请求方的声明本身有问题」的载荷（越界范围之类，起手即拒，不留到线上）。
/// @param detail 点名是哪一枚旋钮、为什么不能用。
/// @return `http-request-invalid` 的错误对象。
[[nodiscard]] auto request_form_invalid(const std::string &detail) -> Error {
    return make_error(ErrorCode::HttpRequestInvalid, detail);
}

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

auto ByteRange::is_valid() const -> bool {
    if (!first.has_value()) {
        // 后缀形态：`last` 装的是长度，零长度后缀不可满足（`RFC 9110 §14.1.2` 把「非零后缀长度」列为
        // 唯一可满足形态），写上线只会换来一句 416。
        return last.has_value() && last.value() > 0;
    }
    if (last.has_value() && last.value() < first.value()) {
        return false;  // 终点在起点之前：没有这样的区间
    }
    return true;
}

auto ByteRange::to_header_value() const -> std::string {
    if (!is_valid()) {
        return {};
    }
    if (!first.has_value()) {
        // `is_valid()` 已保证后缀形态带着长度；这里再问一次只为让静态检查也看见同一件事。
        return last.has_value() ? "bytes=-" + std::to_string(*last) : std::string{};
    }
    std::string out = "bytes=" + std::to_string(first.value()) + "-";  // 开放尾端以此为止
    if (last.has_value()) {
        out += std::to_string(last.value());
    }
    return out;
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

auto Request::request_id(std::string value) -> Request & {
    // 空串是有意义的一档（「这次不要标识」），故原样存下，不折叠成 `nullopt`——`nullopt` 说的是「跟随缺省」。
    request_id_ = std::move(value);
    return *this;
}

auto Request::body(std::vector<char> body) -> Request & {
    // HEAD 请求不得携带消息体：忽略之，而不是让字节层去拒绝一条本不该存在的请求。
    if (!is_head()) {
        body_ = std::move(body);
        body_source_ = nullptr;  // 两种交法互斥，后设者生效
    }
    return *this;
}

auto Request::body(BodySource &source) -> Request & {
    if (!is_head()) {
        body_source_ = &source;
        body_.clear();  // 与内存体互斥：同时挂着会让体形态判定有两个输入，见 detail::plan_request_body
    }
    return *this;
}

auto Request::range(ByteRange value) -> Request & {
    range_ = value;
    return *this;
}

auto Request::if_range(std::string value) -> Request & {
    // 空串与未设同义：一条空值的 `If-Range` 字段线上不携带任何条件，留着只多一次往返与一处歧义。
    if (value.empty()) {
        if_range_.reset();
    } else {
        if_range_ = std::move(value);
    }
    return *this;
}

auto Request::expect_continue(bool value) -> Request & {
    expect_continue_ = value;
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

auto Request::render_head(bool absolute_form) const -> Result<std::string> {
    if (!url_.has_value()) {
        // 目标解析失败在工厂就已定案，这里只是把那条错误原样交出去，不再另编一句。
        // ⚠️ `url_` 与 `url_error_` 是工厂里配对的二选一（`url_` 空即 `url_error_` 必有值，反之亦然），
        // 该不变式跨函数不可见 ⇒ 就地豁免；改写成 `value_or` 会编出一条与工厂定案不同的错误，故不那样写。
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return Result<std::string>{url_error_.value()};
    }
    Headers fields = headers_;
    auto plan = detail::plan_request_body(*this);
    if (!plan) {
        return Result<std::string>{plan.error()};
    }
    // 体形态：调用方声明过就原样保留（那份声明与体泵读到的计划是同一份，见 detail::http_body.h）；没声明
    // 才按推定补一句。无体形态不补任何字段。
    if (!plan.value().declared) {
        switch (plan.value().framing) {
            case detail::RequestBodyFraming::None:
                break;
            case detail::RequestBodyFraming::Length:
                fields.set("Content-Length", std::to_string(plan.value().length.value_or(0)));
                break;
            case detail::RequestBodyFraming::Chunked:
                fields.set("Transfer-Encoding", "chunked");
                break;
        }
    }
    // 分段请求：设有 `range()` 才上线，`If-Range` 是它的附属（没有待修饰的分段请求时它只是一次无谓往返）。
    if (range_.has_value()) {
        if (!range_->is_valid()) {
            return Result<std::string>{request_form_invalid(
                "malformed byte range: a range needs a first byte offset, or a non-zero suffix length, and its "
                "last byte must not precede its first")};
        }
        fields.set("Range", range_->to_header_value());
        if (if_range_.has_value()) {
            fields.set("If-Range", *if_range_);
        }
    }
    // 先问再发体：无体可发时这条字段没有意义，且调用方自己写过的 `Expect` 永远优先。
    if (expect_continue_ && plan.value().has_body && !fields.contains("Expect")) {
        fields.set("Expect", "100-continue");
    }
    return wire::render_request_head(method_text(method_), *url_, fields, absolute_form);
}

Response::Response(std::uint16_t status, std::string reason, Headers headers, std::vector<char> body, bool from_cache)
    : status_{status}, reason_{std::move(reason)}, headers_{std::move(headers)}, body_{std::move(body)},
      from_cache_{from_cache} {}

auto Response::header(std::string_view name) const -> std::optional<std::string> { return headers_.get(name); }

auto Response::content_range() const -> std::optional<ContentRange> {
    const auto field = headers_.get("Content-Range");
    if (!field.has_value()) {
        return std::nullopt;
    }
    // 文法（`RFC 9110 §14.4`）三种：`bytes 0-499/1234`、`bytes 500-1234/1234`、`bytes */1234`。本库是唯一
    // 会发出范围请求的一方，故只认 `bytes` 单位；解不出的形态一律回空而不报错——那是观测口，把「对端把这条
    // 字段写坏了」折成一次失败会掩盖真正要交的应答。
    constexpr std::string_view unit{"bytes"};
    auto value = trim_ows(*field);
    if (!starts_with_ci(value, unit)) {
        return std::nullopt;
    }
    value = trim_ows(value.substr(unit.size()));
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto spec = trim_ows(value.substr(0, slash));
    const auto total_text = trim_ows(value.substr(slash + 1));
    ContentRange out;
    if (total_text != "*" && !total_text.empty()) {
        const auto total = parse_decimal(total_text);
        if (!total.has_value()) {
            return std::nullopt;
        }
        out.total = total;
    }
    if (spec == "*") {
        return out;  // 范围未知（`incomplete-range`）：只有总长，或连总长也用 `*` 表达
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = parse_decimal(trim_ows(spec.substr(0, dash)));
    const auto last = parse_decimal(trim_ows(spec.substr(dash + 1)));
    if (!first.has_value() || !last.has_value()) {
        return std::nullopt;
    }
    out.first = first;
    out.last = last;
    return out;
}

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
            // 兜底档覆盖两类：本族里「动作在代码或配置里而不在网络上」的 18 枚（`net-url-invalid` /
            // `http-request-invalid` / `net-endpoint-invalid` / `net-platform-unavailable` /
            // `net-tls-engine-unavailable` / `net-option-failed` / `net-option-conflict` /
            // `net-timeout-option-failed` / `net-proxy-config-invalid` / `net-proxy-unsupported` /
            // `net-framer-config-invalid` / `net-buffer-limit` / `net-framer-overflow` /
            // `net-policy-violated` / `http-body-too-large` / `http-charset-unsupported` /
            // `http-content-encoding-unsupported` / `http-body-not-replayable`），
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

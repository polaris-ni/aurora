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
    timeout_ = value;
    return *this;
}

auto Request::max_redirects(int value) -> Request & {
    max_redirects_ = value;
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

Response::Response(std::uint16_t status, std::string reason, Headers headers, std::vector<char> body)
    : status_{status}, reason_{std::move(reason)}, headers_{std::move(headers)}, body_{std::move(body)} {}

auto Response::header(std::string_view name) const -> std::optional<std::string> { return headers_.get(name); }

}  // namespace aurora::net::http

// http_body.h 的实现：请求体线上形态的唯一判定处（见该头文件顶部的四条口径）。
//
// 本件不含协程、不碰套接字、不读文件：它只把「一份内存体 / 一枚推送源 / 调用方自报的字段」这三类输入
// 折算成一份可复现的计划，故可在零网络下逐值断言——发起侧两条腿（`send` 与 `open_stream`）与
// `render_head()` 都只问它，判定改一处即三处同改，不存在漏改第三处的可能。
#include "aurora/net/detail/http_body.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/http.h"

namespace aurora::net::detail {

namespace {

/// @brief 一枚「调用方的声明本身有问题」的载荷：形态越界都在起手定案，不留到线上让对端猜。
/// @param detail 点名是哪一枚字段、为什么不合用。
/// @return `http-request-invalid` 的错误对象。
[[nodiscard]] auto declared_form_invalid(const std::string &detail) -> Error {
    return make_error(ErrorCode::HttpRequestInvalid, detail);
}

/// @brief ASCII 单字节小写化；非大写字母原样返回（规避 locale 与负值 `char` 相关的 UB）。
[[nodiscard]] auto to_lower_ascii(char c) -> char {
    const auto raw = static_cast<unsigned char>(c);
    if (raw >= static_cast<unsigned char>('A') && raw <= static_cast<unsigned char>('Z')) {
        return static_cast<char>(raw + static_cast<unsigned char>('a' - 'A'));
    }
    return c;
}

/// @brief 裁去首尾 OWS（空格 / 水平制表）后整串 ASCII 小写化，用于比对字段里的单个令牌。
[[nodiscard]] auto normalized_token(std::string_view value) -> std::string {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    std::string out{value};
    for (char &c : out) {
        c = to_lower_ascii(c);
    }
    return out;
}

/// @brief 解析 `Content-Length` 的声明值：首尾 OWS 允许，其余必须全是十进制数字。
/// @param value 字段原文。
/// @param out 解析出的字节数；仅在返回 true 时被写入。
/// @return 纯十进制且未超出 `std::uint64_t` 为 true。
[[nodiscard]] auto parse_declared_length(std::string_view value, std::uint64_t &out) -> bool {
    auto trimmed = value;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const char *const first = trimmed.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): from_chars 要 [first, last) 两端
    const char *const last = first + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
        return false;
    }
    out = parsed;
    return true;
}

}  // namespace

auto plan_request_body(const aurora::net::http::Request &request) -> Result<RequestBodyPlan> {
    RequestBodyPlan plan;
    // 待写出的体有两种交法，任一存在即「有体」：内存体看长度，流式体看是否挂了源（源此刻还没被问段，
    // 挂了就算有体——`total_size()` 为零的源照样要走完一次 `next_chunk()` 才知道结束，形态不能因此摇摆）。
    plan.has_body = request.body_source() != nullptr || !request.body().empty();

    const auto declared_lengths = request.headers().values("Content-Length");
    const auto declared_encodings = request.headers().values("Transfer-Encoding");
    if (declared_lengths.size() + declared_encodings.size() > std::size_t{1}) {
        // 两枚字段并存，或同名字段各写多条：线上只有一句该作数，而本库不替调用方挑。
        return Result<RequestBodyPlan>{declared_form_invalid(
            "request body framing is ambiguous: declare exactly one of Content-Length and Transfer-Encoding")};
    }
    if (!declared_encodings.empty()) {
        if (normalized_token(declared_encodings.front()) != "chunked") {
            return Result<RequestBodyPlan>{
                declared_form_invalid("unsupported Transfer-Encoding: '" + declared_encodings.front() +
                                      "' (a client sends request bodies as chunked or nothing)")};
        }
        plan.framing = RequestBodyFraming::Chunked;
        plan.declared = true;
        return Result<RequestBodyPlan>{plan};
    }
    if (!declared_lengths.empty()) {
        std::uint64_t parsed = 0;
        if (!parse_declared_length(declared_lengths.front(), parsed)) {
            return Result<RequestBodyPlan>{
                declared_form_invalid("Content-Length is not a decimal number: '" + declared_lengths.front() + "'")};
        }
        plan.framing = RequestBodyFraming::Length;
        plan.length = parsed;
        plan.declared = true;
        return Result<RequestBodyPlan>{plan};
    }

    // 调用方没声明，形态由体的交法现地推定。
    if (!plan.has_body) {
        return Result<RequestBodyPlan>{plan};  // None：两枚字段都不补
    }
    if (auto *source = request.body_source(); source != nullptr) {
        if (const auto total = source->total_size(); total.has_value()) {
            plan.framing = RequestBodyFraming::Length;
            plan.length = total;
        } else {
            plan.framing = RequestBodyFraming::Chunked;
        }
        return Result<RequestBodyPlan>{plan};
    }
    plan.framing = RequestBodyFraming::Length;
    plan.length = static_cast<std::uint64_t>(request.body().size());
    return Result<RequestBodyPlan>{plan};
}

// 无体即无段可交：`has_body` 是计划算出的事实（挂了源就算有体，哪怕它一段也不吐），与形态无关。
RequestBodyPump::RequestBodyPump(const aurora::net::http::Request &request, const RequestBodyPlan &plan)
    : request_(&request), plan_(plan), stage_(plan.has_body ? Stage::Pull : Stage::Finished),
      terminator_pending_(plan.has_body && plan.framing == RequestBodyFraming::Chunked) {}

auto RequestBodyPump::frame(std::vector<char> payload) const -> std::vector<char> {
    if (plan_.framing != RequestBodyFraming::Chunked) {
        return payload;  // 定长形态：正文原样上线，框架就是没有框架
    }
    const std::string_view view{payload.data(), payload.size()};
    const auto segment = wire::render_request_chunk(view);
    return std::vector<char>{segment.begin(), segment.end()};
}

auto RequestBodyPump::drain_to_end() -> std::vector<char> {
    stage_ = Stage::Finished;
    if (!terminator_pending_) {
        return {};
    }
    terminator_pending_ = false;
    const auto segment = wire::render_request_chunk(std::string_view{});  // 空段即 `0\r\n\r\n`
    return std::vector<char>{segment.begin(), segment.end()};
}

auto RequestBodyPump::next_segment() -> Result<std::vector<char>> {
    using Segment = Result<std::vector<char>>;
    // 上一段太大没写完的剩余部分优先：它已经在内存里，不必再问来处。
    if (!pending_.empty()) {
        const std::size_t length = std::min(AURORA_HTTP_WRITE_CHUNK, pending_.size());
        std::vector<char> slice{pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(length)};
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(length));
        payload_bytes_ += static_cast<std::uint64_t>(slice.size());
        return Segment{frame(std::move(slice))};
    }
    if (stage_ == Stage::Finished) {
        return Segment{std::vector<char>{}};  // 幂等：交完之后每问一次都是空段
    }

    auto *source = request_->body_source();
    if (source == nullptr) {
        const auto &body = request_->body();
        if (offset_ >= body.size()) {
            return Segment{drain_to_end()};
        }
        const std::size_t length = std::min(AURORA_HTTP_WRITE_CHUNK, body.size() - offset_);
        const auto first = body.begin() + static_cast<std::ptrdiff_t>(offset_);
        std::vector<char> slice{first, first + static_cast<std::ptrdiff_t>(length)};
        offset_ += length;
        payload_bytes_ += static_cast<std::uint64_t>(slice.size());
        return Segment{frame(std::move(slice))};
    }

    auto piece = source->next_chunk();
    if (!piece) {
        return Segment{piece.error()};  // 源读失败原样上抛：本层不解释「为什么读不到」
    }
    std::vector<char> payload = std::move(piece.value());
    if (payload.empty()) {
        return Segment{drain_to_end()};
    }
    if (payload.size() > AURORA_HTTP_WRITE_CHUNK) {
        // 超单段上限：留下剩余部分，本段先写出去（分块形态切开只多几个长度行，定长形态字节序列不变）。
        pending_ = std::move(payload);
        return next_segment();
    }
    payload_bytes_ += static_cast<std::uint64_t>(payload.size());
    return Segment{frame(std::move(payload))};
}

}  // namespace aurora::net::detail

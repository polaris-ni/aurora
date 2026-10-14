// http_encoding.cpp —— HTTP 应答 `Content-Encoding` 的发起侧解压管线实现（口径条款见 `detail/http_encoding.h`）。
//
// 本文件把两条纯逻辑写在一起：编码链的文法判定（一个字节都不解压）与按链逐层还原字节（core 能力
// `aurora::decompress` 的编排）。两者都不碰平台 socket 头，也不发起任何 I/O——压缩字节从哪儿来属传输切片，
// 本层只吃已经分帧完毕的消息体。因此畸形、回落与错误映射全部可零 socket 逐值断言。

#include "aurora/net/detail/http_encoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/compression.h"
#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"

namespace aurora::net::detail {
namespace {

/// 无环节编码名：按定义「没有环节要解」，解析阶段即归入空链，不产 `Coding` 值。
constexpr std::string_view AURORA_CODING_IDENTITY = "identity";

/// @brief 单字节 ASCII 小写化；非大写字母原样返回（不经 locale 敏感的 `std::tolower`）。
/// @param c 待判字符。
/// @return 小写后的字符。
constexpr auto ascii_lower(char c) -> char {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(static_cast<unsigned char>(c) + 32);
    }
    return c;
}

/// @brief 两段文本是否 ASCII 大小写不敏感相等（长度不同直接判否）。
/// @param left 左段。
/// @param right 右段。
/// @return 逐字符（小写化后）相等为真。
constexpr auto equal_ignore_case(std::string_view left, std::string_view right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left.at(index)) != ascii_lower(right.at(index))) {
            return false;
        }
    }
    return true;
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

/// @brief 取环节的可读名（错误文本与宣告文本共用同一拼写）。
/// @param coding 编码环节。
/// @return 小写编码名。
constexpr auto coding_name(Coding coding) -> std::string_view {
    switch (coding) {
        case Coding::Gzip:
            return "gzip";
        case Coding::Deflate:
            return "deflate";
    }
    return "?";  // 不可达：枚举只有上面两枚，留此仅为编译器穷尽性告警不留洞
}

/// @brief 构造「头部块文法不合」错误（编码链里的空环节属报文问题，不属能力问题）。
/// @param detail 现场细节。
/// @return `http-protocol-error` 错误。
auto protocol_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::HttpProtocolError, std::string{detail});
}

/// @brief 构造「编码名在支持集之外」错误。
/// @param detail 现场细节（点名是哪一枚名）。
/// @return `http-content-encoding-unsupported` 错误。
auto unsupported_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::HttpContentEncodingUnsupported, std::string{detail});
}

/// @brief 构造「消息体过不了所声明的编码」错误。
/// @param detail 现场细节。
/// @return `http-body-corrupt` 错误。
auto corrupt_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::HttpBodyCorrupt, std::string{detail});
}

/// @brief 构造「解压产物越限」错误。
/// @param detail 现场细节（点名越的是哪一枚旋钮）。
/// @return `http-body-too-large` 错误。
auto too_large_error(std::string_view detail) -> Error {
    return make_error(ErrorCode::HttpBodyTooLarge, std::string{detail});
}

/// @brief 把 core 层解压失败翻成 HTTP 出口码（三条固定映射 + 一条兜底）。
///
/// 底层成因（`compression-*` 的 slug 与文本）原样保留在 detail 里：调用方在 HTTP 面只面对 `http-*` 一族，
/// 但仍能顺着文本看到真成因——两族码并存会让「该调哪个旋钮」变成第二个问题。
/// @param source core 层错误。
/// @param coding 出错环节名（写进 detail 定位是哪一层）。
/// @return 对应的 HTTP 出口错误。
auto to_http_error(const Error &source, std::string_view coding) -> Error {
    const std::string context = std::string{coding} + " [" + source.code + "]: " + source.message;
    switch (source.code_enum) {
        case ErrorCode::CompressionUnavailable:
            // 未编入能力（AURORA_ENABLE_COMPRESSION=OFF）或对端用了本构建没有的编码，是同一件事的两种说法。
            return unsupported_error(context);
        case ErrorCode::CompressionOutputTooLarge:
            return too_large_error(std::string{coding} +
                                   " crossed the max_decompressed_bytes ceiling (that knob bounds the "
                                   "transparently decompressed payload; max_body_bytes bounds the bytes that "
                                   "arrived on the wire): " +
                                   source.message);
        case ErrorCode::CompressionDataCorrupt:
        default:
            return corrupt_error(context);
    }
}

/// @brief 还原一个环节：`deflate` 的两型歧义在这里消解。
///
/// 先按 zlib 包装（`RFC 1950`，`deflate` 的名义格式）解；**仅当**它报「流损坏」才回落裸 DEFLATE
/// （`RFC 1951`，历史服务器的实际做法）。越限与能力缺席不回落——回落只会把真成因盖掉。
/// @param coding 编码环节。
/// @param input 本环节的输入字节。
/// @param options 解压选项（逐层上限）。
/// @return 成功携产物；失败携 **core 层** 错误（由调用方翻成 HTTP 出口码）。
auto decode_stage(Coding coding, std::span<const char> input, const DecompressOptions &options)
    -> Result<std::vector<char>> {
    if (coding == Coding::Gzip) {
        return aurora::decompress(CompressionFormat::Gzip, input, options);
    }
    auto wrapped = aurora::decompress(CompressionFormat::Zlib, input, options);
    if (wrapped) {
        return wrapped;
    }
    if (wrapped.error().code_enum != ErrorCode::CompressionDataCorrupt) {
        return wrapped;
    }
    auto raw = aurora::decompress(CompressionFormat::Deflate, input, options);
    if (raw) {
        return raw;
    }
    if (raw.error().code_enum != ErrorCode::CompressionDataCorrupt) {
        return raw;
    }
    return make_error(ErrorCode::CompressionDataCorrupt,
                      "neither the zlib wrapper (RFC 1950) nor a raw DEFLATE stream (RFC 1951) accepted these "
                      "bytes; the zlib attempt said: " +
                          wrapped.error().message);
}

}  // namespace

auto compression_available() -> bool {
#ifdef AURORA_ENABLE_COMPRESSION
    return true;
#else
    return false;
#endif
}

auto parse_content_encoding(std::string_view value) -> Result<std::vector<Coding>> {
    std::vector<Coding> chain;
    const auto list = trim_ows(value);
    if (list.empty()) {
        return chain;  // 空值 = 没有环节（与字段缺失同义），原始字节直通。
    }
    std::size_t cursor = 0;
    bool more = true;
    while (more) {
        const auto comma = list.find(',', cursor);
        const std::size_t stop = comma == std::string_view::npos ? list.size() : comma;
        more = comma != std::string_view::npos;
        const auto member = trim_ows(list.substr(cursor, stop - cursor));
        cursor = stop + 1;
        if (member.empty()) {
            // `gzip,,deflate` / 尾随逗号：列表里有空环节，是报文文法问题，不能被当成「没有环节」忽略掉。
            return protocol_error("Content-Encoding carries an empty coding member: '" + std::string{value} + "'");
        }
        if (equal_ignore_case(member, AURORA_CODING_IDENTITY)) {
            continue;  // identity 按定义是恒等环节：不产环节，也不终止后续环节的解析。
        }
        if (equal_ignore_case(member, "gzip")) {
            chain.push_back(Coding::Gzip);
            continue;
        }
        if (equal_ignore_case(member, "deflate")) {
            chain.push_back(Coding::Deflate);
            continue;
        }
        return unsupported_error("Content-Encoding names '" + std::string{member} +
                                 "', which is outside the advertised set (" + std::string{AURORA_HTTP_ACCEPT_ENCODING} +
                                 ")");
    }
    return chain;
}

auto decode_content_encoding(const std::vector<Coding> &chain, std::span<const char> input,
                             std::optional<std::uint64_t> max_output_bytes) -> Result<std::vector<char>> {
    if (chain.empty() || input.empty()) {
        // 空链 = 没有环节要解；空载荷 = 没有字节可解（声明了编码却给零字节体是常见中间层行为，
        // 解压空流只会凭空造出一个 `compression-data-corrupt`）。两者都交出空产物。
        return std::vector<char>{input.begin(), input.end()};
    }
    const DecompressOptions options{.max_output_bytes = max_output_bytes};
    std::vector<char> current{input.begin(), input.end()};
    // 书写次序是应用次序（`RFC 9110 §8.4`），故还原从右往左：最后应用的环节最先解。
    for (const Coding stage : chain | std::views::reverse) {
        auto decoded = decode_stage(stage, std::span<const char>{current}, options);
        if (!decoded) {
            return to_http_error(decoded.error(), coding_name(stage));
        }
        current = std::move(decoded.value());
    }
    return current;
}

}  // namespace aurora::net::detail

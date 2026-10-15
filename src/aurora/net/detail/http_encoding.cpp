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

/// 流式 `deflate` 环节判定包装型所需的最少输入字节数（zlib 的头正好占两枚）。
constexpr std::size_t AURORA_DEFLATE_PROBE_BYTES = 2;

/// zlib（`RFC 1950 §2.2`）头里 CM 字段的取值：CMF 低 4 位为 8 才是 deflate 方法。
constexpr int AURORA_ZLIB_CM_DEFLATE = 8;

/// zlib 头的校验模数：两字节拼成 16 位整数后必须被 31 整除，不整除即不是 zlib 头。
constexpr int AURORA_ZLIB_HEADER_CHECK_MODULUS = 31;

/// zlib 头 CMF 高 4 位（窗口大小编码）的上界：`2^(CINFO+8)` 最大 32 KiB，超出即非 zlib。
constexpr int AURORA_ZLIB_MAX_WINDOW_CODE = 7;

/// @brief 取一段字节里某处的可比较值（`char` 的符号位是实现定义的，先转 `unsigned char` 再升格）。
/// @param bytes 待取字节（调用方保证长度）。
/// @param index 下标。
/// @return 该字节的 0..255 值。
constexpr auto unsigned_at(std::span<const char> bytes, std::size_t index) -> int {
    // `std::span` 只提供 `operator[]`（无 `at()`），故越界防护落在调用方：本函数只被长度已核过的段调用。
    return static_cast<int>(static_cast<unsigned char>(bytes[index]));
}

/// @brief 流式路径的 `deflate` 包装型判定：只看本环节最前两枚输入字节是否构成合法 zlib 头。
///
/// 三条约束同时成立才算 zlib（`RFC 1950 §2.2`）：CM 为 deflate、窗口编码不越 32 KiB 档、两字节拼出的
/// 16 位数被 31 整除。任一不成立即按裸 DEFLATE（`RFC 1951`）建流。物化路径靠「先试再回落」消解同一歧义，
/// 流式路径不能重播已交出的字节，故只看头（口径见本文件头注释）。
/// @param first_two 本环节的头两枚输入字节（长度不足两枚即调用方的用法错误）。
/// @return 锁定到整条流的包装格式。
constexpr auto probe_deflate_format(std::span<const char> first_two) -> CompressionFormat {
    const int cmf = unsigned_at(first_two, 0);
    const int flg = unsigned_at(first_two, 1);
    const bool method_is_deflate = (cmf & 0x0F) == AURORA_ZLIB_CM_DEFLATE;
    const bool window_in_range = (cmf >> 4) <= AURORA_ZLIB_MAX_WINDOW_CODE;
    const bool header_check_matches = ((cmf << 8) | flg) % AURORA_ZLIB_HEADER_CHECK_MODULUS == 0;
    if (method_is_deflate && window_in_range && header_check_matches) {
        return CompressionFormat::Zlib;
    }
    return CompressionFormat::Deflate;
}

/// @brief 环节开局即可确定的包装格式。
///
/// `Deflate` 回空 = 待首两枚输入字节判定；`Gzip` 恒是 RFC 1952 包装，没有歧义可判。
/// @param coding 编码环节。
/// @return 已确定的格式；待判定为空。
constexpr auto known_format(Coding coding) -> std::optional<CompressionFormat> {
    if (coding == Coding::Gzip) {
        return CompressionFormat::Gzip;
    }
    return std::nullopt;
}

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

/// @brief 把编码链写成可读名单（建链失败时的文本要点名整条链，而不是只点最后一枚）。
/// @tparam Range 可迭代到 `Coding` 的范围。
/// @param chain 编码链。
/// @return 逗号分隔的小写编码名。
template <typename Range>
[[nodiscard]] auto joined_codings(const Range &chain) -> std::string {
    std::string list;
    for (const Coding coding : chain) {
        if (!list.empty()) {
            list += ", ";
        }
        list += coding_name(coding);
    }
    return list;
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

auto StreamingDecoder::create(const std::vector<Coding> &chain, std::optional<std::uint64_t> max_output_bytes,
                              StreamingEmit emit) -> Result<StreamingDecoder> {
    if (!emit) {
        return make_error(ErrorCode::GeneralInvalidArgument,
                          "a streaming decoder was built with no emit sink: bytes would be decoded and dropped");
    }
    StreamingDecoder built;
    built.emit_ = std::move(emit);
    built.max_output_bytes_ = max_output_bytes;
    if (chain.empty()) {
        return Result<StreamingDecoder>{std::move(built)};  // 空链：一个解压器都不建，push 的字节直通宿主
    }
    if (!compression_available()) {
        // 与物化路径同一出口码，只是提前到建链这一步：本构建连 `Accept-Encoding` 都不该宣告，收到带编码的
        // 载荷要么是畸形报文要么是对端越界，两种都没有「先收下半条流再失败」的理由。
        return unsupported_error("the response declares Content-Encoding '" + joined_codings(chain) +
                                 "' but this build has no decompression capability (AURORA_ENABLE_COMPRESSION=OFF)");
    }
    built.stages_.reserve(chain.size());
    // 书写次序是应用次序（`RFC 9110 §8.4`），故还原从右往左：最后应用的环节最先建环、最先接线上字节。
    for (const Coding coding : chain | std::views::reverse) {
        Stage entry;
        entry.coding = coding;
        entry.format = known_format(coding);
        entry.needs_probe = !entry.format.has_value();
        if (entry.format.has_value()) {
            if (auto opened = built.open(entry); !opened) {
                return Result<StreamingDecoder>{to_http_error(opened.error(), coding_name(coding))};
            }
        }
        built.stages_.push_back(std::move(entry));
    }
    return Result<StreamingDecoder>{std::move(built)};
}

auto StreamingDecoder::open(Stage &stage) const -> Result<void> {
    if (!stage.format.has_value()) {
        return corrupt_error("internal: a coding stage was opened before its format was determined");
    }
    const DecompressOptions options{.max_output_bytes = max_output_bytes_};
    auto created = Decompressor::create(stage.format.value(), options);
    if (!created) {
        return Result<void>{created.error()};  // core 成因原样带回，由调用方翻成 HTTP 出口码
    }
    stage.decoder = std::move(created.value());
    return Result<void>{};
}

auto StreamingDecoder::note_halt(Error error) -> void {
    if (!abort_error_.has_value()) {
        abort_error_ = std::move(error);  // 幂等：后到的中止不覆盖先到的成因
    }
}

auto StreamingDecoder::halted() const -> std::optional<Result<EmitFlow>> {
    // 宿主收摊优先于错误：同一轮 `push` 里先交出的段先被宿主拒掉，故 Stop 在字节次序上总是更早的那个事实。
    if (aborted_by_host_) {
        return Result<EmitFlow>{EmitFlow::Stop};
    }
    if (abort_error_.has_value()) {
        return Result<EmitFlow>{abort_error_.value()};
    }
    return std::nullopt;
}

auto StreamingDecoder::emit_from(std::size_t index, std::span<const char> bytes) -> Result<EmitFlow> {
    if (auto stop = halted(); stop.has_value()) {
        return stop.value();
    }
    if (bytes.empty()) {
        return Result<EmitFlow>{EmitFlow::Continue};  // 空段不打扰下一环，也不打扰宿主
    }
    if (index >= stages_.size()) {
        auto handed = emit_(bytes);
        if (!handed) {
            note_halt(handed.error());
            return handed;
        }
        if (handed.value() == EmitFlow::Stop) {
            aborted_by_host_ = true;
        }
        return handed;
    }
    auto &stage = stages_.at(index);
    std::vector<char> buffered;
    std::span<const char> pending = bytes;
    if (!stage.decoder.has_value()) {
        if (stage.needs_probe && !stage.format.has_value()) {
            stage.preamble.insert(stage.preamble.end(), bytes.begin(), bytes.end());
            if (stage.preamble.size() < AURORA_DEFLATE_PROBE_BYTES) {
                return Result<EmitFlow>{EmitFlow::Continue};  // 判定字节还没攒够：本段先按住不交
            }
            stage.format = probe_deflate_format(std::span<const char>{stage.preamble});
        }
        if (auto opened = open(stage); !opened) {
            const auto mapped = to_http_error(opened.error(), coding_name(stage.coding));
            note_halt(mapped);
            return Result<EmitFlow>{mapped};
        }
        // 判定期间攒下的字节就是本流的前缀，换出到局部缓冲再喂入：容器与视图不再互相牵制（swap 后 preamble
        // 已空，随后的段直接走「解码器已就位」那一支）。
        if (!stage.preamble.empty()) {
            buffered.swap(stage.preamble);
            pending = std::span<const char>{buffered};
        }
    }
    const auto forward = [this, index](std::span<const char> produced) -> void {
        (void)emit_from(index + 1, produced);  // 中止事实记在链上，返回值在此无需二次处理
    };
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): 就位发生在 open() 内，跨函数赋值不入路径分析
    const auto pushed = stage.decoder->push(pending, forward);
    if (auto stop = halted(); stop.has_value()) {
        return stop.value();
    }
    if (!pushed) {
        const auto mapped = to_http_error(pushed.error(), coding_name(stage.coding));
        note_halt(mapped);
        return Result<EmitFlow>{mapped};
    }
    return Result<EmitFlow>{EmitFlow::Continue};
}

auto StreamingDecoder::settle(std::size_t index) -> Result<EmitFlow> {
    if (auto stop = halted(); stop.has_value()) {
        return stop.value();
    }
    if (index >= stages_.size()) {
        return Result<EmitFlow>{EmitFlow::Continue};
    }
    auto &stage = stages_.at(index);
    if (!stage.decoder.has_value()) {
        if (stage.preamble.empty()) {
            return settle(index + 1);  // 本环一个字节都没收到：没有流要终结，下游同样没有
        }
        // 不足两枚输入字节就收摊：zlib 的头本身占两枚，故这一环只可能是裸 DEFLATE（RFC 1951）。剩下那一两枚
        // 字节照常喂入并终结——短流的「截断」定性归 core 判，本层不另造一套判据。
        stage.format = CompressionFormat::Deflate;
        if (auto opened = open(stage); !opened) {
            const auto mapped = to_http_error(opened.error(), coding_name(stage.coding));
            note_halt(mapped);
            return Result<EmitFlow>{mapped};
        }
        std::vector<char> buffered;
        buffered.swap(stage.preamble);
        if (auto fed = emit_from(index, std::span<const char>{buffered}); !fed) {
            return fed;
        }
    }
    const auto forward = [this, index](std::span<const char> produced) -> void {
        (void)emit_from(index + 1, produced);
    };
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): 终结的必是已 open 的那一环，赋值跨函数发生
    const auto flushed = stage.decoder->finish(forward);
    if (auto stop = halted(); stop.has_value()) {
        return stop.value();
    }
    if (!flushed) {
        const auto mapped = to_http_error(flushed.error(), coding_name(stage.coding));
        note_halt(mapped);
        return Result<EmitFlow>{mapped};
    }
    return settle(index + 1);
}

auto StreamingDecoder::push(std::span<const char> input) -> Result<EmitFlow> { return emit_from(0, input); }

auto StreamingDecoder::finish() -> Result<EmitFlow> { return settle(0); }

}  // namespace aurora::net::detail

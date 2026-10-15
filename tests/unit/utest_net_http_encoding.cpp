/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_encoding.h + src/aurora/net/detail/http_encoding.cpp
/// 测试说明: HTTP 应答 `Content-Encoding` 的编码链文法、逐层还原与**同一链条的流式串联**的逐值回归
///           （CHG-010 §12「内容编码」的 HTTP 接入层）。覆盖宣告集与支持集同源
///           （`AURORA_HTTP_ACCEPT_ENCODING` 自身可被解析回两枚环节）、编码名大小写不敏感与 OWS 修剪、
///           字段缺席 / 空值 / `identity` 归零环节而列表里的空环节判文法畸形、支持集外的名点名落
///           `http-content-encoding-unsupported`；逐层还原按书写次序反序进行（`gzip, deflate` 先剥 deflate
///           再剥 gzip，正序解必失败）、`deflate` 先试 zlib 包装仅在其报流损坏时回落裸 DEFLATE、空载荷配
///           已声明环节交出空产物而不是流损坏、逐层受 `max_output_bytes` 约束；core 层三枚 `compression-*`
///           一律翻成 HTTP 出口码且把底层成因留在文本里（`compression-unavailable` →
///           `http-content-encoding-unsupported`、`compression-data-corrupt` → `http-body-corrupt`、
///           `compression-output-too-large` → `http-body-too-large` 并点名 `max_decompressed_bytes`）。
///           流式那条另补四条只有逐段交付才成立的判据：`deflate` 的包装格式按**首两枚输入字节**锁定而非
///           「先试再回落」（物化路径能重播字节，流式路径不能）、判定字节不足两枚时按住不交、宿主回 `Stop`
///           后整条链停住且不再打扰宿主、截断只在 `finish()` 显形而不在 `push()` 里猜。
/// 全用例零 socket、零压缩库直调（金样为预生成的 gzip / zlib / 裸 DEFLATE 字节表），故三平台行为一致、
/// wasm 腿同样可跑；能力开关缺席时按 `AURORA_TEST_SKIP` 登记而非缺席。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/http_encoding.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_net_http_encoding {
namespace {

using aurora::net::detail::Coding;
using aurora::net::detail::EmitFlow;
using aurora::net::detail::StreamingDecoder;
using aurora::net::detail::StreamingEmit;

/// @brief 取字节表的缓冲形式（金样以 `unsigned char[]` 声明，用例里按 `std::vector<char>` 用）。
/// @tparam N 表长。
/// @param table 字节表。
/// @return 等长缓冲（压缩字节常 ≥ 0x80，写成 char 字面量会踩实现定义转换，故逐枚显式转）。
template <std::size_t N>
[[nodiscard]] auto bytes(const unsigned char (&table)[N]) -> std::vector<char> {
    std::vector<char> out;
    out.reserve(N);
    for (const unsigned char unit : table) {
        out.push_back(static_cast<char>(unit));
    }
    return out;
}

/// @brief 解析出编码链：失败即致命失败（文法用例里畸形分支另有专门用例，此处只喂合法输入）。
/// @param value 字段值原文。
/// @return 解析成功的编码链。
[[nodiscard]] auto chain_of(std::string_view value) -> std::vector<Coding> {
    const auto parsed = aurora::net::detail::parse_content_encoding(value);
    AURORA_TEST_REQUIRE_MSG(bool(parsed), parsed ? std::string{} : parsed.error().message);
    return parsed.value();
}

/// @brief 还原一条链：失败即致命失败并带上错误文本。
/// @param chain 编码链（应用次序）。
/// @param input 线上字节。
/// @param max_output_bytes 产物上限。
/// @return 还原后的字节。
[[nodiscard]] auto decoded_or_require(const std::vector<Coding> &chain, const std::vector<char> &input,
                                      std::optional<std::uint64_t> max_output_bytes = {}) -> std::vector<char> {
    const auto decoded =
        aurora::net::detail::decode_content_encoding(chain, std::span<const char>{input}, max_output_bytes);
    AURORA_TEST_REQUIRE_MSG(bool(decoded), decoded ? std::string{} : decoded.error().message);
    return decoded.value();
}

/// @brief 把缓冲按文本读出（解压产物本就是 ASCII 明文，比对文本比比对字节好读）。
/// @param buffer 产物缓冲。
/// @return 等长文本。
[[nodiscard]] auto as_text(const std::vector<char> &buffer) -> std::string {
    return std::string{buffer.begin(), buffer.end()};
}

/// @brief 流式链的宿主替身：记下收到的每一段，并可按段数决定何时收摊。
///
/// 它只承担观测口的活：`stop_after` 是「收到第几段起回 `Stop`」，缺省大到永不触发。收摊事实与段数都留在这
/// 里，用例据此判「链停住后是否还来段」——那是流式独有的判据，物化路径没有对应形态。
struct Collector {
    std::string text;  ///< 已交付段的拼接（按交付次序）
    std::size_t segments{0};  ///< 已交付的段数
    std::size_t stop_after{std::numeric_limits<std::size_t>::max()};  ///< 第几段起回 `Stop`
    bool stopped{false};  ///< 本宿主是否回出过 `Stop`

    /// @brief 链末的产出出口：记一段，并按 `stop_after` 决定答复。
    /// @param bytes 本段明文。
    /// @return 恒成功（本替身不制造失败；失败路径另有专门用例）。
    auto emit(std::span<const char> bytes) -> Result<EmitFlow> {
        ++segments;
        text.append(bytes.data(), bytes.size());
        if (segments >= stop_after) {
            stopped = true;
            return Result<EmitFlow>{EmitFlow::Stop};
        }
        return Result<EmitFlow>{EmitFlow::Continue};
    }
};

/// @brief 以 `sink` 为产出出口建一枚 `StreamingEmit`（形参取指针：闭包按值捕获它，宿主对象留在用例栈上）。
/// @param sink 宿主替身。
/// @return 可直接交给 `StreamingDecoder::create()` 的产出出口。
[[nodiscard]] auto emit_for(Collector &sink) -> StreamingEmit {
    return [&sink](std::span<const char> bytes) -> Result<EmitFlow> { return sink.emit(bytes); };
}

/// @brief 建链、按 `piece` 段长喂入 `wire`、随后收摊，把宿主拼到的明文交回。
///
/// 中途任一次 `push()` 回 `Stop` 即按纪律收摊返回（**不**调 `finish()`），故本函数也是那条纪律的可执行形态。
/// @param chain 编码链（应用次序）。
/// @param wire 线上字节。
/// @param piece 每次喂入的段长。
/// @param max_output_bytes 逐层产物上限（`nullopt` = 不设限）。
/// @param sink 宿主替身（就地累计段数与文本）。
/// @return 成功携宿主拼到的明文；失败携建链 / 喂入 / 收摊任一处的错误。
[[nodiscard]] auto stream_decode(const std::vector<Coding> &chain, const std::vector<char> &wire, std::size_t piece,
                                 std::optional<std::uint64_t> max_output_bytes, Collector &sink)
    -> Result<std::string> {
    auto built = StreamingDecoder::create(chain, max_output_bytes, emit_for(sink));
    if (!built) {
        return Result<std::string>{built.error()};
    }
    auto decoder = std::move(built.value());
    std::span<const char> rest{wire};
    while (!rest.empty()) {
        const std::size_t take = std::min(piece, rest.size());
        const auto pushed = decoder.push(rest.subspan(0, take));
        if (!pushed) {
            return Result<std::string>{pushed.error()};
        }
        if (pushed.value() == EmitFlow::Stop) {
            return Result<std::string>{sink.text};  // 掐断即收摊：不再 `finish()`
        }
        rest = rest.subspan(take);
    }
    const auto finished = decoder.finish();
    if (!finished) {
        return Result<std::string>{finished.error()};
    }
    return Result<std::string>{sink.text};
}

/// @brief 流式宿主替身的「永不收摊」缺省值：读起来比 `numeric_limits` 短，用例据此显式设收摊点。
constexpr std::size_t AURORA_NEVER_STOP = std::numeric_limits<std::size_t>::max();

// `hello aurora` 的三种包装金样（gzip = RFC 1952、zlib = RFC 1950、raw = RFC 1951）。
const unsigned char AURORA_GZIP_HELLO[] = {31, 139, 8,  0,   0,  0,  0, 0, 2,   255, 203, 72,  205, 201, 201, 87,
                                           72, 44,  45, 202, 47, 74, 4, 0, 214, 230, 105, 148, 12,  0,   0,   0};
#ifdef AURORA_ENABLE_COMPRESSION  // 金样表只供解码相关的用例消费，缺席的腿不留下未引用的常量
const unsigned char AURORA_ZLIB_HELLO[] = {120, 156, 203, 72, 205, 201, 201, 87,  72, 44,
                                           45,  202, 47,  74, 4,   0,   30,  136, 4,  191};
const unsigned char AURORA_RAW_HELLO[] = {203, 72, 205, 201, 201, 87, 72, 44, 45, 202, 47, 74, 4, 0};
// `hello aurora` 先按 gzip 再按 zlib 加工后的字节，即 `Content-Encoding: gzip, deflate` 的线上形态。
const unsigned char AURORA_GZIP_THEN_ZLIB[] = {120, 156, 147, 239, 230, 96,  0,   1,  166, 255, 167, 61,  206,
                                               158, 60,  25,  238, 161, 163, 123, 74, 223, 139, 133, 225, 218,
                                               179, 204, 41,  60,  12,  12,  12,  0,  162, 234, 10,  42};
// 明显不是任何压缩流的两枚头：gzip 与 zlib 都会在第一字节就拒。
const unsigned char AURORA_NOT_COMPRESSED[] = {78, 79, 84, 95, 71, 90, 73, 80};
#endif

}  // namespace

AURORA_TEST_CASE(advertised_set_is_exactly_the_supported_codings) {
    // 宣告与支持集同源：把宣告文本自身喂回解析器，必须落回两枚环节，否则「宣告了却解不了」就成了自造故障。
    AURORA_TEST_CHECK_EQ(std::string{aurora::net::detail::AURORA_HTTP_ACCEPT_ENCODING}, "gzip, deflate");
    const auto advertised = chain_of(aurora::net::detail::AURORA_HTTP_ACCEPT_ENCODING);
    AURORA_TEST_REQUIRE_EQ(advertised.size(), std::size_t{2});
    AURORA_TEST_CHECK_TRUE(advertised.at(0) == Coding::Gzip);
    AURORA_TEST_CHECK_TRUE(advertised.at(1) == Coding::Deflate);
}

AURORA_TEST_CASE(parse_matches_coding_names_case_insensitively_and_trims_ows) {
    AURORA_TEST_REQUIRE_EQ(chain_of("GZIP").size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(chain_of("GZIP").at(0) == Coding::Gzip);
    AURORA_TEST_CHECK_TRUE(chain_of("  DeFlAte \t").at(0) == Coding::Deflate);
    // 分隔符两侧空白同样不算环节的一部分。
    const auto spaced = chain_of("gzip ,  deflate");
    AURORA_TEST_REQUIRE_EQ(spaced.size(), std::size_t{2});
    AURORA_TEST_CHECK_TRUE(spaced.at(1) == Coding::Deflate);
}

AURORA_TEST_CASE(parse_treats_absent_value_and_identity_as_no_stage) {
    AURORA_TEST_CHECK_TRUE(chain_of("").empty());
    AURORA_TEST_CHECK_TRUE(chain_of("   ").empty());
    AURORA_TEST_CHECK_TRUE(chain_of("identity").empty());
    AURORA_TEST_CHECK_TRUE(chain_of("IDENTITY").empty());
    // identity 是恒等环节：夹在链中间既不产环节也不终止后续环节的解析。
    const auto mixed = chain_of("identity, gzip, identity");
    AURORA_TEST_REQUIRE_EQ(mixed.size(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(mixed.at(0) == Coding::Gzip);
}

AURORA_TEST_CASE(parse_keeps_the_written_application_order) {
    const auto forward = chain_of("gzip, deflate");
    AURORA_TEST_REQUIRE_EQ(forward.size(), std::size_t{2});
    AURORA_TEST_CHECK_TRUE(forward.at(0) == Coding::Gzip);
    AURORA_TEST_CHECK_TRUE(forward.at(1) == Coding::Deflate);
    const auto swapped = chain_of("deflate, gzip");
    AURORA_TEST_REQUIRE_EQ(swapped.size(), std::size_t{2});
    AURORA_TEST_CHECK_TRUE(swapped.at(0) == Coding::Deflate);
    AURORA_TEST_CHECK_TRUE(swapped.at(1) == Coding::Gzip);
}

AURORA_TEST_CASE(parse_rejects_empty_member_as_message_grammar) {
    for (const std::string_view value : {"gzip,,deflate", "gzip,", ",gzip", ","}) {
        const auto parsed = aurora::net::detail::parse_content_encoding(value);
        AURORA_TEST_REQUIRE_MSG(!bool(parsed), "empty member must be rejected: " + std::string{value});
        AURORA_TEST_CHECK_EQ(parsed.error().code_enum, aurora::ErrorCode::HttpProtocolError);
    }
}

AURORA_TEST_CASE(parse_names_the_unsupported_coding) {
    const auto brotli = aurora::net::detail::parse_content_encoding("br");
    AURORA_TEST_REQUIRE_FALSE(bool(brotli));
    AURORA_TEST_CHECK_EQ(brotli.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK(brotli.error().message.find("'br'") != std::string::npos);
    // 链上后面的名同样被点名：不是「解掉前面那层然后交垃圾」。
    const auto chain = aurora::net::detail::parse_content_encoding("gzip, zstd");
    AURORA_TEST_REQUIRE_FALSE(bool(chain));  // 后面的名同样被点名
    AURORA_TEST_CHECK_EQ(chain.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK(chain.error().message.find("zstd") != std::string::npos);
    // 带协商参数的写法只属于 `Accept-Encoding` 语境：应答侧出现它即对端越界，剥参数再解是替对端编造意图。
    const auto parameterized = aurora::net::detail::parse_content_encoding("gzip;q=1.0");
    AURORA_TEST_REQUIRE_FALSE(bool(parameterized));
    AURORA_TEST_CHECK_EQ(parameterized.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
}

AURORA_TEST_CASE(decode_without_a_stage_hands_the_wire_bytes_through) {
    const auto wire = bytes(AURORA_GZIP_HELLO);
    const auto untouched = decoded_or_require({}, wire);
    AURORA_TEST_CHECK_EQ(as_text(untouched), as_text(wire));
    // identity 解析成空链，走的正是同一条直通路径。
    AURORA_TEST_CHECK_EQ(as_text(decoded_or_require(chain_of("identity"), wire)), as_text(wire));
}

AURORA_TEST_CASE(decode_empty_payload_is_not_corruption) {
    // 声明了环节却没有载荷（中间层照抄头部、`Content-Length: 0` 一类）：交出空产物，不凭空造流损坏。
    // 空载荷在触及解码器之前即判空，故本例两腿同形，能力缺席的腿也不落 unsupported。
    const auto empty = decoded_or_require(chain_of("gzip"), std::vector<char>{});
    AURORA_TEST_CHECK_TRUE(empty.empty());
    AURORA_TEST_CHECK_TRUE(decoded_or_require(chain_of("deflate"), std::vector<char>{}).empty());
}

AURORA_TEST_CASE(stream_without_a_stage_hands_the_wire_bytes_through) {
    // 空链（字段缺席 / `identity`）在流式路径同样是直通：字节按喂入的段界原样交出，一段不多一段不少。
    const std::vector<char> wire{'a', 'b', 'c', 'd', 'e', 'f', 'g'};
    Collector sink;
    const auto text = stream_decode({}, wire, 3, std::optional<std::uint64_t>{}, sink);
    AURORA_TEST_REQUIRE_MSG(bool(text), text ? std::string{} : text.error().message);
    AURORA_TEST_CHECK_EQ(text.value(), "abcdefg");
    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{3});  // 7 字节按 3 段长喂 → 3+3+1 三段
}

AURORA_TEST_CASE(stream_without_an_emit_sink_is_a_usage_error) {
    // 没有产出出口的链「解了就丢」，属用法错误而非协议错误：`general-invalid-argument`，且在建链就拒。
    const auto built = StreamingDecoder::create(chain_of("gzip"), std::optional<std::uint64_t>{}, StreamingEmit{});
    AURORA_TEST_REQUIRE_FALSE(bool(built));
    AURORA_TEST_CHECK_EQ(built.error().code_enum, aurora::ErrorCode::GeneralInvalidArgument);
}

AURORA_TEST_CASE(stream_host_stop_holds_the_chain_and_later_segments) {
    // 宿主回 `Stop` 即链停住：此后既不交付也不重播——「掐断不 finish」这条纪律的可执行形态。
    const std::vector<char> wire{'a', 'b', 'c', 'd', 'e', 'f', 'g'};
    Collector sink;
    sink.stop_after = 2;
    auto built = StreamingDecoder::create({}, std::optional<std::uint64_t>{}, emit_for(sink));
    AURORA_TEST_REQUIRE(bool(built));
    auto decoder = std::move(built.value());
    const std::span<const char> view{wire};
    for (std::size_t offset = 0; offset < view.size(); offset += 2) {
        const std::size_t take = std::min<std::size_t>(2, view.size() - offset);
        const auto pushed = decoder.push(view.subspan(offset, take));
        AURORA_TEST_REQUIRE(bool(pushed));
        if (pushed.value() == EmitFlow::Stop) {
            break;  // 掐断：不再 `finish()`，也不再喂后续段
        }
    }
    AURORA_TEST_CHECK_TRUE(sink.stopped);
    const std::size_t segments_at_stop = sink.segments;
    const auto late = decoder.push(std::span<const char>{wire.data(), 1});
    AURORA_TEST_REQUIRE(bool(late));
    AURORA_TEST_CHECK_TRUE(late.value() == EmitFlow::Stop);  // 停住后任何段都得到同一个答复
    AURORA_TEST_CHECK_EQ(sink.segments, segments_at_stop);  // 且宿主一次也没被打扰
    // 收摊答复之外还要证明它不是「恰好没触发」：本用例的收摊点确实被显式设过，而非缺省的永不。
    AURORA_TEST_CHECK_TRUE(sink.stop_after != AURORA_NEVER_STOP);
}

#ifdef AURORA_ENABLE_COMPRESSION

AURORA_TEST_CASE(decode_gzip_layer) {
    AURORA_TEST_CHECK_EQ(as_text(decoded_or_require(chain_of("gzip"), bytes(AURORA_GZIP_HELLO))), "hello aurora");
}

AURORA_TEST_CASE(decode_deflate_prefers_the_zlib_wrapper) {
    // `deflate` 的名义格式是 RFC 1950 包装：合法 zlib 流必须走第一条路径就成功（回落只留给流损坏）。
    AURORA_TEST_CHECK_EQ(as_text(decoded_or_require(chain_of("deflate"), bytes(AURORA_ZLIB_HELLO))), "hello aurora");
}

AURORA_TEST_CASE(decode_deflate_falls_back_to_the_raw_stream) {
    // 历史服务器将裸 DEFLATE 直接称作 `deflate`：zlib 第一字节就拒（0xCB 不是 0x78），回落路径接手。
    AURORA_TEST_CHECK_EQ(as_text(decoded_or_require(chain_of("deflate"), bytes(AURORA_RAW_HELLO))), "hello aurora");
}

AURORA_TEST_CASE(decode_undoes_a_multi_stage_chain_in_reverse) {
    // 线上形态 = 先 gzip 再 zlib，对应书写次序 `gzip, deflate`；还原必须从右往左。
    const auto plain = decoded_or_require(chain_of("gzip, deflate"), bytes(AURORA_GZIP_THEN_ZLIB));
    AURORA_TEST_CHECK_EQ(as_text(plain), "hello aurora");
    // 反过来的链（`deflate, gzip`）按次序先剥 gzip，而首两枚字节是 zlib 头，解不开即判消息体损坏。
    const auto wrong_order = aurora::net::detail::decode_content_encoding(
        chain_of("deflate, gzip"), std::span<const char>{bytes(AURORA_GZIP_THEN_ZLIB)}, std::optional<std::uint64_t>{});
    AURORA_TEST_REQUIRE_FALSE(bool(wrong_order));
    AURORA_TEST_CHECK_EQ(wrong_order.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
}

AURORA_TEST_CASE(decode_ceiling_names_the_decompressed_knob) {
    const auto wire = bytes(AURORA_GZIP_HELLO);  // 解压产物 12 字节
    const auto tight = aurora::net::detail::decode_content_encoding(chain_of("gzip"), std::span<const char>{wire},
                                                                    std::optional<std::uint64_t>{std::uint64_t{4}});
    AURORA_TEST_REQUIRE_FALSE(bool(tight));
    AURORA_TEST_CHECK_EQ(tight.error().code_enum, aurora::ErrorCode::HttpBodyTooLarge);
    // detail 必须点名越的是 `max_decompressed_bytes`——两枚旋钮管不同字节，指错即调用方调错地方。
    AURORA_TEST_CHECK(tight.error().message.find("max_decompressed_bytes") != std::string::npos);
    AURORA_TEST_CHECK(tight.error().message.find("max_body_bytes") != std::string::npos);
    // 放宽到产物长度即放行：证明上限真的在管产物而不是线上字节（线上 32 字节 > 12）。
    const auto loose = aurora::net::detail::decode_content_encoding(chain_of("gzip"), std::span<const char>{wire},
                                                                    std::optional<std::uint64_t>{std::uint64_t{12}});
    AURORA_TEST_REQUIRE(bool(loose));
    AURORA_TEST_CHECK_EQ(as_text(loose.value()), "hello aurora");
}

AURORA_TEST_CASE(decode_maps_core_corruption_to_http_body_corrupt) {
    const auto junk = bytes(AURORA_NOT_COMPRESSED);
    const auto gzip = aurora::net::detail::decode_content_encoding(chain_of("gzip"), std::span<const char>{junk},
                                                                   std::optional<std::uint64_t>{});
    AURORA_TEST_REQUIRE_FALSE(bool(gzip));
    AURORA_TEST_CHECK_EQ(gzip.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
    // 底层成因保留在文本里，但 HTTP 面只出现 `http-*` 一族码。
    AURORA_TEST_CHECK(gzip.error().message.find("compression-data-corrupt") != std::string::npos);
    const auto deflate = aurora::net::detail::decode_content_encoding(chain_of("deflate"), std::span<const char>{junk},
                                                                      std::optional<std::uint64_t>{});
    AURORA_TEST_REQUIRE_FALSE(bool(deflate));
    AURORA_TEST_CHECK_EQ(deflate.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
    // 两型都试过才判损坏：文本必须写明 zlib 包装与裸流各自的遭遇，否则回落形同没发生。
    AURORA_TEST_CHECK(deflate.error().message.find("zlib") != std::string::npos);
    AURORA_TEST_CHECK(deflate.error().message.find("DEFLATE") != std::string::npos);
}

AURORA_TEST_CASE(build_honours_the_compression_capability_leg) {
    AURORA_TEST_CHECK_TRUE(aurora::net::detail::compression_available());
    // 能力在场时 core 的 unavailable 不会串到 HTTP 面：三枚环节名都能被解（哪怕字节不合流）。
    const auto junk = bytes(AURORA_NOT_COMPRESSED);
    const auto result = aurora::net::detail::decode_content_encoding(chain_of("gzip"), std::span<const char>{junk},
                                                                     std::optional<std::uint64_t>{});
    AURORA_TEST_REQUIRE_FALSE(bool(result));
    AURORA_TEST_CHECK_EQ(result.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
}

AURORA_TEST_CASE(stream_gzip_arrives_in_odd_pieces) {
    // 逐段喂入不改变产物：一次喂完与一枚字节一枚字节地喂，拼出来的明文必须逐字节相同。
    const auto wire = bytes(AURORA_GZIP_HELLO);
    Collector whole;
    const auto once = stream_decode(chain_of("gzip"), wire, wire.size(), std::optional<std::uint64_t>{}, whole);
    AURORA_TEST_REQUIRE_MSG(bool(once), once ? std::string{} : once.error().message);
    Collector dribble;
    const auto by_one = stream_decode(chain_of("gzip"), wire, 1, std::optional<std::uint64_t>{}, dribble);
    AURORA_TEST_REQUIRE_MSG(bool(by_one), by_one ? std::string{} : by_one.error().message);
    AURORA_TEST_CHECK_EQ(once.value(), "hello aurora");
    AURORA_TEST_CHECK_EQ(by_one.value(), "hello aurora");
    // 段界由调用方决定，与解码器内部的窗口无关：交付的段数可以不同，拼出的文本不能不同。
    AURORA_TEST_CHECK_TRUE(whole.segments <= dribble.segments);
}

AURORA_TEST_CASE(stream_deflate_probe_locks_the_format_from_the_wrapper_header) {
    // 流式不「先试再回落」：首两枚输入字节构成合法 zlib 头即锁 zlib，否则锁裸流，两型各自都要能解通。
    Collector zlib_sink;
    const auto zlib =
        stream_decode(chain_of("deflate"), bytes(AURORA_ZLIB_HELLO), 4, std::optional<std::uint64_t>{}, zlib_sink);
    AURORA_TEST_REQUIRE_MSG(bool(zlib), zlib ? std::string{} : zlib.error().message);
    AURORA_TEST_CHECK_EQ(zlib.value(), "hello aurora");
    Collector raw_sink;
    const auto raw =
        stream_decode(chain_of("deflate"), bytes(AURORA_RAW_HELLO), 4, std::optional<std::uint64_t>{}, raw_sink);
    AURORA_TEST_REQUIRE_MSG(bool(raw), raw ? std::string{} : raw.error().message);
    AURORA_TEST_CHECK_EQ(raw.value(), "hello aurora");
}

AURORA_TEST_CASE(stream_deflate_probe_holds_the_first_byte_until_the_header_is_known) {
    // 判定要两枚输入字节：只到手一枚时本段按住不交——交出去就收不回来，而格式还没定，交错了整条流就错。
    const auto wire = bytes(AURORA_ZLIB_HELLO);
    Collector sink;
    auto built = StreamingDecoder::create(chain_of("deflate"), std::optional<std::uint64_t>{}, emit_for(sink));
    AURORA_TEST_REQUIRE(bool(built));
    auto decoder = std::move(built.value());
    const auto first = decoder.push(std::span<const char>{wire.data(), 1});
    AURORA_TEST_REQUIRE(bool(first));
    AURORA_TEST_CHECK_TRUE(first.value() == EmitFlow::Continue);
    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{0});
    const auto rest = decoder.push(std::span<const char>{wire}.subspan(1));
    AURORA_TEST_REQUIRE_MSG(bool(rest), rest ? std::string{} : rest.error().message);
    const auto done = decoder.finish();
    AURORA_TEST_REQUIRE_MSG(bool(done), done ? std::string{} : done.error().message);
    // 按住的那一枚头字节必须在锁定后补交在前：产物少一枚前缀字节就是坏流。
    AURORA_TEST_CHECK_EQ(sink.text, "hello aurora");
}

AURORA_TEST_CASE(stream_undoes_a_multi_stage_chain_in_reverse) {
    // 与物化路径同口径：`gzip, deflate` 的线上形态要按反序逐环还原，流式只是把每环的产物接着喂下一环。
    const auto wire = bytes(AURORA_GZIP_THEN_ZLIB);
    Collector sink;
    const auto plain = stream_decode(chain_of("gzip, deflate"), wire, 5, std::optional<std::uint64_t>{}, sink);
    AURORA_TEST_REQUIRE_MSG(bool(plain), plain ? std::string{} : plain.error().message);
    AURORA_TEST_CHECK_EQ(plain.value(), "hello aurora");
    // 次序弄反（`deflate, gzip`）：首两枚字节是 zlib 头，第一环按 gzip 认即拒——判损坏而不是解出垃圾。
    Collector wrong;
    const auto reversed = stream_decode(chain_of("deflate, gzip"), wire, 5, std::optional<std::uint64_t>{}, wrong);
    AURORA_TEST_REQUIRE_FALSE(bool(reversed));
    AURORA_TEST_CHECK_EQ(reversed.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
}

AURORA_TEST_CASE(stream_ceiling_names_the_decompressed_knob) {
    // 上限管产物不管线上：线上 32 字节、产物 12 字节，限到 4 必拒、放到 12 必过。
    const auto wire = bytes(AURORA_GZIP_HELLO);
    Collector tight_sink;
    const auto tight =
        stream_decode(chain_of("gzip"), wire, 8, std::optional<std::uint64_t>{std::uint64_t{4}}, tight_sink);
    AURORA_TEST_REQUIRE_FALSE(bool(tight));
    AURORA_TEST_CHECK_EQ(tight.error().code_enum, aurora::ErrorCode::HttpBodyTooLarge);
    AURORA_TEST_CHECK(tight.error().message.find("max_decompressed_bytes") != std::string::npos);
    Collector loose_sink;
    const auto loose =
        stream_decode(chain_of("gzip"), wire, 8, std::optional<std::uint64_t>{std::uint64_t{12}}, loose_sink);
    AURORA_TEST_REQUIRE_MSG(bool(loose), loose ? std::string{} : loose.error().message);
    AURORA_TEST_CHECK_EQ(loose.value(), "hello aurora");
}

AURORA_TEST_CASE(stream_corruption_surfaces_as_http_body_corrupt) {
    // 两种「字节不对」都要翻成 HTTP 出口码：字节本身不合流（push 就拒）与流半途中断（finish 才显形）。
    Collector junk_sink;
    const auto junk =
        stream_decode(chain_of("gzip"), bytes(AURORA_NOT_COMPRESSED), 4, std::optional<std::uint64_t>{}, junk_sink);
    AURORA_TEST_REQUIRE_FALSE(bool(junk));
    AURORA_TEST_CHECK_EQ(junk.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
    AURORA_TEST_CHECK(junk.error().message.find("compression-data-corrupt") != std::string::npos);
    // 截断：把 gzip 尾部（CRC32 + ISIZE）砍掉，喂入阶段无从判知，收摊时才宣告「这条流没走完」。
    const auto wire = bytes(AURORA_GZIP_HELLO);
    const std::vector<char> truncated{wire.begin(), wire.end() - 8};
    Collector cut_sink;
    const auto cut = stream_decode(chain_of("gzip"), truncated, 6, std::optional<std::uint64_t>{}, cut_sink);
    AURORA_TEST_REQUIRE_FALSE(bool(cut));
    AURORA_TEST_CHECK_EQ(cut.error().code_enum, aurora::ErrorCode::HttpBodyCorrupt);
}

AURORA_TEST_CASE(stream_host_stop_inside_a_chain_holds_later_segments) {
    // 停读发生在链里而非只在直通路径上：后续段即便解得出也不交，且宿主一次也没被打扰。
    const auto wire = bytes(AURORA_GZIP_HELLO);
    Collector sink;
    sink.stop_after = 1;
    auto built = StreamingDecoder::create(chain_of("gzip"), std::optional<std::uint64_t>{}, emit_for(sink));
    AURORA_TEST_REQUIRE(bool(built));
    auto decoder = std::move(built.value());
    bool saw_stop = false;
    const std::span<const char> view{wire};
    for (std::size_t offset = 0; offset < view.size() && !saw_stop; offset += 3) {
        const std::size_t take = std::min<std::size_t>(3, view.size() - offset);
        const auto pushed = decoder.push(view.subspan(offset, take));
        AURORA_TEST_REQUIRE_MSG(bool(pushed), pushed ? std::string{} : pushed.error().message);
        saw_stop = pushed.value() == EmitFlow::Stop;
    }
    AURORA_TEST_REQUIRE(saw_stop);
    const std::size_t segments_at_stop = sink.segments;
    const auto late = decoder.push(std::span<const char>{wire.data(), 1});
    AURORA_TEST_REQUIRE(bool(late));
    AURORA_TEST_CHECK_TRUE(late.value() == EmitFlow::Stop);
    AURORA_TEST_CHECK_EQ(sink.segments, segments_at_stop);
}

#else

// 能力缺席的腿按 TEST-R6 纪律把同名用例登记成 SKIP：登记而非缺席，才能让两腿的用例数逐名可比。
AURORA_TEST_CASE(decode_gzip_layer) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(decode_deflate_prefers_the_zlib_wrapper) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(decode_deflate_falls_back_to_the_raw_stream) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(decode_undoes_a_multi_stage_chain_in_reverse) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(decode_ceiling_names_the_decompressed_knob) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(decode_maps_core_corruption_to_http_body_corrupt) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(build_honours_the_compression_capability_leg) {
    AURORA_TEST_CHECK_FALSE(aurora::net::detail::compression_available());
    const auto wire = bytes(AURORA_GZIP_HELLO);
    const auto result = aurora::net::detail::decode_content_encoding(chain_of("gzip"), std::span<const char>{wire},
                                                                     std::optional<std::uint64_t>{});
    AURORA_TEST_REQUIRE_FALSE(bool(result));
    // 未编入解码器时「解不了」属能力缺席，落 unsupported 而不是 corrupt：静默交压缩字节才是最坏结果。
    AURORA_TEST_CHECK_EQ(result.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK(result.error().message.find("AURORA_ENABLE_COMPRESSION") != std::string::npos);
}

AURORA_TEST_CASE(stream_building_a_chain_without_the_capability_fails_at_create) {
    // 流式同样在**建链这一步**就拒：未编入解码器时连 `Accept-Encoding` 都不该宣告，收到带编码的载荷要么是
    // 畸形报文要么是对端越界，两种都没有「让字节流进宿主半程再失败」的理由。
    Collector sink;
    const auto built = StreamingDecoder::create(chain_of("gzip"), std::optional<std::uint64_t>{}, emit_for(sink));
    AURORA_TEST_REQUIRE_FALSE(bool(built));
    AURORA_TEST_CHECK_EQ(built.error().code_enum, aurora::ErrorCode::HttpContentEncodingUnsupported);
    AURORA_TEST_CHECK(built.error().message.find("AURORA_ENABLE_COMPRESSION") != std::string::npos);
    AURORA_TEST_CHECK_EQ(sink.segments, std::size_t{0});  // 失败在交付之前：一段都没交出去
}

AURORA_TEST_CASE(stream_gzip_arrives_in_odd_pieces) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_deflate_probe_locks_the_format_from_the_wrapper_header) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_deflate_probe_holds_the_first_byte_until_the_header_is_known) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_undoes_a_multi_stage_chain_in_reverse) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_ceiling_names_the_decompressed_knob) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_corruption_surfaces_as_http_body_corrupt) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

AURORA_TEST_CASE(stream_host_stop_inside_a_chain_holds_later_segments) {
    AURORA_TEST_SKIP("AURORA_ENABLE_COMPRESSION is not enabled (build with -DAURORA_ENABLE_COMPRESSION=ON)");
}

#endif

}  // namespace aurora::test_cases::utest_net_http_encoding

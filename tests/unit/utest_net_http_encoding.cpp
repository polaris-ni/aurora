/// 测试类型: unit
/// 目标单元: src/aurora/net/detail/http_encoding.h + src/aurora/net/detail/http_encoding.cpp
/// 测试说明: HTTP 应答 `Content-Encoding` 的编码链文法与逐层还原的逐值回归（CHG-010 §12「内容编码」的
///           HTTP 接入层）。覆盖宣告集与支持集同源（`AURORA_HTTP_ACCEPT_ENCODING` 自身可被解析回两枚
///           环节）、编码名大小写不敏感与 OWS 修剪、字段缺席 / 空值 / `identity` 归零环节而列表里的空
///           环节判文法畸形、支持集外的名点名落 `http-content-encoding-unsupported`；逐层还原按书写
///           次序反序进行（`gzip, deflate` 先剥 deflate 再剥 gzip，正序解必失败）、`deflate` 先试 zlib
///           包装仅在其报流损坏时回落裸 DEFLATE、空载荷配已声明环节交出空产物而不是流损坏、逐层受
///           `max_output_bytes` 约束；core 层三枚 `compression-*` 一律翻成 HTTP 出口码且把底层成因留在
///           文本里（`compression-unavailable` → `http-content-encoding-unsupported`、
///           `compression-data-corrupt` → `http-body-corrupt`、`compression-output-too-large` →
///           `http-body-too-large` 并点名 `max_decompressed_bytes`）。
/// 全用例零 socket、零压缩库直调（金样为预生成的 gzip / zlib / 裸 DEFLATE 字节表），故三平台行为一致、
/// wasm 腿同样可跑；能力开关缺席时按 `AURORA_TEST_SKIP` 登记而非缺席。

#include <cstddef>
#include <cstdint>
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

#endif

}  // namespace aurora::test_cases::utest_net_http_encoding

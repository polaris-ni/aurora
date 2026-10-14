// http_encoding.h —— HTTP 应答 `Content-Encoding` 的发起侧解压管线（CHG-010 §12「内容编码」的 HTTP 接入；
// 仅库实现可见，不属公共 API）。
//
// 为何单列：本文件里是两条各自有独立判据的纯逻辑——**编码名的文法与支持集的判定**（一个字节都不碰）与
// **按编码链逐层还原字节**（core 能力 `aurora::decompress` 的编排）。把它们写进 `Client::send` 的协程体，
// 两条都只能靠真连接才能测；把它们写进 `core/compression.h`，又把 HTTP 特有的歧义渗进了通用 API——那里
// 明确写了「`deflate` 名义 zlib、历史服务器发裸流的歧义，其消解属 HTTP 层编排，不进本通用 API」。
// 落在这里，零 socket、零平台头即可逐值断言畸形、回落与错误映射。
//
// 五条口径（规范性，落码与文档回写以此为准）：
// - **支持集就是宣告出去的那两枚**：`Accept-Encoding` 只宣告 `gzip, deflate`；链上出现任何别的名（`br` /
//   `zstd` / 自定义）落 `http-content-encoding-unsupported` 并在文本里点名是哪一枚。构建未编入解压能力
//   （`AURORA_ENABLE_COMPRESSION=OFF`）时发起侧连宣告都不发，而服务器强行压缩同样落这一枚码——静默把压缩
//   字节当明文交出是最坏的一种失败，它污染的是调用方拿到的每一个字节。
// - **书写次序是应用次序，解压走反序**（`RFC 9110 §8.4`：`Content-Encoding: gzip, deflate` 表示载荷先经
//   gzip 再经 deflate 加工，故还原从右往左——先剥 deflate 再剥 gzip；按书写次序正向解只会解出垃圾）。
// - **`deflate` 的两型歧义在本层消解**：先按 `Zlib`（RFC 1950 包装）解，**仅当**底层报
//   `compression-data-corrupt` 才回落裸 `Deflate`（RFC 1951）；底层报越限或能力缺席时不回落——回落只会把
//   真成因盖掉。两型皆拒即 `http-body-corrupt`，文本写明两条都试过。
// - **core 码一律翻成 HTTP 出口码**：`compression-unavailable` → `http-content-encoding-unsupported`、
//   `compression-data-corrupt` → `http-body-corrupt`、`compression-output-too-large` →
//   `http-body-too-large`（文本点名越的是 `max_decompressed_bytes` 而非线上的 `max_body_bytes`）。调用方在
//   HTTP 面只面对 `http-*` 一族、旋钮与码一一对应，底层成因保留在文本里而不是第二族错误码。
// - **空值与 `identity` 都不算环节**：字段缺失、值为空、值为 `identity` 一律回空链（调用方直通原始字节）；
//   但列表里出现**空的环节**（`gzip,,deflate` 一类）是报文文法问题，落 `http-protocol-error` 而不是被忽略。
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::net::detail {

/// @brief 受支持的内容编码环节（枚举值即本层唯一认得的两枚编码名）。
///
/// 不含 `identity`：它按定义是「没有环节」，解析阶段就归入空链，留一个枚举值只会多一条不该分支的路径。
enum class Coding : std::uint8_t {
    Gzip,  ///< `gzip`：RFC 1952 包装，对应 `CompressionFormat::Gzip`
    Deflate,  ///< `deflate`：名义 RFC 1950（zlib）、历史服务器发 RFC 1951 裸流，两型依次尝试
};

/// @brief 发起侧应当宣告的 `Accept-Encoding` 字段值（与上面的支持集同源）。
inline constexpr std::string_view AURORA_HTTP_ACCEPT_ENCODING = "gzip, deflate";

/// @brief 本构建是否具备解压能力（`AURORA_ENABLE_COMPRESSION` 的编译期事实）。
///
/// 它是「发不发 `Accept-Encoding`」的唯一判据：不具备时恒假，发起侧据此**不宣告**任何编码，而不是宣告后
/// 再在接收侧报错。
/// @return 编入解压能力为 true，否则 false。
[[nodiscard]] auto compression_available() -> bool;

/// @brief 解析 `Content-Encoding` 字段值为编码链（保持线上书写次序，即应用次序）。
///
/// 环节之间以逗号分隔，允许首尾与分隔符周围的空白；编码名按 ASCII 大小写不敏感匹配（`RFC 9110 §8.4` 的
/// coding-token 是不敏感的）。`identity` 与空值不产环节，支持集外的名字一律失败而不是被跳过。环节名**整段
/// 匹配**，带 `;q=` 一类参数的写法不产环节而是落不支持码——参数只属于 `Accept-Encoding` 的协商语境，应答侧
/// 出现它本身就是对端越界，把它剥掉再解是替对端编造意图。
/// @param value 字段值原文（多条同名字段由调用方先按逗号合并，本函数只看一条完整列表）。
/// @return 成功携应用次序的编码链（可为空 = 无需解压）；空的列表环节落 `http-protocol-error`，未知编码名
///         落 `http-content-encoding-unsupported`。
[[nodiscard]] auto parse_content_encoding(std::string_view value) -> Result<std::vector<Coding>>;

/// @brief 按编码链逐层还原字节（反序应用），把 core 解压失败翻成 HTTP 出口码。
///
/// 空链直通原始字节（一次拷贝，语义是「没有环节要解」）；`Deflate` 环节内部先试 zlib、仅流损坏时回落裸
/// DEFLATE。逐层都按 `max_output_bytes` 设上限，故中间环节同样受炸弹防护约束。
/// @param chain 已解析的编码链（应用次序，本函数自右向左逐层还原）。
/// @param input 线上消息体字节（分帧解码后的原始载荷）。
/// @param max_output_bytes 解压产物字节上限（`std::nullopt` = 不设限）；越限落 `http-body-too-large`。
/// @return 成功携还原后的字节；失败携对应 `http-*` 码（`http-content-encoding-unsupported` /
///         `http-body-corrupt` / `http-body-too-large`），底层 `compression-*` 成因写在其文本里。
[[nodiscard]] auto decode_content_encoding(const std::vector<Coding> &chain, std::span<const char> input,
                                           std::optional<std::uint64_t> max_output_bytes) -> Result<std::vector<char>>;

}  // namespace aurora::net::detail

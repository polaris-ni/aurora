// ws_handshake.h —— WebSocket 升级握手的字节层（仅库实现可见，不属公共 API）。
//
// 为何单列：握手在字面上是一条 `GET`，但它要写的字段、要验的答复、以及「答复之后这条连接归谁」三件事
// 都与一条普通请求相反（普通请求把 101 当场判成协议错、把体读全才算完、读完即归还连接池）。把它们收在
// 一处纯函数里，才能做到两件事——① 六类拒因与两处大小写分岔可在零 socket、零线程下逐值断言，真实网关
// 不会配合你造出「`Connection` 写成 `keep-alive, Upgrade` 而 `Upgrade` 混了大小写」这种现场；② `SHA-1`
// 与 base64 这对算法只有在这里被要求，摊开会让别的层以为可以顺手拿去当摘要口用。
//
// 本层只做「写出一段请求头部块」与「判一段应答头部块」，不碰套接字、不做超时、不管重连：那些属发起侧。
//
// 七条口径（规范性，落码与文档回写以此为准）：
// - **方法恒 `GET`、体恒不写**：`RFC 6455 §5.1` 只把 `GET` 列为合法起手（其余方法的应答不可能被解释成
//   一条升级后的连接）。调用方给了体即剥掉、`Content-Length` / `Transfer-Encoding` 一并剥掉——握手腿发出
//   的字节里不该出现任何体形态，「剥掉」与「照发」的差别是对端会拿体字节当第一帧。
// - **四枚字段由库写定**：`Upgrade: websocket`、`Connection: Upgrade`、`Sec-WebSocket-Version: 13`、
//   `Sec-WebSocket-Key`。调用方写了同名值即被覆写而不是并列（并列会让对端拿到两枚 key，验哪一枚都是猜）。
//   `Connection` 写成单 token 而不带 `keep-alive`：升级之后 HTTP 那侧的连接管理不再成立。
// - **`Sec-WebSocket-Key` 是 16 枚系统熵字节的 base64**，取不到熵即硬失败（落 `net-platform-unavailable`，
//   绝不回落伪随机）：这一枚值担的是「第三方不能凭请求内容预判 Accept、从而抢在真客户端之前占住会话」，
//   属协议必需的不可预测值，分界见 `06-app-platform.md` §8.8.27。
// - **`Sec-WebSocket-Accept` 按算法逐字节比对**：`base64(SHA-1(key + 258EAFA5-E914-47DA-95CA-C5AB0DC85B11))`，
//   比对前把两侧值里的一切 ASCII 空白剥掉（`RFC 6455 §1.3` 明许可折行、`RFC 9110 §5.5` 的 obs-fold 同样
//   会让「同一枚摘要」在线上长得不同）。这一比对是握手腿唯一的**双向**身份校验：它对不上意味着对端没有
//   读懂那次升级请求，此后收到的任何字节都不属于这条会话。
// - **三枚字段缺失各判一处、非 101 判状态码**：`Upgrade` 不是 `websocket`（大小写与 OWS 之后）、
//   `Connection` 的逗号列表里不含 `upgrade` token、`Accept` 缺失或算错，三型各点名自己那一枚；状态码不是
//   `101` 时点名状态码（`426` 是「版本不对」，`401`/`403` 是「不让上」，把它们混成一句「握手被拒」就等于
//   把排查的唯一线索丢掉）。全部落 `ws-handshake-rejected`——不是 `http-protocol-error`：后者说的是
//   「这不是一段合法的 HTTP 应答」，而这里那段应答合法得很，只是不肯升级。
// - **`Sec-WebSocket-Extensions` 出现即拒**：本库不请求任何扩展，对端却宣告了（`permessage-deflate` 最常见）
//   就意味着它要把压缩后的字节当帧发过来，而解错的字节看起来像坏帧、不像「少了一个扩展」。这一枚判据
//   反过来也保护发起侧：将来真要接扩展，先在请求侧宣告，本条自然改写。
// - **子协议按提出的次序回验**：服务端回的值必须是本次提出列表里的一枚（回一枚没提过的等于擅自换协议）；
//   没回而调用方要求必选即拒，没回且不要求必选则是**成功**且 `subprotocol` 为空——降级本身是合法的协商
//   结局（`xplat-network-stack` 提案 §18 的定案），库不替宿主把它判成失败。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/detail/http_wire.h"

namespace aurora::net::detail {

/// @brief `RFC 6455 §1.3` 写定在协议里的握手 GUID：拼接在 `Sec-WebSocket-Key` 之后求摘要，恒为这一串。
///
/// 它不是密钥也不是随机数，而是一枚常量盐——作用是让那次握手不可能由只懂 HTTP/1.1 的中间盒代答。
inline constexpr std::string_view AURORA_WS_HANDSHAKE_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// @brief `Sec-WebSocket-Key` 的原始字节数（base64 之后是 24 枚字符，含填充）。
inline constexpr std::size_t AURORA_WS_KEY_BYTES = 16;

/// @brief 本库唯一宣告的协议版本（`RFC 6455 §1.3` 定的第 13 版；其余版本一律不协商）。
inline constexpr std::string_view AURORA_WS_VERSION = "13";

/// @brief 一次握手要提出的子协议与「必选」旗位（由公共侧的选型折算而来，本层不认那枚公共类型）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct WsHandshakeOffer {
    std::vector<std::string> subprotocols;  ///< 依次序提出的子协议；空 = 不提出
    bool require_subprotocol{false};  ///< 必选：服务端没回其中一枚即判拒
};

/// @brief 握手成功交回的事实。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct WsHandshakeAccept {
    std::string subprotocol;  ///< 协商到的子协议；无子协议时为空
};

/// @brief 算出 `Sec-WebSocket-Accept` 应有的值：`base64(SHA-1(key + GUID))`。
///
/// 纯函数、无状态，故可用 `RFC 6455 §1.3` 那道算例逐字符钉住（`dGhlIHNhbXBsZSBub25jZQ==` →
/// `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`）。入参按调用方给的样子拼接，不剥空白——空白是**比对**那一侧的事，
/// 这里剥了会把「我方算的」与「对端写的」用两套口径处理。
/// @param key 本次请求写出的 `Sec-WebSocket-Key` 值原文。
/// @return 应答里那枚字段应有的值。
[[nodiscard]] auto ws_accept_token(std::string_view key) -> std::string;

/// @brief 装配并渲染握手的请求头部块，同时把这次用的 key 交回（校验应答要用同一枚）。
///
/// 走 `wire::render_request_head`，因此头部注入的那两处校验（字段名非 token、值含 CR/LF）与字段次序、
/// `Host` 补齐全部沿用 HTTP 那一条路，本层不另开一套序列化。`absolute_form` 由发起侧按代理形态给：
/// 转发型代理要把请求目标写成 `scheme://authority/path`，隧道型与直连都是 origin-form。
/// @param target 目标 URL（只用 path / query 拼请求目标，用 host / port 拼 `Host`）。
/// @param fields 调用方的请求字段（只借用，调用方那份一字不动）：同名那四枚在返回的新表里被覆写，
///               `Content-Length` / `Transfer-Encoding` / `Content-Encoding` 被剥除。
/// @param offer 子协议与必选旗位。
/// @param absolute_form 请求目标是否写成绝对形态。
/// @param key_out 出参：本次写出的 `Sec-WebSocket-Key` 值；失败时不保证被写过。
/// @return 请求头部块字节（以空行收尾）；字段不合文法为 `http-request-invalid`，取不到熵为
///         `net-platform-unavailable`。
[[nodiscard]] auto render_ws_handshake(const Url &target, const Headers &fields, const WsHandshakeOffer &offer,
                                       bool absolute_form, std::string &key_out) -> Result<std::string>;

/// @brief 判定一条应答头部块是否构成成功的升级。
///
/// 只判本层那七条口径里的应答侧四条（状态码、`Upgrade` / `Connection` / `Accept` 三枚字段、扩展宣告、
/// 子协议回验），不判 HTTP 文法（那是 `wire::parse_response_head` 已经过了的闸）。头部块之后的字节属于
/// 帧流，本函数不消费也不解释它们。
/// @param head 已解析的应答头部块。
/// @param key 本次请求写出的那枚 `Sec-WebSocket-Key`。
/// @param offer 子协议与必选旗位（回验用）。
/// @return 成功携协商事实；失败携 `ws-handshake-rejected` 并在 detail 点名是哪一处。
[[nodiscard]] auto validate_ws_handshake(const wire::ResponseHead &head, std::string_view key,
                                         const WsHandshakeOffer &offer) -> Result<WsHandshakeAccept>;

}  // namespace aurora::net::detail

// http_request_id.h —— 一次 HTTP 交换的链路标识（§12「X-Request-Id 链路标识」；
// 仅库实现可见，不属公共 API）。
//
// 为何单列：本文件里只有「一枚标识取成什么形状」这一条纯逻辑，零 socket、零时钟、零平台头，故它的文法
// 与版本位可以在单测里逐字符断言。写进 `Client::send` 的协程体就只能靠真连接才能看它长什么样。
//
// 四条口径（规范性，落码与文档回写以此为准）：
// - **形状取 `RFC 9562` 的 UUIDv4 文本形态**（8-4-4-4-12 小写十六进制，版本 nibble 恒 `4`、变体 nibble
//   恒落 `8`/`9`/`a`/`b`）。这不是为了「兼容某个服务器」，而是因为对端与中间层的日志里，这枚形状是**可
//   解析的**：运维拿到一串 id 能立刻判定它是 UUID 而非业务键，长度与分隔位也已被广泛工具链接受。
// - **熵取进程级系统熵源**（`detail::random_u64()` → `aurora/net/random.h`）。这里曾是仓内唯一那枚非密码学
//   序列的用法之一；序列随 `RandomSource` 公共面退场后没有「非密码学专用序列」可用了，而系统熵顺带让标识
//   不可预测。**用途仍是关联而非鉴权**：对端与中间层不该把这枚头当成凭据或防重放令牌，本文件也不承诺它
//   不可枚举——调用方要真正的令牌请自带，那条要求没因为换了熵源而放松。
// - **取不到熵即交出失败，不回落伪随机**：`make_request_id()` 返回 `Result<std::string>`，由发起侧
//   （`stamp_request_id`）决定「这一条不发标识 + 记一行 WARN」，请求本身照常发出；调用方手写的标识与
//   `Request::request_id()` 走的不是这条路，故不受熵影响。
// - **唯一性是概率而非保证**：122 位随机位取自系统熵，进程内与跨进程都只到「碰撞概率可忽略」。旧文案里
//   「同一进程两枚标识不可能相同」是那枚单序列给出的性质，换了熵源就不再成立，故本文件只承诺形状合法。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "aurora/core/result.h"

namespace aurora::net::detail {

/// @brief 链路标识的字段名（发起侧补写、调用方覆写、日志取值都问这一处）。
inline constexpr std::string_view AURORA_HTTP_REQUEST_ID = "X-Request-Id";

/// @brief 取一枚新的链路标识：`RFC 9562` UUIDv4 的 36 字符小写文本形态。
///
/// 122 位随机位取自系统熵的两枚 64 位取值，版本位与变体位按规范强制覆盖（随机位填不满那 6 枚 nibble
/// 的约束，覆盖之后仍是合法 UUID 文本）。
/// @return 成功为定长 36 字符：8-4-4-4-12 段以 `-` 相连，全小写十六进制；失败为 `net-platform-unavailable`
///         （本腿取不到系统熵，此时**不发**标识而不是发一枚可猜的）。
[[nodiscard]] auto make_request_id() -> Result<std::string>;

/// @brief 判定一枚文本是否是本库认可的标识形状（供调用方校验自填值，不用于改写它）。
/// @param value 待判文本。
/// @return 恰为 36 字符、分隔位与 nibble 位都对、版本 nibble 为 `4`、变体 nibble 落 `8`–`b` 时为 true。
[[nodiscard]] auto is_request_id_shape(std::string_view value) -> bool;

}  // namespace aurora::net::detail

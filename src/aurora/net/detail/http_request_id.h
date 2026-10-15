// http_request_id.h —— 一次 HTTP 交换的链路标识（CHG-018 §12「X-Request-Id 链路标识」；
// 仅库实现可见，不属公共 API）。
//
// 为何单列：本文件里只有「一枚标识取成什么形状」这一条纯逻辑，零 socket、零时钟、零平台头，故它的文法
// 与版本位可以在单测里逐字符断言。写进 `Client::send` 的协程体就只能靠真连接才能看它长什么样。
//
// 三条口径（规范性，落码与文档回写以此为准）：
// - **形状取 `RFC 9562` 的 UUIDv4 文本形态**（8-4-4-4-12 小写十六进制，版本 nibble 恒 `4`、变体 nibble
//   恒落 `8`/`9`/`a`/`b`）。这不是为了「兼容某个服务器」，而是因为对端与中间层的日志里，这枚形状是**可
//   解析的**：运维拿到一串 id 能立刻判定它是 UUID 而非业务键，长度与分隔位也已被广泛工具链接受。
// - **熵取库内置的非密码学序列**（`noncrypto_random`），刻意不用 `std::random_device`：标识符的用途是把
//   一次交换在多跳与重试之后仍能对上号，不是充当鉴权令牌；`std::random_device` 在本库两条腿上都不给出
//   可陈述的行为（见该文件第二条口径）。**因此它不是安全边界**——调用方要不可预测的关联 id，请自带。
// - **进程内不重复**由「时钟播种 + 逐次推进」给出：同一进程里两枚标识不可能相同（序列状态唯一），跨进程
//   只需不相关。UUID 的「全局唯一」在这里是**期望**而非保证，本文件不承诺后者。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aurora::net::detail {

/// @brief 链路标识的字段名（发起侧补写、调用方覆写、日志取值都问这一处）。
inline constexpr std::string_view AURORA_HTTP_REQUEST_ID = "X-Request-Id";

/// @brief 取一枚新的链路标识：`RFC 9562` UUIDv4 的 36 字符小写文本形态。
///
/// 122 位随机位取自 `noncrypto_u64()` 四次推进，版本位与变体位按规范强制覆盖（随机位填不满那 6 枚 nibble
/// 的约束，覆盖之后仍是合法 UUID 文本）。
/// @return 定长 36 字符：8-4-4-4-12 段以 `-` 相连，全小写十六进制。
[[nodiscard]] auto make_request_id() -> std::string;

/// @brief 判定一枚文本是否是本库认可的标识形状（供调用方校验自填值，不用于改写它）。
/// @param value 待判文本。
/// @return 恰为 36 字符、分隔位与 nibble 位都对、版本 nibble 为 `4`、变体 nibble 落 `8`–`b` 时为 true。
[[nodiscard]] auto is_request_id_shape(std::string_view value) -> bool;

}  // namespace aurora::net::detail

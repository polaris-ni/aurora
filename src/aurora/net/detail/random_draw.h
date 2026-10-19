// random_draw.h —— 库内取值点从系统熵取数的封装（仅库实现可见，不属公共 API）。
//
// 为何单列：公共面 `aurora/net/random.h` 只回答「字节从哪里来、取不到说什么」，而库内四个消费者要的都不是
// 裸字节——WebSocket 每帧要 4 位掩码密钥、`MultipartWriter` 要一枚不可猜的边界、`X-Request-Id` 要 122 位
// 随机位、重试退避要 `[0, 1)` 的一枚乘子。字节→数值的映射若在各处各写一遍，越界与精度损失就会变成四个
// 互不相干的bug 现场；故映射只在这里有一处。
//
// 三条口径（规范性，落码与文档回写以此为准）：
// - **协议要求的不可预测值走 `Result` 口，取不到即硬失败**：`random_fill` / `random_u64` 把缺省源的失败
//   原样交出，调用方据此中止那一次动作。刻意不在这里补一枚伪随机值顶上——掩码密钥与边界属协议规定的
//   不可预测值，「看着像随机」比「明确失败」更坏。
// - **观测性取值失败即降级，且降级不静默**：`random_unit`（退避抖动）在取不到熵时交出 `0.0`，
//   `detail::make_request_id()`（链路标识）交出失败；两者都经 `note_entropy_unavailable` 在进程内记**一行**
//   WARN。抖动的语义是「打散同一时刻整批重发的节奏」，交出 0 只是退回纯指数退避，重放本身照常进行；
//   标识少发一行头也不影响这一条请求的合法性。这两处值得降级而掩码不值得，分界在「失败会不会让对端把
//   一条本该拒绝的东西当成合法」。
// - **字节→`[0, 1)` 只有一条映射**：取 64 位值的高 53 位除以 `2^53`。53 位是 `double` 的尾数精度，再多就
//   被舍入成上界；除以 `2^53` 而非 `2^64` 让结果恒落在半开区间 `[0, 1)`——上界取不到，故「基准 × 乘子」
//   这一族算式不会碰到 `1.0`。这条映射是纯函数（`unit_from_u64`），逐值断言只依赖它而不依赖熵源。
//
// @see `aurora/net/random.h`（缺省源与三条平台腿的口径）
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aurora/core/result.h"

namespace aurora::net::detail {

/// @brief 用系统熵填满一段缓冲（协议要求的不可预测字节走这一口）。
/// @param out 目标缓冲；可为空段（空段直接成功且不动任何字节）。
/// @return 成功即每个字节都已写入；失败为 `net-platform-unavailable`，此时 `out` 的内容不作承诺。
[[nodiscard]] auto random_fill(std::span<std::byte> out) -> Result<void>;

/// @brief 取一枚 64 位系统熵整数。
/// @return 成功为 64 位取值；失败为 `net-platform-unavailable`。
[[nodiscard]] auto random_u64() -> Result<std::uint64_t>;

/// @brief 把一枚 64 位值折算成 `[0, 1)` 的实数（纯函数，单测逐值断言的落点）。
/// @param value 待折算的整数。
/// @return `double` 可表示的最大小于一值之内的取值；`0` 交出 `0.0`，全 1 交出 `(2^53 - 1) / 2^53`。
[[nodiscard]] auto unit_from_u64(std::uint64_t value) -> double;

/// @brief 取一枚 `[0, 1)` 的退避抖动乘子（取不到熵时交出 `0.0` 并记一行 WARN）。
/// @return 落在 `[0, 1)` 的取值——上界取不到，故「基准 × (1 + jitter)」这一族的乘子恒在半开区间内。
[[nodiscard]] auto random_unit() -> double;

/// @brief 记「本进程取不到系统熵」这一事实，进程内只记第一次。
///
/// 供两处降级点共用：日志只有一行不是省字，而是让「熵不可得」在一份现场日志里是一条事实而不是刷屏噪音——
/// 刷屏会把真正的首条原因冲掉。
/// @param where 降级点的名义，写进日志文本（如 `retry jitter`）。
/// @return 无返回值（只落日志）。
/// @note Side-effects: 至多一行 WARN 日志
auto note_entropy_unavailable(std::string_view where) -> void;

}  // namespace aurora::net::detail

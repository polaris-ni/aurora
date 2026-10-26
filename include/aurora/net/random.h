#pragma once

/// @brief 随机源公共面：交出「不可预测字节」的唯一入口。
/// @file net/random.h
///
/// 本头只回答一个问题：**库与应用要的随机字节从哪里来，取不到时说什么**。它存在的原因不是「想给应用一个
/// 随机数工具」，而是仓内原来只有一枚非密码学线性同余序列（为链路标识与重试抖动而生），而 WebSocket 每帧的
/// 掩码密钥按 `RFC 6455` §5.3 必须是不可预测值、`MultipartWriter` 的边界一旦可猜就等于给内容注入开了口子——
/// 同一份伪随机序列罩不住这两件事，于是系统熵升成公共面。
///
/// 四条形态约束（规范性，落码与文档回写以此为准）：
/// - **取不到就硬失败，不假降级**：`fill()` 交出 `net-platform-unavailable` 并在 detail 里点名是哪条腿。
///   刻意不回落伪随机——「以为有熵、实际没有」是一条调用方无从察觉的静默路径，与 `Capabilities` 里
///   `event_source` 那位「不提供假降级」是同一条纪律。
/// - **缺省源是进程级唯一一枚**：`default_random_source()` 交出静态期对象的引用，线程安全。库内的取值点
///   （WebSocket 掩码、multipart 边界、`X-Request-Id`、重试抖动）恒取它，**不受任何注入影响**——协议面上的
///   不可预测值若跟着可注入的源走，等于把协议要求变成一个可以关掉的开关。应用自实现的 `RandomSource`
///   只服务应用自己的需求（例如把随机位塞进可注入的测试替身）。
/// - **一次填满或不填满**：返回失败时 `out` 的字节内容不作承诺（可能已写入一部分），调用方不得使用；
///   长度为 0 时视为成功且不动任何字节。
/// - **平台腿**：Windows 走 `BCryptGenRandom`；POSIX 与 macOS 走 `/dev/urandom`。macOS 刻意不用
///   `SecRandomCopyBytes`——那枚 API 要挂 `Security` 框架，而该框架今天只在 TLS 的 SecureTransport provider
///   开着时才链上，随机面不该依赖一条可选的 TLS 配置；也刻意不用 `getentropy`——它在 glibc 上有版本前提，
///   要判就得给构建面加一枚探测（新的缓存变量 + `BUILD_OPTIONS.md` 登记项），而换来的只是少几行 `open` /
///   `read`。`/dev/urandom` 四条 POSIX 腿（Linux / macOS / Android / BSD）都供给，且不是「等熵池攒够」的
///   阻塞源——本层的调用点在握手与发送路径上，绝不允许长时间阻塞。wasm 走 WebCrypto 的
///   `crypto.getRandomValues`。单次系统调用的字节上限在各腿内部切段处理（WebCrypto 超 64 KiB 直接抛），
///   故「填多大都行」这条承诺不押在某个未验证的平台上限上。
///
/// @code
/// std::array<std::byte, 16> nonce{};
/// const auto filled = aurora::net::default_random_source().fill(nonce);
/// if (!filled) {  // 本腿无熵源：按 net-platform-unavailable 处置，绝不要拿未填满的缓冲
/// }
/// @endcode
/// @note Thread: thread-safe（每次直取系统熵源，无共享可变状态）
/// @note Side-effects: 读操作系统熵源（或 wasm 的 WebCrypto），不写任何全局状态

#include <cstddef>
#include <span>

#include "aurora/core/result.h"

namespace aurora::net {

/// @brief 随机字节的来源抽象：唯一一口 `fill`，实现方决定熵从哪来。
/// @note Thread: 由实现方声明；本头的缺省源是 thread-safe
/// @note Side-effects: 由实现方决定（缺省源读系统熵源）
/// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 多态抽象基类，按值拷贝 / 移动无意义
class RandomSource {
  public:
    /// @brief 虚析构（default）：保证经基类指针销毁派生源，本类自身不持有资源。
    virtual ~RandomSource() = default;

    /// @brief 用随机字节填满 `out`。
    ///
    /// 成功即每个字节都已写入；失败即整段不可用（部分写入不作承诺）。长度 0 直接成功，实现方不必为此
    /// 触碰熵源——那是「调用方没要东西」而不是「熵源没给东西」。
    /// @param out 目标缓冲；可为空段。
    /// @return 成功，或 `net-platform-unavailable`（当前腿取不到系统熵）。
    [[nodiscard]] virtual auto fill(std::span<std::byte> out) -> Result<void> = 0;
};

/// @brief 进程级系统熵源：库内所有取值点与应用的缺省入口。
///
/// 返回的是静态期对象的引用（生命周期等同进程），可反复取、可并发调。取不到熵时不在这里报失败——那是
/// `fill()` 的事，本函数只负责交出那唯一一枚源。
/// @return 系统熵源的引用。
/// @note Thread: thread-safe
/// @note Side-effects: none
[[nodiscard]] auto default_random_source() -> RandomSource &;

}  // namespace aurora::net

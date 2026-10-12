#pragma once

/// @brief 事件驱动的多句柄就绪等待（CHG-007 底座切片：把「等到某些句柄可读可写」收进唯一适配落点）。
/// @file net/poll.h
///
/// `wait_ready()` 是网络栈所有事件驱动形态共用的「等待基元」：自带 poll 线程引擎、并入宿主
/// 等待集的通用事件源注册口（`app::EventSourceRegistry`）、以及第三方非阻塞会话（libssh2、DBus
/// 一类）的就绪通知，都建立在同一条「给一组句柄 + 关注方向，阻塞到部分就绪或超时」的语义上。
///
/// 与 `net::Listener::wait_readable`（单句柄、`select`、同步回环）刻意分工：本基元是**多句柄、
/// 事件驱动、读/写/错误/挂断四向**的等待形态，且**只做等待、不碰任何协议语义**——它不知道句柄背
/// 后是 socket、管道还是 DBus 描述符。真正的平台差异（POSIX `poll` 的 `nfds`/`POLLNVAL`、Winsock
/// `WSAPoll` 的 `SOCKET` 形态与 `SHORT` 事件位）在 `src/aurora/net/socket.cpp` 一次消化（CHG-006
/// 不变量：平台 socket/poll 头的仓内唯一落点）。
///
/// @code
/// std::vector<aurora::net::PollDescriptor> want{
///     {.handle = fd_a, .want_read = true},
///     {.handle = fd_b, .want_write = true},
/// };
/// auto ready = aurora::net::wait_ready(want, std::chrono::milliseconds{200});
/// if (ready && (*ready)[0].readable) { /* fd_a 可读 */ }
/// @endcode
/// @note Thread: thread-safe（无共享状态；`handle` 所指的底层对象其自身线程规则不变）
/// @note Side-effects: 阻塞至多 timeout（每次系统调用上限，`EINTR` 时重新武装，不吞剩余时间预算的
///       精度承诺），除此之外不改任何句柄状态

#include <chrono>
#include <cstdint>
#include <vector>

#include "aurora/core/result.h"

namespace aurora::net {

/// @brief 一个待等待的句柄及其关注方向。
/// @note `handle` 是从不转移所有权的借用值：其有效性由句柄持有者保证，本基元绝不关闭它。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PollDescriptor {
    std::intptr_t handle = -1;  ///< 平台承载的原生句柄/描述符（-1 恒无效）
    bool want_read = false;  ///< 是否等待可读（含对端 FIN / 错误 / 挂断）
    bool want_write = false;  ///< 是否等待可写（发送缓冲腾出空间，供写侧背压）
};

/// @brief 单个句柄的一次就绪结果；与对应的 `PollDescriptor` 同序、按位表达。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct PollOutcome {
    bool readable = false;  ///< 可读：有字节到达，或 `want_read` 下对端已半关闭
    bool writable = false;  ///< 可写：可继续写入而不阻塞
    bool error = false;  ///< 出错：`POLLERR` / `POLLNVAL` 一类，后续读写都会失败
    bool hangup = false;  ///< 挂断：`POLLHUP`（对端关闭写向）
};

/// @brief 事件驱动地等待一组句柄就绪（POSIX `poll` / Winsock `WSAPoll`）。
///
/// 语义定见（提案 §2/§18）：
/// - 返回值与 `descriptors` **等长同序**，第 `i` 项描述第 `i` 个句柄的就绪方向；纯超时（无句柄
///   就绪）返回全 false 向量而非错误——「timeout 内没等到」是正常态，不是失败，调用方据此轮询自有停止标志。
/// - 一个句柄在 `descriptors` 里出现多次即等待多次，各槽位独立回报（不做去重），与事件源注册口
///   「同句柄可多登记、各自独立令牌」的口径一致。
/// - `timeout` 是**单次系统调用**的上限，被 `EINTR` 打断时以同一预算重新武装（跨平台一致：不因信号
///   而提前返回错误，也不承诺单调时钟意义下的精确总时长）。`timeout <= 0` 表示非阻塞立即回报当前就绪态。
/// - `handle` 为 `-1` 的槽位恒回报全 false（无效句柄不参与系统调用，避免整批等待失败）。
/// @param descriptors 待等待的句柄集合；可为空。
/// @param timeout 单次等待上限；负值与非正值一律按「立即回报」处理。
/// @return 成功为与输入同序的就绪结果向量；系统调用失败为 `net-wait-failed`（message 含 OS 原生码）。
[[nodiscard]] auto wait_ready(const std::vector<PollDescriptor> &descriptors, std::chrono::milliseconds timeout)
    -> Result<std::vector<PollOutcome>>;

}  // namespace aurora::net

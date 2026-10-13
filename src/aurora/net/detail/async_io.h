#pragma once

/// @brief 非阻塞 socket 出口（CHG-010 传输切片的平台腿）。
/// @file net/detail/async_io.h
///
/// **仅供 `src/aurora/net/` 实现侧使用，不占公共 API 预算。**存在理由有三：
/// 1. 「平台 socket 头唯一落点在 `src/aurora/net/socket.cpp`」这条既有不变量（CHG-006）要继续成立，
///    异步传输层就不能自己碰 `<winsock2.h>` / `<sys/socket.h>`，只能经本头借道。
/// 2. 非阻塞收发的事实形态是**三态**（完成若干字节 / 内核暂无进展 / 失败），而公共面的失败形态是
///    `Result`。「暂无进展」是一次正常的空转，不该占用一枚错误码——让它进 `Result` 会诱导调用方
///    去分支一个不存在的故障（与 `net::Resolver` 里「半个应答不是错误」同一条纪律）。
/// 3. `would-block` 的具体 errno / WSA 值随平台而异（`EAGAIN` 与 `EWOULDBLOCK` 是否同值、Winsock 的
///    `WSAEALREADY` / `WSAEWOULDBLOCK` / `WSAEINPROGRESS`  trio），换算只在本头的实现里做一次。
///
/// 句柄一律以 `std::intptr_t` 承载、`-1` 恒无效（与 `net::StreamSocket::native_handle()` 同口径），
/// 所有权归调用方：`open_*` 得到的句柄必须由 `close_handle()` 归还一次。
///
/// @note Thread: 与底层句柄同线程使用（本层不加锁；串行化由传输层的投递上下文负责）
/// @note Side-effects: 系统调用（非阻塞，故不产生「等待」这一类跨线程副作用）

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "aurora/core/result.h"
#include "aurora/net/address.h"

namespace aurora::net {
/// @brief 关停方向（定义在公共头 `aurora/net/transport.h`）：这里按固定底层类型前向声明，
/// 以免内部头反向依赖公共面。
enum class ShutdownMode : std::uint8_t;
}  // namespace aurora::net

namespace aurora::net::detail {

/// @brief 单次非阻塞读写的结果三态。
///
/// 判读次序：先看 `failed()`，再看 `blocked`，最后才看 `bytes`。
/// `bytes == 0 && !blocked && !failed()` 在读方向有确定含义——**对端已关闭写向（FIN）**。
struct IoAttempt {
    std::size_t bytes = 0;  ///< 本次实际传输的字节数
    bool blocked = false;  ///< 内核暂无进展（`EAGAIN` / `EWOULDBLOCK` 一类），不是错误
    std::optional<Error> failure;  ///< 失败原因；空值 = 未失败

    /// @brief 是否失败。
    /// @return `failure` 已置值时为 true。
    [[nodiscard]] auto failed() const -> bool { return failure.has_value(); }
};

/// @brief 单次非阻塞接受的结果三态。
struct AcceptAttempt {
    std::optional<std::intptr_t> peer;  ///< 已接受的对端句柄；空值 = 没有新连接（或失败）
    bool blocked = false;  ///< 暂无待接受连接，不是错误
    std::optional<Error> failure;  ///< 失败原因；空值 = 未失败

    /// @brief 是否失败。
    /// @return `failure` 已置值时为 true。
    [[nodiscard]] auto failed() const -> bool { return failure.has_value(); }
};

/// @brief 创建一个**非阻塞**的 TCP 流式套接字（地址族取自端点，未发起连接）。
/// @param endpoint 决定地址族的目标端点；须为 IPv4 / IPv6 且端口非 0。
/// @return 成功为句柄；失败为 `net-endpoint-invalid` / `net-socket-failed` / `net-option-failed`
///         （非阻塞置位失败落后者）。
[[nodiscard]] auto open_nonblocking_stream(const Endpoint &endpoint) -> Result<std::intptr_t>;

/// @brief 创建一个**非阻塞**的监听套接字（bind + listen，地址族取自端点）。
///
/// 与 `net::Listener::bind` 的分工：那条是同步阻塞面（工具与测试宿主专用），本函数服务事件驱动传输层。
/// 绑到非回环地址即接受来自其它主机的连接，访问控制仍归调用方。
/// @param endpoint 监听端点；须为 IPv4 / IPv6（本机 IPC 形态在发起系统调用前即拒）。
/// @param backlog 挂起连接队列深度（透传 `listen(2)`）。
/// @return 成功为句柄；失败为 `net-endpoint-invalid` / `net-socket-failed` / `net-bind-failed` /
///         `net-listen-failed` / `net-option-failed`（失败路径已关闭句柄）。
[[nodiscard]] auto open_nonblocking_listener(const Endpoint &endpoint, int backlog) -> Result<std::intptr_t>;

/// @brief 归还一个由本头创建的句柄（幂等：无效句柄无操作）。
/// @param handle 原生句柄承载值。
auto close_handle(std::intptr_t handle) -> void;

/// @brief 发起非阻塞连接。
///
/// 「已发起」与「已建立」在本函数里不做区分：两者都回 OK，真正的建立结果由可写就绪后的
/// `take_connect_error()` 判定。这一条不是偷懒——非阻塞 connect 的即时成功在两条腿上都可发生
/// （回环尤甚），把它当成「已连接」返回会让传输层多出一条只在某些机器上走到的分支。
/// @param handle `open_nonblocking_stream()` 得到的句柄。
/// @param endpoint 目标端点（地址族须与句柄一致）。
/// @return 已发起为 OK；确定失败为 `net-connect-failed` / `net-endpoint-invalid`（含 OS 原生码）。
[[nodiscard]] auto start_connect(std::intptr_t handle, const Endpoint &endpoint) -> Result<void>;

/// @brief 取非阻塞连接的最终结果（`getsockopt(SO_ERROR)`）。
/// @param handle 已 `start_connect()` 的句柄。
/// @return 连接已建立为 OK；被拒或仍在进行中为 `net-connect-failed`（message 带 OS 原生码，
///         进行中同样报该码并在文本里写明，因为「可写了却还在进行」是平台缺陷而非用户可重试的状态）。
[[nodiscard]] auto take_connect_error(std::intptr_t handle) -> Result<void>;

/// @brief 非阻塞读一次（`recv`）。
/// @param handle 已连接的句柄。
/// @param buffer 目标缓冲；空 span 回「无进展」。
/// @return 三态结果（见 `IoAttempt` 的判读次序）。
[[nodiscard]] auto try_read(std::intptr_t handle, std::span<char> buffer) -> IoAttempt;

/// @brief 非阻塞写一次（`send`，单轮请求不超过 int 可表示区间）。
/// @param handle 已连接的句柄。
/// @param data 待发送字节；空 span 回「无进展」。
/// @return 三态结果：`bytes` 是**已接受**的字节数，调用方据此推进队列游标。
[[nodiscard]] auto try_write(std::intptr_t handle, std::span<const char> data) -> IoAttempt;

/// @brief 非阻塞接受一次（`accept`）。
/// @param handle 监听句柄。
/// @return 三态结果；成功得到的对端句柄**已置为非阻塞**。
[[nodiscard]] auto try_accept(std::intptr_t handle) -> AcceptAttempt;

/// @brief 单向或双向关停写/读（`shutdown`），不归还句柄。
/// @param handle 目标句柄。
/// @param mode 关停方向；`ShutdownMode::Read` 的效果包含「释放对该句柄可读方向的关注」的内核侧对应物。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
[[nodiscard]] auto shutdown_handle(std::intptr_t handle, ShutdownMode mode) -> Result<void>;

/// @brief 实际绑定端口（`getsockname`），供 `port == 0` 时回报系统分配值。
/// @param handle 已 bind 的句柄。
/// @return 端口；查询失败或地址族不认识时为 0。
[[nodiscard]] auto bound_port(std::intptr_t handle) -> std::uint16_t;

/// @brief 读出对端地址（`getpeername`）。
/// @param handle 目标句柄。
/// @return 成功为对端端点；失败为 `net-socket-failed`。
[[nodiscard]] auto peer_of(std::intptr_t handle) -> Result<Endpoint>;

/// @brief 对端是否位于本机回环（与 `StreamSocket::is_loopback_peer()` 同一套三段判据）。
/// @param handle 目标句柄。
/// @return 落在回环段为 true；查询失败或地址族不认识为 false。
[[nodiscard]] auto peer_is_loopback(std::intptr_t handle) -> bool;

}  // namespace aurora::net::detail

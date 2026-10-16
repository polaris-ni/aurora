#pragma once

/// @brief 非阻塞 socket 出口。
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
/// 句柄一律以 `std::intptr_t` 承载、`-1` 恒无效（与 `net::blocking::StreamSocket::native_handle()` 同口径），
/// 所有权归调用方：`open_*` 得到的句柄必须由 `close_handle()` 归还一次。
///
/// 本头**包含**公共头 `aurora/net/transport.h` 而不是前向声明其中的类型：`NativeOptions` 与
/// `ShutdownMode` 的形态定义在公共面，实现侧按值传递它们就必须见到完整类型；内部头依赖本层公共头
/// 不改变任何分层方向（L1 实现 → L1 公共面，与 `datagram.cpp` 同一条路）。
///
/// @note Thread: 与底层句柄同线程使用（本层不加锁；串行化由传输层的投递上下文负责）
/// @note Side-effects: 系统调用（非阻塞，故不产生「等待」这一类跨线程副作用）

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/transport.h"

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

/// @brief 单次非阻塞收取数据报的结果三态。
///
/// 与 `IoAttempt` 的分工：数据报是**消息**边界，一次成功必然带着来源地址，故这里放 `source` 而不是
/// 让调用方再问一次（`getpeername` 在未连接的套接字上根本不可用）。
struct DatagramAttempt {
    std::size_t bytes = 0;  ///< 本次收到的字节数（一条数据报的全部载荷）
    bool blocked = false;  ///< 内核暂无数据报，不是错误
    std::optional<Endpoint> source;  ///< 来源端点；未连接套接字由 `recvfrom` 填出，已连接套接字等于该对端
    std::optional<Error> failure;  ///< 失败原因；空值 = 未失败

    /// @brief 是否失败。
    /// @return `failure` 已置值时为 true。
    [[nodiscard]] auto failed() const -> bool { return failure.has_value(); }
};

/// @brief 创建一个**非阻塞**的流式套接字（地址族取自端点，未发起连接）。
///
/// 本机 IPC 形态（`Endpoint::local()`）与 TCP 走同一条通路：同一枚 `socket(2)` / `connect(2)` /
/// `accept(2)` 序列，只是地址族换成 `AF_UNIX`。因此传输层不需要为它另起一套状态机，就绪、背压、
/// 半关闭、取消的语义逐字复用（这也是 `StreamTransport` 敢于「TCP / 本机 IPC / TLS 同型」的根据）。
/// @param endpoint 目标端点：IPv4 / IPv6 且端口非 0，或本机 IPC 路径形态。
/// @param options 原生选项；在 bind / connect 之前逐条应用，未置值的字段不发起任何 `setsockopt`。
/// @return 成功为句柄；失败为 `net-endpoint-invalid` / `net-socket-failed` / `net-option-failed` /
///         `net-option-conflict`（选项与地址族不匹配）/ `net-platform-unavailable`（本腿无本机 IPC 通路）。
[[nodiscard]] auto open_nonblocking_stream(const Endpoint &endpoint, const NativeOptions &options)
    -> Result<std::intptr_t>;

/// @brief 创建一个**非阻塞**的监听套接字（bind + listen，地址族取自端点）。
///
/// 与 `net::blocking::Listener::bind` 的分工：那条是同步阻塞面（工具与测试宿主专用），本函数服务事件驱动传输层。
/// 绑到非回环地址即接受来自其它主机的连接，访问控制仍归调用方。
/// @param endpoint 监听端点；IPv4 / IPv6 或本机 IPC 路径形态。
/// @param backlog 挂起连接队列深度（透传 `listen(2)`）。
/// @param options 原生选项；`reuse_address` 必须在 bind 之前设置才有效，故在本函数内部先应用再绑定。
/// @return 成功为句柄；失败码集合同 `open_nonblocking_stream()`，另加 `net-bind-failed` /
///         `net-listen-failed`（失败路径已关闭句柄）。
[[nodiscard]] auto open_nonblocking_listener(const Endpoint &endpoint, int backlog, const NativeOptions &options)
    -> Result<std::intptr_t>;

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

/// @brief 在既有句柄上应用原生选项——已接受连接的创建时刻。
///
/// 为什么要有这一枚：监听口的 `options.native` 若只在 `bind()` 前下发一次，被接受的连接就落到内核的
/// 选项继承表上，而那张表在两腿上并不一致（`TCP_NODELAY` 是否随监听套接字继承，Linux 与 Windows 给出
/// 不同答案）。同一份 `StreamOptions` 必须在两条腿上得到同一件事实，故每条被交付的连接都在自己的句柄
/// 上重放一遍；地址族由句柄自身回报，调用方无须声明这是 TCP 还是本机 IPC。
/// @param handle 目标句柄（已连接）。
/// @param options 原生选项；`source` 字段在此口径下无意义，置值即落 `net-option-conflict`。
/// @return 成功为 OK；不合用为 `net-option-conflict`，系统调用失败为 `net-option-failed`。
[[nodiscard]] auto apply_native_options(std::intptr_t handle, const NativeOptions &options) -> Result<void>;

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

/// @brief 对端是否位于本机回环（与 `blocking::StreamSocket::is_loopback_peer()` 同一套三段判据）。
/// @param handle 目标句柄。
/// @return 落在回环段为 true；查询失败或地址族不认识为 false。
[[nodiscard]] auto peer_is_loopback(std::intptr_t handle) -> bool;

/// @brief 创建一个**非阻塞**的 UDP 数据报套接字并绑定到本地端点（地址族取自端点）。
///
/// 数据报没有流式套接字的「先 bind 后 connect」次序约束：`local.port() == 0` 由系统分配实端口，
/// 通配地址即接受发往本机任一地址的数据报。本机 IPC 形态在发起系统调用前即拒——`DatagramTransport`
/// 的公共口径全程带端口，路径形态没有端口可带，半边可用的类型不进公共面。
/// @param local 本地端点；须为 IPv4 / IPv6。
/// @return 成功为句柄；失败为 `net-endpoint-invalid` / `net-socket-failed` / `net-bind-failed` /
///         `net-option-failed`（失败路径已关闭句柄）。
[[nodiscard]] auto open_nonblocking_datagram(const Endpoint &local) -> Result<std::intptr_t>;

/// @brief 给数据报套接字固定对端（`connect(2)`，**不产生任何网络往返**）。
///
/// 连接后的内核语义是「只收该对端的数据报」，故组播关注与本调用互斥（裁决见提案 §18-3），换算由
/// 上层做；本函数只管系统调用结果。
/// @param handle `open_nonblocking_datagram()` 得到的句柄。
/// @param peer 目标端点；须为 IPv4 / IPv6 且端口非 0。
/// @return 成功为 OK；失败为 `net-endpoint-invalid` / `net-connect-failed`（含 OS 原生码）。
[[nodiscard]] auto associate_datagram_peer(std::intptr_t handle, const Endpoint &peer) -> Result<void>;

/// @brief 非阻塞发送一条完整数据报到指定目标（`sendto`）。
///
/// 数据报不可拆分：内核要么整条接受、要么回报「暂无进展」，故 `bytes` 只可能是 `data.size()` 或 0。
/// 已固定对端的套接字**不能**走本出口：Linux 让带地址的 `sendto` 在已连接的数据报套接字上直接以
/// `EISCONN` 失败，Windows 却忽略该地址——同一调用在两条腿上有两种结局，故那种形态改走
/// `try_send_datagram()`。
/// @param handle 数据报句柄。
/// @param data 一条数据报的载荷；空 span 是一条**合法的零长数据报**，照样发起系统调用。
/// @param target 目标端点；须与句柄同族且端口非 0。
/// @return 三态结果（`blocked` = 内核发送缓冲满，等下一次可写就绪）。
[[nodiscard]] auto try_send_to(std::intptr_t handle, std::span<const char> data, const Endpoint &target) -> IoAttempt;

/// @brief 非阻塞发送一条完整数据报给固定对端（`send`，即已连接形态的唯一发送出口）。
///
/// 与 `try_write()` 的区别只有一处：`try_write` 把空 span 判为「无事可做」，而空载荷在数据报侧是
/// 一条合法消息，必须真的落到系统调用上。已连接套接字的接收侧仍可复用 `try_receive_datagram()`。
/// @param handle 已 `associate_datagram_peer()` 的数据报句柄。
/// @param data 一条数据报的载荷（可为空 span）。
/// @return 三态结果（判读次序同 `try_send_to()`）。
[[nodiscard]] auto try_send_datagram(std::intptr_t handle, std::span<const char> data) -> IoAttempt;

/// @brief 非阻塞收取一条完整数据报（`recvfrom`）。
///
/// 缓冲由调用方给出且**必须装得下一条完整数据报**（本层按 UDP 载荷上界配缓冲），故不存在「一条报
/// 分两轮投递」的形态；Windows 在缓冲不足时会让 `recvfrom` 直接失败并**丢弃该数据报**，POSIX 则静默
/// 截断——两条腿都不允许出现，故截断判定收在上层（按调用方缓冲长度裁剪并显式回报）。
/// @note `bytes == 0` 在这里是**一条零长数据报**（UDP 合法消息），不是流式读的那个 EOF：暂无数据报一律
///       走 `blocked` 分支，因此上层可以按「成功且 0 字节」投递一条空消息而不必猜是否对端已收摊。
/// @param handle 数据报句柄。
/// @param buffer 目标缓冲；空 span 回「无进展」。
/// @return 三态结果，成功时携来源端点。
[[nodiscard]] auto try_receive_datagram(std::intptr_t handle, std::span<char> buffer) -> DatagramAttempt;

/// @brief 加入或离开一个组播组（`IP_ADD_MEMBERSHIP` / `IP_DROP_MEMBERSHIP` 及其 IPv6 对应项）。
///
/// 不指定接收网卡：`imr_interface` 取通配值，由内核按路由选择——把「用哪张网卡」做成公共选项会让
/// 每条腿的默认值都不一样，而本层今天没有跨腿一致的网卡枚举形态。
/// @param handle 数据报句柄（须已绑定）。
/// @param group 组播组地址；族须与句柄一致（`Endpoint::is_multicast()` 由调用方先判）。
/// @param joining 为真时加入，为假时离开。
/// @return 成功为 OK；失败为 `net-endpoint-invalid`（族不符 / 非组播地址）或 `net-option-failed`
///         （含 OS 原生码；重复加入与未加入即离开都由内核回报）。
[[nodiscard]] auto set_multicast_membership(std::intptr_t handle, const Endpoint &group, bool joining) -> Result<void>;

/// @brief 组播回环开关（`IP_MULTICAST_LOOP` / `IPV6_MULTICAST_LOOP`）：本端发出的组播报是否回投给本端。
/// @param handle 数据报句柄。
/// @param enabled 为真时回投（内核缺省值），为假时不回投。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
[[nodiscard]] auto set_multicast_loopback(std::intptr_t handle, bool enabled) -> Result<void>;

/// @brief 广播允许位（`SO_BROADCAST`）：未置位时向广播地址发送由内核拒绝。
/// @param handle 数据报句柄。
/// @param enabled 为真时允许广播发送。
/// @return 成功为 OK；失败为 `net-option-failed`（含 OS 原生码）。
[[nodiscard]] auto set_broadcast_allowed(std::intptr_t handle, bool enabled) -> Result<void>;

/// @brief 读回句柄自身的地址族（`getsockname`，只取族而不还原地址）。
///
/// 存在的理由：本机 IPC 与 TCP 共用同一套流式状态机，公共面上的两处分歧（`peer_endpoint()` 与
/// `peer_credentials()`）都必须按**这条连接实际的族**回答，而不是按调用方当初传入的端点形态猜——
/// `StreamListener` 接受到的连接由内核给出，监听口是什么族它就是什么族。
/// @param handle 目标句柄。
/// @return 成功为 `IPv4` / `IPv6` / `LocalPath`；查询失败或地址族不认识为 `net-socket-failed`。
[[nodiscard]] auto handle_family(std::intptr_t handle) -> Result<AddressFamily>;

/// @brief 读出本机 IPC 连接的对端身份（`SO_PEERCRED` / `LOCAL_PEERCRED` 一类的内核登记）。
///
/// 直接返回公共面的 `PeerCredentials`：本头已包含 `transport.h`，中间再加一层同义结构只会让字段命名
/// 在两处漂移。不提供该能力的腿把 `anonymous` 置真后回 OK——「查到了身份但对方是匿名的」与「这个
/// 查询本身失败」是两件事，调用方据此分别走「按匿名策略放行 / 拒绝」与「报告错误」两条分支。
/// @param handle 本机 IPC 流式句柄（已连接）。
/// @return 成功为身份；查询失败为 `net-option-failed`（含 OS 原生码）。
[[nodiscard]] auto peer_identity(std::intptr_t handle) -> Result<PeerCredentials>;

/// @brief 本腿是否具备可用的本机 IPC 通路（一次性探测，进程内缓存）。
///
/// 判据是「能建出本机 IPC 的流式套接字」而不是「编译期认得这个宏」：Windows 的 AF_UNIX 腿随系统版本
/// 而来，编译通过不代表运行可用，而 `Capabilities` 是唯一真值源（提案 §9）。
/// @return 可用为 true；供 `Engine::capabilities().local_ipc` 消费。
[[nodiscard]] auto local_ipc_supported() -> bool;

}  // namespace aurora::net::detail

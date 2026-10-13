#pragma once

/// @brief 同步阻塞 socket 面（CHG-006 落地的能力，CHG-010 §4 处置后收缩进 `aurora::net::blocking`）。
/// @file net/blocking.h
///
/// **定位：仅供非 UI 线程的工具与测试宿主使用**——InspectorServer 的工作线程、`tools/` 下的服务器与
/// 探针、单测里的回环桩。面向使用者的一侧（控件、状态、异步业务）一律走 `aurora/net/transport.h` 的
/// 事件驱动传输族：阻塞收发会占死 UI 线程，且没有事件源可挂，把它的类型留在公共面上就会被误当成
/// 推荐用法。头名与命名空间一起承担这条界线，不是靠注释约束。
///
/// 本头**不引入任何平台 socket 头**（`winsock2.h` / `sys/socket.h` 全部封闭在
/// `src/aurora/net/socket.cpp`）：句柄以 `std::intptr_t` 承载、`-1` 恒为无效值，字节序转换、
/// `sockaddr` 形态、`socklen_t`、`SO_RCVTIMEO` 两平台不同形（Windows 收 DWORD 毫秒、POSIX 收
/// `struct timeval`）等差异全部在实现侧一次消化。
///
/// 运行时生命周期**不需调用方感知**：Windows 侧的 `WSAStartup` 由实现侧在首次用到 socket 时钉住，
/// 故本头只提供「拿句柄、用句柄」两类对象，没有任何需要先构造的运行时前置件。历史上公共面那枚
/// `net::Session` 已按 CHG-010 §4 降为库内细节（`src/aurora/net/detail/runtime.h`）。
///
/// 范围刻意收敛为「本仓 Inspector 工具链 + 后续本机 AI 交互」所需：
/// - **地址族由 `Endpoint` 决定**（IPv4 / IPv6 都走同一条 `connect` / `bind`）：不经 DNS，
///   名字解析是 `Resolver` 的职责。`*_loopback` 两枚工厂固定绑 `127.0.0.1`，
///   与 `tools/servers/inspector_client.h` 的信任模型同源（把运行中的 UI 交给 AI 操作的边界
///   绝不允许离开本机）；`bind(任意端点)` 把访问控制交回调用方。
/// - 同步阻塞语义 + 显式超时/可中断等待，无事件循环、无异步回调。
/// - 失败一律 `Result<T>` + 表驱动 `Error`（`net-*` 码，`ERROR_CATALOG.md`），
///   OS 原生错误码（`WSAGetLastError`/`errno`）写入 `message`。
///
/// @code
/// auto listener = aurora::net::blocking::Listener::listen_loopback(0, 5);   // port=0：系统分配
/// if (listener) {
///     auto ready = listener->wait_readable(std::chrono::milliseconds{200});
///     if (ready && *ready) { auto peer = listener->accept(); /* ... */ }
/// }
/// @endcode

#include <chrono>
#include <cstdint>
#include <span>

#include "aurora/core/result.h"
#include "aurora/net/address.h"

namespace aurora::net::blocking {

/// @brief 面向字节流的 TCP 套接字（move-only RAII，析构自动关闭）。
///
/// 单所有权：同一时刻仅一个持有者可收发；跨线程移交用 std::move。接收返回 0 表示对端
/// 已关闭写向（FIN），与出错（`Result` 失败态）严格区分。Windows 的 Winsock 运行时由实现侧
/// 随句柄钉住，本类型不提供也不需要任何「会话」前置条件。
/// @note Thread: 不 thread-safe（单所有权句柄）
/// @note Side-effects: 网络 I/O（仅限 loopback）
class StreamSocket {
  public:
    /// @brief 构造无效句柄态（可作占位/移动后的落点）。
    StreamSocket() = default;

    /// @brief 连接本机回环端口（127.0.0.1，不经 DNS）。
    /// @param port 目标 TCP 端口；非 0。
    /// @return 成功为已连接套接字；失败为 `net-socket-failed` / `net-connect-failed`（message 含 OS 原生码）。
    [[nodiscard]] static auto connect_loopback(std::uint16_t port) -> Result<StreamSocket>;

    /// @brief 连接一个 TCP 端点（地址族取自端点，IPv4 / IPv6 同一条通路，不经 DNS）。
    ///
    /// 与 `connect_loopback` 的分工：本函数是地址族无关的通用入口，名字→端点的转换归 `Resolver`；
    /// IPv6 上的 `%zone` 只接受**十进制作用域号**（两平台同口径，接口名→索引的查表需额外平台依赖，
    /// 故不做），非数字 zone 在发起系统调用前即拒。本机 IPC 端点同样即拒——本同步面按裁决只承载 TCP 流
    /// （逐字节阻塞收发没有事件源可挂），Unix 域流只经 `StreamTransport` 出口。
    /// @param endpoint 目标端点；须为有效的 IPv4 / IPv6 端点且端口非 0。
    /// @return 成功为已连接套接字；失败为 `net-endpoint-invalid`（端点不合用）/ `net-socket-failed` /
    ///         `net-connect-failed`（后两者 message 含 OS 原生码）。
    [[nodiscard]] static auto connect(const Endpoint &endpoint) -> Result<StreamSocket>;

    StreamSocket(const StreamSocket &) = delete;
    auto operator=(const StreamSocket &) -> StreamSocket & = delete;
    /// @brief 移动构造：句柄所有权移交，源对象转为无效态。
    /// @param other 被移交的源套接字。
    StreamSocket(StreamSocket &&other) noexcept;
    /// @brief 移动赋值：先关闭自有句柄再接管，源对象转为无效态。
    /// @param other 被移交的源套接字。
    /// @return 本对象引用。
    auto operator=(StreamSocket &&other) noexcept -> StreamSocket &;
    /// @brief 析构：句柄有效则自动关闭。
    ~StreamSocket();

    /// @brief 句柄是否有效。
    /// @return 未关闭且非默认构造时为 true。
    [[nodiscard]] auto is_valid() const -> bool { return handle_ != AURORA_INVALID_HANDLE; }

    /// @brief 借出原生句柄（不转移所有权）：交 `wait_ready`、事件源注册口或第三方库接管。
    ///
    /// 返回值仅在**本对象存活且未 `close()`** 期间有效；调用方不得自行关闭它，也不得在本对象析构后
    /// 使用（双重关闭会误伤系统复用后的新句柄）。跨平台形态：Windows 为 `SOCKET`、POSIX 为文件描述符，
    /// 一律以 `std::intptr_t` 承载、`-1` 恒无效（与 `is_valid()` 同口径）。
    /// @return 原生句柄的 `intptr_t` 承载值；已关闭或默认构造为 -1。
    [[nodiscard]] auto native_handle() const -> std::intptr_t { return handle_; }

    /// @brief 设置收发超时（两方向同值，毫秒统一口径）。
    ///
    /// 这是本层存在的核心理由之一：`SO_RCVTIMEO`/`SO_SNDTIMEO` 在 Winsock 收 DWORD **毫秒数**、
    /// 在 POSIX 收 `struct timeval`，按平台字节形态误传的生效值会差三个数量级（5 秒变 5 毫秒）。
    /// 调用方只表达毫秒意图，形态换算封闭在实现侧。
    /// @param timeout 单次 recv/send 的最长阻塞时长；须 > 0。
    /// @return 成功为 OK；失败为 `net-timeout-option-failed`。
    [[nodiscard]] auto set_io_timeout(std::chrono::milliseconds timeout) const -> Result<void>;

    /// @brief 读取当前可读字节（至多填满 buffer，允许部分读）。
    /// @param buffer 接收缓冲；空 span 直接返回 0。
    /// @return 成功为实际读取字节数（**0 = 对端已关闭**）；失败为 `net-recv-failed`，超时为
    /// `net-recv-timeout`（两者以 code 区分，不依赖措辞）。
    [[nodiscard]] auto read_some(std::span<char> buffer) const -> Result<std::size_t>;

    /// @brief 写完全部字节（内部 ≤1MiB 分块循环，规避两平台长度参数窄化陷阱）。
    /// @param data 待发送字节；空 span 直接返回 OK。
    /// @return 全部写入为 OK；中途失败为 `net-send-failed`（流已不可靠，调用方应放弃该连接）。
    [[nodiscard]] auto write_all(std::span<const char> data) const -> Result<void>;

    /// @brief 对端是否位于本机回环（服务端访问控制判据）。
    ///
    /// 两族都判：IPv4 取 `127.0.0.0/8` 全段，IPv6 取 `::1`，另把 IPv4 内嵌的 IPv6 形态
    /// （`::ffff:127.0.0.1`，双栈机上回环连接的常见回报形态）按其内嵌 IPv4 判。
    /// @return 对端地址落在回环段时为 true；句柄无效、查询失败或地址族不认识时为 false。
    [[nodiscard]] auto is_loopback_peer() const -> bool;

    /// @brief 读出对端地址（`getpeername`，地址族与端口都还原为值类型）。
    /// @return 成功为对端端点（IPv4 / IPv6）；句柄无效、查询失败或地址族不认识时为 `net-socket-failed`
    ///         （message 含 OS 原生码）。
    [[nodiscard]] auto peer_endpoint() const -> Result<Endpoint>;

    /// @brief 立即关闭套接字（幂等）。
    auto close() -> void;

  private:
    friend class Listener;
    static constexpr std::intptr_t AURORA_INVALID_HANDLE = -1;
    /// @brief 以原生句柄接管（仅 `Listener::accept` 与本类工厂使用）。
    /// @param handle 已创建/已接受的 `intptr_t` 化原生句柄；-1 视为无效。
    explicit StreamSocket(std::intptr_t handle) : handle_(handle) {}

    std::intptr_t handle_ = AURORA_INVALID_HANDLE;
};

/// @brief 本机回环 TCP 监听套接字（move-only RAII，析构自动关闭）。
/// @note Thread: 不 thread-safe（单所有权句柄）；`close()` 允许与另一线程的 `wait_readable`/
/// `accept` 竞态唤醒（设计用途：宿主以 close 中断 accept 线程）。
/// @note Side-effects: 网络 I/O（仅限 loopback）
class Listener {
  public:
    /// @brief 构造无效句柄态（可作占位/移动后的落点）。
    Listener() = default;

    /// @brief 在 127.0.0.1 上监听（只绑回环，非本机连接一律不进 accept）。
    /// @param port TCP 端口；**0 = 系统分配临时端口**，实际值经 `port()` 查询。
    /// @param backlog 挂起连接队列深度（透传 `listen(2)`）。
    /// @return 成功为就绪监听器；失败为 `net-socket-failed` / `net-bind-failed` / `net-listen-failed`
    /// （失败时句柄已在实现侧关闭）。
    [[nodiscard]] static auto listen_loopback(std::uint16_t port, int backlog) -> Result<Listener>;

    /// @brief 在指定端点上监听（地址族取自端点；IPv4 / IPv6 通配与具体地址都接受）。
    ///
    /// 与 `listen_loopback` 的分工：本函数不做任何本机性校验——绑到非回环地址意味着接受来自其它主机的
    /// 连接，访问控制（回环判定、凭据核验）由调用方在 `accept()` 之后负责。已绑定地址经
    /// `Endpoint` 语义表达，`port=0` 时实际端口由 `port()` 回报。
    /// @param endpoint 监听端点；须为有效的 IPv4 / IPv6 端点且非本机 IPC 形态。
    /// @param backlog 挂起连接队列深度（透传 `listen(2)`）。
    /// @return 成功为就绪监听器（句柄已在失败路径关闭）；失败为 `net-endpoint-invalid` /
    ///         `net-socket-failed` / `net-bind-failed` / `net-listen-failed`。
    [[nodiscard]] static auto bind(const Endpoint &endpoint, int backlog) -> Result<Listener>;

    Listener(const Listener &) = delete;
    auto operator=(const Listener &) -> Listener & = delete;
    /// @brief 移动构造：句柄与端口一同移交，源对象转为无效态。
    /// @param other 被移交的源监听器。
    Listener(Listener &&other) noexcept;
    /// @brief 移动赋值：先关闭自有句柄再接管，源对象转为无效态。
    /// @param other 被移交的源监听器。
    /// @return 本对象引用。
    auto operator=(Listener &&other) noexcept -> Listener &;
    /// @brief 析构：句柄有效则自动关闭（等价 `close()`）。
    ~Listener();

    /// @brief 句柄是否有效。
    /// @return 未关闭且非默认构造时为 true。
    [[nodiscard]] auto is_valid() const -> bool { return handle_ != AURORA_INVALID_HANDLE; }

    /// @brief 实际绑定端口。
    /// @return `listen_loopback` 成功时的端口（port=0 时为系统分配值）；无效态为 0。
    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// @brief 借出原生句柄（不转移所有权）：交 `wait_ready` 或事件源注册口做可中断等待。
    ///
    /// 与 `StreamSocket::native_handle()` 同口径：仅在本对象存活且未 `close()` 期间有效，调用方不得
    /// 自行关闭；Windows 为 `SOCKET`、POSIX 为文件描述符，以 `std::intptr_t` 承载、`-1` 恒无效。
    /// @return 监听句柄的 `intptr_t` 承载值。
    [[nodiscard]] auto native_handle() const -> std::intptr_t { return handle_; }

    /// @brief 可中断地等待监听口进入可读（有连接挂起）状态。
    /// @param timeout 最长等待时长。
    /// @return 成功为「timeout 内是否可读」（false = 纯超时，不是错误，调用方据此轮询自有停止标志）；
    /// 失败为 `net-wait-failed`。
    [[nodiscard]] auto wait_readable(std::chrono::milliseconds timeout) const -> Result<bool>;

    /// @brief 接受一条已到达连接（须先 `wait_readable` 返回 true，否则阻塞）。
    /// @return 成功为已连接套接字（对端地址经 `is_loopback_peer()` 复核）；失败为 `net-accept-failed`。
    [[nodiscard]] auto accept() const -> Result<StreamSocket>;

    /// @brief 先 `shutdown(both)` 再关闭（幂等）：唤醒并终止另一线程可能阻塞其中的
    /// `wait_readable`/`accept`，语义与 InspectorServer 停机场径一致。
    auto close() -> void;

  private:
    static constexpr std::intptr_t AURORA_INVALID_HANDLE = -1;
    std::intptr_t handle_ = AURORA_INVALID_HANDLE;
    std::uint16_t port_ = 0;
};

}  // namespace aurora::net::blocking

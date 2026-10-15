#pragma once

/// @brief 事件驱动的数据报传输（CHG-018 传输切片：UDP 的消息语义、组播与广播）。
/// @file net/datagram.h
///
/// 与 `StreamTransport` 的分工是**边界**而不是能力：流面向字节、没有消息分界，数据报一条一条、每条
/// 自带边界与来源地址。四条形态裁决（规范性，落码与文档回写以此为准）：
/// - **一次 `receive()` 交付恰一条数据报**：绝不跨数据报拼装，也不把一条拆成两轮投递。
/// - **零字节是一条消息**：`Datagram::bytes` 为空且 `truncated == false` 表示对端发了空报。数据报没有
///   「对端已收摊」这个状态（UDP 无连接语义），故 0 字节不是 EOF 而是内容——把它做成终止信号会让上层
///   永远读不到合法的空调用（与 `net::Frame` 的「空帧是语义而非噪声」同源）。
/// - **截断显式回报**：本层自持一枚按内核上界（65535 字节）配足的接收缓冲，整条读进库内后再按
///   「调用方缓冲长度与 `DatagramOptions::max_datagram_bytes` 取小者」裁剪并置 `Datagram::truncated`。
///   Windows 在缓冲不足时让 `recvfrom` 直接失败并**丢弃该条数据报**，POSIX 则静默截断——两种行为
///   都不能对外，故把判定收进库内，代价是一次复制。
/// - **拉模型与监听口同族**：没有待交付的接收槽就不等待可读，数据报留在内核接收队列；队列满时由内核
///   丢弃。这是 UDP 的固有语义，本层不伪造排队、不为它编造错误码，也不设无界队列。
///
/// `connect` 模式与组播**互斥**（提案 §18-3）：内核在已 connect 的套接字上只收对端数据报，静默接受
/// 组播等于造出一个永远收不到包的配置，故该形态下的 `join_multicast_group()` 报 `net-option-conflict`。
/// 需同时收单播与组播时用两枚 `DatagramTransport`。**不提供 `disconnect()`**：POSIX 无可靠取消连接
/// 语义、Windows 需换协议技巧，跨平台一致性不值得为此破例。
///
/// 本机 IPC 形态（`Endpoint::local()`）在本层一律拒绝：Windows 命名管道没有数据报语义，POSIX 半边
/// 可用的形态不进公共面。
///
/// @code
/// auto socket = aurora::net::DatagramTransport::bind(
///     aurora::net::Endpoint::loopback(0), aurora::net::DatagramOptions{});
/// if (socket) {
///     socket->receive(buffer, [](auto arrived) {
///         if (!arrived) { return; }
///         const auto report = arrived.value();  // 一条数据报 + 来源端点 + 截断标志
///     });
/// }
/// @endcode
/// @note Thread: 见各类型；句柄单所有权，跨线程移交用 std::move
/// @note Side-effects: 非阻塞收发系统调用与进程级事件源登记

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/cancellation.h"

namespace aurora::net {

class DatagramTransport;

namespace detail {
struct DatagramState;  ///< 一枚数据报套接字的状态（字段与依赖全部留在实现侧，公共头不暴露）
class DatagramFactory;  ///< 实现侧工厂：唯一能以状态指针构造公共句柄的类型
}  // namespace detail

/// @brief 一条收到的数据报：载荷视图、来源端点与截断标志。
/// @note `bytes` 指向调用方在 `receive()` 登记的那枚缓冲，回调返回后仍由调用方持有——与
///       `net::Frame::bytes`（指向分帧器内部缓冲、仅本次回调内有效）方向相反，因为两者的缓冲归属不同。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct Datagram {
    std::span<const char> bytes;  ///< 本条数据报的载荷（已按 `buffer.size()` 裁剪）
    Endpoint source;  ///< 来源端点；已连接套接字上恒等于该对端
    bool truncated = false;  ///< 本条长于本次可交付的字节数（调用方缓冲与 `max_datagram_bytes` 取小者），尾部已丢弃

    /// @brief 逐字段相等比较（字节内容、来源、截断标志）。
    /// @param other 被比较的数据报。
    /// @return 三者全同为 true。
    [[nodiscard]] auto operator==(const Datagram &other) const -> bool {
        return bytes.size() == other.bytes.size() && std::equal(bytes.begin(), bytes.end(), other.bytes.begin()) &&
               source == other.source && truncated == other.truncated;
    }
};

/// @brief 一次发送的结果：成功携已进入内核的字节数（整条接受或明确失败）。
using DatagramSendDone = std::function<void(Result<std::size_t>)>;
/// @brief 一次接收的结果：成功携恰一条数据报，失败携原因（含 `net-cancelled`）。
using DatagramDone = std::function<void(Result<Datagram>)>;

/// @brief 一枚数据报套接字的缓冲口径（字段全部可显式覆盖）。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct DatagramOptions {
    /// @brief 单条数据报的字节口径：发送侧越过它即在发起系统调用前拒收，接收侧它是单次交付的上限
    ///        （与调用方缓冲长度取小者，多出的部分随 `Datagram::truncated` 显式回报）。
    ///        合法区间 `1..65535`（`UDP` 载荷的内核上界，不是平台差异）；越界的口径在绑定时即拒。
    /// @note 库内接收缓冲恒按内核上界配足，与本合同口径无关：Windows 的 `recvfrom` 在缓冲不足时
    ///       会失败并整条丢弃数据报，POSIX 则静默截断，只有「先整条读进来」能让两条腿给出同一结果。
    std::size_t max_datagram_bytes = std::size_t{65535};
    /// @brief 待发数据报的总字节上限：排队中的载荷副本总字节数超过它即拒（`net-buffer-limit`）。
    /// 上限的存在是「慢消费者不得造成无界缓冲」这条不变量在数据报侧的形状，与 `max_pending_bytes` 同族。
    std::size_t max_pending_bytes = std::size_t{1024} * 1024;
};

/// @brief 一枚 UDP 数据报套接字（move-only RAII 句柄；析构即关闭）。
///
/// 空态（默认构造或移动后）是合法值：对它调用成员是安全的空操作，完成回调以失败形态抵达而不是崩溃
/// 或悬挂。连接与绑定都是**同步**工厂：UDP 没有握手，`connect(2)` 在本地登记对端即完成，把它做成异步
/// 等于给一个不会失败的调用编造完成通道。
/// @note Thread: 状态 thread-safe（每枚套接字一把互斥量，完成回调一律在锁外触发并投回发起线程）；
///               句柄本身单所有权，移动即移交
/// @note Side-effects: 非阻塞收发系统调用；`close()` 注销事件源并归还句柄
class DatagramTransport {
  public:
    /// @brief 构造空句柄（占位 / 移动后的落点）。
    DatagramTransport() = default;

    DatagramTransport(const DatagramTransport &) = delete;
    auto operator=(const DatagramTransport &) -> DatagramTransport & = delete;
    /// @brief 移动构造：所有权移交，源转为空态（其析构因此不再关闭本套接字）。
    /// @param other 被移交的源句柄。
    DatagramTransport(DatagramTransport &&other) noexcept;
    /// @brief 移动赋值：先关闭自有套接字再接管。
    /// @param other 被移交的源句柄。
    /// @return 本对象引用。
    auto operator=(DatagramTransport &&other) noexcept -> DatagramTransport &;
    /// @brief 析构：套接字仍开着则关闭（等价 `close()`）。
    ~DatagramTransport();

    /// @brief 绑定一枚数据报套接字到本地端点（不设定对端，收任何来源）。
    ///
    /// 绑到通配地址即接受发往本机任一地址的数据报；组播须先绑定再 `join_multicast_group()`。
    /// @param local 本地端点；须为 IPv4 / IPv6，端口 `0` 表示由系统分配（实际值经 `local_port()` 回报）。
    /// @param options 缓冲口径。
    /// @return 成功为就绪的套接字；失败为 `net-endpoint-invalid` / `net-socket-failed` /
    ///         `net-bind-failed` / `net-option-failed` / `net-buffer-limit`（口径非法）/
    ///         `net-platform-unavailable`（本腿无此形态或无驱动方）。
    [[nodiscard]] static auto bind(const Endpoint &local, const DatagramOptions &options) -> Result<DatagramTransport>;

    /// @brief 绑定并固定对端：此后只与该地址收发（`connect(2)`，不产生任何网络往返）。
    ///
    /// 固定对端后的套接字上组播一律报 `net-option-conflict`（提案 §18-3），且**没有** `disconnect()`。
    /// @param peer 目标端点；须为 IPv4 / IPv6 且端口非 0。
    /// @param options 缓冲口径。
    /// @return 成功为已连接的数据报套接字；失败为 `net-endpoint-invalid` / `net-socket-failed` /
    ///         `net-bind-failed` / `net-connect-failed` / `net-option-failed` / `net-platform-unavailable`。
    [[nodiscard]] static auto connect(const Endpoint &peer, const DatagramOptions &options)
        -> Result<DatagramTransport>;

    /// @brief 套接字是否仍开着。
    /// @return 已建立且未 `close()` 时为 true。
    [[nodiscard]] auto is_open() const -> bool;

    /// @brief 借出原生句柄（不转移所有权）：交第三方库接管或 `wait_ready` 等待。
    /// @return 原生句柄承载值；空态或已关闭为 -1。
    [[nodiscard]] auto native_handle() const -> std::intptr_t;

    /// @brief 实际绑定端口。
    /// @return 入参端口为 0 时的系统分配值；空态或未绑定为 0。
    [[nodiscard]] auto local_port() const -> std::uint16_t;

    /// @brief 固定对端（`connect` 形态的登记结果）。
    /// @return 已连接形态携该对端；未连接形态返回无效端点（`is_valid() == false`），不是错误。
    [[nodiscard]] auto peer() const -> Endpoint;

    /// @brief 发送一条完整数据报到指定目标（非阻塞；结果恰一次经 `on_done` 抵达）。
    ///
    /// 数据报不可拆分：内核发送缓冲满时本层排队并等可写就绪，队列总字节越过
    /// `DatagramOptions::max_pending_bytes` 即拒（`net-buffer-limit`）。目标不合用（本机 IPC 形态、
    /// 端口 0、族与本套接字不符）在发起系统调用前即落 `net-endpoint-invalid`。
    /// 已固定对端的套接字上本调用报 `net-option-conflict`：Linux 让带地址的 `sendto` 在已连接的
    /// 数据报套接字上直接失败，Windows 却忽略该地址，同一调用不能有两种结局，请改用 `send()`。
    /// @param data 一条数据报的载荷；空载荷是一条**合法的零长数据报**（不是「无事可做」）。
    /// @param target 目标端点；族须与本套接字一致且端口非 0。向广播地址发送须先 `set_broadcast_allowed(true)`。
    /// @param on_done 完成回调；携已发出的字节数。
    auto send_to(std::span<const char> data, const Endpoint &target, DatagramSendDone on_done) -> void;

    /// @brief 发送一条完整数据报给固定对端（未 `connect` 时以 `net-endpoint-invalid` 收尾）。
    /// @param data 一条数据报的载荷（可为空 span = 一条空数据报）。
    /// @param on_done 完成回调；形态同 `send_to()`。
    auto send(std::span<const char> data, DatagramSendDone on_done) -> void;

    /// @brief 登记一枚接收槽（缓冲必须存活到本次回调抵达；每枚槽只被满足一次）。
    ///
    /// 没有待交付槽时本套接字不再被等待可读，数据报留在内核接收队列（满时由内核丢弃，这是 UDP 固有
    /// 语义）。缓冲装不下整条时裁剪并置 `Datagram::truncated`，**不丢其余字节**：下一次 `receive()`
    /// 交付的是**下一条**数据报。
    /// @param buffer 目标缓冲；空 span 是用法错误，回调以 `net-buffer-limit` 形态抵达。
    /// @param on_done 完成回调；携恰一条数据报。
    auto receive(std::span<char> buffer, DatagramDone on_done) -> void;

    /// @brief 加入一个组播组（`IP_ADD_MEMBERSHIP` / `IPV6_JOIN_GROUP`；接收网卡由内核按路由选择）。
    ///
    /// 与 `connect` 形态互斥：已固定对端的套接字上本调用报 `net-option-conflict`（内核只收对端数据报，
    /// 静默接受会造出一个永远收不到包的配置）。
    /// @param group 组播组地址；须为 `Endpoint::is_multicast()` 为真的地址且族与本套接字一致。
    /// @return 成功为 OK；失败为 `net-option-conflict` / `net-endpoint-invalid` / `net-option-failed`。
    [[nodiscard]] auto join_multicast_group(const Endpoint &group) -> Result<void>;

    /// @brief 离开一个组播组（与 `join_multicast_group()` 同一组互斥与校验判据）。
    /// @param group 组播组地址。
    /// @return 成功为 OK；未加入过或系统调用失败为 `net-option-failed`，互斥与校验分支同上。
    [[nodiscard]] auto leave_multicast_group(const Endpoint &group) -> Result<void>;

    /// @brief 组播回环开关：本端发出的组播报是否回投给本端（`IP_MULTICAST_LOOP`）。
    /// @param enabled 为真时回投（内核缺省），为假时不回投。
    /// @return 成功为 OK；失败为 `net-option-failed`（本层记录的口径不随之改变）。
    [[nodiscard]] auto set_multicast_loopback(bool enabled) -> Result<void>;

    /// @brief 本层记录的组播回环口径。
    /// @return 最近一次成功 `set_multicast_loopback()` 的值；未设置过时为 true（内核缺省）。
    [[nodiscard]] auto multicast_loopback() const -> bool;

    /// @brief 广播允许位（`SO_BROADCAST`）：未置位时向广播地址发送由内核拒绝。
    /// @param enabled 为真时允许广播发送。
    /// @return 成功为 OK；失败为 `net-option-failed`（本层记录的口径不随之改变）。
    [[nodiscard]] auto set_broadcast_allowed(bool enabled) -> Result<void>;

    /// @brief 本层记录的广播允许口径。
    /// @return 最近一次成功 `set_broadcast_allowed()` 的值；未设置过时为 false（内核缺省）。
    [[nodiscard]] auto broadcast_allowed() const -> bool;

    /// @brief 待发数据报的总字节数（背压断言点：慢消费者期间它不得越过 `max_pending_bytes`）。
    /// @return 已排队但尚未整条进入内核的字节数；空态为 0。
    [[nodiscard]] auto pending_send_bytes() const -> std::size_t;

    /// @brief 关闭套接字：注销事件源、丢弃未完成的收发（各以 `net-cancelled` 收尾一次）并归还句柄。
    ///
    /// 顺序是「先注销、后关句柄」——反过来的话常驻引擎手里那份快照会撞上系统已复用的句柄号。幂等。
    auto close() -> void;

    /// @brief 本套接字的取消令牌（`cancel()` 会中止所有未完成的收发并关闭套接字）。
    /// @return 与本对象共享状态的令牌；空态返回一枚独立令牌（取消它不产生任何效果）。
    [[nodiscard]] auto cancellation() const -> Cancellation;

  private:
    friend class detail::DatagramFactory;

    /// @brief 以既有状态接管（仅实现侧工厂使用）。
    /// @param state 已建立的数据报状态；空指针即空态。
    explicit DatagramTransport(std::shared_ptr<detail::DatagramState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::DatagramState> state_;  ///< 数据报状态（空态为 nullptr）
};

}  // namespace aurora::net

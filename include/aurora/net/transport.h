#pragma once

/// @brief 事件驱动的字节流传输。
/// @file net/transport.h
///
/// 这是 L1 的「面向字节的异步流」：连接、读、写、接受四类操作都不阻塞任何线程，完成以回调抵达。
/// 同步阻塞面在 `net/blocking.h`（工具与测试宿主专用，面向使用者的一侧不得用它），二者互不重叠。
///
/// 三条形态裁决（规范性，落码与文档回写以此为准）：
/// - **异步形态是回调而不是 `Task`**：本层在 L1，`Task` / 协程在 L4（`state/async.h`），把 `Task`
///   写进 `net/` 公共头会让下层反向依赖上层——与 `net::Resolver` 同一条分层方向裁决。协程包装
///   （`co_read` / `co_write` / `co_await connect`）随绑定层在上层给出，本头只给回调出口。
/// - **完成恒回到发起线程**：状态由每连接一枚互斥量串行化（定时器到期落在引擎线程、`cancel()` 落在
///   调用方线程，两者都是真实入口），就绪通知则经 `app::EventSourceRegistry` 的 `Dispatcher` 投递
///   （缺省引擎驱动时即引擎线程，宿主自带等待集时即其 `dispatch()` 所在线程）。本层把求得的完成结果
///   **统一投回发起 `connect()` / `bind()` 的那条线程**：与 `Task<T>` 共用 `post_to_main` 这一条主线程
///   通道，不新增第三种排队形态；**完成回调一律在锁外触发**，回调内可直接再发起读写而不必担心重入死锁。
/// - **半关闭是合法状态而不是错误**：读得 0 字节 = 对端已关闭写向（FIN），此后本端继续写仍成立；
///   FIN 之后再读恒返回 0，不设 `net-half-closed-*` 之类的码（提案 §18-2）。
/// - **本机 IPC 与 TCP 同型**：同一个 `StreamTransport` / `StreamListener`，形态只由 `Endpoint` 的地址族
///   决定（路径形态即本机 IPC），因此读写、背压、取消、半关闭、事件源驱动这一整套语义在两种形态上逐字
///   相同——TLS 叠加后同构也是这条的推论。两点差异明写在类型成员上：本机 IPC 上 `peer_endpoint()` 无
///   可移植的地址形态可返回（落 `net-option-conflict`），对端身份改由 `peer_credentials()` 回答；
///   `StreamOptions::native` 里的 TCP 专属选项在本机 IPC 上同样落 `net-option-conflict`。
///
/// 背压契约（提案 §11）为**拉模型**：写侧的待发字节越过高水位后，新的 `write()` 直接被拒
/// （`net-buffer-limit`），回落到低水位才重新接受；读侧可 `pause_read()` 让字节留在内核而不进本层缓冲。
/// 单次写入可以越过高水位（超发至多一枚请求），否则大于高水位的正文永远发不出去。两侧都不存在无界缓冲。
///
/// @code
/// aurora::net::StreamTransport::connect(
///     endpoint, aurora::net::StreamOptions{}, aurora::net::Cancellation{},
///     [](auto result) {
///         if (!result) { return; }
///         auto stream = std::move(result.value());
///         stream.write(payload, [](auto written) { /* ... */ });
///     });
/// @endcode
/// @note Thread: 见各类型；句柄本身单所有权，跨线程移交用 std::move
/// @note Side-effects: 非阻塞 socket 系统调用与进程级事件源登记

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "aurora/core/result.h"
#include "aurora/net/address.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/proxy.h"

namespace aurora::net {

/// @brief 单向停读 / 停写 / 双向关停的意图（`StreamTransport::shutdown` 的入参）。
/// @note 停写方向的观测效果是「对端读得 0 字节」，停读方向的观测效果是「本层不再收到该句柄的可读关注」。
enum class ShutdownMode : std::uint8_t {
    Read,  ///< 只停读（`SHUT_RD` / `SD_RECEIVE`）
    Write,  ///< 只停写，即半关闭（`SHUT_WR` / `SD_SEND`）
    Both,  ///< 双向关停（`SHUT_RDWR` / `SD_BOTH`）
};

class StreamListener;
class StreamTransport;

namespace detail {
struct StreamState;  ///< 一条异步连接的状态（字段与依赖全部留在实现侧，公共头不暴露）
struct ListenerState;  ///< 一个监听口的状态（同上）
class TransportFactory;  ///< 实现侧工厂：唯一能以状态指针构造上述两个公共句柄的类型

/// @brief 实现侧接缝：把应用侧已经读到的字节接到该流的读队列前端，先于内核字节投递。
///
/// 存在的理由只有一处：代理隧道握手必须一次读满一个不定长的头部块，读多的那段字节属于隧道，若不接回
/// 队列前端就会凭空丢在应用侧（表现为「 CONNECT 建好后首包随机缺字节」）。它不属公共 API：调用方没有
/// 理由手工伪造「内核之前的字节」，那会破坏「读到的字节恒等于线上字节」这条不变量。
/// @param stream 目标流；空态即空操作。
/// @param bytes 待接回的字节（就地复制，调用方保留自身缓冲的所有权）。
auto seed_read(StreamTransport &stream, std::span<const char> bytes) -> void;
}  // namespace detail

/// @brief 一次连接尝试的结果：成功携已建立的流，失败携原因（含 `net-cancelled` / `net-connect-timeout`）。
using ConnectDone = std::function<void(Result<StreamTransport>)>;
/// @brief 一次接受的结果（`StreamListener` 交付新连接的通道）。
using AcceptDone = std::function<void(Result<StreamTransport>)>;
/// @brief 一次读的结果：成功携实际读取字节数，**0 = 对端已关闭写向**（不是错误）。
using ReadDone = std::function<void(Result<std::size_t>)>;
/// @brief 一次写的结果：成功携已进入内核的字节数，失败携原因。
using WriteDone = std::function<void(Result<std::size_t>)>;

/// @brief 原生选项逃生舱：少量高频 socket 选项（提案 §11 的原则是「描述符级逃生舱 + 少量高频选项」，
/// **不封装全选项矩阵**）。
///
/// 每个字段都是 `std::optional`：**未置值的字段一条 `setsockopt` 都不发起**，内核缺省原样保留。
/// 生效时机只有一个——在 `connect()` / `bind()` 发起系统调用之前（`SO_REUSEADDR` 与源地址绑定在 bind
/// 之后再设就已无效，因此本层不提供「连接建立后再改」的第二入口）。此后要动没覆盖到的选项，就用
/// `native_handle()` 交出描述符自行接管，那正是逃生舱的另一半。
///
/// 与地址族的配合：`no_delay` 与 keepalive 各档（含其总开关）是 TCP 专属，本机 IPC 流上没有对应物，请求它们落
/// `net-option-conflict` 而**不是静默忽略**——静默忽略会让「以为开了保活的长连接」等到下一次中间
/// 设备超时才现形，那时现场已经没有配置信息。
/// @note Thread: thread-safe（值类型）
struct NativeOptions {
    std::optional<bool> no_delay;  ///< `TCP_NODELAY`：关闭 Nagle 算法（低延迟小报文场景）
    std::optional<int> receive_buffer_bytes;  ///< `SO_RCVBUF`：内核接收缓冲请求值（须为正）
    std::optional<int> send_buffer_bytes;  ///< `SO_SNDBUF`：内核发送缓冲请求值（须为正）
    std::optional<bool> reuse_address;  ///< `SO_REUSEADDR`：允许绑定处于 TIME_WAIT 的本地地址
    /// @brief `SO_LINGER`：**置值即启用**该选项（未置值时不改动内核缺省的「优雅关闭」形态）；
    /// 正值是等待待发数据发完的时限，0 表示关闭时立即丢弃待发数据并发 RST。
    std::optional<std::chrono::milliseconds> linger;
    std::optional<bool> keep_alive_enabled;  ///< `SO_KEEPALIVE`：连接空闲期发探活报文
    std::optional<std::chrono::milliseconds> keep_alive_idle;  ///< 空闲多久后开始探活（`TCP_KEEPIDLE`）
    std::optional<std::chrono::milliseconds> keep_alive_interval;  ///< 探活报文间隔（`TCP_KEEPINTVL`）
    std::optional<int> keep_alive_probes;  ///< 连续失败多少次判死（`TCP_KEEPCNT`）
    std::optional<Endpoint> source;  ///< 客户端侧绑定源地址 / 网卡（多网卡与出接口选择）
};

/// @brief 本机 IPC 连接的对端身份（提案 §11：本机服务的访问控制从「只看回环地址」升级为「看对端身份」）。
///
/// 三枚数值只在 `anonymous` 为假时有意义。身份是**连接建立那一刻**由内核登记的快照：此后对端换了
/// 属主（进程不会）或对端已退出而连接尚未回收，本结构都不会变，故它回答的是「谁连上了我」，不是
/// 「它现在还活着吗」。
/// @note Thread: thread-safe（值类型）
struct PeerCredentials {
    bool anonymous = true;  ///< 本平台腿不提供对端身份（Windows 的本机 IPC 腿即恒为真）
    std::uint32_t user_id = 0;  ///< 对端进程属主用户标识
    std::uint32_t group_id = 0;  ///< 对端进程属主组标识
    std::uint32_t process_id = 0;  ///< 对端进程标识（连接建立时登记的内核快照）
};

/// @brief 一条流的缓冲与超时口径（全部字段可显式覆盖；缺省值是提案 §18 定案的数字）。
/// @note Thread: thread-safe（值类型）
struct StreamOptions {
    /// @brief 单次投递给读回调的最大字节数：内核里多于这个数就分轮投递，故调用方的缓冲可以比它小。
    std::size_t read_chunk_bytes = std::size_t{64} * 1024;
    /// @brief 待发读请求的容量上限：排队中的读缓冲总字节数超过它即拒（读请求本身也是一种缓冲）。
    std::size_t max_pending_bytes = std::size_t{1024} * 1024;
    /// @brief 写高水位：待发字节越过它后新的 `write()` 直接被拒（`net-buffer-limit`），慢消费者不得造成无界缓冲。
    std::size_t write_high_water_bytes = std::size_t{256} * 1024;
    /// @brief 写低水位：待发字节回落到它以下才重新接受写入（与高水位成对，避免在阈值上反复抖动）。
    /// 缺省是高水位的四分之一；`set_high_water_mark()` 会按新的高水位重算它（置 0 时同为 0）。
    std::size_t write_low_water_bytes = std::size_t{64} * 1024;
    /// @brief 连接尝试的时限；非正值表示不设时限（由对端或路由决定何时出结果）。
    std::chrono::milliseconds connect_timeout{std::chrono::milliseconds{5000}};
    /// @brief 双栈竞速的交错窗口：首条候选立即发起，其后每条在上条发起后隔这么久才发起（Happy
    /// Eyeballs 的 RFC 8305 形态，提案 §11 定案 200 ms）。**任一候选提前失败即立刻发起下一条**，
    /// 不等这个窗口；置 0 即关掉竞速，只发偏好族的首条。
    std::chrono::milliseconds race_stagger{std::chrono::milliseconds{200}};
    /// @brief 原生选项逃生舱：缺省全部字段未置值，即**一条 `setsockopt` 都不发起**（内核缺省原样保留）。
    NativeOptions native;
};

/// @brief 面向字节的异步流（move-only RAII 句柄；析构即关闭连接）。
///
/// 一个对象代表一条连接的所有权。空态（默认构造或移动后）是合法值：对它调用成员是安全的空操作，
/// 完成回调以 `net-socket-failed` 形态抵达而不是崩溃或悬挂。
/// @note Thread: 状态 thread-safe（每连接一枚互斥量）；句柄本身单所有权，移动即移交
/// @note Side-effects: 非阻塞收发系统调用；`close()` 注销事件源并归还句柄
class StreamTransport {
  public:
    /// @brief 构造空句柄（占位 / 移动后的落点）。
    StreamTransport() = default;

    StreamTransport(const StreamTransport &) = delete;
    auto operator=(const StreamTransport &) -> StreamTransport & = delete;
    /// @brief 移动构造：所有权移交，源转为空态（其析构因此不再关闭本连接）。
    /// @param other 被移交的源句柄。
    StreamTransport(StreamTransport &&other) noexcept;
    /// @brief 移动赋值：先关闭自有连接再接管。
    /// @param other 被移交的源句柄。
    /// @return 本对象引用。
    auto operator=(StreamTransport &&other) noexcept -> StreamTransport &;
    /// @brief 析构：连接仍开着则关闭（等价 `close()`）。
    ~StreamTransport();

    /// @brief 发起一条异步连接（非阻塞；结果恰一次经 `on_done` 抵达）。
    ///
    /// 失败也走回调：本函数没有返回通道，因此**不存在「既不成功也不失败」的第三种结局**——一次永不
    /// 触发的回调是这类接口最难定位的缺陷，故连入参不合用（端口为 0、空 / 超长的本机路径）与引擎起不来都
    /// 以回调携带原因的形式送达。
    /// @param endpoint 目标端点：IPv4 / IPv6 且端口非 0，或本机 IPC 路径形态（`Endpoint::local`）。
    /// @param options 缓冲、超时与原生选项口径。
    /// @param cancellation 本次操作的取消令牌；`cancel()` 后回调恰一次以 `net-cancelled` 抵达。
    /// @param on_done 完成回调；成功携已连接的流。
    static auto connect(const Endpoint &endpoint, const StreamOptions &options, const Cancellation &cancellation,
                        ConnectDone on_done) -> void;

    /// @brief 按主机名发起异步连接：解析 → 双栈竞速 → 交出最先建成的那条流。
    ///
    /// 与端点形态的分工：本入口持的是**主机文本**，字面量的接受形态与 `Endpoint::from_ip` 一致（IPv6
    /// 可带方括号与作用域号），数字字面量在解析层短路而不查 DNS。竞速次序与交错窗口见
    /// `StreamOptions::race_stagger`；解析成功但零条可用记录落 `net-dns-failed`（与 `net::Resolver`
    /// 同码，不在这里另造一套分岔）。
    /// @param host 目标主机文本；空串是用法错误，落 `net-endpoint-invalid`。
    /// @param port 目标端口（主机序）；0 落 `net-endpoint-invalid`。
    /// @param options 缓冲、超时与竞速口径。
    /// @param cancellation 本次操作的取消令牌；`cancel()` 后未落地的候选全部收尾。
    /// @param on_done 完成回调；恰一次抵达。
    static auto connect(std::string_view host, std::uint16_t port, const StreamOptions &options,
                        const Cancellation &cancellation, ConnectDone on_done) -> void;

    /// @brief 经代理建立到 `host:port` 的隧道，产出即隧道里的流（对上层与直连同构）。
    ///
    /// `ProxyKind::Direct` 等价于走直连入口——显式关闭代理就是直连，不是「还没决定」。隧道只在握手
    /// 成功时成立：**代理不可达、被拒或不会说话一律以失败收尾，绝不回退直连**（提案 §11），诊断文本
    /// 只带代理与目标的 `host:port` 而不带凭据。握手期间读过头部块之后的字节属于隧道，本层把它们接回
    /// 读队列前端，不会因为「一次读多了」而丢字节。
    /// @param proxy 代理配置；由调用方经 `ProxyEnvironment::for_target` 决定（本层不读环境变量）。
    /// @param host 目标主机文本；`socks5`（非 `h`）形态下必须是数字字面量，否则落 `net-endpoint-invalid`。
    /// @param port 目标端口（主机序）。
    /// @param options 缓冲、超时与竞速口径；`connect_timeout` 在此覆盖「直到隧道可用」的整体时限。
    /// @param cancellation 本次操作的取消令牌。
    /// @param on_done 完成回调；恰一次抵达，失败携 `net-proxy-*` 系列原因或连接类原因。
    static auto connect_via(const ProxyConfig &proxy, std::string_view host, std::uint16_t port,
                            const StreamOptions &options, const Cancellation &cancellation, ConnectDone on_done)
        -> void;

    /// @brief 连接是否仍开着。
    /// @return 已建立且未 `close()` 时为 true（半关闭不影响本判据：停写方向仍算开着）。
    [[nodiscard]] auto is_open() const -> bool;

    /// @brief 借出原生句柄（不转移所有权）：交第三方库接管或事件源等待。
    ///
    /// 仅在**本对象存活且未 `close()`** 期间有效；调用方不得关闭它。Windows 为 `SOCKET`、POSIX 为
    /// 文件描述符，一律以 `std::intptr_t` 承载、`-1` 恒无效。
    /// @return 原生句柄承载值；空态或已关闭为 -1。
    [[nodiscard]] auto native_handle() const -> std::intptr_t;

    /// @brief 读出对端地址。
    /// @return 成功为对端端点；空态、已关闭或查询失败为 `net-socket-failed`；**本机 IPC 连接**为
    ///         `net-option-conflict`（路径形态的对端没有可移植的地址文本，身份问 `peer_credentials()`）。
    [[nodiscard]] auto peer_endpoint() const -> Result<Endpoint>;

    /// @brief 对端是否位于本机回环（服务端访问控制判据，含 IPv4 内嵌形态）。
    /// @return 落在回环段为 true；查询失败或地址族不认识为 false（本机 IPC 连接恒为 false）。
    [[nodiscard]] auto is_loopback_peer() const -> bool;

    /// @brief 读出本机 IPC 连接的对端身份（`SO_PEERCRED` / `LOCAL_PEERCRED` 一类的内核登记）。
    ///
    /// 存在的理由是把本机服务的访问控制从「只看回环地址」升级为「看对端身份」：回环段上坐着谁，单靠
    /// 地址无法回答，而本机 IPC 的连接本就只可能在同机发生。
    /// @return 成功为对端身份（平台腿不提供时 `anonymous` 为真）；空态或已关闭为 `net-socket-failed`；
    ///         **非本机 IPC 连接**（IPv4 / IPv6）为 `net-option-conflict`——IP 对端没有内核登记的身份，
    ///         这一条不是失败而是形态不匹配，静默回全零身份会把「以为做了身份校验」的授权判断变成空转。
    [[nodiscard]] auto peer_credentials() const -> Result<PeerCredentials>;

    /// @brief 登记一次读请求（缓冲必须存活到本次回调抵达）。
    ///
    /// 至多一次投递给一次回调；0 字节是「对端已关闭写向」（EOF），不是错误。请求在待发队列里按登记
    /// 次序满足，排在队首的请求决定本轮最多读多少字节。
    /// @param buffer 目标缓冲；空 span 是用法错误，回调以 `net-buffer-limit` 形态抵达（0 字节只留给 EOF）。
    /// @param on_done 完成回调；携实际读取字节数。
    auto read(std::span<char> buffer, ReadDone on_done) const -> void;

    /// @brief 登记一次写请求（全部字节都进入内核或明确失败才算完成）。
    ///
    /// 待发字节达到高水位时新的写入被拒（`net-buffer-limit`），回落到低水位后重新接受；这是背压的
    /// 表达点，调用方据此减速而不是把数据堆进本层。
    /// @param data 待发送字节；空 span 直接以 OK 完成（不产生任何系统调用）。
    /// @param on_done 完成回调；携**已接受**的字节数（失败时携已接受的那一部分的意义由 message 说明）。
    auto write(std::span<const char> data, WriteDone on_done) const -> void;

    /// @brief 调整写高水位（低水位按其四分之一回落，最小 1 字节）。
    /// @param bytes 新的待发字节上限；0 视为「不设缓冲」（每次写入都必须能立即进内核）。
    auto set_high_water_mark(std::size_t bytes) const -> void;

    /// @brief 暂停读投递：不再从内核取字节，数据留在对端 / 内核缓冲里。
    /// @note 与 `shutdown(ShutdownMode::Read)` 的区别：暂停随时可恢复且不影响连接状态，停读是单向关闭。
    auto pause_read() const -> void;

    /// @brief 恢复读投递（未暂停时为空操作）。
    auto resume_read() const -> void;

    /// @brief 当前是否处于暂停读状态。
    /// @return `pause_read()` 生效且未 `resume_read()` 时为 true。
    [[nodiscard]] auto read_paused() const -> bool;

    /// @brief 待发写字节数（背压断言点：慢消费者期间它不得越过 `write_high_water_bytes`）。
    /// @return 已排队但尚未全部进入内核的字节数；空态为 0。
    [[nodiscard]] auto pending_write_bytes() const -> std::size_t;

    /// @brief 单向或双向关停（不归还句柄，也不丢弃已排队的读请求）。
    /// @param mode 关停方向。
    /// @return 成功为 OK；空态为 `net-socket-failed`，系统调用失败为 `net-option-failed`。
    [[nodiscard]] auto shutdown(ShutdownMode mode) const -> Result<void>;

    /// @brief 关闭连接：注销事件源、丢弃未完成的读写请求（各以 `net-cancelled` 收尾一次）并归还句柄。
    ///
    /// 顺序是「先注销、后关句柄」——反过来会让常驻引擎手里那份快照撞上系统已复用的句柄号。幂等。
    auto close() const -> void;

    /// @brief 本连接的取消令牌（`cancel()` 会中止所有未完成的读写并关闭连接）。
    /// @return 与本对象共享状态的令牌；空态返回一枚独立令牌（ cancel 它不产生任何效果）。
    [[nodiscard]] auto cancellation() const -> Cancellation;

  private:
    friend class detail::TransportFactory;
    friend auto detail::seed_read(StreamTransport &stream, std::span<const char> bytes) -> void;

    /// @brief 以既有状态接管（仅实现侧工厂与 `detail::TransportFactory` 使用）。
    /// @param state 已建立的连接状态；空指针即空态。
    explicit StreamTransport(std::shared_ptr<detail::StreamState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::StreamState> state_;  ///< 连接状态（空态为 nullptr）
};

/// @brief 服务端监听口（move-only RAII 句柄；析构即关闭监听）。
/// @note Thread: 状态 thread-safe；句柄单所有权
/// @note Side-effects: 监听句柄与进程级事件源登记
class StreamListener {
  public:
    /// @brief 构造空句柄。
    StreamListener() = default;

    StreamListener(const StreamListener &) = delete;
    auto operator=(const StreamListener &) -> StreamListener & = delete;
    /// @brief 移动构造：所有权移交，源转为空态。
    /// @param other 被移交的源句柄。
    StreamListener(StreamListener &&other) noexcept;
    /// @brief 移动赋值：先关闭自有监听再接管。
    /// @param other 被移交的源句柄。
    /// @return 本对象引用。
    auto operator=(StreamListener &&other) noexcept -> StreamListener &;
    /// @brief 析构：监听仍开着则关闭。
    ~StreamListener();

    /// @brief 绑定监听口（不阻塞任何线程；新连接经 `accept()` 登记的回调交付）。
    ///
    /// 绑到非回环地址即接受来自其它主机的连接，访问控制由调用方在取得流之后负责（本层不做隐式本机
    /// 拦截，否则第三方接管句柄会得到无法解释的拒绝）。本机 IPC 形态下**已存在的路径不会被本层删除**：
    /// 残留 socket 文件的清理归调用方，误删他人正在监听的同名路径是不可逆动作，而本层无从判断它是否
    /// 活跃（路径占用在 bind 失败的那一刻才可见）。
    /// @param endpoint 监听端点：IPv4 / IPv6（端口 0 表示由系统分配，实际值经 `local_port()` 回报），
    ///        或本机 IPC 路径形态（`Endpoint::local`，此时 `local_port()` 恒 0）。
    /// @param backlog 挂起连接队列深度。
    /// @param options 交付给每条被接受连接的缓冲与超时口径；`options.native` 在 bind **之前**生效
    ///        （`reuse_address` 只有这个时机有意义）。
    /// @return 成功为就绪监听器；失败为 `net-endpoint-invalid` / `net-socket-failed` / `net-bind-failed` /
    ///         `net-listen-failed` / `net-option-failed` / `net-option-conflict` /
    ///         `net-platform-unavailable`（本腿没有本机 IPC 通路）。
    [[nodiscard]] static auto bind(const Endpoint &endpoint, int backlog, const StreamOptions &options)
        -> Result<StreamListener>;

    /// @brief 登记一枚新连接回调（每枚只被满足一次；没有待交付回调时本监听口不再被等待）。
    ///
    /// 「没有回调就不等」是有意的：连接在此期间的归宿是内核 backlog（由 `backlog` 限定深度），
    /// 而不是本层队列——后者没有上限，等于把慢宿主变成无界缓冲。
    /// @param on_ready 新连接回调；携已建立的对端流。
    auto accept(AcceptDone on_ready) const -> void;

    /// @brief 实际绑定端口。
    /// @return 成功绑定时的端口（入参 0 时为系统分配值）；空态为 0。
    [[nodiscard]] auto local_port() const -> std::uint16_t;

    /// @brief 借出原生句柄（不转移所有权）：交第三方事件循环或 `wait_ready`。
    /// @return 原生句柄承载值；空态或已关闭为 -1。
    [[nodiscard]] auto native_handle() const -> std::intptr_t;

    /// @brief 是否仍在监听。
    /// @return 已成功绑定且未 `close()` 时为 true。
    [[nodiscard]] auto is_valid() const -> bool;

    /// @brief 关闭监听：注销事件源并归还句柄（已交付的连接不受影响）。幂等。
    auto close() const -> void;

  private:
    friend class detail::TransportFactory;

    /// @brief 以既有状态接管（仅实现侧工厂使用）。
    /// @param state 已绑定的监听状态；空指针即空态。
    explicit StreamListener(std::shared_ptr<detail::ListenerState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::ListenerState> state_;  ///< 监听状态（空态为 nullptr）
};

}  // namespace aurora::net

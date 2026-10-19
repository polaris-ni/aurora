// ws_session.h —— 升级之后的会话状态机（仅库实现可见，不属公共 API）。
//
// 为何单列：一次 WebSocket 会话的**存续期与任何一次协程交换都不重合**。握手那条腿在 `co_return` 那一刻就
// 收尾了，而这条连接还要一直活着——读环由谁驱动、发送队列由谁持有、心跳定时器挂在谁的令牌上、宿主在交付
// 回调里把句柄销毁之后还能不能碰那个 sink，四件事都需要一个自持所有权的对象。把它写进 `Client` 的那条
// 协程里就只能靠「协程永不返回」实现，那是把一次交换伪装成一条会话（SSE 那条订阅环之所以能同帧跑完，
// 是因为它每条流都有终点；这条没有）。
//
// 本层只做会话编排：分帧与握手判定各在 `ws_frame.h` 与 `ws_handshake.h`，协议语义（谁必须回 pong、哪种
// 收场算干净）在这里定。公共形态（八条约束）见 `include/aurora/net/websocket.h` 的文件头，本文件不复述。
//
// 六条落码口径（规范性，公共头未覆盖的部分）：
// - **所有权只有一条正向链**：`WebSocket` 句柄 → `shared_ptr<WsSession>`；读环与心跳环两条协程帧**各自**持
//   一份 `shared_ptr`。因此「句柄被销毁」不会把会话当场拆掉（两条环还在跑），而「两条环都收尾」也不会让
//   会话先于句柄消失。会话绝不自持自己的帧：那会让收尾路径变成「正在执行的栈帧把自己释放掉」。
// - **`detached_` 之后不再触碰 sink**：句柄一销毁，本会话立刻停止调用那个抽象类的任何入口，连 ping 的自动
//   pong 回复也一并停掉（链路已在收摊，回不回都不再是宿主能观察的事实）。宿主因此可以在最后一条消息的
//   回调里就地销毁句柄，不必担心同一段字节里后续帧再撞上它。
// - **控制帧不受发送队列的闸**：`ping` / `pong` / `close` 各自 ≤125 字节且是协议义务，一次数据突发不该把
//   「对端问了 ping」变成「我方没答」。数据帧与分片则全部计入那两枚旋钮（字节数与条数）。
// - **`pong_timeout` 挂在心跳周期上**：一个周期 = 睡 `heartbeat_interval` → 发一枚 ping → 睡 `pong_timeout`
//   并在醒来时问「这一枚等到了吗」，没等到即 `ws-closed`。本层**不读任何时钟**：到点判定全部由引擎定时器
//   的次序给出，于是同一份代码在脚本化时钟（`FakeEngine`）下逐值可断言。代价写在公共头：健康链路的实际
//   心跳周期是两枚之和；`heartbeat_interval` 未设时 `pong_timeout` 无事可管（它守的是「我方发出的 ping」）。
// - **收尾只有一个出口**：`settle()`。四型事实（对端 close、协议违例、传输失败、对端没说 close 就 EOF）与
//   本端 `close()` 的落地全部汇到这一处，`on_closed` 因此恒恰一次（`detached_` 那次是零次）。它掐连接在前、
//   报宿主在后，并自带一份 `shared_from_this` 看守——`close()` 会在回调链里被再次触发，没有那份看守就是
//   在已释放的会话对象上继续走后半段。
// - **对端的 close 要等回声写完再定局**：本端尚未发起关闭时收到 close，必须回一枚（`RFC 6455 §7.1.1` 的双向
//   交换）。若当场 `settle()`，那次 `abort_link()` 会把还压在队列与内核缓冲里的回声连同连接一起丢掉，对端
//   只能把这次收摊记成异常断连、拿不到任何状态码。于是收到的 close 先记进 `pending_close_`，由写环在写完那枚
//   带 `terminates` 旗位的帧之后再走 `settle()`；回声写不出去（链路已死）同样定局，且不改口径——这次收摊仍由
//   对端开启。代价是「`on_closed` 可能比 close 帧晚一个写出回合」，宿主在回调里看到的事实不变。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/ws_frame.h"
#include "aurora/net/transport.h"
#include "aurora/net/websocket.h"

namespace aurora::net::detail {

/// @brief 一次发送的落地事实（跨分片共用一枚：整条消息写出才算落地，半途失败即整条失败）。
/// @note Thread: 单线程使用（随发送队列与写环存活，恒在主线程访问）
/// @note Side-effects: 结算完成时调用宿主交回的 `WsSent`
struct SendSettlement {
    WsSent on_sent;  ///< 落地回报（可空 = 宿主不关心）
    std::size_t frames_left{0};  ///< 本条消息还有几枚分片未写出
    std::optional<Error> failure;  ///< 首个失败原因；写出全部成功且它为空即干净落地
    bool reported{false};  ///< 是否已报过（一条消息的若干分片共用本对象，这枚旗位保证「恰好一次」）
};

/// @brief 发送队列里的一枚待发字节串（已按线上形态编好帧，含掩码）。
/// @note Thread: 单线程使用
/// @note Side-effects: none
struct QueuedFrame {
    std::string bytes;  ///< 线上字节（一帧一条目，写出即整条消费）
    std::shared_ptr<SendSettlement> settlement;  ///< 落地归属；控制帧为空
    bool terminates{false};  ///< 写完这一枚就该定局（对端 close 的回声）；其余帧为假
};

/// @brief 一条升级后的会话：读环、发送队列、心跳环与收尾的唯一持有者。
///
/// 只能经 `std::make_shared` 建并由 `start(self)` 起跑（两条协程帧各持一份 `shared_ptr`，故本对象必须
/// 一开始就由 `shared_ptr` 托管——`enable_shared_from_this` 在没有主人时是未定义行为）。不可拷贝、不可移动：
/// 一条连接的两个持有者意味着两次关闭。
/// @note Thread: 全部成员恒在主线程调用（续体经主线程投递器恢复；引擎定时器亦回投主线程）
/// @note Side-effects: 读写传输、调用宿主 sink、登记引擎定时器
class WsSession : public std::enable_shared_from_this<WsSession> {
  public:
    /// @brief 以已握手的连接、宿主与选型构造（此刻连接已经升级完成，本对象直接接管读写口）。
    /// @param plain 明文流（`http://` / `ws://` 的目标）；与 `secure` 恰好一枚非空。
    /// @param secure TLS 流（`wss://` 的目标）；与 `plain` 恰好一枚非空。
    /// @param sink 宿主对象；由调用方持有，生命周期须覆盖到 `settle()` 之后或 `detach()` 之时。
    /// @param options 三枚上限、子协议必选旗位与心跳两档。
    /// @param subprotocol 协商到的子协议（无则空）。
    /// @param leftover 握手那次读多带回来的字节（头部块之后属于帧流的那一段；无则空）。
    WsSession(std::shared_ptr<StreamTransport> plain, std::shared_ptr<TlsTransport> secure, WebSocketSink &sink,
              WebSocketOptions options, std::string subprotocol, std::string leftover);

    WsSession(const WsSession &) = delete;
    auto operator=(const WsSession &) -> WsSession & = delete;
    WsSession(WsSession &&) = delete;
    auto operator=(WsSession &&) -> WsSession & = delete;

    /// @brief 析构：不主动掐连接（收摊由 `settle()` / `detach()` 那条路做完）；这里只归还本对象的内存。
    ~WsSession();

    /// @brief 起跑读环与（按需）心跳环。
    ///
    /// 与构造分成两步是必需的：两条协程帧要持本对象的 `shared_ptr`，而 `shared_from_this()` 只有在已经
    /// 由 `shared_ptr` 托管之后才可用。调用方交出 owning 句柄之后叫一次；叫第二次会被就地拒绝（`already_started_`）。
    /// @param self 本会话的持有者（帧随它存活；调用方那份与句柄那份各自独立）。
    auto start(const std::shared_ptr<WsSession> &self) -> void;

    /// @brief 入队一条消息（必要时自动分片）并踢一次写环。
    /// @param kind 消息类型（文本 / 二进制；控制帧不走本入口）。
    /// @param payload 载荷原文。
    /// @param on_sent 落地回报（可空）。
    /// @return 成功 = 已入队；`ws-closed` = 已进入关闭或已收尾；`net-buffer-limit` = 越过整消息上限；
    ///         `ws-protocol-error` = 文本不是合法 UTF-8；`ws-send-queue-full` = 队列两枚闸不过（会话不受影响）。
    [[nodiscard]] auto send(WsMessageKind kind, std::string_view payload, WsSent on_sent) -> Result<void>;

    /// @brief 投递一枚 `close` 帧（本端发起的收摊）。
    /// @param code 线上状态码（须可上线）。
    /// @param reason 原因文本（UTF-8，与码合起来 ≤125 字节）。
    /// @return 成功 = 已入队；`ws-protocol-error` = 码或原因不合形态；`ws-closed` = 已进入关闭或已收尾。
    [[nodiscard]] auto close_wire(std::uint16_t code, std::string_view reason) -> Result<void>;

    /// @brief 句柄析构那条路：尽力投递 `close(1000)`、随后掐连接，且此后不再触碰宿主 sink。
    ///
    /// 与 `close_wire()` 的区别只在**谁观察结局**：本入口不做 `on_closed`，也不接受调用方指定的状态码。
    /// 幂等（移动赋值与重复销毁都只是空操作）。
    auto detach() -> void;

    /// @brief 会话是否还没终结（进入关闭阶段仍算未终结，见公共头 `WebSocket::is_open()`）。
    /// @return 未 `settle()` 为 true。
    [[nodiscard]] auto is_open() const -> bool { return !settled_; }

    /// @brief 协商到的子协议。
    /// @return 服务端回的那一枚；无子协议时为空视图。
    [[nodiscard]] auto subprotocol() const -> std::string_view { return std::string_view{subprotocol_}; }

    /// @brief 发送队列里还压着的待发字节数（含尚未写出的分片与控制帧）。
    /// @return 字节数。
    [[nodiscard]] auto queued_bytes() const -> std::size_t { return queued_bytes_; }

  private:
    // 三枚环体都不读 `self` 的内容（成员一律经 `this` 触碰），但都必须持它到收尾：协程成员函数在帧里存的是
    // 裸 `this`，而句柄可以在任何挂起点被销毁——这份 `shared_ptr` 就是「帧醒来时 `this` 还活着」的唯一凭据。
    /// @brief 读环主体：逐段读出、喂分帧器，直到传输失败、对端收摊或本会话已定局。
    /// @param self 本会话的持有者（随帧存活，体内不读）。
    /// @return 协程任务（收尾即帧销毁；结局已写进 `settled_`，无返回价值）。
    auto read_loop([[maybe_unused]] std::shared_ptr<WsSession> self) -> CoroTask<void>;

    /// @brief 写环主体：按队列次序逐条写出，落地即结算该条消息的 `WsSent`。
    /// @param self 本会话的持有者（随帧存活，体内不读）。
    /// @return 协程任务。
    auto write_loop([[maybe_unused]] std::shared_ptr<WsSession> self) -> CoroTask<void>;

    /// @brief 心跳环主体：睡到点 → 发 ping → 等 pong 窗口 → 问「等到了吗」。
    /// @param self 本会话的持有者（随帧存活，体内不读）。
    /// @return 协程任务。
    auto heartbeat_loop([[maybe_unused]] std::shared_ptr<WsSession> self) -> CoroTask<void>;

    /// @brief 踢写环：没有写环在跑就起一条（在途那条会在下一轮看到新入队的字节）。
    /// @note 调用点全在主线程且检查与置位之间没有挂起点，故「入队后必有人写」不存在竞态窗口。
    auto kick_writer() -> void;

    /// @brief 把一帧的线上字节编好、追加进队列并记账（不做上限判定：调用方负责）。
    /// @param opcode 操作码。
    /// @param final FIN 位。
    /// @param payload 载荷原文。
    /// @param settlement 落地归属（可空 = 控制帧）。
    /// @return 成功为空；`write_ws_frame` 的失败（取不到熵 / 控制帧形态不合）原样交出，此时队列与记账
    ///         一字未动——调用方因此可以按「失败即回滚」的简单形状写发送侧。
    [[nodiscard]] auto queue_frame(WsOpcode opcode, bool final, std::string_view payload,
                                   std::shared_ptr<SendSettlement> settlement) -> Result<void>;

    /// @brief 报出一枚发送的落地事实，恰好一次（`SendSettlement::reported` 是闸）。
    /// @param settlement 落地归属；空指针即空操作。
    /// @param outcome 交回宿主的结局。
    static auto report(const std::shared_ptr<SendSettlement> &settlement, Result<void> outcome) -> void;

    /// @brief 队列里当前排着的条数。
    /// @return 条数。
    [[nodiscard]] auto queued_frames() const -> std::size_t { return queue_.size(); }

    /// @brief 唯一收尾出口：定局、掐连接、报 `on_closed`（`detached_` 时不报）。
    /// @param failure 非干净收尾的原因；`nullopt` = 正常关闭。
    /// @param code 抵达的线上状态码；无即空。
    /// @param reason 随码抵达的原因文本。
    /// @param by_peer 这条结局是否由对端的 close 开启。
    auto settle(std::optional<Error> failure, std::optional<std::uint16_t> code, std::string reason, bool by_peer)
        -> void;

    /// @brief 放弃在途读写并归还连接（幂等）。
    auto abort_link() -> void;

    /// @brief 装配分帧器的四个出口（消息派发、两种探活帧、对端关闭）。
    /// @return 出口束。
    [[nodiscard]] static auto make_sinks(WsSession *self) -> WsFrameSinks;

    std::shared_ptr<StreamTransport> plain_;  ///< 明文流（与下面那枚互斥非空）
    std::shared_ptr<TlsTransport> secure_;  ///< TLS 流
    WebSocketSink &sink_;  ///< 宿主对象（借用；`detached_` 之后不再触碰）
    WebSocketOptions options_;  ///< 三枚上限与心跳两档（构造时读定，此后改旋钮不影响本会话）
    std::string subprotocol_;  ///< 协商到的子协议
    WsFrameReader reader_;  ///< 本条会话的分帧器（分片状态属于这一条流）

    std::vector<QueuedFrame> queue_;  ///< 待发队列（按写出次序）
    std::size_t queued_bytes_{0};  ///< 队列字节数（与 `queue_` 同步维护）
    std::string leftover_;  ///< 头部块之后的富余字节（握手读多了的那一段属于帧流）
    std::optional<WsCloseInfo> pending_close_;  ///< 对端的 close 已记下、只等回声写出后再定局（口径见文件头第六条）

    Cancellation lifecycle_;  ///< 会话级取消令牌：心跳环的等待据此当场放行
    std::size_t pings_sent_{0};  ///< 已发出的 ping 枚数
    std::size_t pongs_seen_{0};  ///< 已收到的 pong 枚数（含对端主动 ping 之外的应答）
    bool writing_{false};  ///< 写环是否在跑
    bool closing_{false};  ///< 本端是否已发起关闭（不再接受新的发送）
    bool settled_{false};  ///< 是否已定局（`on_closed` 的唯一触发点据此幂等）
    bool detached_{false};  ///< 句柄是否已销毁（此后 sink 一律不碰）
    bool already_started_{false};  ///< 读环是否已起过（防二次 `start()` 起两条读环）
};

}  // namespace aurora::net::detail

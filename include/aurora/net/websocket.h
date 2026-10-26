#pragma once

/// @brief `RFC 6455` 客户端的会话面：升级后的双向句柄、逐条消息的交付抽象、关闭与心跳口径。
///
/// 本头只回答一个问题：**「一条已经换过协议的连接，怎么接成宿主能用的双向会话」**。它住在
/// `Client::connect_websocket()` 之上而不是旁边：握手之前的全部事情（解析与竞速、代理隧道或转发、TLS
/// 信任根、策略闸、超时三档、取消令牌）都已经由 `Client` 那条腿兑现了；本头加的是**升级之后的形态**
/// ——帧流的所有权、发送队列的边界、心跳与关闭的时序。
///
/// 与 `net/http_sse.h` 一样，本头只定义类型，不做任何收发；分帧与握手的字节层在
/// `net/detail/ws_frame.h` 与 `net/detail/ws_handshake.h`（库内可见，不属公共 API）。
///
/// 八条形态约束（规范性，落码与文档回写以此为准）：
/// - **句柄自持读环，且 move-only**：一次成功握手的产物是一条**没有结束时刻**的连接，协程在握手结算时就
///   返回了，此后驱动读环的是句柄内部的那份会话状态。因此「句柄被移走」不能中断读环（移动只换持有者，
///   不换状态对象），而「句柄被拷贝」在语义上不存在——一条连接有两个持有者就意味着两次关闭。
/// - **析构 = 尽力投递 `close(1000)` 再掐**：不阻塞、不等待对端回帧、不保证对方看到。这条与「析构里做 IO」
///   的常见忌讳相反，理由是本句柄的资源是一条**协议会话**而非文件描述符：只掐不告会让对端把这次断开记成
///   异常断连（它的 `on_closed` 拿不到状态码），而阻塞等待回帧在析构里等于把一条网络时延挂进宿主对象的
///   销毁路径。要拿到确定的收尾结局就显式调 `close()`。**在交付回调里销毁句柄是合法用法**：句柄一销毁，
///   本会话立即停止触碰那个 sink（后续帧仍被解析与判定，只是不再派发）——宿主因此可以在收到最后一条消息时
///   就地收摊，不必先把句柄搬到别处再等一轮。
/// - **发送是「入队即回 + 回调报落地」两拍**：`send()` 的 `Result` 说的是**这次发送被不被接受**（连接已关、
///   队列越限、体量越限），不是「对端收到了」。写出完成经 `WsSent` 交回。合在一拍里做不到那条队满判据
///   ——队列满时要么阻塞协程（本库协程不阻塞），要么撒谎说已送达。
/// - **队满只拒这一次发送，不断连接**：`ws-send-queue-full` 是 `send()` 的返回值，不是会话的结局；已排队的
///   照发、读环照跑。这条纪律让「宿主一时产出过快」成为一次可恢复的失败，而不是把连接连带赔进去。
/// - **上限三枚都可覆盖，且恒有界**：单帧 / 整消息 / 发送队列（字节数与条数各一枚旋钮）。发出侧超过单帧
///   上限的消息由库**自动分片**（首片带真实类型，续片按序），因此单帧那枚管的是线上帧而不是消息；超过整
///   消息上限的 `send()` 直接以 `net-buffer-limit` 拒掉。读侧越限交出的是 `ws-protocol-error` 并随后收摊。
/// - **心跳被动为主**：缺省**不发**主动 ping（对端的心跳与业务帧已经足以说明它活着，多发一族帧是给别人
///   的带宽与日志负责）。`heartbeat_interval` 有值才由引擎定时器驱动发 ping；`pong_timeout` 管的是「我发的
///   ping 什么时候必须等到 pong」，到点即 `ws-closed`。收到 ping **恒自动回同载荷 pong**，不给宿主否决权：
///   不回 ping 是协议违例，把这种违例交给 UI 层决定等于让一次界面卡顿掐断一条连接。
/// - **`SinkFlow::Stop` 只关派发，不关会话**：与 `SseSink` 的 `Stop`（整次订阅就此收摊）刻意不同——SSE 是
///   「一条没有结束的流」，宿主收摊就是这次交付结束；WebSocket 是一条双向会话，宿主不想再听消息不代表
///   不该再发。要停会话用 `close()` 或销毁句柄。派发关掉之后控制帧照报、协议判定照走（宿主胃口不改变
///   对端是否违例）。
/// - **交付的载荷视图只在回调期间有效**：`on_message` 的 `payload` 指向解析器的内部缓冲，回调返回即失效，
///   要留用请自行拷贝（与 `DataSink::on_data`、`SseEvent` 同一句纪律）。文本与二进制由 `WsMessageKind`
///   区分，文本的 UTF-8 已在解析器上于**整条消息**判过，宿主不必再判。
///
/// 握手腿上有三件在 HTTP 那侧成立、在这里恒不成立的事，写清免得宿主按 `send()` 的直觉去配：
/// 重定向永不自动跟随（一条已经升级的连接没法「换个地址再升一次」，3xx 一律判 `ws-handshake-rejected`）；
/// 请求体恒不写出（`GET` 无体是协议文法）；`Content-Encoding` 那侧的透明解压在握手腿上不存在（101 没有体）。
///
/// `connect_websocket()` 的取消令牌**管到握手结算为止**，此后不再驱动会话：令牌说的是「这次交换别做了」，
/// 而握手成功之后这条连接已经交付给宿主，它的终止由 `close()` 与句柄析构表达。让一枚宿主可能早已丢弃的令牌
/// 去掐一条宿主正在用的会话，等于给同一份资源两个互不知情的所有者。
///
/// 本腿无流式能力（`Engine::capabilities().streaming` 为假，如 wasm）时 `connect_websocket()` 起手即落
/// `net-platform-unavailable`，绝不退化成别的形态：WebSocket 需要的是**一条可以被持续读写的裸连接**，
/// 而浏览器 `fetch` 那侧连可读的 socket 都不给（要接浏览器原生 `WebSocket` 得另立一套引擎，属另一件事）。
///
/// @code
/// struct Printer final : aurora::net::WebSocketSink {
///     auto on_message(aurora::net::WsMessageKind kind, std::string_view payload)
///         -> aurora::net::http::SinkFlow {
///         AURORA_LOG_RAW((kind == aurora::net::WsMessageKind::Text ? "text " : "bin ") << payload);
///         return aurora::net::http::SinkFlow::Continue;
///     }
/// };
/// Printer sink;
/// auto task = client.connect_websocket(aurora::net::http::Request::get("wss://example.com/chat"), sink);
/// // 宿主帧内泵到 task.is_done()，co_await 得 Result<WebSocket>；此后句柄自持读环，析构即收摊。
/// @endcode
/// @note Thread: `WebSocketSink` 的四个入口恒在主线程同步调用（续体经主线程投递器恢复）；`WebSocket` 的
/// 六个成员（`is_open` / `subprotocol` / `queued_bytes` / `send` / `send_binary` / `close`）同样只从主线程调用——
/// 会话状态不设互斥，「线程安全」在这里是给不出跨线程写序的
/// @note Side-effects: none（本头只定义类型；握手与收发在 `Client::connect_websocket()`）

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/http_stream.h"

namespace aurora::net {

namespace detail {
class WsSession;  ///< 会话状态：读环、发送队列、心跳定时器与 sink 的持有者（实现侧完整）
}  // namespace detail

namespace http {
class Client;  ///< 发起侧客户端（`connect_websocket()` 的所在，完整声明见 `net/http_client.h`）
}  // namespace http

/// @brief 一条消息的线上类型。
///
/// 只有两型：`Close` / `Ping` / `Pong` 属控制帧，由 sink 的另外三个入口各自报出，不混进「消息」这一族
/// （把它们当消息交给宿主，等于让每个宿主自己写一遍控制帧的分岔）。
/// @note Thread: 值类型
/// @note Side-effects: none
enum class WsMessageKind : std::uint8_t {
    Text,  ///< 文本消息：已按整条消息判过 UTF-8（`RFC 6455 §8.1` 的 D7）
    Binary  ///< 二进制消息：字节原样交付，本库不做任何编码解释
};

/// @brief 一次会话收尾的事实（`WebSocketSink::on_closed` 的形参）。
///
/// 两种结局共用一枚类型，是因为「对端先说了 close」与「本层把连接掐了」在宿主要做的事上完全一致：落状态、
/// 清计时器、决定要不要再连。区别只在分量里：前者带线上状态码与原因文本，后者的 `failure` 携本层给出的原因
/// （`ws-protocol-error` / `ws-closed` / 传输类失败），且线上码可能为空。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct WsCloseInfo {
    std::optional<std::uint16_t> code;  ///< 抵达的线上状态码；对端没带码、或压根没走到 close 交换时为空
    std::string reason;  ///< 随码抵达的原因文本（可能为空）
    bool by_peer{false};  ///< 这条结局是否由对端的 close 帧开启（假 = 本端发起或本地判定掐断）
    Error failure{};  ///< 非干净收尾的原因；`code_enum == ErrorCode::GeneralUnknown` 且文本为空即「正常关闭」
};

/// @brief 会话侧的宿主对象：逐条收下消息并回报是否继续听，外加三种控制帧与收尾的出口。
///
/// 一次会话对应一个实例（由调用方持有，生命周期必须覆盖到会话收尾——即句柄析构或 `close()` 之后）。
/// 入口的调用次序是：`on_message` / `on_ping` / `on_pong`（任意交错，零次或多次）→ `on_closed`（**恰一次**，
/// 它是这条会话的唯一终态出口）。`on_closed` 之后本会话不再调用该 sink 的任何入口。
/// @note Thread: 四个入口恒在主线程同步调用，且由同一处续体串行驱动（不并发、不重入）
/// @note Side-effects: 由实现方决定（写信号、更新界面等）；本库只约定调用时机，不干预实现内容
/// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 多态抽象基类，按值拷贝 / 移动无意义
class WebSocketSink {
  public:
    /// @brief 虚析构（default）：保证经基类指针销毁派生的会话侧宿主，本类自身不持有资源。
    virtual ~WebSocketSink() = default;

    /// @brief 一条消息成形时调用（分片消息在**最后一片到齐**之后才算成形，中间片不派发生物）。
    /// @param kind 消息类型（取自分片消息的首片）。
    /// @param payload 载荷字节（视图只在本次调用内有效）。
    /// @return 是否继续交付消息。回 `Stop` 只关掉派发，会话本身照跑（见本头文件头第八条）。
    virtual auto on_message(WsMessageKind kind, std::string_view payload) -> http::SinkFlow = 0;

    /// @brief 收到 ping 时调用（回同载荷 pong 由本库自动做完，这里是纯观测口）。
    /// @param payload ping 的载荷（视图只在本次调用内有效）。
    virtual auto on_ping(std::string_view payload) -> void { (void)payload; }

    /// @brief 收到 pong 时调用（`pong_timeout` 的到点判定据此复位）。
    /// @param payload pong 的载荷（视图只在本次调用内有效）。
    virtual auto on_pong(std::string_view payload) -> void { (void)payload; }

    /// @brief 会话收尾时调用，恰一次。
    /// @param close 收尾事实（状态码 / 原因 / 由谁开启 / 本地判定的失败）；只在本次调用内有效，留用请自行拷贝分量。
    virtual auto on_closed(const WsCloseInfo &close) -> void { (void)close; }
};

/// @brief 一条会话的口径（上限、子协议、心跳）。
///
/// 三枚上限与提案 §18 的同源数字一致，取值依据在 `net/detail/ws_frame.h` 的文件头。心跳那两枚缺省**都不设**：
/// 被动为主是这里的默认立场——发 ping 是「我凭什么认为你不活着」这件事的发起方，而宿主通常不需要发起它。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct WebSocketOptions {
    std::vector<std::string> subprotocols;  ///< 依次序提出的子协议；空 = 不提出
    bool require_subprotocol{false};  ///< 必选：服务端没回其中一枚即判握手被拒
    std::size_t max_frame_bytes{std::size_t{1024U} * 1024U};  ///< 单帧上限（读写两侧同一枚旋钮）
    std::size_t max_message_bytes{std::size_t{4U} * 1024U * 1024U};  ///< 整条消息上限（重组与发出共用）
    std::size_t max_queued_bytes{std::size_t{1024U} * 1024U};  ///< 发送队列的字节上限
    std::size_t max_queued_frames{256U};  ///< 发送队列的条数上限
    std::optional<std::chrono::milliseconds> heartbeat_interval;  ///< 主动 ping 的周期；空 = 不发
    std::optional<std::chrono::milliseconds> pong_timeout;  ///< 发出的 ping 等多久算对方失联；空 = 不等
};

/// @brief 一次发送的落地回报（`WebSocket::send` 的第二拍）。
///
/// 交回的是「这几枚字节写出后本层的结局」：成功 = 已交给传输；失败 = 写出途中的传输失败或会话已收尾。
/// 恒在主线程调用。为空 = 不关心落地（仍要检查 `send()` 的第一拍返回值，那才是「这次发送被不被接受」）。
using WsSent = std::function<void(Result<void>)>;

/// @brief 一条升级后的 WebSocket 会话句柄：move-only，自持读环。
///
/// 由 `Client::connect_websocket()` 交出，之后它是这条会话的唯一持有者：成员函数只做「入队 / 打旗 / 读数」，
/// 真正的收发在会话内部的读环与写环里推进。**默认构造的句柄是空的**（不持会话），对空句柄调用成员函数一律
/// 交出 `ws-closed` 而不是崩溃——那是「连接还没建成」与「连接已经没了」在同一段代码里的共同形态。
/// @note Thread: 四个成员恒从主线程调用（会话状态不设互斥，理由见本头文件头）
/// @note Side-effects: `send` / `close` 改变本会话的发送队列与收尾态；不触碰其它对象
class WebSocket {
  public:
    /// @brief 空句柄（不持会话）。
    WebSocket() = default;

    WebSocket(const WebSocket &other) = delete;
    auto operator=(const WebSocket &other) -> WebSocket & = delete;

    /// @brief 移动构造：接管那份会话状态，源句柄落回空句柄。
    ///
    /// 读环不受影响——它持有的是同一份状态对象，不是这个句柄。
    /// @param other 被接管的句柄（移动后为空）。
    WebSocket(WebSocket &&other) noexcept;

    /// @brief 移动赋值：先按析构语义收摊自己那份，再接管对方的。
    /// @param other 被接管的句柄（移动后为空）。
    /// @return 自身引用。
    auto operator=(WebSocket &&other) noexcept -> WebSocket &;

    /// @brief 析构：尽力投递 `close(1000)` 后掐断（不阻塞、不等回帧）。
    ~WebSocket();

    /// @brief 这条会话是否还没终结。
    /// @return 会话尚未收尾为真——`close()` 之后到收尾事实落地之前**仍算真**（那时对端的回帧还没读到，
    ///         队列里的字节也还在写）。队列未排空不影响本判据：「活着」说的是协议状态，不是字节数。
    [[nodiscard]] auto is_open() const -> bool;

    /// @brief 协商到的子协议。
    /// @return 服务端回的那一枚；无子协议时为空视图。
    [[nodiscard]] auto subprotocol() const -> std::string_view;

    /// @brief 发一条文本消息（必要时由库自动分片）。
    ///
    /// 载荷的 UTF-8 合法性由本库在**写出之前**判：写出一枚对端会判成违例的文本帧，等于把同一份坏字节
    /// 交给一条已经建立的会话去踩刹车。
    /// @param text 消息文本（UTF-8）。
    /// @param on_sent 落地回报；可空。
    /// @return 本次发送是否被接受：成功 = 已入队；`ws-send-queue-full` = 队列满（会话不受影响）；
    ///         `net-buffer-limit` = 超过整消息上限；`ws-protocol-error` = 文本不是合法 UTF-8；
    ///         `ws-closed` = 会话已收尾或是空句柄。
    [[nodiscard]] auto send(std::string_view text, WsSent on_sent = {}) const -> Result<void>;

    /// @brief 发一条二进制消息（必要时由库自动分片）。
    ///
    /// 名字与 `send()` **刻意不同**：两枚入口若同名，`send("文本")` 选中的一定是这一枚——数组到
    /// `std::span<const char>` 的绑定是同一转换，比到 `std::string_view` 的用户定义转换更优，于是最自然的那句
    /// 写法会静默发出一条**带尾零字节的二进制帧**。本库的调用方大量由 AI 生成，这类静默分岔是它们最难自证的
    /// 缺陷，故在此把两条路拆成两个动词，让写错的人拿到一次编译错误而不是一帧线上怪字节。
    /// @param binary 消息字节。
    /// @param on_sent 落地回报；可空。
    /// @return 同 `send(std::string_view, WsSent)` 的结局（二进制不判 UTF-8，故无 `ws-protocol-error` 那一型）。
    [[nodiscard]] auto send_binary(std::span<const char> binary, WsSent on_sent = {}) const -> Result<void>;

    /// @brief 发起关闭：投递一枚 close 帧，之后本会话不再接受新的发送。
    ///
    /// 与析构的区别只有一件：**这里是显式的一次收摊**，本层会把对端回过来的 close 计入 `on_closed`，并把
    /// 状态码原样带着；析构那条路只保证「尽力投递」，落地与否都不再由调用方观察。
    /// @param code 线上状态码；必须在 `RFC 6455 §7.4` 的上线可取值内（缺省 1000 = 正常关闭）。
    /// @param reason 原因文本（UTF-8，与码合起来不得越过 125 字节的控制帧上限）。
    /// @return 关闭帧是否被接受：`ws-closed` = 已收尾；`ws-protocol-error` = 码或原因不合形态。
    [[nodiscard]] auto close(std::uint16_t code = 1000U, std::string_view reason = {}) const -> Result<void>;

    /// @brief 发送队列里还压着多少待发字节（含尚未写出的分片）。
    /// @return 字节数；空句柄为 0。
    [[nodiscard]] auto queued_bytes() const -> std::size_t;

  private:
    /// @param session 会话状态。
    explicit WebSocket(std::shared_ptr<detail::WsSession> session) : session_(std::move(session)) {}

    // 唯一能造出持有会话的句柄的手：`Client::connect_websocket()` 装配完会话后交柄。
    //
    // 私有构造 + 单点友元而不是公开工厂，为的是「句柄与一次成功握手之间没有第二种对应关系」——公开的
    // 装配口会让任何人递进一枚 `shared_ptr`，而那一处类型在公共头里连完整声明都没有。
    friend class http::Client;

    std::shared_ptr<detail::WsSession> session_;  ///< 会话状态；空指针 = 空句柄
};

}  // namespace aurora::net

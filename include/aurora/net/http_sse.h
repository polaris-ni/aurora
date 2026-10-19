#pragma once

/// @brief `text/event-stream` 的订阅面：逐条事件的交付抽象、`SseOutcome` 完成事实。
///
/// 本头只回答一个问题：**「服务端不结束、一条一条往外推的流，怎么接成宿主能用的事件」**。它住在
/// `Client::open_stream()` 之上而不是旁边：事件流的线上形态就是「一条永远读不完的分块应答」，分帧、透明解压、
/// 重定向、三档超时、连接池、Cookie 与策略闸全都已经在那条腿上兑现了；本头加的只有**行与字段的语义**
/// （`net/detail/http_sse.h` 的解析器）与**断线重连的会话状态**（续传标识与重连窗口）。
///
/// 四条形态约束（规范性，落码与文档回写以此为准）：
/// - **接收侧是抽象基类，不是回调**：与 `DataSink` 同构的理由在这里同样成立——`on_open` / `on_event` /
///   `on_comment` / `on_end` 要共享的是同一个宿主对象的状态（已累计的事件、要不要中途收摊），拆成四枚
///   `std::function` 就得让调用方自己把那份状态缝四次。生命周期由宿主对象表达：协程帧按引用持有 Sink，
///   其有效期必须覆盖到协程收尾。
/// - **一条流的边界是「一条交换」，一次 `open_sse` 的边界是「宿主收摊或被取消」**：重连之后是**另一条**
///   HTTP 交换（另一条连接、另一次 `on_open`），但对宿主来说始终是同一次调用——续传标识由库带着，
///   不用调用方接力。这是 EventSource 的既有语义，也是本入口与 `open_stream` 唯一的存在理由。
/// - **`SinkFlow::Stop` 是正常结局而不是失败**：宿主主动收摊（拿到想要的、用户关了页面）时本次订阅成功返回，
///   停读事实由 `SseOutcome::stopped_by_sink` 表达。失败只用于「本库做不到」或对端把流说坏了。
/// - **事件视图只在回调期间有效**：`SseEvent` 的三个视图指向解析器的内部缓冲，回调返回即失效；要留用请
///   自行拷贝（与 `Frame::bytes`、`DataSink::on_data` 同一句纪律）。
///
/// 三型行终止（CR / CRLF / LF）、BOM 剥离、多行 `data` 以 LF 连接、未知字段忽略、注释行（心跳）单独出口、
/// 未以空行收尾的事件随连接收摊丢弃——判据全文在 `net/detail/http_sse.h` 的文件头。
///
/// @code
/// struct Printer final : aurora::net::http::SseSink {
///     auto on_event(const aurora::net::http::SseEvent &event) -> aurora::net::http::SinkFlow {
///         AURORA_LOG_RAW(event.type << ' ' << event.data);
///         return keep_listening ? aurora::net::http::SinkFlow::Continue
///                               : aurora::net::http::SinkFlow::Stop;
///     }
/// };
/// Printer sink;
/// auto task = client.open_sse(aurora::net::http::Request::get("https://example.com/stream"), sink);
/// // 宿主帧内泵至 task.is_done()，再读 task.result() 得 Result<SseOutcome>
/// @endcode
/// @note Thread: `SseSink` 的四个入口恒在主线程同步调用（续体经主线程投递器恢复）；实现方可在其中触碰
/// 响应式状态，但不得阻塞事件循环
/// @note Side-effects: none（本头只定义类型；收发与重连在 `Client::open_sse()`）

#include <cstdint>
#include <string>
#include <string_view>

#include "aurora/net/http.h"
#include "aurora/net/http_stream.h"

namespace aurora::net::http {

/// @brief 解析器派发出来的一条事件（`SseSink::on_event` 的形参）。
///
/// 三个视图**只在本次回调内有效**：它们指向解析器的内部缓冲，回调返回后缓冲会被重排。事件类型按规范折过：
/// 服务端没写 `event:` 时是 `message`，而不是空视图——DOM 的 `MessageEvent.type` 就是这个值，留空只会让
/// 每个宿主再判一次。`id` 是**派发时刻这条流的续传标识**（与 DOM 的 `lastEventId` 同口径），可能是更早某条
/// 事件带下来的；从没带过才是空。
/// @note Thread: 单线程使用（视图借用自解析器缓冲）
/// @note Side-effects: none
struct SseEvent {
    std::string_view type;  ///< 事件类型；未命名时是 `message`
    std::string_view data;  ///< 消息体：多行 `data` 以 LF 连接，不含收尾空行；恒非空
    std::string_view id;  ///< 本条派发时该流的续传标识；从未有过 `id:` 字段时为空
};

/// @brief 订阅侧的宿主对象：逐条收下事件并回报是否继续。
///
/// 一次 `open_sse` 对应一个实例（由调用方持有，生命周期须覆盖协程收尾）。四个入口的调用次序恒为
/// `on_open`（每条流一次，含重连之后的每一条）→ `on_comment` 与 `on_event`（交错，零次或多次）→
/// `on_end`（至多一次）。**失败结局不调 `on_end`**：流断在半途时收尾该由调用方按 `Result` 分流，而不是
/// 库替它假装「收完了」——这一条与 `DataSink` 同判据。
/// @note Thread: 四个入口恒在主线程同步调用，且由同一条协程串行驱动（不并发、不重入）
/// @note Side-effects: 由实现方决定（写信号、更新界面等）；本库只约定调用时机，不干预实现内容
/// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 多态抽象基类，按值拷贝 / 移动无意义
class SseSink {
  public:
    /// @brief 虚析构（default）：保证经基类指针销毁派生的订阅侧宿主，本类自身不持有资源。
    virtual ~SseSink() = default;

    /// @brief 一条事件流**建立**（应答头部已定且内容类型合格）时调用一次；重连之后会再叫一次。
    ///
    /// 它是「流断过」这件事的唯一出口：宿主在这里可以把状态重置（清计时器、报「重连中」）。形参的 `body()`
    /// 恒为空：事件的字节属于 `on_event`。`head` 是本次调用的只借用，需要留用请自行拷贝分量。
    /// 回 `Stop` 即整条订阅就此收摊，一条事件都不会有。
    /// @param head 本次交换已装配的头部块（状态与字段）。
    /// @return 是否继续交付。
    virtual auto on_open(const Response &head) -> SinkFlow {
        (void)head;  // 缺省实现只关心事件，建流是观测口而非判定口
        return SinkFlow::Continue;
    }

    /// @brief 一条事件成形（空行收尾）时调用（零次或多次）。
    ///
    /// 交出的恒是**服务端说完了的**那一条：未以空行收尾的残留随连接收摊一起丢弃，库不补发半条事件。
    /// @param event 本条事件（视图只在本次调用内有效）。
    /// @return 是否继续交付。
    virtual auto on_event(const SseEvent &event) -> SinkFlow = 0;

    /// @brief 一行注释到达时调用（可缺省不覆写 = 心跳就地丢弃）。
    ///
    /// 服务端用它保活（`:ping` 一类），语义上不属于事件；但它确实是「对端还活着」的硬证据，宿主想区分
    /// 「没消息」与「没连接」时就覆写这一口。文本已经剥掉一个前导空格。
    /// @param text 注释正文（视图只在本次调用内有效）。
    virtual auto on_comment(std::string_view text) -> void { (void)text; }

    /// @brief 本次订阅正常结束（宿主收摊，或不再重连且流已收完）时调用一次。
    ///
    /// 它是「把状态落回 Idle」的时机：完整还是停读由 `SseOutcome` 表达，本入口不重复那个判定。
    virtual auto on_end() -> void {}
};

/// @brief 一次订阅的完成事实（`Client::open_sse()` 的成功出口值）。
///
/// 「成功」在这里有两种：宿主自己收的摊，以及流被对端正常写完且不再需要重连（`reconnect` 关着，或一次都没
/// 断过）。两种都带着实数交回，调用方不需要靠猜判断到底发生了多少次交换。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct SseOutcome {
    Response response;  ///< 最后一条**建立成功**的流的头部块；`body()` 恒为空
    std::uint64_t events{0};  ///< 交给 `SseSink::on_event` 的事件累计（跨全部流）
    std::uint32_t exchanges{0};  ///< 发起过的交换次数（= 流条数）；`1` 即一次都没重连
    std::uint64_t wire_bytes{0};  ///< 跨全部流的线上字节累计（`StreamOutcome::wire_bytes` 口径）
    std::string last_event_id;  ///< 收摊时的续传标识；调用方想手动接着收就把它写进 `Last-Event-ID`
    bool stopped_by_sink{false};  ///< 是否因宿主回 `SinkFlow::Stop` 而收摊（成功结局的一种，不是失败）
    bool reconnect_exhausted{false};  ///< 是否因重连额度用尽而停（最后一次交换本身是干净的收摊，故仍算成功）
};

}  // namespace aurora::net::http

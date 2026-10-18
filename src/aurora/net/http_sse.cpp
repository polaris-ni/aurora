// http_sse.cpp —— 事件流订阅侧：`Client::open_sse` 的交换序列、重连窗口与终止判定。
//
// 本文件只管一层会话：一次订阅是**若干次流式交换的序列**，每次交换各新建一条流与一枚解析器，交换的结局
// 在这里译成三种去向之一——宿主收摊（成功）、这条流断了（带续传标识再接一条）、对端把流说坏了（终止失败、
// 绝不重连）。收发、分帧、透明解压、重定向、三档超时、连接池、Cookie 与策略闸一律由 `Client::open_stream()`
// 原样兑现，本件不重复那条腿；行与字段的语义在 `net/detail/http_sse.h` 的解析器里。全部判据的规范文本在
// `net/http_sse.h` 与 `Client::open_sse()` 的文档里，本文件不复述。
//
// 协程结构：本库的 `CoroTask` 不是 awaitable，「跑完一次流式交换再据其结局决定下一步」因此经
// `detail::await_coro` 桥在同一帧里完成（约束与理由见其文档）——订阅层正是它存在的第一个用户。
#include "aurora/net/http_client.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/log.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/http_sse.h"
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/net/http_sse.h"
#include "aurora/net/http_stream.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {
namespace {

/// @brief 本入口唯一认的媒体类型：其余（含缺失）一律按「对端没在说事件流」终止。
constexpr std::string_view AURORA_SSE_EVENT_STREAM_TYPE{"text/event-stream"};

/// @brief 单字节 ASCII 小写化（媒体类型按 `RFC 9110 §3.1.1.5` 不区分大小写；不经 locale 敏感的转换）。
/// @param text 原文。
/// @return 逐字符小写后的副本。
[[nodiscard]] auto lower_ascii(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

/// @brief 剥掉首尾的可选空白（`RFC 9110 §5.6.3` 的 OWS：空格与水平制表）。
/// @param text 原文视图。
/// @return 剥后的视图。
[[nodiscard]] auto trim_ows(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 应答是否声明了事件流：只取 `;` 之前那段媒体类型比对，参数（`charset` 一类）允许。
///
/// 严判而不放宽是有理由的：`text/plain` 上一条「看着像事件流」的文本会让宿主收到一堆服务端从没打算推的
/// 记录，而那种文本随时可能长成别的东西（日志、进度条、HTML）。字段缺失同样不合格——没说自己是什么的流
/// 不是「大概是事件流」，而是「对端没在说这件事」。
/// @param head 本次交换已装配的头部块。
/// @return 声明合格为 true。
[[nodiscard]] auto declares_event_stream(const Response &head) -> bool {
    const auto value = head.header("Content-Type");
    if (!value.has_value()) {
        return false;
    }
    const std::size_t semicolon = value->find(';');
    const std::string token = value->substr(0, semicolon);
    return lower_ascii(trim_ows(std::string_view{token})) == AURORA_SSE_EVENT_STREAM_TYPE;
}

/// @brief 这次失败是不是**发起侧在自己家门口**判掉的拒绝（重连只会拿到同一个答案）。
///
/// 这里刻意不用 `Error::retryable` 作判据，两者问的不是同一件事：那一枚说的是「同一条请求原样重发有没有
/// 意义」（HTTP 重试子系统的口径，`net-recv-failed` 因此标假——读了一半的字节流确实不能再续），而订阅要问的
/// 是「这条事件流断得值不值得再订一次」。对端砸断连接、半路超时、正文没写完就收摊都是后者最常见的形态，
/// 把它们划成「别再试了」等于把本片的标题（自动重连）作废。故本面的口径**反过来**：只有这几种本地判定终止，
/// 余下一律按额度重连。
///
/// 不在这份清单里、却也绝不重连的还有两类，它们走的是另一条路：闸门当场判定的不符（非 2xx、内容类型不合格、
/// 两枚内置上限）由交换宿主记成终止失败并在成功侧分支里交回；宿主的 `SinkFlow::Stop` 与取消同样在别的分支收摊。
/// @param code 本次交换交出的错误码。
/// @return 属于本地拒绝为 true。
[[nodiscard]] auto refused_before_the_wire(const ErrorCode code) -> bool {
    switch (code) {
        case ErrorCode::HttpRequestInvalid:       // 请求形态本身不对（非 GET）
        case ErrorCode::NetPolicyViolated:        // 安全策略不放行这个目标
        case ErrorCode::NetPlatformUnavailable:   // 本腿没有流式语义
        case ErrorCode::HttpRedirectLimit:        // 重定向上限：再来一条流还是同一条路
        case ErrorCode::NetCancelled:             // 取消（循环头部另有兜底，这里防它从交换里冒出来）
            return true;
        default:
            return false;
    }
}

/// @brief 一次订阅里跨交换存活的事实：解析器的字段效应与累计量都写在这一处。
///
/// 三个引用指向协程帧上的局部量。帧在 `co_await` 挂起期间不销毁（本库协程的基本契约），交换宿主只活一次
/// 交换，故引用在宿主存活期间恒有效。
/// @note Thread: 单线程使用（随一条协程帧）
/// @note Side-effects: none
struct SseSession {
    // 三枚引用刻意不改指针、也不包一层 `std::reference_wrapper`：本结构唯一的用处是把「跨交换存活的那份
    // 事实」交给每条流的解析器出口闭包，写它们就是写协程帧上那三个局部量本身，语义上没有第二种目标可指。
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members) 会话事实即协程帧上局部量的本体
    std::string &last_event_id;  ///< 续传标识：服务端写 `id:` 即覆盖，空值同样覆盖（那是「清空」）
    std::optional<std::chrono::milliseconds> &window;  ///< 服务端写的 `retry:` 窗口；从未写过为空
    std::uint64_t &events;  ///< 已交给宿主的事件累计（跨全部流）
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

/// @brief 一条流的交换侧宿主：把 `DataSink` 的三段交付译成事件、注释与「这条流该怎么收场」。
///
/// 三类出口各一枚旗：`terminal_` 记**终止失败**（非 2xx、内容类型不合格、单行或单条记录越限——重连一万次
/// 拿到的也是同一个答案），`stopped_by_host_` 记宿主收摊，两者都没立而起本条流干净收摊才是「断了」。
/// 每（重）建一条流新建一个实例：行缓冲与未成形的记录属于这一条流，跨连接不残留。
/// @note Thread: 三个入口恒在主线程由同一条协程串行调用
/// @note Side-effects: 经 `SseSink` 交出的宿主回调（写信号、更新界面等）
class ExchangeSink final : public DataSink {
  public:
    /// @brief 以宿主与会话构造，并就地装好本条流的解析器。
    /// @param host 订阅侧宿主（由调用方持有，本壳只借用）。
    /// @param session 跨交换存活的那份会话事实。
    ExchangeSink(SseSink &host, SseSession session)
        : host_(host),
          session_(session),
          parser_(detail::SseParserSinks{
              .on_event = [this](const detail::SseParsedEvent &event) -> bool { return deliver(event); },
              .on_comment = [this](std::string_view text) -> void { host_.on_comment(text); },
              .on_last_id = [this](const std::string &value) -> void { session_.last_event_id = value; },
              .on_retry = [this](std::chrono::milliseconds window) -> void { session_.window = window; },
          }) {}

    ExchangeSink(const ExchangeSink &) = delete;
    auto operator=(const ExchangeSink &) -> ExchangeSink & = delete;
    ExchangeSink(ExchangeSink &&) = delete;
    auto operator=(ExchangeSink &&) -> ExchangeSink & = delete;
    ~ExchangeSink() override = default;

    /// @brief 头部到达：先过状态与内容类型两道闸，再问宿主「这条流要不要收」。
    /// @param head 已装配的头部块。
    /// @return 继续交付为 `Continue`；定局（终止失败或宿主收摊）为 `Stop`。
    auto on_head(const Response &head) -> SinkFlow override {
        if (head.status() < 200 || head.status() > 299) {
            terminal_ = make_error(ErrorCode::HttpProtocolError,
                                   ErrorParams{{"detail",
                                                "an event-stream subscription ended on a non-success status (" +
                                                    std::to_string(head.status()) + ")"}});
            return SinkFlow::Stop;
        }
        if (!declares_event_stream(head)) {
            terminal_ = make_error(ErrorCode::HttpProtocolError,
                                   ErrorParams{{"detail", "the peer did not declare text/event-stream"}});
            return SinkFlow::Stop;
        }
        established_ = true;
        if (host_.on_open(head) == SinkFlow::Stop) {
            stopped_by_host_ = true;
            return SinkFlow::Stop;
        }
        return SinkFlow::Continue;
    }

    /// @brief 一段流字节到达：喂解析器，并把它的终态与宿主的收摊答复译成停读。
    /// @param body 本段明文字节。
    /// @return 继续交付为 `Continue`；定局为 `Stop`。
    auto on_data(std::span<const char> body) -> SinkFlow override {
        const auto pushed = parser_.push(std::string_view{body.data(), body.size()});
        if (!pushed) {
            terminal_ = pushed.error();
            return SinkFlow::Stop;
        }
        if (parser_.host_paused()) {
            stopped_by_host_ = true;
            return SinkFlow::Stop;
        }
        return SinkFlow::Continue;
    }

    /// @brief 本条流读全（或宿主停读）：把没自带终止符的残尾结算掉，未成形的记录由解析器丢弃。
    auto on_end() -> void override {
        if (terminal_.has_value() || stopped_by_host_) {
            return;  // 结局已定：残尾里不会再有能改变判定的事实
        }
        const auto finished = parser_.finish();
        if (!finished) {
            terminal_ = finished.error();
        }
    }

    /// @brief 本条流是否通过了两道闸（内容类型合格且宿主没在 `on_open` 收摊）。
    /// @return 通过为 true；只有通过的流才有头部块记进订阅的完成事实。
    [[nodiscard]] auto established() const -> bool { return established_; }

    /// @brief 宿主是否回了「不再收」。
    /// @return 回过为 true（本次订阅因此以成功结局收摊）。
    [[nodiscard]] auto stopped_by_host() const -> bool { return stopped_by_host_; }

    /// @brief 取走终止失败（若有）：移出而非拷贝，取一次即清空。
    /// @return 终止失败的事实；本条流没说坏时为 `nullopt`。
    [[nodiscard]] auto take_terminal() -> std::optional<Error> { return std::move(terminal_); }

  private:
    /// @brief 解析器派发一条事件：落累计、交宿主，并把宿主的收摊答复回传给解析器。
    /// @param event 成形的那条事件（视图属于解析器缓冲，本次调用内有效）。
    /// @return 宿主继续收为 true。
    auto deliver(const detail::SseParsedEvent &event) -> bool {
        // `id` 取的是**派发时刻**该流的续传标识（DOM 的 `lastEventId` 同口径），可能是更早某条带下来的；
        // 视图指向帧上的 `last_event_id`，本次回调内不会被改写（解析器的字段效应只在下一行才写它）。
        const SseEvent view{.type = std::string_view{event.type},
                            .data = std::string_view{event.data},
                            .id = std::string_view{session_.last_event_id}};
        ++session_.events;
        return host_.on_event(view) == SinkFlow::Continue;
    }

    SseSink &host_;  ///< 订阅侧宿主（借用）
    SseSession session_;  ///< 跨交换存活的那份会话事实
    detail::SseStreamParser parser_;  ///< 本条流的解析器（成员在后：出口闭包借用上面两枚）
    std::optional<Error> terminal_;  ///< 终止失败（非 2xx / 内容类型 / 两枚内置上限）
    bool established_{false};  ///< 本条流是否过了闸
    bool stopped_by_host_{false};  ///< 宿主是否回了 `Stop`
};

}  // namespace

auto Client::sse(SseOptions value) -> Client & {
    sse_options_ = value;
    // 与 `pool()` / `cache()` 不同，这里没有共享状态要同步：重连口径只被 `open_sse` 在起手读一次，
    // 之后随协程帧存活——改旋钮不影响已在途的订阅，那正是「一次订阅的额度」该有的形态。
    return *this;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): Sink 由调用方持有，帧只借用
auto Client::open_sse(Request request, SseSink &sink, Cancellation cancellation) const
    -> CoroTask<Result<SseOutcome>> {
    // 取消先于一切判定，与 `send` / `open_stream` 同一位置：已取消的令牌不问方法、不看本腿能力，也不调宿主任何一口。
    if (cancellation.is_cancelled()) {
        co_return Result<SseOutcome>{
            make_error(ErrorCode::NetCancelled, "the subscription was cancelled before it started")};
    }
    // 方法闸在起手：事件流的语义就是一条 GET 出来的长应答。其余方法既不静默改成 GET（那会让调用方以为
    // 发出去了体），也不照发（那会打出一个本库不认识的协议）。
    if (request.method() != Method::Get) {
        co_return Result<SseOutcome>{make_error(
            ErrorCode::HttpRequestInvalid,
            ErrorParams{{"detail",
                         std::string{"an event-stream subscription must be a GET, the request said "} +
                             std::string{method_text(request.method())}}})};
    }
    // 本腿没有流式语义（wasm 无 ReadableStream）时直接失败，绝不退化成「物化一份应答再慢慢切行」：
    // 那等于把一条不会结束的流等成一次也不会结束的挂死。
    if (!Engine::instance().capabilities().streaming) {
        co_return Result<SseOutcome>{
            make_error(ErrorCode::NetPlatformUnavailable, "open_sse: this leg has no streaming response body")};
    }

    // 口径取「请求覆盖 → 客户端缺省」两级，读定在起手：此后改 `Client::sse()` 不影响在途的订阅。
    const SseOptions options = request.sse().value_or(sse_options_);
    Request origin = std::move(request);
    // 不查也不写缓存：一次订阅要的是**此刻之后**的事件，一份新鲜的历史副本照着交付只会让宿主以为收到了新事件。
    origin.no_cache(true);
    if (!origin.headers().contains("Accept")) {
        // 缺省宣告与 `Accept-Encoding` 同一纪律：调用方自己写过就不动，内容协商是调用方的表达。
        origin.header("Accept", "text/event-stream");
    }

    // 会话事实挂在帧上：跨交换存活的只有续传标识、重连窗口与三枚累计量（一条流的边界另有解析器持有）。
    std::string last_event_id;
    std::optional<std::chrono::milliseconds> retry_window;
    std::uint64_t events = 0;
    std::uint64_t wire_bytes = 0;
    std::uint32_t exchanges = 0;
    std::uint32_t reconnects = 0;
    std::optional<Response> last_head;
    Result<SseOutcome> outcome{
        make_error(ErrorCode::HttpProtocolError, ErrorParams{{"detail", "open_sse produced no outcome"}})};

    // 成功出口的唯一构造点：`last_head` 在两条成功路径上必有值（宿主收摊与干净收摊都在过了闸之后才可能
    // 抵达，见 `ExchangeSink::on_head` 的旗位次序），故这里不留兜底分支去编造一份头部。
    auto summary = [&](bool stopped, bool exhausted) -> Result<SseOutcome> {
        return Result<SseOutcome>{SseOutcome{
            .response = std::move(*last_head),
            .events = events,
            .exchanges = exchanges,
            .wire_bytes = wire_bytes,
            .last_event_id = std::move(last_event_id),
            .stopped_by_sink = stopped,
            .reconnect_exhausted = exhausted,
        }};
    };

    // 订阅环与单次交换**同帧**：`CoroTask` 不是 awaitable，故「把一次流式交换抽成子协程再 co_await」
    // 经 `detail::await_coro` 桥在本帧里完成——订阅层要的正是这条序列。
    for (;;) {
        // 取消落在两次交换之间（含退避等待期间）：这里就是唯一的收摊口。已交付给宿主的事件不回滚，
        // 库也不替它假装「收完了」——失败结局不调 `on_end`，口径与 `DataSink` 同源。
        if (cancellation.is_cancelled()) {
            outcome = Result<SseOutcome>{make_error(ErrorCode::NetCancelled, "the subscription was cancelled")};
            break;
        }
        Request current = origin;
        if (!last_event_id.empty()) {
            // 只在非空时带：从未有过 `id:` 就没必要发一枚内容为空的头；服务端用空值 id 表达「清空」，
            // 其线上形态同样是不带。
            current.set_header("Last-Event-ID", last_event_id);
        }
        ExchangeSink exchange{sink, SseSession{.last_event_id = last_event_id,
                                               .window = retry_window,
                                               .events = events}};
        auto one = co_await detail::await_coro(open_stream(current, exchange, cancellation));
        ++exchanges;
        if (one) {
            wire_bytes += one.value().wire_bytes;
            if (exchange.established()) {
                last_head = std::move(one.value().response);
            }
        }

        if (!one) {
            // 这条流以失败收场。取消与本地拒绝（策略闸、请求形态、本腿能力、重定向上限）不重连——
            // 它们不是「流断了」，再来一次拿到的还是同一个答案。
            if (cancellation.is_cancelled()) {
                outcome = Result<SseOutcome>{
                    make_error(ErrorCode::NetCancelled, "the subscription was cancelled while a stream was in flight")};
                break;
            }
            const bool stream_broke = !refused_before_the_wire(one.error().code_enum);
            const bool budget_open =
                options.reconnect && (options.max_reconnects == 0U || reconnects < options.max_reconnects);
            if (!stream_broke || !budget_open) {
                // 额度用尽不新造码：交回的还是最后一次交换那枚真实失败，只是把「本库不再试了」补在句尾。
                auto failure = one.error();
                if (stream_broke && options.reconnect) {
                    failure.message += ", and the event-stream reconnect budget of " +
                                       std::to_string(options.max_reconnects) + " was exhausted";
                }
                outcome = Result<SseOutcome>{std::move(failure)};
                break;
            }
            ++reconnects;
        } else {
            // 这条流交出了成功结局：要么宿主收了摊，要么闸说了「对端没在推事件流」，要么它被对端写完关掉。
            if (exchange.stopped_by_host()) {
                sink.on_end();
                outcome = summary(true, false);
                break;
            }
            if (auto gate = exchange.take_terminal(); gate.has_value()) {
                outcome = Result<SseOutcome>{std::move(*gate)};
                break;
            }
            const bool budget_open =
                options.reconnect && (options.max_reconnects == 0U || reconnects < options.max_reconnects);
            if (!budget_open) {
                // 关着重连是「宿主只要这一条流」，用尽额度是「本库试够了」：两种都算成功，后者另有旗位。
                sink.on_end();
                outcome = summary(false, options.reconnect);
                break;
            }
            ++reconnects;
        }
        // 窗口只有一个来源：服务端写过的 `retry:` 原样采用（不封顶、不退避、不加抖动），从没写过才用本地初值。
        const std::chrono::milliseconds window = retry_window.value_or(options.retry_default);
        AURORA_LOG_INFO("net", "event stream reconnect: stream ", exchanges, " ended, waiting ", window.count(),
                        "ms before reconnecting");
        co_await detail::await_sleep(window, cancellation);
    }
    co_return outcome;
}

}  // namespace aurora::net::http

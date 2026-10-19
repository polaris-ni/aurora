/// @brief 声明式交付面的实现：协程出口 → 作业状态 → 响应式资源的桥（§12）。
/// @file src/aurora/net/http_binding.cpp
///
/// 本文件只做一件事：**把 `Client` 的协程出口接到 `Task` / `StreamResource` 上**，并把取消接回套接字。
/// 三条实现约束决定了下面的形态：
/// - 协程帧持有 `this`（`send` / `open_stream` 是成员协程），所以会话必须是堆上的一份拷贝，且由**帧的收尾口**
///   持有——挂在调用栈上或挂在返回的 `Task` 上都会早于帧收尾而失效。
/// - `CoroTask<T>` 不是 awaitable（promise 永不首挂也不终挂），桥只能以观察者形态存在（`on_done()`），
///   不能把一次交换 `co_await` 进另一条协程里。
/// - 作业状态的落定形态与 `async()` 完全一致（写结果 → `take_for_delivery` 单点裁决 → 回投主线程），
///   于是取消 / 超时 / 去抖这些既有语义在 `Resource` 里照常成立，本文件不另造一套投递纪律。

#include "aurora/net/http_binding.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_sse.h"
#include "aurora/net/http_stream.h"
#include "aurora/net/websocket.h"
#include "aurora/state/async.h"
#include "aurora/state/coroutine.h"
#include "aurora/state/resource.h"

namespace aurora::net::http {
namespace {

/// @brief 把流式交换的三段交付转成 `StreamResource<StreamChunk>` 提交的宿主壳。
///
/// 状态码在 `on_head` 记下、随每一条一起交出（`Replace` 策略下读数时只剩最后一条，状态码不住在条目里就问不
/// 出来）；收尾标记**不在 `on_end` 出**，而在协程结束时统一出——那里才拿得到最终一跳的事实，且失败结局不该
/// 带着一枚「收尾了」的标记。本壳由交换的收尾口持有，故它活得比 `open_stream` 的帧短不了。
class StreamBinder final : public DataSink {
  public:
    /// @brief 以要提交的流资源构造（`StreamResource` 是共享内核的句柄，拷贝即可写）。
    /// @param resource 提交目标。
    explicit StreamBinder(aurora::StreamResource<StreamChunk> resource) : resource_(std::move(resource)) {}

    /// @brief 记下最终一跳的状态码并继续收。
    /// @param head 头部块（`body()` 恒空）。
    /// @return 恒 `Continue`：本壳没有「够了」的判定，收摊归宿主或取消。
    auto on_head(const Response &head) -> SinkFlow override {
        status_ = head.status();
        return SinkFlow::Continue;
    }

    /// @brief 把一段明文折成一条 `StreamChunk` 提交。
    /// @param body 本段明文（只在本次调用内有效，故必须拷贝进条目）。
    /// @return 恒 `Continue`。
    auto on_data(std::span<const char> body) -> SinkFlow override {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(body.size());
        for (const char datum : body) {
            // char→uint8_t 显式取模转换：段字节是二进制载荷，实现定义的符号性不该进到交付面。
            bytes.push_back(static_cast<std::uint8_t>(datum));
        }
        // `push` 的第二参数按 `StreamResource` 的口径记的是「该条的字节数」；本壳交得出的是明文段长，
        // 线上口径（透明解压前）留在 `StreamOutcome::wire_bytes` 里，不进条目。
        resource_.push(StreamChunk{.status = status_, .offset = offset_, .bytes = std::move(bytes), .last = false},
                       body.size());
        offset_ += body.size();
        return SinkFlow::Continue;
    }

    /// @brief 记一次交付收摊（收尾标记由协程结束时统一出，本入口不提交）。
    auto on_end() -> void override {}

  private:
    aurora::StreamResource<StreamChunk> resource_;  ///< 提交目标（共享内核句柄）
    std::uint16_t status_{0};  ///< 最终一跳的状态码
    std::uint64_t offset_{0};  ///< 已交付的明文段累计
};

/// @brief 把事件流订阅的交付转成 `StreamResource<SseRecord>` 提交的宿主壳。
///
/// 状态码在 `on_open` 记下、随每条事件一起交出——重连之后是另一条流、另一个状态码，而条目序列仍连着一条不差。
/// 三枚文本一律物化进条目：`SseEvent` 的视图属于解析器缓冲，回调返回即失效，而 `StreamResource` 的条目要活到
/// 宿主下一次读数。收尾标记不在 `on_end` 出，而在协程结束时统一出——那里才拿得到跨全部流的累计事实。
class SseBinder final : public SseSink {
  public:
    /// @brief 以要提交的流资源构造。
    /// @param resource 提交目标。
    explicit SseBinder(aurora::StreamResource<SseRecord> resource) : resource_(std::move(resource)) {}

    /// @brief 记下本条流的状态码并继续收（重连后每条流各叫一次）。
    /// @param head 头部块（`body()` 恒空）。
    /// @return 恒 `Continue`：本壳没有「够了」的判定，收摊归取消令牌。
    auto on_open(const Response &head) -> SinkFlow override {
        status_ = head.status();
        return SinkFlow::Continue;
    }

    /// @brief 一条事件成形：物化成一条 `SseRecord` 提交。
    /// @param event 本条事件（视图只在本次调用内有效，故必须拷贝）。
    /// @return 恒 `Continue`。
    auto on_event(const SseEvent &event) -> SinkFlow override {
        resource_.push(SseRecord{.status = status_,
                                 .index = index_,
                                 .event = std::string{event.type},
                                 .data = std::string{event.data},
                                 .id = std::string{event.id},
                                 .last = false});
        ++index_;
        return SinkFlow::Continue;
    }

  private:
    aurora::StreamResource<SseRecord> resource_;  ///< 提交目标（共享内核句柄）
    std::uint16_t status_{0};  ///< 当前那条流的状态码
    std::uint64_t index_{0};  ///< 已提交的事件条数（= 下一条的序号）
};

/// @brief 把一条升级后的会话接成 `StreamResource<WsRecord>` 提交的宿主壳。
///
/// 与上面两枚壳的差别只有一处，而那一处决定了本壳全部的所有权形态：那两枚由发起侧的收尾口（`on_done`）持有，
/// 因为它们的订阅在**同一条协程帧**里跑完——收尾口就是最后一次交付。本壳对应的会话却在握手结算那一刻**才刚
/// 开始**：帧收尾时交回句柄，此后读环还要跑很久。收尾口那份持有随那次调用一起销毁，壳就成了一根没人认领的
/// 悬空引用（会话借用的 `WebSocketSink&` 指着一块已释放的内存）。于是本壳自己保住自己（`self_`），并在会话
/// 对该 sink 的最后一次调用里松手——`on_closed` 恒恰一次且是唯一的终态出口（`net/websocket.h` 第八条），从
/// 那里放手既保证会话此后摸不到已销毁的 sink，也保证壳不会活成一根泄漏。
///
/// 第二处差别是取消的去向。`connect_websocket()` 的令牌管到握手结算为止，此后不再驱动会话——那条纪律守的是
/// 「同一份资源不留两个互不知情的所有者」。本壳要的形状是「宿主停手 = 这条订阅结束」，于是**由壳自己**把那枚
/// 令牌接成一次显式的 `close()`：接的是交付面这一侧的钩子，会话的所有者仍只有会话自己，那条纪律一字未动。
/// 钩子取 `weak_ptr`：取 `shared_ptr` 就成了「令牌的状态持有壳、壳持有令牌」的环，取消与不取消都松不开。
///
/// 心跳帧不进条目流：收 ping 的自动 pong 是协议义务、库里已经做完，把探活字节混进消息流会让 `Replace` 策略
/// 下的读数变成心跳而不是内容（要观察探活另有出口，就是自己实现那个抽象类）。
/// @note Thread: 与 `WebSocketSink` 同口径——四个入口恒在主线程同步调用；`submit()` 亦由主线程的收尾口触发
/// @note Side-effects: 向 `StreamResource` 提交条目、发起一次 close 交换
class WsBinder final : public WebSocketSink, public std::enable_shared_from_this<WsBinder> {
  public:
    /// @brief 以提交目标与那枚要接成收摊的令牌构造。
    /// @param resource 提交目标（共享内核句柄，拷贝即可写）。
    /// @param cancellation 本次订阅的取消令牌（副本与宿主那枚同指一份共享状态）。
    WsBinder(aurora::StreamResource<WsRecord> resource, Cancellation cancellation)
        : resource_(std::move(resource)), cancellation_(std::move(cancellation)) {}

    WsBinder(const WsBinder &) = delete;
    auto operator=(const WsBinder &) -> WsBinder & = delete;
    WsBinder(WsBinder &&) = delete;
    auto operator=(WsBinder &&) -> WsBinder & = delete;

    /// @brief 虚析构（default）：销毁只发生在 `release()` 松开最后一份自持之后，本壳自身不另持资源。
    ~WsBinder() override = default;

    /// @brief 握手成功：接管那条会话、开始自持，并把令牌的取消接成本端收摊。
    ///
    /// 登记钩子时若取消事实已经成立，那条契约要求在当前线程立即触发一次——正好是「宿主在握手期间就按了停手」
    /// 的现场：句柄此刻已在手上，`close_from_host()` 当场把这条会话关掉，不留一条没人收摊的连接。
    /// @param session 握手交出的会话句柄（单所有权，随本壳存活到收尾）。
    auto submit(WebSocket session) -> void {
        session_ = std::move(session);
        self_ = shared_from_this();  // 自持：发起侧那份收尾口在本次调用返回后即销毁，此后只有本壳认得自己
        cancellation_.on_cancel([weak = weak_from_this()]() -> void {
            if (const auto held = weak.lock()) {
                held->close_from_host();
            }
        });
    }

    /// @brief 握手未成：把失败交进资源，一条记录都不出（那次会话从未存在过）。
    ///
    /// 本入口**不**自持：发起侧的收尾口还握着这份，随那次调用一起放手即可——没有会话要活过它，也就没有
    /// 「最后一次 sink 调用」可等。
    /// @param failure 握手失败的成因（`ws-*` / `http-*` / `net-*`）。
    auto refuse(const Error &failure) -> void { resource_.fail(failure); }

  private:
    /// @brief 一条消息成形：物化成一条 `WsRecord` 提交（载荷的视图只在本次调用内有效，故必须拷贝）。
    /// @param kind 消息的线上类型。
    /// @param payload 消息载荷。
    /// @return 恒 `Continue`：本壳没有「够了」的判定，收摊归那枚令牌与对端。
    auto on_message(WsMessageKind kind, std::string_view payload) -> SinkFlow override {
        resource_.push(WsRecord{.index = index_, .kind = kind, .payload = std::string{payload}});
        ++index_;
        return SinkFlow::Continue;
    }

    /// @brief 会话收尾：干净收摊出一条 `last` 标记，非干净收摊落 `fail()`，随后松开这份持有。
    /// @param close 收尾事实（线上状态码 / 原因 / 由谁开启 / 本地判定的失败），只借用，分量随记录各自拷走。
    auto on_closed(const WsCloseInfo &close) -> void override {
        // 「正常关闭」的判据就是公共头给 `WsCloseInfo::failure` 那一句：既无码又无文本。本壳不另立一条分岔，
        // 否则同一件事在库里有两个说法。
        const bool clean =
            close.failure.code_enum == aurora::ErrorCode::GeneralUnknown && close.failure.message.empty();
        if (clean) {
            resource_.push(WsRecord{.index = index_,
                                    .kind = WsMessageKind::Text,
                                    .payload = {},
                                    .close_code = close.code,
                                    .close_reason = close.reason,
                                    .by_peer = close.by_peer,
                                    .last = true});
        } else {
            resource_.fail(close.failure);  // 已提交的消息保留，未成形的那一条由会话丢弃
        }
        release();  // 本行是最后一次成员触碰：`on_closed` 之后没有下一个入口，此后本对象不再存在
    }

    /// @brief 宿主取消那枚令牌时收摊：本端发起 `close(1000)`，终态仍由 `on_closed` 落进资源。
    auto close_from_host() -> void {
        if (!session_.has_value()) {
            return;
        }
        // 关不掉（已在关闭 / 已定局）不另报事实：那次收摊的结局同样经 `on_closed` 交出，这里多说一遍只会
        // 让宿主看见两个终态。理由文本走 ASCII——它会上线，也进对端的日志。
        (void)session_->close(1000U, "the host stopped watching this subscription");
    }

    /// @brief 松开这两份持有：先句柄后自持，次序不可颠倒。
    ///
    /// 先 `reset()` 句柄即 `detach()`：会话把 `detached_` 立起来，此后读环与协议判定照走但**不再触碰 sink**，
    /// 于是紧随其后的那次自持松手（销毁本对象）不会被任何在途调用撞上。反过来先松自持，本对象就在还握着
    /// `this` 的调用栈里被销毁了。
    auto release() -> void {
        session_.reset();
        self_.reset();
    }

    aurora::StreamResource<WsRecord> resource_;  ///< 提交目标（共享内核句柄）
    Cancellation cancellation_;  ///< 宿主那枚令牌的副本（钩子据此接回本端收摊）
    std::optional<WebSocket> session_;  ///< 握手交出的会话句柄（本壳是它唯一的持有者）
    std::shared_ptr<WsBinder> self_;  ///< 自持：会话的存续期长于发起它的那条协程帧
    std::uint64_t index_{0};  ///< 已提交的消息条数（= 下一条的序号，也是收尾标记那条的序号）
};

}  // namespace

auto detail::exchange_material(const Client &client, Request request, Cancellation cancellation)
    -> aurora::Task<Response> {
    auto session = std::make_shared<Client>(client);  // 帧持有 this：会话上堆，由收尾口持有
    auto state = std::make_shared<aurora::detail::AsyncState<Response>>();
    aurora::Task<Response> job{state};

    auto coro = session->send(std::move(request), cancellation);
    // 收尾口持有会话：钩子在 promise 析构时被**摘出**再调用，随那次调用结束而销毁，故会话恰好在帧收尾那一刻
    // 放手——不留「会话持有共享状态、共享状态持有会话」的自持环（自持环的形态判据见 `utest_coroutine` 的
    // `on_done_releases_its_own_capture_once_it_has_fired`，本面「调用方客户端先亡仍完成交换」一条验的是它的后果）。
    coro.on_done([state, session](const Result<Result<Response>> &outer) mutable -> void {
        if (outer.ok()) {
            detail::settle_job<Response>(state, outer.value());
            return;
        }
        // 帧本身以异常收尾（协程内未捕获抛出）：折成那条错误，不让一次交付悬着。
        detail::settle_job<Response>(state, Result<Response>{outer.error()});
    });
    // 取消相接：`job.cancel()` 丢结果（`delivered` 单点裁决）之外还掐掉在途连接，二者缺一都算不上真取消。
    job.on_cancel([cancellation]() mutable { cancellation.cancel(); });
    return job;
}

auto deliver_stream(const Client &client, Request request, Cancellation cancellation, aurora::StreamOptions options)
    -> aurora::StreamResource<StreamChunk> {
    auto resource = aurora::StreamResource<StreamChunk>::open(options);
    auto session = std::make_shared<Client>(client);
    auto binder = std::make_shared<StreamBinder>(resource);

    // 令牌按值流进发起侧即可：`Cancellation` 是共享状态句柄，移交副本不切断与调用方那枚的相连关系。
    auto coro = session->open_stream(std::move(request), *binder, std::move(cancellation));
    coro.on_done([resource, binder, session](const Result<Result<StreamOutcome>> &outer) mutable -> void {
        if (outer.ok() && outer.value().ok()) {
            const auto &outcome = outer.value().value();
            // 收尾标记带最终状态码与明文累计：偏移在此与交付段累计一致，乱序或多出一段都当场可见。
            resource.push(
                StreamChunk{
                    .status = outcome.response.status(), .offset = outcome.delivered_bytes, .bytes = {}, .last = true},
                0);
            return;
        }
        const aurora::Error &error = outer.ok() ? outer.value().error() : outer.error();
        resource.fail(error);  // 未提交的批次由 `StreamResource` 丢弃，已提交数据保留
    });
    return resource;
}

auto deliver_sse(const Client &client, Request request, Cancellation cancellation, aurora::StreamOptions options)
    -> aurora::StreamResource<SseRecord> {
    auto resource = aurora::StreamResource<SseRecord>::open(options);
    auto session = std::make_shared<Client>(client);
    auto binder = std::make_shared<SseBinder>(resource);

    auto coro = session->open_sse(std::move(request), *binder, std::move(cancellation));
    coro.on_done([resource, binder, session](const Result<Result<SseOutcome>> &outer) mutable -> void {
        if (outer.ok() && outer.value().ok()) {
            const auto &outcome = outer.value().value();
            // 收尾标记带的是**跨全部流**的事实：`index` 取事件总条数（= 下一条本该用的序号），`id` 取收摊时的
            // 续传标识。宿主据此既能判「订阅收完了」，也能在下一次订阅里手动接上。
            resource.push(SseRecord{.status = outcome.response.status(),
                                    .index = outcome.events,
                                    .event = {},
                                    .data = {},
                                    .id = outcome.last_event_id,
                                    .last = true});
            return;
        }
        const aurora::Error &error = outer.ok() ? outer.value().error() : outer.error();
        resource.fail(error);  // 断在半途：已提交的事件保留，未成形的那条由订阅侧丢弃
    });
    return resource;
}

auto deliver_ws(const Client &client, Request request, WebSocketOptions session_options, Cancellation cancellation,
                aurora::StreamOptions options) -> aurora::StreamResource<WsRecord> {
    auto resource = aurora::StreamResource<WsRecord>::open(options);
    auto session = std::make_shared<Client>(client);
    auto binder = std::make_shared<WsBinder>(resource, cancellation);

    // 令牌在这里被交出去两次，而两次都指向**同一份共享状态**：壳上那枚取消钩子与握手那条腿各握一份引用，
    // 宿主一次 `cancel()` 两边都看得见。后一次是本入口对该局部的最后一次使用，故直接搬走而非再拷一份。
    auto coro =
        session->connect_websocket(std::move(request), *binder, std::move(session_options), std::move(cancellation));
    // 收尾口拿到的那份是可变引用（`CoroTask<T>::on_done` 的形参形态），故单所有权的会话句柄能从结果槽里
    // **搬**进壳——按 `const` 接就只有拷贝这一条路，而那枚句柄压根没有拷贝构造。
    coro.on_done([binder, session](Result<Result<WebSocket>> &outer) mutable -> void {
        if (outer.ok() && outer.value().ok()) {
            binder->submit(std::move(outer.value().value()));
            return;
        }
        binder->refuse(outer.ok() ? outer.value().error() : outer.error());
    });
    return resource;
}

auto image_fetcher(Client client) -> std::function<aurora::Task<std::vector<std::uint8_t>>(std::string_view)> {
    auto session = std::make_shared<Client>(std::move(client));  // 一份配置服务全部取图；闭包持有它
    return [session](std::string_view url) -> aurora::Task<std::vector<std::uint8_t>> {
        return deliver<std::vector<std::uint8_t>>(
            *session, Request::get(std::string{url}),
            [](const Response &response) -> Result<std::vector<std::uint8_t>> {
                const std::uint16_t status = response.status();
                if (status < 200 || status >= 300) {
                    // 取图没有「非 2xx 也算成功」的中间态：404 的 HTML 交给解码器只会换来一句「不是图片」，
                    // 原因必须在这里就点名状态码，否则现场无从判断是没取到还是取错了。
                    return Result<std::vector<std::uint8_t>>{make_error(
                        ErrorCode::RenderImageLoadFailed, "image fetch answered status " + std::to_string(status))};
                }
                const auto &body = response.body();
                std::vector<std::uint8_t> bytes;
                bytes.reserve(body.size());
                for (const char datum : body) {
                    bytes.push_back(static_cast<std::uint8_t>(datum));
                }
                return Result<std::vector<std::uint8_t>>{std::move(bytes)};
            });
    };
}

}  // namespace aurora::net::http

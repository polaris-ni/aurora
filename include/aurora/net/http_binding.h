#pragma once

/// @brief HTTP 交换的声明式交付面：把一次交换接成 `Task` / `Resource` / `StreamResource`（§12）。
/// @file net/http_binding.h
///
/// 本头回答一个问题：**「发起侧的协程出口，怎么直接落到 widget 能读的信号上」**。`Client::send` 交出的是
/// `CoroTask<Result<Response>>`——它只能被轮询（`is_done()` / `result()`），而响应式状态要的是「落定那一刻
/// 定点刷新」。中间这段桥接（持有会话、将结局写进作业状态、把取消接回套接字）每次由宿主手写就会各写一份，
/// 且几乎每次都漏掉同一条：取消只丢结果、不掐连接。本头把它收成一处。
///
/// 四条形态约束（规范性，落码与文档回写以此为准）：
/// - **桥在 net 侧，状态层不改**：`state/resource.h` 的既有纪律是「不认识网络」，它只依赖 `Task<T>` 这一抽象。
///   于是接线件住在网络侧、把交换**折算成 `Task<T>`** 交给既有的 `Resource<T>::from(producer)` 口——状态层
///   一行不动，文件读取、进程间调用同样能产出 `Resource`，这条通道没有被网络独占。
/// - **取消是真的取消**：`Resource::reload()` 与宿主卸载走的是 `Task::cancel()`。本头让那次取消经
///   `Task::on_cancel()` 接回 `Cancellation`，于是「不再关心结果」同时是「掐掉在途那条连接」——对端因此看到的
///   不是一条被读完的连接而是一次中断。反过来说，**没有本头时，`Resource` 装 HTTP 只是把结果丢掉，连接照跑完**。
/// - **会话由这次交付自持**：`Client::send` 的协程帧持有 `this`，所以适配器必须把 `Client` 拷一份交给帧的收尾
///   口（`CoroTask::on_done()`）持有，而不能借用调用方的那一份——调用方的 `Client` 早于交换收尾就销毁是常态
///   （临时对象、栈上客户端）。这份拷贝连同 Sink 都只活到帧收尾那一刻，不留自持环（判据见单测）。
/// - **非 2xx 不是失败**：与 `send` 同口径，物化出口把 4xx / 5xx 作为**成功的 `Response`** 交出，状态码归调用方
///   判（`Projection` 里折成错误、或 `Resource` 读数时按 `status()` 分支）；本头不新造「HTTP 状态错误」这件事。
///   只有传输与协议层失败才走 `Result` 的错误侧。
///
/// 流式一侧的条目形态是 `StreamChunk`（状态码 + 偏移 + 该段字节 + 收尾标记），每段一条、追加进
/// `StreamResource`。SSE 一侧另有条目形态 `SseRecord`（一条事件一条记录），因为按记录解码住在
/// `Client::open_sse()` 里、本头只负责把它交出的事件落成条目；WebSocket 的按帧解码仍是后续切片。
///
/// @code
/// aurora::net::http::Client client;
/// auto profile = aurora::Resource<Profile>::from(  // reload() / 卸载即掐掉在途连接
///     [&client, url] { return aurora::net::http::deliver<Profile>(
///         client, aurora::net::http::Request::get(url),
///         [](const auto &response) { return aurora::net::http::Result<Profile>{parse(response)}; }); });
///
/// // 取图：直接交给 ImageView 的 fetcher 口（结构同取图口别名 `aurora::ImageFetcher`，本头不依赖 widget）
/// aurora::set_default_image_fetcher(aurora::net::http::image_fetcher());
/// @endcode
/// @note Thread: 出口与 `Client` 同口径——结局经主线程投递器抵达；未装投递器时在承接作业的线程直调
/// @note Side-effects: `deliver` / `bind_resource` / `deliver_stream` / `deliver_sse` / `image_fetcher` 发起网络 I/O

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_sse.h"
#include "aurora/net/http_stream.h"
#include "aurora/state/async.h"
#include "aurora/state/resource.h"

namespace aurora::net::http {

/// @brief 应答投影：把物化的 `Response` 折成调用方要的值类型（解码、解析、校验都在这里）。
/// @tparam T 投影后的值类型。
/// @note 投影在交付线程上被调用；抛出异常折成 `runtime-async-exception` 错误结局，不逃出作业边界。
template <typename T>
using Projection = std::function<Result<T>(const Response &)>;

/// @brief 流式交付的一条分块：`StreamResource<StreamChunk>` 的条目类型。
///
/// 状态码随**每一条**在场而不是只报一次：`Replace` 策略的流只保留最新一条，若状态码不住在条目里，读数时就
/// 问不出「这批字节来自哪一次应答」。`last` 是收尾标记（体读全或宿主停读后各出一条，失败结局不出而改由
/// `StreamResource::fail()` 表达），字节为空。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct StreamChunk {
    std::uint16_t status{0};  ///< 该段所属应答的状态码（最终一跳）
    std::uint64_t offset{0};  ///< 该段起点：此前已交付的明文字节数
    std::vector<std::uint8_t> bytes;  ///< 该段明文（已按 `Content-Encoding` 还原）
    bool last{false};  ///< 是否为收尾标记条目（`bytes` 此时为空）
};

/// @brief 事件流交付的一条记录：`StreamResource<SseRecord>` 的条目类型。
///
/// 与 `StreamChunk` 同一条纪律——**状态码随每一条在场**：`Replace` 策略的流只留最新一条，状态码不住在条目里
/// 就问不出「这条事件来自哪一次应答」；重连之后是另一条流、另一个状态码，而条目序列仍是一条接一条。
/// `index` 是这次订阅里的序号（从 `0` 起、跨全部流单调递增），乱序或漏条当场可见。三枚文本都是**自有拷贝**：
/// `SseEvent` 的视图只在回调期间有效，落成条目必须物化，这也是本类型与 `SseEvent` 分开的理由。
/// `last` 是收尾标记条目（订阅正常结束时恰出一条，失败结局不出而改由 `StreamResource::fail()` 表达），
/// 此时 `event` 与 `data` 为空，`id` 是收摊时的续传标识——调用方想手动接着收就把它写进 `Last-Event-ID`。
/// @note Thread: thread-safe（值类型）
/// @note Side-effects: none
struct SseRecord {
    std::uint16_t status{0};  ///< 该条所属流的状态码（重连之后是另一条流）
    std::uint64_t index{0};  ///< 本次订阅里该条的序号；`last` 标记那条取的是「事件总条数」
    std::string event;  ///< 事件类型；服务端未命名时是 `message`
    std::string data;  ///< 消息体：多行 `data` 已以 LF 连接
    std::string id;  ///< 派发时刻该流的续传标识；从未有过 `id:` 字段时为空
    bool last{false};  ///< 是否为收尾标记条目（`event` 与 `data` 此时为空）
};

namespace detail {

/// @brief 把一份结果写进作业状态并投递——`async()` 收尾三步（写结果、单点裁决、回投主线程）的同一形态。
///
/// 本头自己起作业状态（交换的结局生在协程收尾那一刻，不在任何线程池任务里），于是那三步必须原样复刻；
/// 差异只在「谁触发写入」：`async()` 由 worker 触发，这里由 `CoroTask::on_done()` 触发。
/// @tparam T 作业值类型。
/// @param state 与返回的 `Task<T>` 共享的状态。
/// @param outcome 本次落定的结果。
template <typename T>
auto settle_job(const std::shared_ptr<aurora::detail::AsyncState<T>> &state, Result<T> outcome) -> void {
    {
        std::scoped_lock<std::mutex> lock(state->mutex);
        state->result = std::move(outcome);
    }
    std::function<void(const Result<T> &)> to_call;
    auto maybe = aurora::detail::take_for_delivery(*state, to_call);
    if (maybe) {
        auto out = std::move(*maybe);
        aurora::detail::post_to_main([to_call, out]() mutable -> void { to_call(out); });
    }
}

/// @brief 起一次物化交换并折算成 `Task<Response>`：会话自持、取消相接、结局经投递器抵达。
///
/// 非模板的核心（`deliver` 的模板外壳只负责在其上再投影一次），实现住在 `src/aurora/net/http_binding.cpp`。
/// @param client 借用的客户端（内部拷一份交给帧的收尾口持有）。
/// @param request 待发送的请求。
/// @param cancellation 本次交换的取消令牌。
/// @return 交出物化应答的作业句柄。
[[nodiscard]] auto exchange_material(const Client &client, Request request, Cancellation cancellation)
    -> aurora::Task<Response>;

}  // namespace detail

/// @brief 发起一次交换并投影成 `Task<T>`：`Resource<T>::from()` 的作业形态，也是回调式取结果的唯一短径。
///
/// 非 2xx 是成功的 `Response`（投影里自己判），传输与协议失败才走错误侧；投影抛出折成
/// `runtime-async-exception`。取消链完整：`Task::cancel()` 既丢结果，也经 `Cancellation` 掐掉在途连接。
/// @tparam T 投影后的值类型。
/// @param client 借用的客户端（本次交付自持一份拷贝，调用后可立即销毁）。
/// @param request 待发送的请求（按值接管）。
/// @param project 应答投影；在交付线程上调用一次。
/// @param cancellation 取消令牌（缺省 = 独立令牌、无人可取消）。
/// @return 落定为 `Result<T>` 的作业句柄。
template <typename T>
[[nodiscard]] auto deliver(const Client &client, Request request, Projection<T> project, Cancellation cancellation = {})
    -> aurora::Task<T> {
    auto raw = detail::exchange_material(client, std::move(request), cancellation);
    auto state = std::make_shared<aurora::detail::AsyncState<T>>();
    aurora::Task<T> job{state};
    // 取消相接：结果侧由 `delivered` 单点裁决丢弃，连接侧由这枚钩子掐掉——两条各归一半，合起来才是真取消。
    job.on_cancel([cancellation]() mutable { cancellation.cancel(); });
    raw.then([state, project = std::move(project)](const Result<Response> &r) -> void {
        if (!r.ok()) {
            detail::settle_job<T>(state, Result<T>{r.error()});
            return;
        }
        // 投影经 `invoke_safe` 走：它的异常属于这次交付，不该逃出作业边界，也不该由本库解释成别的码。
        auto projected =
            aurora::detail::invoke_safe([&project, &response = r.value()]() -> Result<T> { return project(response); });
        detail::settle_job<T>(state, std::move(projected));
    });
    return job;
}

/// @brief 不投影的形态：直接把物化 `Response` 装进作业（非 2xx 同样算成功落定）。
/// @param client 借用的客户端（本次交付自持一份拷贝）。
/// @param request 待发送的请求（按值接管）。
/// @param cancellation 取消令牌。
/// @return 落定为 `Result<Response>` 的作业句柄。
[[nodiscard]] inline auto deliver(const Client &client, Request request, Cancellation cancellation = {})
    -> aurora::Task<Response> {
    return detail::exchange_material(client, std::move(request), std::move(cancellation));
}

/// @brief 发起一次交换并直接绑成三态 `Resource<T>`：读取即订阅，落定即定点刷新。
///
/// 绑的是**作业来源**形态，故 `reload()` 是真重发：前序那次的结果由代次守卫丢弃，且它的取消钩子会把在途
/// 连接掐掉。每次发送各拿一枚**新的**取消令牌——令牌是一次交换的共享状态，复用一枚已取消的令牌只会让下一次
/// 发送起手就落 `net-cancelled`。
/// @tparam T 投影后的值类型。
/// @param client 借用的客户端（由作业来源持有拷贝，可随后销毁原对象）。
/// @param request 待发送的请求（每次发送重新拷一份）。
/// @param project 应答投影；每次发送各调一次。
/// @param options 资源选项（超时、去抖）。
/// @return 处于 Loading 的资源句柄。
template <typename T>
[[nodiscard]] auto bind_resource(const Client &client, Request request, Projection<T> project,
                                 aurora::ResourceOptions options = {}) -> aurora::Resource<T> {
    // 会话与投影由来源闭包自持：`Resource` 的拷贝共享同一内核，内核共享这份来源，故不需要外部的宿主再保一份。
    // 闭包按 `std::function` 装箱，检查器据此认为它处在一个不应抛的上下文；本面的口径相反：装箱拷贝（会话 + 请求
    // + 投影）确实能抛，抛了就让它上抛——把装配期故障折成一次永不落定的 Loading，比一句崩溃更难排查。
    return aurora::Resource<T>::from(
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [client, request = std::move(request), project = std::move(project)]() mutable -> aurora::Task<T> {
            return deliver<T>(client, request, project);
        },
        options);
}

/// @brief 不投影的形态：把物化 `Response` 直接绑成三态资源（非 2xx 仍是 `Ready`，状态码在值里）。
/// @param client 借用的客户端。
/// @param request 待发送的请求。
/// @param options 资源选项。
/// @return 处于 Loading 的资源句柄。
[[nodiscard]] inline auto bind_resource(const Client &client, Request request, aurora::ResourceOptions options = {})
    -> aurora::Resource<Response> {
    return bind_resource<Response>(
        client, std::move(request),
        [](const Response &response) -> Result<Response> { return Result<Response>{response}; }, options);
}

/// @brief 发起一次流式交换并绑成 `StreamResource<StreamChunk>`：字节到手一段就提交一段。
///
/// 交付口径三条：**每条一个 `StreamChunk`**（`offset` 单调递增，故乱序或重放一眼可见）；**收尾恒出一条 `last`
/// 标记**，成功结局（含宿主停读之外的一切正常收摊）才有它；**失败只有 `fail()`**——交换失败时未提交的批次被
/// 丢弃、已提交数据保留（`StreamResource` 的既有纪律）。取消经 `Cancellation` 掐连接并落 `net-cancelled`，
/// 于是宿主卡顿期间的持续提交会随取消一起停住。
/// @param client 借用的客户端（本次交付自持一份拷贝）。
/// @param request 待发送的请求（按值接管）。
/// @param cancellation 取消令牌。
/// @param options 提交策略（`Append` = 逐段累积，`Replace` = 只留最新一段）。
/// @return 处于 Loading 的流资源句柄（首批字节到达后进 Ready）。
[[nodiscard]] auto deliver_stream(const Client &client, Request request, Cancellation cancellation = {},
                                  aurora::StreamOptions options = {}) -> aurora::StreamResource<StreamChunk>;

/// @brief 订阅一次事件流并绑成 `StreamResource<SseRecord>`：一条事件成形就提交一条。
///
/// 与 `deliver_stream` 同一条交付纪律，只是条目从「一段字节」换成「一条事件」：每条一个 `SseRecord`（`index`
/// 单调递增）、收尾恒恰出一条 `last` 标记、失败只走 `fail()` 且已提交的事件保留。重连与续传标识由
/// `Client::open_sse()` 带着，宿主不必接力，也**不会**因此多出条目边界——断线前后是同一个 `index` 序列。
///
/// 一次订阅按形态可以永不结束（服务端不写完、额度又不设上限），故收摊只有两条路：宿主 `cancel()` 这枚令牌
/// （在途那条流当场中断，已提交的事件保留，结局落 `net-cancelled` 走 `fail()`），以及本入口之外的
/// `SseSink` 侧停读——本壳没有「够了」的判定，它只把事件落成条目。
/// @param client 借用的客户端（本次交付自持一份拷贝）。
/// @param request 待订阅的请求（只认 GET，其余起手落 `http-request-invalid`）。
/// @param cancellation 本次订阅的取消令牌。
/// @param options 提交策略（`Append` = 逐条累积，`Replace` = 只留最新一条）。
/// @return 处于 Loading 的流资源句柄（首条事件到达后进 Ready）。
[[nodiscard]] auto deliver_sse(const Client &client, Request request, Cancellation cancellation = {},
                               aurora::StreamOptions options = {}) -> aurora::StreamResource<SseRecord>;

/// @brief 取图器：把「URL → 字节」接成一次 HTTP 交换，形态与 widget 侧的取图口结构一致。
///
/// 返回类型刻意写成与取图口别名 `aurora::ImageFetcher`（声明于 widget 侧头文件）相同的 `std::function` 形态
/// 而不是引用那个别名：网络层不依赖 widget 层，接线由装配根完成
/// （`aurora::set_default_image_fetcher(net::http::image_fetcher())`）。
/// 三条口径：
/// - **只有 2xx 给字节**：其余状态落 `render-image-load-failed`，detail 点名状态码——取图没有「非 2xx 也算成功」
///   这条中间态，图片解码器拿到 404 的 HTML 只会报一句「不是图片」，把原因留在那儿就丢了。
/// - **失败即错误、不交空字节**：空 vector 与「取到了 0 字节的图」在本口无从区分，故一律走错误侧。
/// - **每次调用一条连接**：与 `Client::send` 同形态（无连接池、无会话）；装配根把一份 `Client` 交给本函数，
///   此后每次取图共用它的配置（上限、代理、策略、缓存）。
/// @param client 取图用的客户端（缺省即默认构造的一份；由返回的闭包自持）。
/// @return 可交给 `ImageView` / `Environment` 的取图器。
[[nodiscard]] auto image_fetcher(Client client = {})
    -> std::function<aurora::Task<std::vector<std::uint8_t>>(std::string_view)>;

}  // namespace aurora::net::http

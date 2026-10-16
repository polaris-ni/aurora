/// @brief 声明式交付面的实现：协程出口 → 作业状态 → 响应式资源的桥（CHG-018 §12）。
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
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/http.h"
#include "aurora/net/http_client.h"
#include "aurora/net/http_stream.h"
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

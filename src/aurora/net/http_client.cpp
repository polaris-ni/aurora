// http_client.cpp —— HTTP 发起侧客户端的 send 核心闭环（CHG-010 §6 / §12）。
//
// 落在这里的是「把回调式传输按 HTTP 线上次序编排成一条协程」：连接（http 直连 / https 先握手）、
// 写出 render_head() + 体、逐段喂接收侧装配器物化应答、按 Location 跟随重定向、并施加整请求时限。
// 协程出口 `CoroTask<Result<Response>>` 的成败一律以 Result 表达，绝不抛异常穿过协程帧（§18-4）。
#include "aurora/net/http_client.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/error_codes.h"
#include "aurora/core/headers.h"
#include "aurora/core/result.h"
#include "aurora/core/url.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/http_wire.h"
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "aurora/state/coroutine.h"

namespace aurora::net::http {
namespace {

/// @brief 单段读取的投喂缓冲字节数：足够让常见的头部 + 首块一次到位，又不无界占用栈帧。
constexpr std::size_t AURORA_READ_CHUNK = std::size_t{16} * 1024;

/// @brief 三档超时的档号：写进 `TimeoutState::fired`，收尾据此拼 `http-timeout` 的 detail。
enum class TimeoutStage : std::uint8_t {
    None,
    Connect,
    ReadIdle,
    Total,
};

/// @brief 三档看门狗共享的触发状态：第一个到点的档写 `fired`，其余档见其非 `None` 即不再触发。
///
/// 用 `shared_ptr` 让看门狗与引擎 `post_after` 闭包共享——闭包在 send 结束后仍可能到期，共享所有权保证
/// 那块内存活到闭包跑完，帧销毁后写它不再是悬垂访问。
struct TimeoutState {
    /// @brief 首先到点的档号（`None` = 尚未触发）。
    std::atomic<int> fired{static_cast<int>(TimeoutStage::None)};
};

/// @brief 一档超时的看门狗：构造登记引擎定时器，到点且本段仍在计时则记 `fired` 并取消令牌；析构即停表。
///
/// 引擎 `post_after` 无撤销口，故「停表」靠把共享的 `live` 置假——到期的闭包见 `live` 为假即 no-op，
/// 不会误伤该段结束之后的操作。`connect` 在拿到可用连接后停表、`read_idle` 每读到一次字节即重置。
class StageTimer {
  public:
    /// @brief 登记一档看门狗。
    /// @param window 该档时限；`nullopt` 或非正值即不登记（本段不设限）。
    /// @param stage 到点写入 `state->fired` 的档号。
    /// @param token 到点取消的令牌（在途操作以 net-cancelled 收尾）。
    /// @param state 三档共享的触发状态。
    StageTimer(std::optional<std::chrono::milliseconds> window, TimeoutStage stage, Cancellation token,
               const std::shared_ptr<TimeoutState> &state) {
        if (!window.has_value() || window->count() <= 0) {
            return;
        }
        live_ = std::make_shared<std::atomic<bool>>(true);
        // 引擎投递闭包经 std::function 类型擦除转入，命中本仓记录在案的「不应抛出」系统性假告警面；
        // 到期体只做原子写与取消，不抛。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        (void)Engine::instance().post_after(*window, [live = live_, stage, token, state]() mutable -> void {
            if (live->load() && state->fired.load() == static_cast<int>(TimeoutStage::None)) {
                state->fired.store(static_cast<int>(stage));
                token.cancel();
            }
        });
    }

    StageTimer(const StageTimer &) = delete;
    auto operator=(const StageTimer &) -> StageTimer & = delete;
    StageTimer(StageTimer &&) = delete;
    auto operator=(StageTimer &&) -> StageTimer & = delete;
    ~StageTimer() { stop(); }

    /// @brief 停表：令尚未到期的定时器闭包到点时 no-op（幂等）。
    auto stop() -> void {
        if (live_ != nullptr) {
            live_->store(false);
        }
    }

  private:
    std::shared_ptr<std::atomic<bool>> live_;  ///< 本段是否仍在计时；空 = 未登记（该档不设限）
};

/// @brief 该状态码是否为需要跟随的重定向。
[[nodiscard]] auto is_redirect(std::uint16_t status) -> bool {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/// @brief 按方法取对应工厂产出一枚指向 `url` 的请求（重定向换目标时复用）。
[[nodiscard]] auto make_request(Method method, std::string_view url) -> Request {
    switch (method) {
        case Method::Get:
            return Request::get(url);
        case Method::Head:
            return Request::head(url);
        case Method::Post:
            return Request::post(url);
        case Method::Put:
            return Request::put(url);
        case Method::Patch:
            return Request::patch(url);
        case Method::Delete:
            return Request::delete_(url);
    }
    return Request::get(url);
}

/// @brief 依 `Location` 与状态码算出重定向的下一跳请求（方法/体的改写按 RFC 9110 §15.4 惯例）。
[[nodiscard]] auto follow(const Request &current, const Response &response) -> Result<Request> {
    const auto location = response.header("Location");
    if (!location.has_value()) {
        return Result<Request>{make_error(ErrorCode::HttpProtocolError, "redirect response carried no Location")};
    }
    const auto &base = current.url();
    if (!base.has_value()) {
        return Result<Request>{
            make_error(ErrorCode::HttpProtocolError, "redirect from a target with no resolvable base")};
    }
    auto next = base->resolve(*location);
    if (!next) {
        return Result<Request>{next.error()};
    }
    const Url next_url = std::move(next.value());

    Method method = current.method();
    bool keep_body = true;
    const std::uint16_t status = response.status();
    if (status == 303) {
        if (method != Method::Head) {  // 303 把非 HEAD 一律改判 GET、弃体
            method = Method::Get;
            keep_body = false;
        }
    } else if (status == 301 || status == 302) {
        if (method == Method::Post) {  // 历史兼容：POST 的 301/302 转 GET、弃体；其余保方法
            method = Method::Get;
            keep_body = false;
        }
    }  // 307 / 308 严格保留方法与体

    Request redirected = make_request(method, next_url.to_string());
    for (const auto &entry : current.headers().entries()) {
        redirected.header(entry.name, entry.value);
    }
    if (keep_body && !current.body().empty()) {
        redirected.body(current.body());
    }
    return Result<Request>{std::move(redirected)};
}

}  // namespace

auto Client::send(Request request) const -> CoroTask<Result<Response>> {
    Cancellation token;
    Request current = std::move(request);
    const Timeouts timeouts = current.timeouts();
    const int limit = current.max_redirects();
    int hops = 0;
    Result<Response> outcome{make_error(ErrorCode::HttpProtocolError, "send produced no outcome")};
    const StreamOptions options;

    // 三档超时共享一枚触发状态：total 全程登记一次，connect / read_idle 在各自阶段前登记、结束即停表。
    auto tstate = std::make_shared<TimeoutState>();
    StageTimer total_timer(timeouts.total, TimeoutStage::Total, token, tstate);

    for (;;) {
        // ── 写出物化：请求头部 + 体（render_head 失败即 URL / 字段问题，原样交出）。
        auto head = current.render_head();
        if (!head) {
            outcome = Result<Response>{head.error()};
            break;
        }
        const auto &url_slot = current.url();  // render_head 成功即目标已解析
        if (!url_slot.has_value()) {
            outcome = Result<Response>{make_error(ErrorCode::HttpProtocolError, "target vanished after render_head")};
            break;
        }
        const Url &target = url_slot.value();
        std::string payload = head.value();
        payload.insert(payload.end(), current.body().begin(), current.body().end());

        const auto port = target.effective_port();
        if (!port.has_value()) {
            outcome = Result<Response>{make_error(ErrorCode::HttpProtocolError, "target has no effective port")};
            break;
        }
        const std::string host{target.host()};

        // ── 建连并统一读写口：明文 / TLS 两条流经捕获 shared_ptr 的类型擦除闭包收敛到同一对 awaiter。
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<char>)> read_one;
        std::function<detail::CallbackAwaiter<std::size_t>(std::span<const char>)> write_one;
        StageTimer connect_timer(timeouts.connect, TimeoutStage::Connect, token, tstate);
        if (target.is_secure()) {
            auto plain = co_await detail::await_connect(host, *port, options, token);
            if (!plain) {
                outcome = Result<Response>{plain.error()};
                break;
            }
            auto secure = co_await detail::await_handshake(std::move(plain.value()), host, tls_, token);
            if (!secure) {
                outcome = Result<Response>{secure.error()};
                break;
            }
            auto holder = std::make_shared<TlsTransport>(std::move(secure.value()));
            read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
        } else {
            auto plain = co_await detail::await_connect(host, *port, options, token);
            if (!plain) {
                outcome = Result<Response>{plain.error()};
                break;
            }
            auto holder = std::make_shared<StreamTransport>(std::move(plain.value()));
            read_one = [holder](std::span<char> buffer) { return detail::await_read(*holder, buffer); };
            write_one = [holder](std::span<const char> data) { return detail::await_write(*holder, data); };
        }
        connect_timer.stop();  // 建连（含握手）完成，connect 档停表，不误伤后续读写

        // ── 发出请求。
        auto written = co_await write_one(std::span<const char>{payload});
        if (!written) {
            outcome = Result<Response>{written.error()};
            break;
        }

        // ── 逐段装配应答：喂 ResponseAssembler，按分帧物化体，直到 complete 或对端收摊。
        wire::ResponseAssembler assembler(max_body_bytes_, current.is_head());
        std::vector<char> body;
        std::array<char, AURORA_READ_CHUNK> chunk{};
        bool halted = false;
        while (!assembler.complete()) {
            StageTimer read_timer(timeouts.read_idle, TimeoutStage::ReadIdle, token, tstate);
            auto read = co_await read_one(std::span<char>{chunk});
            read_timer.stop();  // 本段交付已落地，重置空闲计时：下一轮 read 重新起表
            if (!read) {
                outcome = Result<Response>{read.error()};
                halted = true;
                break;
            }
            const std::size_t got = read.value();
            if (got == 0) {  // 对端 FIN：UntilClose 据此判全，其余形态提前收摊即协议错位
                auto closed = assembler.peer_closed();
                if (!closed) {
                    outcome = Result<Response>{closed.error()};
                    halted = true;
                    break;
                }
                if (!assembler.complete()) {
                    outcome = Result<Response>{
                        make_error(ErrorCode::HttpProtocolError, "peer closed before the body was complete")};
                    halted = true;
                    break;
                }
                break;
            }
            auto pushed = assembler.push(std::string_view{chunk.data(), got});
            if (!pushed) {
                outcome = Result<Response>{pushed.error()};
                halted = true;
                break;
            }
            assembler.drain_body(body);
        }
        if (halted) {
            break;
        }
        assembler.drain_body(body);

        const auto &response_head = assembler.head();
        outcome = Result<Response>{
            Response{response_head.status, response_head.reason, response_head.headers, std::move(body)}};

        // ── 重定向判定。
        if (!is_redirect(response_head.status) || limit == 0) {
            break;
        }
        if (++hops > limit) {
            outcome = Result<Response>{
                make_error(ErrorCode::HttpRedirectLimit, "redirect chain exceeded the configured limit")};
            break;
        }
        auto next = follow(current, outcome.value());
        if (!next) {
            outcome = Result<Response>{next.error()};
            break;
        }
        current = std::move(next.value());
    }

    // 超时改道：失败若因某档看门狗取消了令牌，一律改报 http-timeout，并在 detail 标明是哪一档。
    if (!outcome) {
        switch (static_cast<TimeoutStage>(tstate->fired.load())) {
            case TimeoutStage::Connect:
                outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "connect timeout")};
                break;
            case TimeoutStage::ReadIdle:
                outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "read-idle timeout")};
                break;
            case TimeoutStage::Total:
                outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "total timeout")};
                break;
            case TimeoutStage::None:
                break;
        }
    }
    co_return outcome;
}

}  // namespace aurora::net::http

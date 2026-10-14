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
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "aurora/state/coroutine.h"
#include "aurora/net/detail/http_await.h"
#include "aurora/net/detail/http_wire.h"

namespace aurora::net::http {
namespace {

/// @brief 单段读取的投喂缓冲字节数：足够让常见的头部 + 首块一次到位，又不无界占用栈帧。
constexpr std::size_t AURORA_READ_CHUNK = std::size_t{16} * 1024;

/// @brief 跨协程帧与超时定时器共享的截止状态：`timed_out` 由定时器写、协程收尾时读。
///
/// 用 `shared_ptr` 而非协程局部量，是因为 `post_after` 的闭包在协程结束后仍可能到期——共享所有权保证
/// 那块内存活到闭包跑完，协程帧销毁后写它不再是悬垂访问。
struct Deadline {
    std::atomic<bool> timed_out{false};  ///< 到点置真；收尾据此把 cancelled/失败改道为 http-timeout
};

/// @brief 该状态码是否为需要跟随的重定向。
[[nodiscard]] auto is_redirect(std::uint16_t status) -> bool {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/// @brief 按方法取对应工厂产出一枚指向 `url` 的请求（重定向换目标时复用）。
[[nodiscard]] auto make_request(Method method, std::string_view url) -> Request {
    switch (method) {
        case Method::Get: return Request::get(url);
        case Method::Head: return Request::head(url);
        case Method::Post: return Request::post(url);
        case Method::Put: return Request::put(url);
        case Method::Patch: return Request::patch(url);
        case Method::Delete: return Request::delete_(url);
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
        return Result<Request>{make_error(ErrorCode::HttpProtocolError, "redirect from a target with no resolvable base")};
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
    auto deadline = std::make_shared<Deadline>();

    // 整请求时限：跨连接 / TLS / 读写 / 全部重定向一次性登记，到点取消令牌（在途操作以 net-cancelled 收尾）。
    if (const auto total = request.timeout(); total.has_value() && total->count() > 0) {
        (void)Engine::instance().post_after(*total, [token, deadline]() mutable -> void {
            deadline->timed_out.store(true);
            token.cancel();
        });
    }

    Request current = std::move(request);
    const int limit = current.max_redirects();
    int hops = 0;
    Result<Response> outcome{make_error(ErrorCode::HttpProtocolError, "send produced no outcome")};
    const StreamOptions options;

    for (;;) {
        // ── 写出物化：请求头部 + 体（render_head 失败即 URL / 字段问题，原样交出）。
        auto head = current.render_head();
        if (!head) {
            outcome = Result<Response>{head.error()};
            break;
        }
        const Url &target = current.url().value();  // render_head 成功即目标已解析
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
            auto read = co_await read_one(std::span<char>{chunk});
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

    // 超时改道：因取消而失败的结局，若确由整请求时限触发，对外一律报 http-timeout。
    if (!outcome && deadline->timed_out.load()) {
        outcome = Result<Response>{make_error(ErrorCode::HttpTimeout, "request deadline exceeded")};
    }
    co_return outcome;
}

}  // namespace aurora::net::http

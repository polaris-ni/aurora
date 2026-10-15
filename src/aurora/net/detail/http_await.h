#pragma once

// http_await.h —— 把回调式传输桥接成协程可等待体（CHG-010 §6 HTTP 发起侧；仅库实现可见，不属公共 API）。
//
// 为何单列：`StreamTransport` / `TlsTransport` 的 connect / read / write / handshake 一律是「登记一次操作、
// 完成恰一次经回调抵达、且完成恒回到发起线程」的回调形态——这是 L1 刻意不给协程出口的分层裁决
// （`net/transport.h`：把 Task 写进 L1 公共头会让下层反向依赖 L4）。HTTP 发起侧（L3）面向应用、要协程形态，
// 于是桥接落在**库内**这一层：把回调翻成 `co_await` 求得 `Result<T>`，公共头只暴露协程签名而不把 awaiter
// 外露。三个约束必须守住：
// - **续体经 `post_to_main` 恢复**：与 `state::co_async` 同一条回投通道，保证「完成恒在主线程」，
//   上层协程体内可安全触碰响应式状态；无 poster（headless / 测试）时退化为同步恢复。
// - **awaiter 的生命周期由协程帧担保**：`co_await` 的 awaiter 临时活到该表达式结束（含恢复之后），
//   故 `await_suspend` 里捕获的 `this` 在回调触发那一刻必然存活；结果先落 `result_`、再投恢复，
//   `await_resume` 取用时恒有值。
// - **不吞取消**：`Cancellation` 被 cancel 后，在途操作的回调以 `net-cancelled` 恰一次抵达，awaiter 原样
//   把它作为 `Result<T>` 交回协程，由上层决定改道成 `http-timeout` 还是 `net-cancelled`——本层不解释取消。
//
// @note Thread: single-threaded（每个 awaiter 随一条协程帧串行使用，无共享状态）
// @note Side-effects: 触发一次异步传输发起；恢复经主线程投递器

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/engine.h"
#include "aurora/net/tls.h"
#include "aurora/net/transport.h"
#include "aurora/state/async.h"  // aurora::detail::post_to_main

namespace aurora::net::detail {

/// @brief 通用回调→协程等待体：持一枚「接受完成回调的发起器」，挂起协程直到该回调携 `Result<T>` 抵达。
/// @tparam T 传输操作的成功值类型（`StreamTransport` / `TlsTransport` / `std::size_t`）。
/// @note Thread: single-threaded（随一条协程帧使用）
/// @note Side-effects: await_suspend 触发一次异步发起
/// @note Rebuildable: no
template <typename T>
class CallbackAwaiter {
  public:
    using Sink = std::function<void(Result<T>)>;  ///< 完成回调形态，与传输层的 `*Done` 别名同构

    /// @brief 以发起器构造。
    /// @param start 发起器；`await_suspend` 时被调用一次，收一枚完成回调。
    explicit CallbackAwaiter(std::function<void(Sink)> start) : start_(std::move(start)) {}

    CallbackAwaiter(const CallbackAwaiter &) = delete;
    auto operator=(const CallbackAwaiter &) -> CallbackAwaiter & = delete;
    // 允许移动：发起侧把明文 / TLS 两条流的读写经 std::function 统一时需移动 awaiter；移动只发生在
    // `co_await` 之前（此时 handle_ / result_ 尚未使用），最终对象承接 await 三件套，语义安全。
    CallbackAwaiter(CallbackAwaiter &&) = default;
    auto operator=(CallbackAwaiter &&) -> CallbackAwaiter & = default;
    ~CallbackAwaiter() = default;

    /// @brief awaiter 协议：始终挂起（异步操作没有「已完成得可直接返回」的快路径）。
    /// @return 恒 false。
    /// NOLINTNEXTLINE(readability-convert-member-functions-to-static) 协程 awaiter 三件套形态由标准固定
    [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

    /// @brief awaiter 协议：登记续体为完成回调的恢复目标，再触发发起器。
    /// @param handle 待恢复的协程句柄；结果落地后经 `post_to_main` 恢复之。
    auto await_suspend(std::coroutine_handle<> handle) -> void {
        handle_ = handle;
        start_([this](Result<T> result) -> void {
            result_.emplace(std::move(result));
            aurora::detail::post_to_main([handle = handle_]() -> void { handle.resume(); });
        });
    }

    /// @brief awaiter 协议：取回传输操作的结局。
    /// @return 完成回调写入的 `Result<T>`（移出）。
    /// NOLINTNEXTLINE(bugprone-unchecked-optional-access) 契约：续体只可能经写入 result_ 的那次 resume 前进
    auto await_resume() -> Result<T> { return std::move(*result_); }

  private:
    std::function<void(Sink)> start_;  ///< 发起器（含目标句柄/参数副本，await_suspend 时调一次）
    std::coroutine_handle<> handle_;  ///< 被挂起、待恢复的续体
    std::optional<Result<T>> result_;  ///< 完成结果；resume 前必有值
};

/// @brief 按主机名 + 端口建连（内含解析与 Happy Eyeballs 竞速），结果携已建立的流。
inline auto await_connect(std::string host, std::uint16_t port, const StreamOptions &options,
                          const Cancellation &cancellation) -> CallbackAwaiter<StreamTransport> {
    return CallbackAwaiter<StreamTransport>(
        // connect 发起器经 std::function 类型擦除转入，命中本仓记录在案的「转入 std::function 一律判
        // 不应抛出」系统性假告警面；发起器实际不抛（connect 全程回调，失败以 Result 交付，绝不抛出穿越）。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [host = std::move(host), port, options, cancellation](CallbackAwaiter<StreamTransport>::Sink on_done) mutable {
            StreamTransport::connect(host, port, options, cancellation, std::move(on_done));
        });
}

/// @brief 在已建立的明文流上握手；接管传入的流（协程侧交出所有权）。
inline auto await_handshake(StreamTransport stream, std::string server_name, const TlsContext &context,
                            const Cancellation &cancellation) -> CallbackAwaiter<TlsTransport> {
    // 底层流是 move-only，而发起闭包须能进 std::function（要求可拷贝），故用 shared_ptr 承载它；
    // 发起只发生一次，握手时从共享对象 move 出（此后该对象转为空态，无人再用）。
    auto plain = std::make_shared<StreamTransport>(std::move(stream));
    return CallbackAwaiter<TlsTransport>(
        // 同 await_connect：发起器经 std::function 类型擦除转入触发的系统性假告警，握手失败以 Result 交付、不抛。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [plain, server_name = std::move(server_name), context,
         cancellation](CallbackAwaiter<TlsTransport>::Sink on_done) {
            TlsTransport::handshake(std::move(*plain), server_name, context, cancellation, std::move(on_done));
        });
}

/// @brief 在任意具备 write 的流（明文或 TLS）上登记一次写；`data` 须在 awaiter 存活期内有效。
template <typename Stream>
auto await_write(Stream &stream, std::span<const char> data) -> CallbackAwaiter<std::size_t> {
    return CallbackAwaiter<std::size_t>(
        [&stream, data](CallbackAwaiter<std::size_t>::Sink on_done) { stream.write(data, std::move(on_done)); });
}

/// @brief 在任意具备 read 的流上登记一次读；`buffer` 须在 awaiter 存活期内有效。
template <typename Stream>
auto await_read(Stream &stream, std::span<char> buffer) -> CallbackAwaiter<std::size_t> {
    return CallbackAwaiter<std::size_t>(
        [&stream, buffer](CallbackAwaiter<std::size_t>::Sink on_done) { stream.read(buffer, std::move(on_done)); });
}

/// @brief 等待一段时间（重试退避用）；到期与取消谁先到就当场放行，二者都求得一次 `Result<void>`。
///
/// 引擎的 `post_after` 没有撤销口，且**停机时未到期的定时器被丢弃**（`net/engine.h`：定时器只承载「放弃
/// 等待」的语义）。故本等待体自带一枚「恰好完成一次」的闸，并同时挂到取消令牌上——否则退避中的协程会在
/// 引擎停机后永远悬着。取消不改变结局形态：调用方按 `is_cancelled()` 判这一下等待是被谁结束的。
/// @param delay 等待时长；非正值等价于立即放行（仍经完成回调走，不与挂起点抢栈）。
/// @param cancellation 本次交换的取消令牌（跨尝试的墙到点即取消它；按值持有，随 awaiter 存活）。
inline auto await_sleep(std::chrono::milliseconds delay, Cancellation cancellation) -> CallbackAwaiter<void> {
    return CallbackAwaiter<void>(
        // 同 await_connect：发起器经 std::function 类型擦除转入触发的系统性假告警面；本发起器只做登记与
        // 一次原子写，不抛。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [delay, cancellation](CallbackAwaiter<void>::Sink on_done) mutable {
            auto fired = std::make_shared<std::atomic<bool>>(false);
            // 完成回调移入 `fire` 持有：发起器只被调一次，之后再复制的都是这份持有而非再拷一次参数。
            auto fire = [fired, on_done = std::move(on_done)]() mutable {
                auto expected = false;
                if (fired->compare_exchange_strong(expected, true)) {
                    on_done(Result<void>{});
                }
            };
            if (cancellation.is_cancelled()) {
                fire();
                return;
            }
            cancellation.on_cancel([fire]() mutable { fire(); });
            if (delay.count() <= 0) {
                fire();
                return;
            }
            // 登记不成（本腿无引擎语义）也必须放行：协程悬着比退避失败更糟。
            if (!Engine::instance().post_after(delay, [fire]() mutable { fire(); })) {
                fire();
            }
        });
}

}  // namespace aurora::net::detail

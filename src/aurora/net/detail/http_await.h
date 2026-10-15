#pragma once

// http_await.h —— 把回调式传输桥接成协程可等待体。
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
#include <vector>

#include "aurora/core/result.h"
#include "aurora/net/cancellation.h"
#include "aurora/net/engine.h"
#include "aurora/net/proxy.h"
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

/// @brief 按代理决策建连：`Direct` 走直连入口，其余经代理建隧道，隧道不成即失败、绝不回退直连。
///
/// 发起侧只保留这一条建连路（代理与否在 `ProxyEnvironment::for_target` 处一次决定，之后不再分岔）：
/// `connect_via` 对 `ProxyKind::Direct` 的契约就是「等价于走直连入口」（`net/transport.h`），故无代理的
/// 缺省路径与接入代理前逐字节同型，竞速与地址族口径也共用同一份编排。
/// @param proxy 本跳的代理决策（含 `Direct`）。
/// @param host 目标主机文本。
/// @param port 目标端口（主机序）。
/// @param options 缓冲、超时与竞速口径；`connect_timeout` 在此覆盖「直到隧道可用」的整段。
/// @param cancellation 本次交换的取消令牌。
/// @return 可等待体，求得已建立的明文流（可能是隧道里的那条）。
inline auto await_connect_via(ProxyConfig proxy, std::string host, std::uint16_t port, const StreamOptions &options,
                              const Cancellation &cancellation) -> CallbackAwaiter<StreamTransport> {
    return CallbackAwaiter<StreamTransport>(
        // 发起器经 std::function 类型擦除转入，命中本仓记录在案的「转入 std::function 一律判不应抛出」系统性
        // 假告警面；发起器实际不抛（代理握手全程回调，失败以 Result 交付，绝不抛出穿越协程帧）。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [proxy = std::move(proxy), host = std::move(host), port, options,
         cancellation](CallbackAwaiter<StreamTransport>::Sink on_done) mutable {
            StreamTransport::connect_via(proxy, host, port, options, cancellation, std::move(on_done));
        });
}

/// @brief 在已建立的明文流上握手；接管传入的流（协程侧交出所有权）。
inline auto await_handshake(StreamTransport stream, std::string server_name, const TlsContext &context,
                            const Cancellation &cancellation) -> CallbackAwaiter<TlsTransport> {
    // 底层流是 move-only，而发起闭包须能进 std::function（要求可拷贝），故用 shared_ptr 承载它；
    // 发起只发生一次，握手时从共享对象 move 出（此后该对象转为空态，无人再用）。
    auto plain = std::make_shared<StreamTransport>(std::move(stream));
    return CallbackAwaiter<TlsTransport>(
        // 同 await_connect_via：发起器经 std::function 类型擦除转入触发的系统性假告警，握手失败以 Result 交付、不抛。
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

/// @brief 一次「带窗口的读」的结局：`Expect: 100-continue` 那条等待路上唯一能交回协程的事实。
///
/// 它不解释读到的字节是什么——那是装配器的事。本层只回答三问之一：字节到了（`Bytes`）、窗口到点或对端
/// 已关写向（`WaitElapsed`）、这次读本身失败（`Failure`）。三者都不带「这是一条 1xx」的判断，因为把
/// 协议语义放进等待体就会让同一份字节在两处被解释。
/// @note Thread: single-threaded（随一条协程帧使用）
/// @note Side-effects: none
struct WindowedRead {
    /// @brief 结局形态。
    enum class Outcome : std::uint8_t {
        Bytes,  ///< 窗口内读到至少一字节（`bytes` 非空）
        WaitElapsed,  ///< 窗口到点没等到字节；对端提前关闭写向也归这里（照走「不再等」那条路）
        Failure,  ///< 这次读本身失败（含被取消：`net-cancelled`），载荷在 `error`
    };

    Outcome outcome{Outcome::WaitElapsed};  ///< 结局形态
    std::vector<char> bytes;  ///< `Bytes` 时读到的线上字节，其余形态为空
    std::optional<Error> error;  ///< `Failure` 时的载荷，其余形态为空
};

/// @brief 在任意具备 read 的流上登记一次「带窗口的读」：字节先到就交字节，窗口先到就交「不再等」。
///
/// 存在的理由是 `Expect: 100-continue` 那一段等待**不能借用本次交换的取消令牌**：令牌的 `cancel()` 会
/// 顺带掐掉这条连接（它是连接级的，见 `StreamTransport::cancellation()`），而「对端没回 100」是一条可以
/// 继续往下走的事实，不是一次取消。也不能借用 `StageTimer` 那类超时壳——同理，它到点就是取消。
/// 于是这里让两路各跑各的、由一枚原子闸裁决：
/// - **先到者承接结局**：另一路只把字节交给 `late` 暂存、绝不触碰协程帧与 awaiter（awaiter 可能在先到者
///   恢复协程后已经销毁，这是唯一安全的失者动作）。
/// - **放弃的那次读不会丢字节**：传输层的读请求按 FIFO 排队（`StreamTransport` 的队列口径），故被放弃那次
///   的回调必然早于后续那次读的回调落地，`late` 里的字节总在下一批之前已就位，发起侧每轮先清 `late` 即可
///   保持线上次序。
/// - **放弃的那次读若以失败收场，那份失败就地丢弃**：FIN 是可重复观察的（后续读继续交出关闭事实），其余
///   错误以下一次的结局为准——本层不去补救一条已经被判定「不必再等」的连接。
/// @param stream 待读的流（明文或 TLS，须比 awaiter 活得更久）。
/// @param capacity 本次读的缓冲上限（一节的读块大小即可）。
/// @param window 等待时长；非正值等价于「不读、直接交 `WaitElapsed`」（连队列都不占）。
/// @param late 失者字段的暂存处（调用方持有、并在每轮读之前清空的队列）。
/// @return 可等待体，求得一次 `WindowedRead`。
template <typename Stream>
auto await_read_within(Stream &stream, std::size_t capacity, std::chrono::milliseconds window,
                       const std::shared_ptr<std::vector<char>> &late) -> CallbackAwaiter<WindowedRead> {
    return CallbackAwaiter<WindowedRead>(
        // 同 await_connect_via：发起器经 std::function 类型擦除转入触发的系统性假告警面；本发起器只做登记
        // 与一次原子写，结局一律以 Result 交付、不抛出穿越协程帧。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [&stream, capacity, window, late](CallbackAwaiter<WindowedRead>::Sink on_done) mutable {
            // NewDeleteLeaks 也属上面那一片假告警面：分析器把「共享对象 → std::function 的 _Rep」当作分配点，
            // 之后追不到 awaiter 随协程帧销毁的那次释放（同一构造形态在 `await_connect_via` / `await_handshake`
            // 上只是没被走到）。本发起器交出的每一枚对象都由回调链持有，`claim` 那一枚原子闸保证只析构一次。
            // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
            auto claim = [fired = std::make_shared<std::atomic<bool>>(false)]() -> bool {
                auto expected = false;
                return fired->compare_exchange_strong(expected, true);
            };
            // 完成回调移入 `deliver` 持有：发起器只被调一次，之后再复制的都是这份持有而非再拷一次参数。
            // `deliver` 是这条路上唯一的「恰好一次」闸，返回值即胜负：读侧据此决定没胜出的字节要不要转交
            // 暂存队列（两处若各调一次 Sink，后到那次会踩在已销毁的协程帧上）。
            auto deliver = [claim, on_done = std::move(on_done)](WindowedRead outcome) mutable -> bool {
                if (!claim()) {
                    return false;
                }
                on_done(Result<WindowedRead>{std::move(outcome)});
                return true;
            };
            if (window.count() <= 0) {
                deliver(WindowedRead{});  // 不等待：连一次读登记都不做，别占住那条 FIFO 队列
                return;
            }
            auto buffer = std::make_shared<std::vector<char>>(capacity);
            stream.read(std::span<char>{buffer->data(), capacity},
                        [buffer, late, deliver](Result<std::size_t> counted) mutable -> void {
                            WindowedRead out;
                            if (!counted) {
                                out.outcome = WindowedRead::Outcome::Failure;
                                out.error = counted.error();
                            } else if (counted.value() == 0) {
                                out.outcome = WindowedRead::Outcome::WaitElapsed;  // 对端关写向：与到点同处理
                            } else {
                                out.outcome = WindowedRead::Outcome::Bytes;
                                const std::span<const char> arrived{buffer->data(), counted.value()};
                                out.bytes.assign(arrived.begin(), arrived.end());
                            }
                            if (deliver(std::move(out))) {
                                return;
                            }
                            // 窗口先到：字节交给调用方的暂存队列，其余一律不碰（awaiter 可能已随协程帧销毁）。
                            if (counted && counted.value() > 0) {
                                const std::span<const char> arrived{buffer->data(), counted.value()};
                                late->insert(late->end(), arrived.begin(), arrived.end());
                            }
                        });
            if (!Engine::instance().post_after(window, [deliver]() mutable { deliver(WindowedRead{}); })) {
                deliver(WindowedRead{});  // 登记不成（本腿无引擎语义）也必须放行：悬着比不等更糟
            }
        });
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
        // 同 await_connect_via：发起器经 std::function 类型擦除转入触发的系统性假告警面；本发起器只做登记与
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

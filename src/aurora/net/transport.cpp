// transport.cpp —— 事件驱动字节流的编排面（CHG-010 传输切片）。
//
// 分层纪律：本文件**不出现任何平台 socket 头**，非阻塞系统调用一律借道 `net/detail/async_io.h`
// （CHG-006「平台 socket 头唯一落点是 socket.cpp」不变量）；等待由 `app::EventSourceRegistry` 的
// 驱动方负责（缺省是 `net::Engine` 的常驻 poll 线程，宿主自带等待集时是宿主自己）。
//
// 并发形态（三条，规范性）：
// 1. **每连接一枚互斥量**：状态转换与锁内的非阻塞系统调用一起串行化。锁内做系统调用是安全的，
//    因为句柄已置非阻塞——`recv` / `send` 要么立即返回、要么回报「暂无进展」，绝不会睡在锁里。
//    这比「把 socket 调用限定在某一条线程上」诚实：定时器到期落在引擎线程、`cancel()` 落在调用方
//    线程，两者都是真实入口，靠线程约定而非互斥量保护会留下竞争。
// 2. **完成回调一律在锁外触发**，且按登记次序：用户回调里再发起读写会重新取锁，不构成重入死锁。
// 3. **完成回调投递到 owner 线程**（= 发起 `connect()` / `bind()` 的线程）；不在该线程时经
//    `aurora::detail::post_to_main` 归队，与 `Task<T>` 共用同一条主线程通道，不新增第三种排队形态。
//
// 背压形态：读侧「无待交付请求即不等待可读」，写侧「无待发字节即不等待可写」，故一次就绪不会被
// 空转消费；待发写字节越过高水位即拒新写、回落低水位才重新接受。两侧都不存在无界缓冲。

#include "aurora/net/transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/error_codes.gen.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/detail/remote_connect.h"
#include "aurora/net/detail/runtime.h"
#include "aurora/net/engine.h"
#include "aurora/state/async.h"

namespace aurora::net {

namespace detail {

/// @brief 一条连接的全部可变状态（句柄、待发队列、事件源登记、串行化互斥量）。
struct StreamState {
    StreamState() = default;
    /// @brief 一枚排队的读请求：目标缓冲由调用方持有，须存活到对应回调抵达。
    struct PendingRead {
        std::span<char> buffer;  ///< 目标缓冲
        ReadDone done;  ///< 完成回调
    };

    /// @brief 一枚排队的写请求：字节已复制进本层（这正是被高水位约束的那份缓冲）。
    struct PendingWrite {
        std::vector<char> data;  ///< 待发送副本
        std::size_t offset = 0;  ///< 已进入内核的字节数
        WriteDone done;  ///< 完成回调
    };

    /// @brief 析构兜底：句柄仍有效时归还（正常路径由 `close_stream()` 先行处理）。
    ~StreamState() { close_handle(handle); }

    /// @brief 不可复制：状态由 `std::shared_ptr` 独占语义管理，复制即出现两份互斥量与双重归还。
    StreamState(const StreamState &) = delete;
    /// @brief 不可复制赋值：同 `StreamState(const StreamState &)`。
    auto operator=(const StreamState &) -> StreamState & = delete;
    /// @brief 不可移动：持有 `std::mutex` 与事件源登记，移动会让已登记的回调指向错位的地址。
    StreamState(StreamState &&) = delete;
    /// @brief 不可移动赋值：同 `StreamState(StreamState &&)`。
    auto operator=(StreamState &&) -> StreamState & = delete;

    std::mutex mutex;  ///< 保护以下全部字段（锁内只做非阻塞系统调用）
    detail::Session runtime;  ///< Winsock 运行时归属（与句柄创建处的进程级钉住叠加）
    std::intptr_t handle = -1;  ///< 原生句柄（已置非阻塞）；-1 = 已归还
    StreamOptions options;  ///< 缓冲与超时口径
    Cancellation cancellation;  ///< 本连接的取消令牌
    std::thread::id owner = std::this_thread::get_id();  ///< 完成回调的目标线程
    std::uint64_t generation = 0;  ///< 事件源登记代数：旧登记的尾批据此作废
    std::optional<app::EventSourceRegistration> registration;  ///< 当前登记（无关注时为空）
    app::WaitInterest interest{};  ///< 当前登记的关注方向
    std::deque<PendingRead> reads;  ///< 待满足的读请求（FIFO）
    std::deque<PendingWrite> writes;  ///< 待发写请求（FIFO）
    std::deque<char> read_ahead;  ///< 应用侧接回、须先于内核字节投递的字节（代理隧道握手留下的尾批）
    std::size_t pending_read_bytes = 0;  ///< reads 各缓冲容量之和（与 max_pending_bytes 对照）
    std::size_t pending_write_bytes = 0;  ///< writes 里尚未进入内核的字节总数
    std::size_t write_low_water_bytes = StreamOptions{}.write_low_water_bytes;  ///< 由高水位派生
    bool write_blocked = false;  ///< 高水位闩锁：为真时新写直接被拒
    bool read_paused = false;  ///< 读投递已暂停
    bool peer_eof = false;  ///< 已读到对端 FIN（此后每次读恒 0 字节）
    bool write_shut = false;  ///< 本端已停写
    bool connecting = false;  ///< 非阻塞 connect 尚未出结果
    bool closed = false;  ///< 已关闭（含错误关闭与显式 close）
    ConnectDone on_connect;  ///< 未完成的 connect 回调（建立或失败即移空）
};

/// @brief 一个监听口的全部可变状态。
struct ListenerState {
    ListenerState() = default;
    /// @brief 析构兜底：句柄仍有效时归还。
    ~ListenerState() { close_handle(handle); }

    /// @brief 不可复制：同 `StreamState`，两份状态会各自注销同一句柄。
    ListenerState(const ListenerState &) = delete;
    /// @brief 不可复制赋值：同 `ListenerState(const ListenerState &)`。
    auto operator=(const ListenerState &) -> ListenerState & = delete;
    /// @brief 不可移动：持有 `std::mutex` 与事件源登记，地址必须稳定。
    ListenerState(ListenerState &&) = delete;
    /// @brief 不可移动赋值：同 `ListenerState(ListenerState &&)`。
    auto operator=(ListenerState &&) -> ListenerState & = delete;

    std::mutex mutex;  ///< 保护以下全部字段
    detail::Session runtime;  ///< Winsock 运行时归属
    std::intptr_t handle = -1;  ///< 监听句柄（已置非阻塞）
    std::uint16_t port = 0;  ///< 实际绑定端口（入参 0 时为系统分配值）
    StreamOptions options;  ///< 交付给每条被接受连接的口径
    std::thread::id owner = std::this_thread::get_id();  ///< 完成回调的目标线程
    std::uint64_t generation = 0;  ///< 事件源登记代数
    std::optional<app::EventSourceRegistration> registration;  ///< 当前登记（无待交付回调时为空）
    std::deque<AcceptDone> pending;  ///< 待交付的新连接回调（每枚只用一次）
    bool closed = false;  ///< 已关闭
};

/// @brief 状态指针 → 公共句柄的装配口：两个公共类型的构造函数是私有的，只对本类型敞开。
/// @note 只在实现侧定义，故不出现在任何公共头，也不占 API 预算。
class TransportFactory {
  public:
    /// @brief 以连接状态构造流句柄。
    /// @param state 已建立的连接状态。
    /// @return 接管该状态的流句柄。
    static auto wrap_stream(std::shared_ptr<StreamState> state) -> StreamTransport {
        return StreamTransport{std::move(state)};
    }

    /// @brief 以监听状态构造监听句柄。
    /// @param state 已绑定的监听状态。
    /// @return 接管该状态的监听句柄。
    static auto wrap_listener(std::shared_ptr<ListenerState> state) -> StreamListener {
        return StreamListener{std::move(state)};
    }
};

}  // namespace detail

namespace {

/// @brief 一批待触发的回调（锁内收集、锁外按次序执行）。
using Tasks = std::vector<std::function<void()>>;

/// @brief 连接状态的共享指针（本文件自由函数的书写形态）。
using StreamPointer = std::shared_ptr<detail::StreamState>;

/// @brief 监听状态的共享指针。
using ListenerPointer = std::shared_ptr<detail::ListenerState>;

/// @brief 构造「本层已关闭」形态的取消原因（未完成的读写各以此收尾一次）。
/// @param what 操作描述，进 message 便于现场定位。
/// @return `net-cancelled` 错误。
auto cancelled_reason(std::string what) -> Error {
    return make_error(ErrorCode::NetCancelled, std::move(what) + " was cancelled before completion");
}

/// @brief 构造「句柄已不存在」形态的失败（空态句柄上的成员调用）。
/// @param what 操作描述。
/// @return `net-socket-failed` 错误。
auto no_stream_reason(std::string what) -> Error {
    return make_error(ErrorCode::NetSocketFailed, std::move(what) + " has no live connection");
}

/// @brief 按登记次序执行一批回调（回调为空则跳过：被移空的槽位不该产生调用）。
/// @param tasks 待执行回调（执行后清空）。
auto run_tasks(Tasks &tasks) -> void {
    for (auto &task : tasks) {
        if (task) {
            task();
        }
    }
    tasks.clear();
}

/// @brief 在 owner 线程执行一批回调；不在该线程时经主线程投递通道归队。
/// @param tasks 待执行回调（接管所有权）。
/// @param owner 目标线程标识。
auto deliver(Tasks tasks, std::thread::id owner) -> void {
    if (std::this_thread::get_id() == owner) {
        run_tasks(tasks);
        return;
    }
    aurora::detail::post_to_main([tasks = std::move(tasks)]() mutable { run_tasks(tasks); });
}

/// @brief 尚未出结果的连接状态表：连接在「已发起、未送达」期间的强所有权归本表。
///
/// 发起方 `connect()` 是非阻塞的，返回即失去所有权；成功要把状态交给新建的流句柄、失败要随
/// `close_stream()` 释放，而事件源登记的闭包只能持弱观测（否则「状态 → 登记闭包 → 状态」成环，
/// 句柄永不归还）。所以这张表就是那段时间的唯一强持有者，结果落地即摘除。
/// @return 进程级静态表。
auto connecting_table() -> std::vector<StreamPointer> & {
    static std::vector<StreamPointer> table;
    return table;
}

/// @brief 本表的守护锁。
/// @return 进程级静态 mutex 的引用。
auto connecting_table_mutex() -> std::mutex & {
    static std::mutex mutex;
    return mutex;
}

/// @brief 登记一枚在途连接（由 `StreamTransport::connect` 在发起前调用）。
/// @param state 在途连接状态。
auto hold_connecting(const StreamPointer &state) -> void {
    std::scoped_lock lock(connecting_table_mutex());
    connecting_table().push_back(state);
}

/// @brief 摘除一枚在途连接（成功交付或关闭时调用；不在表里则为空操作）。
/// @param raw 目标连接状态地址。
auto drop_connecting(const detail::StreamState *raw) -> void {
    std::scoped_lock lock(connecting_table_mutex());
    std::erase_if(connecting_table(), [raw](const StreamPointer &held) { return held.get() == raw; });
}

/// @brief 把未完成的读写请求各以一次失败收尾，并归还句柄。
///
/// 顺序是「先注销、后关句柄」——反过来的话常驻引擎手里那份 `sources()` 快照会撞上系统已复用的
/// 句柄号，表现为下一次等待偶发 `POLLNVAL`。
/// @param state 连接状态（调用方持锁）。
/// @param reason 未完成请求的失败原因。
/// @param tasks 出参：收集待触发的回调。
auto close_stream(detail::StreamState &state, const Error &reason, Tasks &tasks) -> void {
    if (state.closed) {
        return;
    }
    state.closed = true;
    state.connecting = false;
    state.registration.reset();  // 注销 = 停止产生新回调（§18-1）
    drop_connecting(&state);  // 在途连接的强所有权随结果落地而解除
    detail::close_handle(state.handle);
    state.handle = -1;
    // 以下三处把「以同一原因收尾」写成转入 Tasks（`std::function`）的可调用对象：本检查对任何无
    // noexcept 规格的可调用对象一律判为「不应抛出」，而闭包里的 Error/std::function 拷贝确实会分配内存。
    // 不能按建议改成 noexcept 或预分配——抛出只可能来自调用方回调自身，本层在派发边界不吞宿主异常，
    // 让它沿 std::function 的既有传播路径走（同 CODING_STANDARDS §5.2 记录的假告警面）。
    // NOLINTBEGIN(bugprone-exception-escape)
    if (state.on_connect) {
        auto done = std::move(state.on_connect);
        tasks.emplace_back([done, reason]() { done(Result<StreamTransport>{reason}); });
    }
    for (auto &request : state.reads) {
        auto done = std::move(request.done);
        tasks.emplace_back([done, reason]() { done(Result<std::size_t>{reason}); });
    }
    state.reads.clear();
    state.pending_read_bytes = 0;
    for (auto &request : state.writes) {
        auto done = std::move(request.done);
        tasks.emplace_back([done, reason]() { done(Result<std::size_t>{reason}); });
    }
    // NOLINTEND(bugprone-exception-escape)
    state.writes.clear();
    state.pending_write_bytes = 0;
    state.write_blocked = false;
}

/// @brief 待发字节是否已越过写高水位（`high == 0` 表示「不设缓冲」：只要还有待发就算越界）。
/// @param pending 尚未进入内核的字节数。
/// @param high 高水位。
/// @return 越界为 true。
auto above_high_water(std::size_t pending, std::size_t high) -> bool { return pending > 0 && pending >= high; }

/// @brief 刷新写闩锁：越过高水位即锁，回落到低于等于低水位才解锁（成对出现，避免阈值抖动）。
/// @param state 连接状态（调用方持锁）。
auto refresh_write_latch(detail::StreamState &state) -> void {
    if (above_high_water(state.pending_write_bytes, state.options.write_high_water_bytes)) {
        state.write_blocked = true;
    } else if (state.pending_write_bytes <= state.write_low_water_bytes) {
        state.write_blocked = false;
    }
}

/// @brief 释放一枚已出队的读请求占用的额度（按**登记时的容量**而非实收字节）。
///
/// 短读是常态（内核给多少算多少），若按实收字节扣，队列里就会攒出凭空占着的额度，攒到 `max_pending_bytes`
/// 之后新读会被误判成越限拒收——那是记账错位而不是背压。
/// @param state 连接状态（调用方持锁）。
/// @param capacity 出队请求的缓冲容量。
auto release_read_slot(detail::StreamState &state, std::size_t capacity) -> void {
    state.pending_read_bytes = state.pending_read_bytes > capacity ? state.pending_read_bytes - capacity : 0;
}

/// @brief 从应用侧接回的前端缓冲里满足排队读（先于内核字节，保证线上次序不被打乱）。
/// @param state 连接状态（调用方持锁）。
/// @param tasks 出参：收集待触发的回调。
/// @return true = 本轮满足了队首请求，调用方应继续循环。
auto satisfy_reads_from_ahead(detail::StreamState &state, Tasks &tasks) -> bool {
    if (state.reads.empty() || state.read_ahead.empty()) {
        return false;
    }
    auto &front = state.reads.front();
    const std::size_t limit = std::min(front.buffer.size(), state.read_ahead.size());
    std::copy_n(state.read_ahead.begin(), limit, front.buffer.begin());
    state.read_ahead.erase(state.read_ahead.begin(), state.read_ahead.begin() + static_cast<std::ptrdiff_t>(limit));
    auto done = std::move(front.done);
    release_read_slot(state, front.buffer.size());
    state.reads.pop_front();
    tasks.emplace_back([done, bytes = limit]() { done(Result<std::size_t>{bytes}); });
    return true;
}

/// @brief 以对端 FIN 的形态满足全部排队读（0 字节、OK，不是错误）。
/// @param state 连接状态（调用方持锁）。
/// @param tasks 出参：收集待触发的回调。
auto satisfy_reads_with_eof(detail::StreamState &state, Tasks &tasks) -> void {
    for (auto &request : state.reads) {
        auto done = std::move(request.done);
        tasks.emplace_back([done]() { done(Result<std::size_t>{std::size_t{0}}); });
    }
    state.reads.clear();
    state.pending_read_bytes = 0;
}

/// @brief 推进读方向：按队首请求的容量分轮从内核取字节。
///
/// 一轮里可以连续满足多枚请求（内核缓冲够深时正是如此），也可以一枚都不满足（暂无进展）。
/// 读到 0 字节即置 `peer_eof`，此后每次读恒得 0——这是半关闭的合法终局，不是错误。
/// @param state 连接状态（调用方持锁）。
/// @param tasks 出参：收集待触发的回调。
auto drive_read(detail::StreamState &state, Tasks &tasks) -> void {
    if (state.closed || state.read_paused) {
        return;
    }
    while (!state.reads.empty()) {
        if (satisfy_reads_from_ahead(state, tasks)) {
            continue;  // 接回的尾批先于内核字节，否则隧道握手多读的那段会凭空消失
        }
        if (state.peer_eof) {
            satisfy_reads_with_eof(state, tasks);
            return;
        }
        auto &front = state.reads.front();
        const std::size_t limit = std::min(front.buffer.size(), state.options.read_chunk_bytes);
        const auto attempt = detail::try_read(state.handle, front.buffer.first(limit));
        if (attempt.failed()) {
            close_stream(state, *attempt.failure, tasks);
            return;
        }
        if (attempt.blocked) {
            return;  // 暂无进展：等下一次可读就绪
        }
        if (attempt.bytes == 0) {
            state.peer_eof = true;
            continue;  // 尾批排队请求同批以 0 收尾
        }
        auto done = std::move(front.done);
        release_read_slot(state, front.buffer.size());
        state.reads.pop_front();
        tasks.emplace_back([done, bytes = attempt.bytes]() { done(Result<std::size_t>{bytes}); });
    }
}

/// @brief 推进写方向：把队首请求的剩余字节尽量送进内核。
/// @param state 连接状态（调用方持锁）。
/// @param tasks 出参：收集待触发的回调。
auto drive_write(detail::StreamState &state, Tasks &tasks) -> void {
    while (!state.writes.empty()) {
        if (state.closed || state.write_shut) {
            return;
        }
        auto &front = state.writes.front();
        const auto rest = std::span<const char>(front.data).subspan(front.offset);
        const auto attempt = detail::try_write(state.handle, rest);
        if (attempt.failed()) {
            close_stream(state, *attempt.failure, tasks);
            return;
        }
        if (attempt.blocked || attempt.bytes == 0) {
            refresh_write_latch(state);
            return;  // 发送缓冲满了：等下一次可写就绪
        }
        front.offset += attempt.bytes;
        state.pending_write_bytes -= std::min(state.pending_write_bytes, attempt.bytes);
        if (front.offset >= front.data.size()) {
            auto done = std::move(front.done);
            const std::size_t total = front.data.size();
            state.writes.pop_front();
            tasks.emplace_back([done, total]() { done(Result<std::size_t>{total}); });
        }
    }
    refresh_write_latch(state);
}

/// @brief 让流的事件源登记与当前关注方向一致（必要时重登记）。
///
/// 注册口没有「改关注方向」的接缝，故一次变更 = 一次重登记，代数随之递增：旧那次登记的尾批回调
/// 即使仍会抵达一次，也会因代数不符而被丢弃——重登记复用同一句柄，不校验代数就会出现「同一次就绪
/// 投递两遍」。
/// @param state 连接状态（调用方持锁）。
/// @param tasks 出参：登记失败时收集失败回调。
auto refresh_stream_interest(const StreamPointer &state, Tasks &tasks) -> void;

/// @brief 一次就绪通知的处理入口（引擎线程或宿主 dispatch 线程进入）。
/// @param state 连接状态。
/// @param generation 触发本次回调的登记代数。
/// @param ready 就绪方向。
auto on_stream_ready(const StreamPointer &state, std::uint64_t generation, const app::SourceReady &ready) -> void {
    Tasks fired;
    {
        std::scoped_lock lock(state->mutex);
        if (state->closed || state->generation != generation) {
            return;  // 已关闭，或旧登记的尾批
        }
        if (state->connecting && (ready.writable || ready.error || ready.hangup)) {
            const auto resolved = detail::take_connect_error(state->handle);
            state->connecting = false;
            if (resolved) {
                if (auto done = std::move(state->on_connect)) {
                    fired.emplace_back([done, held = state]() {
                        done(Result<StreamTransport>{detail::TransportFactory::wrap_stream(held)});
                    });
                    drop_connecting(state.get());  // 任务已握有强引用，本表可以让位
                }
            } else {
                close_stream(*state, resolved.error(), fired);
                deliver(std::move(fired), state->owner);
                return;
            }
        }
        if (ready.readable || ready.error || ready.hangup || state->peer_eof) {
            drive_read(*state, fired);
        }
        if (!state->closed) {
            drive_write(*state, fired);
        }
        if (!state->closed) {
            refresh_stream_interest(state, fired);
        }
    }
    deliver(std::move(fired), state->owner);
}

/// @brief 计算流当前想等待的方向。
/// @param state 连接状态（调用方持锁）。
/// @return 关注方向。
auto stream_interest(const detail::StreamState &state) -> app::WaitInterest {
    app::WaitInterest wanted;
    wanted.read = !state.closed && !state.connecting && !state.read_paused && !state.peer_eof && !state.reads.empty();
    wanted.write = !state.closed && (state.connecting || (!state.write_shut && !state.writes.empty()));
    return wanted;
}

auto refresh_stream_interest(const StreamPointer &state, Tasks &tasks) -> void {
    const auto wanted = stream_interest(*state);
    if (!wanted.read && !wanted.write) {
        state->registration.reset();
        state->interest = wanted;
        return;
    }
    if (state->registration && state->interest.read == wanted.read && state->interest.write == wanted.write) {
        return;  // 关注方向没变，不动登记
    }
    state->registration.reset();
    ++state->generation;
    const std::uint64_t generation = state->generation;
    std::weak_ptr<detail::StreamState> witness = state;
    auto registered = app::EventSourceRegistry::instance().register_source(
        state->handle, wanted, [witness, generation](const app::SourceReady &ready) {
            const auto held = witness.lock();
            if (held) {
                on_stream_ready(held, generation, ready);
            }
        });
    if (!registered) {
        close_stream(*state, registered.error(), tasks);
        state->interest = app::WaitInterest{};
        return;
    }
    state->registration = std::move(registered.value());
    state->interest = wanted;
}

/// @brief 取消 / 关闭钩子的公共形态：关流并把结果投递回 owner。
/// @param witness 连接状态的弱观测。
/// @param reason 失败原因。
auto close_and_deliver(const std::weak_ptr<detail::StreamState> &witness, const Error &reason) -> void {
    const auto held = witness.lock();
    if (!held) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(held->mutex);
        close_stream(*held, reason, fired);
    }
    deliver(std::move(fired), held->owner);
}

/// @brief 读取「已关闭」标志（发起路径上的检查点：钩子可能在任意两步之间把流关掉）。
/// @param state 连接状态。
/// @return 已关闭为 true。
auto stream_closed(const StreamPointer &state) -> bool {
    std::scoped_lock lock(state->mutex);
    return state->closed;
}

/// @brief 用已建立的非阻塞对端句柄组装一条连接状态（`StreamListener` 接受路径）。
/// @param handle 已置非阻塞的对端句柄（所有权移交本状态）。
/// @param options 交付口径。
/// @param owner 完成回调的目标线程。
/// @return 新建的共享状态。
auto make_accepted_stream(std::intptr_t handle, const StreamOptions &options, std::thread::id owner) -> StreamPointer {
    auto state = std::make_shared<detail::StreamState>();
    state->handle = handle;
    state->options = options;
    state->write_low_water_bytes = options.write_low_water_bytes;
    state->owner = owner;
    return state;
}

/// @brief 让监听口的事件源登记与「是否有待交付回调」一致。
///
/// 没有待交付回调就不等待：连接在这段期间的归宿是内核 backlog（深度由 `backlog` 限定），而不是
/// 本层无上限的队列。
/// @param state 监听状态（调用方持锁）。
/// @param tasks 出参：登记失败时把错误交付给队首回调。
auto refresh_listener_interest(const ListenerPointer &state, Tasks &tasks) -> void {
    if (state->closed || state->pending.empty()) {
        state->registration.reset();
        return;
    }
    if (state->registration) {
        return;  // 已经是读关注，登记不动
    }
    ++state->generation;
    const std::uint64_t generation = state->generation;
    std::weak_ptr<detail::ListenerState> witness = state;
    app::WaitInterest wanted;
    wanted.read = true;
    auto registered = app::EventSourceRegistry::instance().register_source(
        state->handle, wanted, [witness, generation](const app::SourceReady &ready) {
            const auto held = witness.lock();
            if (!held || held->closed || held->generation != generation) {
                return;
            }
            Tasks fired;
            {
                std::scoped_lock lock(held->mutex);
                if (held->closed || held->generation != generation) {
                    return;
                }
                (void)ready;  // 电平触发：循环到「暂无待接受连接」为止，不必分辨方向
                while (!held->pending.empty()) {
                    const auto attempt = detail::try_accept(held->handle);
                    if (attempt.failed()) {
                        auto done = std::move(held->pending.front());
                        held->pending.pop_front();
                        const auto error = *attempt.failure;
                        // 完成回调转入 Tasks（std::function），本检查对无 noexcept 规格的可调用对象一律判为
                        // 「不应抛出」；抛出只可能来自调用方回调，本层不在派发边界吞它（口径同 close_stream）。
                        // NOLINTNEXTLINE(bugprone-exception-escape)
                        fired.emplace_back([done, error]() { done(Result<StreamTransport>{error}); });
                        break;
                    }
                    if (!attempt.peer) {
                        break;  // 暂无待接受连接（含 accept 被信号打断的空转）
                    }
                    if (auto applied = detail::apply_native_options(*attempt.peer, held->options.native); !applied) {
                        // 交付口径里的原生选项落不了地，这条连接就不交给调用方——它会以为选项已经生效。
                        // 句柄就地归还、待交付的回调以原因收尾一次，监听口本身继续可用（口径同上面的
                        // `attempt.failed()`）。
                        detail::close_handle(*attempt.peer);
                        auto done = std::move(held->pending.front());
                        held->pending.pop_front();
                        // 完成回调转入 Tasks（std::function），口径同 close_stream。原因按值进闭包：
                        // `applied` 在派发之前就已析构。
                        // NOLINTNEXTLINE(bugprone-exception-escape)
                        fired.emplace_back([done, error = applied.error()]() { done(Result<StreamTransport>{error}); });
                        break;
                    }
                    auto stream = make_accepted_stream(*attempt.peer, held->options, held->owner);
                    auto done = std::move(held->pending.front());
                    held->pending.pop_front();
                    fired.emplace_back([done, stream]() mutable {
                        done(Result<StreamTransport>{detail::TransportFactory::wrap_stream(std::move(stream))});
                    });
                }
                if (!held->closed) {
                    refresh_listener_interest(held, fired);
                }
            }
            deliver(std::move(fired), held->owner);
        });
    if (!registered) {
        const Error &error = registered.error();
        // 登记失败即无驱动方：待交付回调各以一次失败收尾。
        for (auto &done : state->pending) {
            auto callback = std::move(done);
            // 回调转入 Tasks（std::function）故被本检查判为「不应抛出」，口径同 close_stream。
            // NOLINTNEXTLINE(bugprone-exception-escape)
            tasks.emplace_back([callback, error]() { callback(Result<StreamTransport>{error}); });
        }
        state->pending.clear();
        return;
    }
    state->registration = std::move(registered.value());
}

/// @brief 关闭监听口：注销事件源、归还句柄，并把待交付回调各以一次失败收尾。
/// @param state 监听状态（调用方持锁）。
/// @param reason 失败原因。
/// @param tasks 出参：收集待触发的回调。
auto close_listener(detail::ListenerState &state, const Error &reason, Tasks &tasks) -> void {
    if (state.closed) {
        return;
    }
    state.closed = true;
    state.registration.reset();
    detail::close_handle(state.handle);
    state.handle = -1;
    // 待交付回调各以一次失败收尾。
    for (auto &done : state.pending) {
        auto callback = std::move(done);
        // 回调转入 Tasks（std::function）故被本检查判为「不应抛出」，同 close_stream。
        // NOLINTNEXTLINE(bugprone-exception-escape)
        tasks.emplace_back([callback, reason]() { callback(Result<StreamTransport>{reason}); });
    }
    state.pending.clear();
}

/// @brief 构造「选项与套接字形态不合用」的失败原因（公共面两条对端查询按地址族分岔后的回报形态）。
/// @param detail 冲突的现场描述。
/// @return `net-option-conflict` 错误。
auto option_conflict_reason(std::string_view detail) -> Error {
    return make_error(ErrorCode::NetOptionConflict, ErrorParams{{"detail", std::string{detail}}});
}

/// @brief 确保本腿有驱动方在等待这些句柄（缺省引擎驱动时把引擎叫醒）。
/// @return 可为 true；常驻引擎起不来时携原因。
auto ensure_driver() -> Result<void> {
    if (app::EventSourceRegistry::instance().host_driven()) {
        return Result<void>{};
    }
    return Engine::instance().post([]() {});
}

}  // namespace

// ---- StreamTransport ------------------------------------------------------------------

auto StreamTransport::connect(const Endpoint &endpoint, const StreamOptions &options, const Cancellation &cancellation,
                              ConnectDone on_done) -> void {
    if (!on_done) {
        return;  // 没有回调就没有承诺：不建句柄、不留悬挂的连接
    }
    auto state = std::make_shared<detail::StreamState>();
    hold_connecting(state);  // 本次非阻塞发起的所有权锚点：结果落地前由在途表持有
    state->options = options;
    state->write_low_water_bytes = options.write_low_water_bytes;
    state->cancellation = cancellation;
    const auto owner = state->owner;
    std::weak_ptr<detail::StreamState> witness = state;

    auto fail = [&state, owner](const Error &error) {
        Tasks fired;
        {
            std::scoped_lock lock(state->mutex);
            close_stream(*state, error, fired);
        }
        deliver(std::move(fired), owner);
    };

    // 回调先入状态：此后任何失败路径（含 close_stream）都能把它交付出去，不存在「既不成功也不失败」
    // 的第三种结局。
    {
        std::scoped_lock lock(state->mutex);
        state->on_connect = std::move(on_done);
    }
    // 取消钩子紧跟着装：`cancel()` 可能来自任何线程，也可能在下列任何两步之间发生；`on_cancel` 对
    // 「登记时已取消」的口径是在当前线程就地执行钩子，因此这里不需要额外的补偿分支。钩子借状态里那份
    // 令牌登记（入参是 const 引用），且在锁外调用以免就地执行时自锁。
    Cancellation token;
    {
        std::scoped_lock lock(state->mutex);
        token = state->cancellation;
    }
    token.on_cancel([witness]() { close_and_deliver(witness, cancelled_reason("connection")); });
    if (stream_closed(state)) {
        return;
    }

    if (!state->runtime.ok()) {
        fail(make_error(ErrorCode::NetRuntimeFailed, std::string{"network runtime could not be initialised"}));
        return;
    }
    const auto target = endpoint.to_string();
    auto opened = detail::open_nonblocking_stream(endpoint, options.native);
    if (!opened) {
        fail(opened.error());
        return;
    }
    state->handle = opened.value();
    auto driven = ensure_driver();
    if (!driven) {
        fail(driven.error());
        return;
    }
    state->connecting = true;

    Tasks armed;
    {
        std::scoped_lock lock(state->mutex);
        refresh_stream_interest(state, armed);  // 连接期只需要可写关注
    }
    deliver(std::move(armed), owner);
    if (stream_closed(state)) {
        return;  // 登记失败已由 close_stream 交付过结果
    }

    auto started = detail::start_connect(state->handle, endpoint);
    if (!started) {
        fail(started.error());
        return;
    }
    if (stream_closed(state)) {
        return;
    }

    if (options.connect_timeout.count() > 0) {
        // 这段定时提交整段豁免：闭包交给引擎的定时排队口（无 noexcept 规格的可调用对象），本检查一律判为
        // 「不应抛出」；其中的分配与关闭链是超时收尾的必经路径，改成 noexcept 等于在引擎线程上 terminate。
        // NOLINTBEGIN(bugprone-exception-escape)
        auto timed = Engine::instance().post_after(options.connect_timeout, [witness, target]() {
            const auto held = witness.lock();
            if (!held) {
                return;
            }
            Tasks fired;
            {
                std::scoped_lock lock(held->mutex);
                if (held->closed || !held->connecting) {
                    return;
                }
                close_stream(
                    *held, make_error(ErrorCode::NetConnectTimeout, ErrorParams{{"detail", "connect(" + target + ")"}}),
                    fired);
            }
            deliver(std::move(fired), held->owner);
        });
        // NOLINTEND(bugprone-exception-escape)
        if (!timed) {
            fail(timed.error());
        }
    }
}

auto StreamTransport::connect(std::string_view host, std::uint16_t port, const StreamOptions &options,
                              const Cancellation &cancellation, ConnectDone on_done) -> void {
    detail::connect_by_name(host, port, options, cancellation, std::move(on_done));
}

auto StreamTransport::connect_via(const ProxyConfig &proxy, std::string_view host, std::uint16_t port,
                                  const StreamOptions &options, const Cancellation &cancellation, ConnectDone on_done)
    -> void {
    detail::connect_via_proxy(proxy, host, port, options, cancellation, std::move(on_done));
}

auto StreamTransport::is_open() const -> bool {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return !state_->closed;
}

auto StreamTransport::native_handle() const -> std::intptr_t {
    if (!state_) {
        return -1;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->handle;
}

auto StreamTransport::peer_endpoint() const -> Result<Endpoint> {
    if (!state_) {
        return Result<Endpoint>{no_stream_reason("stream")};
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return Result<Endpoint>{no_stream_reason("closed stream")};
    }
    auto family = detail::handle_family(state_->handle);
    if (family && family.value() == AddressFamily::LocalPath) {
        // 本机 IPC 流上没有「对端 IP 地址」这件事实：命名键只登记在监听侧的文件系统路径上，内核不会
        // 在双向流的两端都保留它。对端身份由 `peer_credentials()` 回答，而不是伪造成一个空端点。
        return Result<Endpoint>{
            option_conflict_reason("a local IPC stream has no peer IP address; ask peer_credentials() instead")};
    }
    return detail::peer_of(state_->handle);
}

auto StreamTransport::peer_credentials() const -> Result<PeerCredentials> {
    if (!state_) {
        return Result<PeerCredentials>{no_stream_reason("stream")};
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return Result<PeerCredentials>{no_stream_reason("closed stream")};
    }
    auto family = detail::handle_family(state_->handle);
    if (!family) {
        return Result<PeerCredentials>{family.error()};
    }
    if (family.value() != AddressFamily::LocalPath) {
        // 跨主机连接没有可对等的进程身份：这一问只在同机命名空间里成立。
        return Result<PeerCredentials>{option_conflict_reason(
            "peer credentials are only registered for a local IPC stream, not for an IP connection")};
    }
    return detail::peer_identity(state_->handle);
}

auto StreamTransport::is_loopback_peer() const -> bool {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return false;
    }
    return detail::peer_is_loopback(state_->handle);
}

auto StreamTransport::read(std::span<char> buffer, ReadDone on_done) -> void {
    if (!on_done) {
        return;
    }
    if (!state_) {
        on_done(Result<std::size_t>{no_stream_reason("read")});
        return;
    }
    if (buffer.empty()) {
        // 0 字节留给「对端已关闭写向」这一个含义，空缓冲只能是用法错误。
        on_done(Result<std::size_t>{
            make_error(ErrorCode::NetBufferLimit, ErrorParams{{"detail", "read target buffer is empty"}})});
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        // 四条回执分支都把回调转入 Tasks（无 noexcept 规格的可调用对象），本检查一律判为「不应抛出」。
        // 不能按建议在锁内预构造错误对象：那会把分配搬进临界区，而本层的口径是锁内只做非阻塞系统调用。
        // NOLINTBEGIN(bugprone-exception-escape)
        if (state_->closed) {
            fired.emplace_back([done = std::move(on_done), reason = cancelled_reason("queued read")]() {
                done(Result<std::size_t>{reason});
            });
        } else if (state_->peer_eof) {
            fired.emplace_back([done = std::move(on_done)]() { done(Result<std::size_t>{std::size_t{0}}); });
        } else if (state_->pending_read_bytes + buffer.size() > state_->options.max_pending_bytes) {
            const auto size = std::to_string(state_->options.max_pending_bytes);
            fired.emplace_back([done = std::move(on_done), size]() {
                done(Result<std::size_t>{
                    make_error(ErrorCode::NetBufferLimit, ErrorParams{{"detail", "pending read cap " + size}})});
            });
        } else {
            state_->reads.push_back(detail::StreamState::PendingRead{.buffer = buffer, .done = std::move(on_done)});
            state_->pending_read_bytes += buffer.size();
            drive_read(*state_, fired);
            if (!state_->closed) {
                refresh_stream_interest(state_, fired);
            }
        }
        // NOLINTEND(bugprone-exception-escape)
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamTransport::write(std::span<const char> data, WriteDone on_done) -> void {
    if (!on_done) {
        return;
    }
    if (!state_) {
        on_done(Result<std::size_t>{no_stream_reason("write")});
        return;
    }
    if (data.empty()) {
        on_done(Result<std::size_t>{std::size_t{0}});  // 空写不产生任何系统调用
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        // 同 read() 的四条分支：回调转入 Tasks 即被本检查判为「不应抛出」，错误对象留在锁外构造。
        // NOLINTBEGIN(bugprone-exception-escape)
        if (state_->closed) {
            fired.emplace_back([done = std::move(on_done), reason = cancelled_reason("queued write")]() {
                done(Result<std::size_t>{reason});
            });
        } else if (state_->write_shut) {
            const auto detail_text = std::string{"stream write side is shut down"};
            fired.emplace_back([done = std::move(on_done), detail_text]() {
                done(Result<std::size_t>{make_error(ErrorCode::NetOptionFailed, ErrorParams{{"detail", detail_text}})});
            });
        } else if (state_->write_blocked) {
            const auto size = std::to_string(state_->options.write_high_water_bytes);
            fired.emplace_back([done = std::move(on_done), size]() {
                done(Result<std::size_t>{make_error(
                    ErrorCode::NetBufferLimit, ErrorParams{{"detail", "write high-water " + size + " exceeded"}})});
            });
        } else {
            auto request = detail::StreamState::PendingWrite{
                .data = std::vector<char>(data.begin(), data.end()),
                .offset = 0,
                .done = std::move(on_done),
            };
            state_->pending_write_bytes += request.data.size();
            state_->writes.push_back(std::move(request));
            drive_write(*state_, fired);
            if (!state_->closed) {
                refresh_stream_interest(state_, fired);
            }
        }
        // NOLINTEND(bugprone-exception-escape)
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamTransport::set_high_water_mark(std::size_t bytes) -> void {
    if (!state_) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        state_->options.write_high_water_bytes = bytes;
        // 0 表示「不设缓冲」：此时低水位也归零，待发队列清空后立刻重新接受写入。
        state_->write_low_water_bytes = bytes == 0U ? 0U : std::max<std::size_t>(1U, bytes / 4U);
        refresh_write_latch(*state_);
        if (!state_->closed && !state_->write_shut) {
            drive_write(*state_, fired);
            refresh_stream_interest(state_, fired);
        }
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamTransport::pause_read() -> void {
    if (!state_) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        state_->read_paused = true;
        if (!state_->closed) {
            refresh_stream_interest(state_, fired);
        }
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamTransport::resume_read() -> void {
    if (!state_) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        state_->read_paused = false;
        if (!state_->closed) {
            drive_read(*state_, fired);
            refresh_stream_interest(state_, fired);
        }
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamTransport::read_paused() const -> bool {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->read_paused;
}

auto StreamTransport::pending_write_bytes() const -> std::size_t {
    if (!state_) {
        return 0U;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->pending_write_bytes;
}

auto StreamTransport::shutdown(ShutdownMode mode) -> Result<void> {
    if (!state_) {
        return Result<void>{no_stream_reason("shutdown")};
    }
    Tasks fired;
    Result<void> outcome{Result<void>{}};
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->closed) {
            return Result<void>{no_stream_reason("closed stream")};
        }
        auto stopped = detail::shutdown_handle(state_->handle, mode);
        if (!stopped) {
            return stopped;
        }
        const bool stop_read = mode == ShutdownMode::Read || mode == ShutdownMode::Both;
        const bool stop_write = mode == ShutdownMode::Write || mode == ShutdownMode::Both;
        if (stop_read) {
            // 停读复用 EOF 形态：已排队的读恰一次得 0，之后不再关注可读方向（提案 §18-2）。
            state_->peer_eof = true;
            satisfy_reads_with_eof(*state_, fired);
        }
        if (stop_write) {
            state_->write_shut = true;
            if (!state_->writes.empty()) {
                const auto reason = cancelled_reason("queued write");
                for (auto &request : state_->writes) {
                    auto done = std::move(request.done);
                    // 回调转入 Tasks 即被本检查判为「不应抛出」（口径同 close_stream）；停写排队的收尾不可省。
                    // NOLINTNEXTLINE(bugprone-exception-escape)
                    fired.emplace_back([done, reason]() { done(Result<std::size_t>{reason}); });
                }
                state_->writes.clear();
                state_->pending_write_bytes = 0;
                state_->write_blocked = false;
            }
        }
        refresh_stream_interest(state_, fired);
        outcome = Result<void>{};
    }
    deliver(std::move(fired), state_->owner);
    return outcome;
}

auto StreamTransport::close() -> void {
    if (!state_) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        close_stream(*state_, cancelled_reason("stream"), fired);
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamTransport::cancellation() const -> Cancellation {
    if (!state_) {
        return Cancellation{};
    }
    std::scoped_lock lock(state_->mutex);
    return state_->cancellation;
}

StreamTransport::StreamTransport(StreamTransport &&other) noexcept : state_(std::move(other.state_)) {}

// 移动赋值与析构都要先关掉自有连接：`close()` 的收尾链里有一次 Error 与若干 std::function 构造，
// 理论上能 bad_alloc。这两个函数不能按建议加 noexcept 了事（抛出即 terminate），也不能因抛出而跳过
// 关停——句柄泄漏比栈展开更糟，析构期传出异常同样是 terminate，故按库侧析构关停的既有口径逐点豁免。
// NOLINTBEGIN(bugprone-exception-escape)
auto StreamTransport::operator=(StreamTransport &&other) noexcept -> StreamTransport & {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

StreamTransport::~StreamTransport() { close(); }
// NOLINTEND(bugprone-exception-escape)

// ---- StreamListener -------------------------------------------------------------------

auto StreamListener::bind(const Endpoint &endpoint, int backlog, const StreamOptions &options)
    -> Result<StreamListener> {
    auto state = std::make_shared<detail::ListenerState>();
    state->options = options;
    if (!state->runtime.ok()) {
        return Result<StreamListener>{
            make_error(ErrorCode::NetRuntimeFailed, std::string{"network runtime could not be initialised"})};
    }
    auto opened = detail::open_nonblocking_listener(endpoint, backlog, options.native);
    if (!opened) {
        return Result<StreamListener>{opened.error()};
    }
    state->handle = opened.value();
    state->port = detail::bound_port(state->handle);
    auto driven = ensure_driver();
    if (!driven) {
        detail::close_handle(state->handle);
        state->handle = -1;
        return Result<StreamListener>{driven.error()};
    }
    return Result<StreamListener>{detail::TransportFactory::wrap_listener(std::move(state))};
}

auto StreamListener::accept(AcceptDone on_ready) -> void {
    if (!on_ready) {
        return;
    }
    if (!state_) {
        on_ready(Result<StreamTransport>{no_stream_reason("accept")});
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        if (state_->closed) {
            fired.emplace_back([done = std::move(on_ready), reason = cancelled_reason("accept")]() {
                done(Result<StreamTransport>{reason});
            });
        } else {
            state_->pending.push_back(std::move(on_ready));
            refresh_listener_interest(state_, fired);
        }
    }
    deliver(std::move(fired), state_->owner);
}

auto StreamListener::local_port() const -> std::uint16_t {
    if (!state_) {
        return 0U;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->port;
}

auto StreamListener::native_handle() const -> std::intptr_t {
    if (!state_) {
        return -1;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->handle;
}

auto StreamListener::is_valid() const -> bool {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return !state_->closed && state_->handle != -1;
}

auto StreamListener::close() -> void {
    if (!state_) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        close_listener(*state_, cancelled_reason("listener"), fired);
    }
    deliver(std::move(fired), state_->owner);
}

StreamListener::StreamListener(StreamListener &&other) noexcept : state_(std::move(other.state_)) {}

// 同 StreamTransport 的移动赋值与析构：关停不可因抛出而跳过。
// NOLINTBEGIN(bugprone-exception-escape)
auto StreamListener::operator=(StreamListener &&other) noexcept -> StreamListener & {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

StreamListener::~StreamListener() { close(); }
// NOLINTEND(bugprone-exception-escape)

namespace detail {

auto seed_read(StreamTransport &stream, std::span<const char> bytes) -> void {
    if (!stream.state_ || bytes.empty()) {
        return;  // 空态与零字节都不构成一次投递
    }
    Tasks fired;
    {
        std::scoped_lock lock(stream.state_->mutex);
        if (stream.state_->closed) {
            return;  // 已关闭的流没有读队列可接：未完成的读已由 close_stream 各收尾一次
        }
        stream.state_->read_ahead.insert(stream.state_->read_ahead.end(), bytes.begin(), bytes.end());
        drive_read(*stream.state_, fired);
        if (!stream.state_->closed) {
            refresh_stream_interest(stream.state_, fired);
        }
    }
    deliver(std::move(fired), stream.state_->owner);
}

}  // namespace detail

}  // namespace aurora::net

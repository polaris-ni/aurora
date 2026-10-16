// datagram.cpp —— 事件驱动数据报的编排面。
//
// 分层纪律：本文件**不出现任何平台 socket 头**，非阻塞系统调用一律借道 `net/detail/async_io.h`
// （CHG-006「平台 socket 头唯一落点是 socket.cpp」不变量）；等待由 `app::EventSourceRegistry` 的
// 驱动方负责（缺省是 `net::Engine` 的常驻 poll 线程，宿主自带等待集时是宿主自己）。
//
// 与流式传输同族的并发形态此处不重述（每套接字一枚互斥量、完成回调锁外触发并投回 owner 线程、
// 关注方向拉模型现算、登记代数作废尾批、收尾顺序「先注销事件源、再关句柄、最后投递回调」）。
// 数据报侧另有三条：
// 1. **一条消息一个槽**：一次 `receive()` 恰交付一条数据报，绝不跨消息拼装，也不把一条拆成两轮投递。
// 2. **接收缓冲恒按内核上界（65535 字节）自持**：Windows 在缓冲不足时让 `recvfrom` 失败并**整条丢弃**、
//    POSIX 静默截断，两种行为都不能对外，故先把整条读进库内，再按「调用方缓冲长度与
//    `max_datagram_bytes` 取小者」裁剪并置 `truncated`。代价是一次复制，换来同一输入在两条腿上有
//    同一输出（`SPEC.PLATFORM.CONSISTENT-BEHAVIOR.001`）。
// 3. **接收槽不排队字节**：槽里只有调用方缓冲的视图，本层不额外持有内存，故不给接收队列设上限；
//    没有待交付槽就不等待可读，数据报留在内核接收队列，队列满时由内核丢弃——那是 UDP 的固有语义，
//    本层不伪造排队、不为它编造错误码，也不设无界缓冲。

#include "aurora/net/datagram.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/error_codes.gen.h"
#include "aurora/core/result.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/detail/runtime.h"
#include "aurora/net/engine.h"
#include "aurora/state/async.h"

namespace aurora::net {

namespace detail {

/// @brief 一枚数据报套接字的全部可变状态（句柄、收发队列、事件源登记、串行化互斥量）。
struct DatagramState {
    DatagramState() = default;
    /// @brief 一枚排队的发送请求：载荷已复制进本层（这正是 `max_pending_bytes` 约束的那份缓冲）。
    struct PendingSend {
        std::vector<char> data;  ///< 载荷副本（空向量 = 一条合法的零长数据报）
        Endpoint target;  ///< 目标端点；固定对端形态下由 `send` 通道忽略
        DatagramSendDone done;  ///< 完成回调
    };

    /// @brief 一枚排队的接收槽：目标缓冲由调用方持有，须存活到对应回调抵达。
    struct PendingReceive {
        std::span<char> buffer;  ///< 目标缓冲
        DatagramDone done;  ///< 完成回调
    };

    /// @brief 析构兜底：句柄仍有效时归还（正常路径由 `close_datagram()` 先行处理）。
    ~DatagramState() { close_handle(handle); }

    /// @brief 不可复制：状态由 `std::shared_ptr` 独占语义管理，复制即出现两份互斥量与双重归还。
    DatagramState(const DatagramState &) = delete;
    /// @brief 不可复制赋值：同 `DatagramState(const DatagramState &)`。
    auto operator=(const DatagramState &) -> DatagramState & = delete;
    /// @brief 不可移动：持有 `std::mutex` 与事件源登记，移动会让已登记的回调指向错位的地址。
    DatagramState(DatagramState &&) = delete;
    /// @brief 不可移动赋值：同 `DatagramState(DatagramState &&)`。
    auto operator=(DatagramState &&) -> DatagramState & = delete;

    std::mutex mutex;  ///< 保护以下全部字段（锁内只做非阻塞系统调用）
    detail::Session runtime;  ///< Winsock 运行时归属（与句柄创建处的进程级钉住叠加）
    std::intptr_t handle = -1;  ///< 原生句柄（已置非阻塞）；-1 = 已归还
    std::uint64_t generation = 0;  ///< 事件源登记代数：旧登记的尾批据此作废
    std::size_t pending_send_bytes = 0;  ///< sends 里尚未整条进入内核的字节总数
    std::thread::id owner = std::this_thread::get_id();  ///< 完成回调的目标线程
    DatagramOptions options;  ///< 缓冲口径
    Cancellation cancellation;  ///< 本套接字的取消令牌
    std::vector<char> scratch;  ///< 恒按 UDP 载荷上界配足的接收缓冲（见文件头第 2 条口径）
    std::vector<Endpoint> groups;  ///< 已加入的组播组：未加入过即离开据此报错
    std::optional<app::EventSourceRegistration> registration;  ///< 当前登记（无关注时为空）
    std::deque<PendingSend> sends;  ///< 待发数据报（FIFO，整条进入内核才算完成）
    std::deque<PendingReceive> receives;  ///< 待满足的接收槽（FIFO，每枚只满足一条消息）
    Endpoint peer;  ///< 固定对端；无效端点 = 未连接形态（组播可用）
    app::WaitInterest interest{};  ///< 当前登记的关注方向
    // 以下五个窄字段刻意排在末尾（8 字节对齐的成员之后），省下的填充位正是本结构的全部空洞。
    AddressFamily family = AddressFamily::IPv4;  ///< 套接字地址族（发送目标与组播组的族判据）
    std::uint16_t port = 0;  ///< 实际绑定端口（入参 0 时为系统分配值）
    bool multicast_loopback = true;  ///< 本层记录的组播回环口径（内核缺省为回投）
    bool broadcast_allowed = false;  ///< 本层记录的广播允许口径（内核缺省为禁止）
    bool closed = false;  ///< 已关闭（含错误关闭与显式 close）
};

/// @brief 状态指针 → 公共句柄的装配口：`DatagramTransport` 的该构造函数是私有的，只对本类型敞开。
/// @note 只在实现侧定义，故不出现在任何公共头，也不占 API 预算。
class DatagramFactory {
  public:
    /// @brief 以数据报状态构造套接字句柄。
    /// @param state 已建立的数据报状态。
    /// @return 接管该状态的套接字句柄。
    static auto wrap(std::shared_ptr<DatagramState> state) -> DatagramTransport {
        return DatagramTransport{std::move(state)};
    }
};

}  // namespace detail

namespace {

/// @brief 一批待触发的回调（锁内收集、锁外按次序执行）。
using Tasks = std::vector<std::function<void()>>;

/// @brief 数据报状态的共享指针（本文件自由函数的书写形态）。
using DatagramPointer = std::shared_ptr<detail::DatagramState>;

/// @brief 构造「本层已取消」形态的失败原因（未完成的收发各以此收尾一次）。
/// @param what 操作描述，进 message 便于现场定位。
/// @return `net-cancelled` 错误。
auto cancelled_reason(std::string what) -> Error {
    return make_error(ErrorCode::NetCancelled, std::move(what) + " was cancelled before completion");
}

/// @brief 构造「句柄已不存在」形态的失败（空态句柄上的成员调用）。
/// @param what 操作描述。
/// @return `net-socket-failed` 错误。
auto no_socket_reason(std::string what) -> Error {
    return make_error(ErrorCode::NetSocketFailed, std::move(what) + " has no live socket");
}

/// @brief 构造「口径非法」形态的失败（背压与缓冲上限共用一枚码，差别写在 detail 里）。
/// @param detail_text 越界的具体口径与数值。
/// @return `net-buffer-limit` 错误。
auto buffer_limit_reason(std::string detail_text) -> Error {
    return make_error(ErrorCode::NetBufferLimit, ErrorParams{{"detail", std::move(detail_text)}});
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

/// @brief 把未完成的收发请求各以一次失败收尾，并归还句柄。
///
/// 顺序是「先注销、后关句柄」——反过来的话常驻引擎手里那份 `sources()` 快照会撞上系统已复用的
/// 句柄号，表现为下一次等待偶发 `POLLNVAL`。
/// @param state 套接字状态（调用方持锁）。
/// @param reason 未完成请求的失败原因。
/// @param tasks 出参：收集待触发的回调。
auto close_datagram(detail::DatagramState &state, const Error &reason, Tasks &tasks) -> void {
    if (state.closed) {
        return;
    }
    state.closed = true;
    state.registration.reset();  // 注销 = 停止产生新回调（§18-1）
    detail::close_handle(state.handle);
    state.handle = -1;
    // 以下两处把「以同一原因收尾」写成转入 Tasks（`std::function`）的可调用对象：本检查对任何无
    // noexcept 规格的可调用对象一律判为「不应抛出」，而闭包里的 Error/std::function 拷贝确实会分配内存。
    // 抛出只可能来自调用方回调自身，本层在派发边界不吞宿主异常（口径同流式传输的 close_stream）。
    // NOLINTBEGIN(bugprone-exception-escape)
    for (auto &request : state.sends) {
        auto done = std::move(request.done);
        tasks.emplace_back([done, reason]() { done(Result<std::size_t>{reason}); });
    }
    for (auto &slot : state.receives) {
        auto done = std::move(slot.done);
        tasks.emplace_back([done, reason]() { done(Result<Datagram>{reason}); });
    }
    // NOLINTEND(bugprone-exception-escape)
    state.sends.clear();
    state.receives.clear();
    state.pending_send_bytes = 0;
    state.groups.clear();
}

/// @brief 推进发送方向：把队首数据报整条送进内核（不拆分，故一次成功即一条完成）。
///
/// 失败**不关闭套接字**：数据报没有连接状态可破坏，一条消息的失败（向广播地址发送却没置允许位、
/// 目标不可达引发的 `ECONNREFUSED`）不该让整枚套接字退役，故失败恰一次落到该条请求的回调上、队列
/// 继续推进。这与流式传输「写失败即关流」是刻意的差别——那里失败意味着那条字节流已不可信。
/// @param state 套接字状态（调用方持锁）。
/// @param tasks 出参：收集待触发的回调。
auto drive_send(detail::DatagramState &state, Tasks &tasks) -> void {
    while (!state.sends.empty()) {
        if (state.closed) {
            return;
        }
        auto &front = state.sends.front();
        const std::span<const char> payload{front.data};
        // 固定对端形态必须用 `send`：Linux 让带地址的 `sendto` 在已连接的数据报套接字上以 `EISCONN`
        // 失败，Windows 却忽略该地址——同一调用两条腿两种结局，故分两个出口而不是传目标地址。
        const auto attempt = state.peer.is_valid() ? detail::try_send_datagram(state.handle, payload)
                                                   : detail::try_send_to(state.handle, payload, front.target);
        if (attempt.blocked) {
            return;  // 内核发送缓冲满：队首原样留着（记账不动），等下一次可写就绪
        }
        const std::size_t total = front.data.size();  // 零长数据报同样整条完成
        auto done = std::move(front.done);
        state.pending_send_bytes = state.pending_send_bytes > total ? state.pending_send_bytes - total : 0;
        state.sends.pop_front();
        if (attempt.failed()) {
            const auto error = *attempt.failure;
            // 回调转入 Tasks（std::function）故被本检查判为「不应抛出」，口径同 close_datagram。
            // NOLINTNEXTLINE(bugprone-exception-escape)
            tasks.emplace_back([done, error]() { done(Result<std::size_t>{error}); });
            continue;  // 只收尾这一条，后面的数据报照常推进
        }
        tasks.emplace_back([done, total]() { done(Result<std::size_t>{total}); });
    }
}

/// @brief 推进接收方向：一条数据报满足一枚接收槽，直到内核暂无数据报为止。
///
/// 交付副本落在**调用方**的缓冲里，故 `Datagram::bytes` 的存活期由调用方负责（与分帧器的 `Frame`
/// 方向相反）；被裁掉的尾部随 `truncated` 显式回报，**不回头补投**——下一条槽交付的是下一条消息。
/// 收取失败同样**不关闭套接字**（口径见 `drive_send`）：`ECONNREFUSED` 这类由上一条发送引发的
/// 内核报错只作废当前这一枚接收请求，随后各槽仍能收到真正的数据报。
/// @param state 套接字状态（调用方持锁）。
/// @param tasks 出参：收集待触发的回调。
auto drive_receive(detail::DatagramState &state, Tasks &tasks) -> void {
    while (!state.receives.empty()) {
        const auto attempt = detail::try_receive_datagram(state.handle, std::span<char>(state.scratch));
        if (attempt.blocked) {
            return;  // 暂无数据报：等下一次可读就绪
        }
        auto &front = state.receives.front();
        auto done = std::move(front.done);
        const std::span<char> buffer = front.buffer;  // 出队即销毁该节点，故先把缓冲视图取出来
        state.receives.pop_front();
        if (attempt.failed()) {
            const auto error = *attempt.failure;
            // 回调转入 Tasks（std::function）故被本检查判为「不应抛出」，口径同 close_datagram。
            // NOLINTNEXTLINE(bugprone-exception-escape)
            tasks.emplace_back([done, error]() { done(Result<Datagram>{error}); });
            continue;  // 只收尾这一枚请求，后面的槽照常等下一条数据报
        }
        const std::size_t limit = std::min({buffer.size(), state.options.max_datagram_bytes, attempt.bytes});
        std::copy_n(state.scratch.begin(), limit, buffer.begin());
        Datagram message;
        message.bytes = buffer.first(limit);
        message.source = attempt.source.value_or(Endpoint{});
        message.truncated = attempt.bytes > limit;
        tasks.emplace_back([done, message]() { done(Result<Datagram>{message}); });
    }
}

/// @brief 让套接字的事件源登记与当前关注方向一致（必要时重登记）。
/// @param state 套接字状态（调用方持锁）。
/// @param tasks 出参：登记失败时收集失败回调。
auto refresh_datagram_interest(const DatagramPointer &state, Tasks &tasks) -> void;

/// @brief 一次就绪通知的处理入口（引擎线程或宿主 dispatch 线程进入）。
/// @param state 套接字状态。
/// @param generation 触发本次回调的登记代数。
/// @param ready 就绪方向。
auto on_datagram_ready(const DatagramPointer &state, std::uint64_t generation, const app::SourceReady &ready) -> void {
    Tasks fired;
    {
        std::scoped_lock lock(state->mutex);
        if (state->closed || state->generation != generation) {
            return;  // 已关闭，或旧登记的尾批
        }
        if (ready.writable || ready.error || ready.hangup) {
            drive_send(*state, fired);
        }
        if (!state->closed && (ready.readable || ready.error || ready.hangup)) {
            drive_receive(*state, fired);
        }
        if (!state->closed) {
            refresh_datagram_interest(state, fired);
        }
    }
    deliver(std::move(fired), state->owner);
}

/// @brief 计算套接字当前想等待的方向（拉模型：队列现算，不存在「空转消费一次就绪」）。
/// @param state 套接字状态（调用方持锁）。
/// @return 关注方向。
auto datagram_interest(const detail::DatagramState &state) -> app::WaitInterest {
    app::WaitInterest wanted;
    wanted.read = !state.closed && !state.receives.empty();
    wanted.write = !state.closed && !state.sends.empty();
    return wanted;
}

auto refresh_datagram_interest(const DatagramPointer &state, Tasks &tasks) -> void {
    const auto wanted = datagram_interest(*state);
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
    std::weak_ptr<detail::DatagramState> witness = state;
    auto registered = app::EventSourceRegistry::instance().register_source(
        state->handle, wanted, [witness, generation](const app::SourceReady &ready) {
            const auto held = witness.lock();
            if (held) {
                on_datagram_ready(held, generation, ready);
            }
        });
    if (!registered) {
        close_datagram(*state, registered.error(), tasks);
        state->interest = app::WaitInterest{};
        return;
    }
    state->registration = std::move(registered.value());
    state->interest = wanted;
}

/// @brief 取消 / 关闭钩子的公共形态：关套接字并把结果投递回 owner。
/// @param witness 套接字状态的弱观测。
/// @param reason 失败原因。
auto close_and_deliver(const std::weak_ptr<detail::DatagramState> &witness, const Error &reason) -> void {
    const auto held = witness.lock();
    if (!held) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(held->mutex);
        close_datagram(*held, reason, fired);
    }
    deliver(std::move(fired), held->owner);
}

/// @brief 确保本腿有驱动方在等待这些句柄（缺省引擎驱动时把引擎叫醒）。
/// @return 可为 true；常驻引擎起不来时携原因。
auto ensure_driver() -> Result<void> {
    if (app::EventSourceRegistry::instance().host_driven()) {
        return Result<void>{};
    }
    return Engine::instance().post([]() {});
}

/// @brief 建立一枚已绑定、非阻塞、接收缓冲配足的数据报状态（两种工厂的公共腿）。
///
/// 失败路径不需要手工归还句柄：状态出作用域时其析构即关闭已拿到的句柄。
/// @param local 本地端点（`connect` 形态取对端族的通配地址与端口 0）。
/// @param options 缓冲口径。
/// @return 成功为新建状态；失败为口径非法 / 端点不合用 / 运行时或系统调用失败 / 本腿无驱动方。
auto make_datagram_state(const Endpoint &local, const DatagramOptions &options) -> Result<DatagramPointer> {
    constexpr std::size_t scratch_bytes = 65535U;  // UDP 载荷的内核上界，两条腿同值
    if (options.max_datagram_bytes < 1U || options.max_datagram_bytes > scratch_bytes) {
        const auto size = std::to_string(options.max_datagram_bytes);
        return Result<DatagramPointer>{buffer_limit_reason("datagram size cap " + size + " is outside 1..65535")};
    }
    if (options.max_pending_bytes < 1U) {
        return Result<DatagramPointer>{buffer_limit_reason("pending datagram byte cap is zero")};
    }
    if (!local.is_valid() || local.is_local_path()) {
        return Result<DatagramPointer>{
            make_error(ErrorCode::NetEndpointInvalid, "a datagram socket needs an IPv4 or IPv6 local endpoint")};
    }
    auto state = std::make_shared<detail::DatagramState>();
    state->options = options;
    state->family = local.family();
    if (!state->runtime.ok()) {
        return Result<DatagramPointer>{
            make_error(ErrorCode::NetRuntimeFailed, std::string{"network runtime could not be initialised"})};
    }
    auto opened = detail::open_nonblocking_datagram(local);
    if (!opened) {
        return Result<DatagramPointer>{opened.error()};
    }
    state->handle = opened.value();
    state->port = detail::bound_port(state->handle);
    state->scratch.assign(scratch_bytes, '\0');
    auto driven = ensure_driver();
    if (!driven) {
        return Result<DatagramPointer>{driven.error()};
    }
    // 取消钩子在状态就绪后立即登记：此后任何 `cancel()` 都走「先注销事件源、再关句柄、最后投递回调」，
    // 落实 §18-4 的「先释放 fd」硬约束。钩子借状态里那份令牌登记，且在锁外调用以免就地执行时自锁。
    std::weak_ptr<detail::DatagramState> witness = state;
    state->cancellation.on_cancel([witness]() { close_and_deliver(witness, cancelled_reason("datagram socket")); });
    return Result<DatagramPointer>{state};
}

/// @brief 发送目标的合用性判据（本机 IPC 形态、端口 0、族不符都在发起系统调用前即拒）。
/// @param state 套接字状态（调用方持锁）。
/// @param target 目标端点。
/// @return 不合用时携 `net-endpoint-invalid`；合用时为空值。
auto target_problem(const detail::DatagramState &state, const Endpoint &target) -> std::optional<Error> {
    if (!target.is_valid() || target.is_local_path()) {
        return make_error(ErrorCode::NetEndpointInvalid, "a datagram target needs an IPv4 or IPv6 endpoint");
    }
    if (target.port() == 0) {
        return make_error(ErrorCode::NetEndpointInvalid, "a datagram target endpoint needs a non-zero port");
    }
    if (target.family() != state.family) {
        return make_error(ErrorCode::NetEndpointInvalid, "datagram target family differs from the socket family");
    }
    return std::nullopt;
}

/// @brief 组播关注与固定对端的互斥判据（提案 §18-3，两种成员操作共用）。
/// @param state 套接字状态（调用方持锁）。
/// @return 冲突时携 `net-option-conflict`；可用时为空值。
auto multicast_problem(const detail::DatagramState &state) -> std::optional<Error> {
    if (!state.peer.is_valid()) {
        return std::nullopt;
    }
    return make_error(ErrorCode::NetOptionConflict,
                      ErrorParams{{"detail",
                                   "a connected datagram socket receives only from its peer, so multicast membership "
                                   "would never deliver; use a second DatagramTransport for the group"}});
}

/// @brief 把一条待发数据报排进队列（两种发送入口的公共腿）。
///
/// 六条回执分支都在锁内定形、锁外派发：入队与关注刷新是一次转换，不存在「已入队但没登记」的中间态
/// 暴露给另一个线程。
/// @param state 套接字状态。
/// @param data 一条数据报的载荷（可为空 = 一条零长数据报）。
/// @param target 目标端点（固定对端形态下仅作族与端口校验，真正发送走 `send` 通道）。
/// @param on_done 完成回调。
/// @param connected 为真时来自 `send()`（要求本套接字已固定对端）。
auto queue_send(const DatagramPointer &state, std::span<const char> data, const Endpoint &target,
                DatagramSendDone on_done, bool connected) -> void {
    Tasks fired;
    {
        std::scoped_lock lock(state->mutex);
        // 各条分支都把回调转入 Tasks（无 noexcept 规格的可调用对象），本检查一律判为「不应抛出」；
        // 抛出只可能来自调用方回调，本层不在派发边界吞它（口径同流式传输的 read / write）。
        // NOLINTBEGIN(bugprone-exception-escape)
        if (state->closed) {
            fired.emplace_back([done = std::move(on_done), reason = cancelled_reason("queued datagram")]() {
                done(Result<std::size_t>{reason});
            });
        } else if (!connected && state->peer.is_valid()) {
            const auto reason =
                make_error(ErrorCode::NetOptionConflict,
                           ErrorParams{{"detail",
                                        "this datagram socket has a fixed peer, so send_to() is not available; "
                                        "call send() instead"}});
            fired.emplace_back([done = std::move(on_done), reason]() { done(Result<std::size_t>{reason}); });
        } else if (connected && !state->peer.is_valid()) {
            const auto reason =
                make_error(ErrorCode::NetEndpointInvalid,
                           ErrorParams{{"detail",
                                        "this datagram socket has no fixed peer; call send_to() with a target "
                                        "endpoint"}});
            fired.emplace_back([done = std::move(on_done), reason]() { done(Result<std::size_t>{reason}); });
        } else if (const auto problem = target_problem(*state, target)) {
            const Error &reason = *problem;
            fired.emplace_back([done = std::move(on_done), reason]() { done(Result<std::size_t>{reason}); });
        } else if (data.size() > state->options.max_datagram_bytes) {
            const auto size = std::to_string(state->options.max_datagram_bytes);
            fired.emplace_back([done = std::move(on_done), size, bytes = std::to_string(data.size())]() {
                done(Result<std::size_t>{buffer_limit_reason("datagram of " + bytes + " bytes exceeds cap " + size)});
            });
        } else if (state->pending_send_bytes + data.size() > state->options.max_pending_bytes) {
            const auto size = std::to_string(state->options.max_pending_bytes);
            fired.emplace_back([done = std::move(on_done), size]() {
                done(Result<std::size_t>{buffer_limit_reason("pending datagram cap " + size)});
            });
        } else {
            detail::DatagramState::PendingSend request;
            request.data.assign(data.begin(), data.end());  // 零长数据报同样入队
            request.target = target;
            request.done = std::move(on_done);
            state->pending_send_bytes += data.size();
            state->sends.push_back(std::move(request));
            refresh_datagram_interest(state, fired);
        }
        // NOLINTEND(bugprone-exception-escape)
    }
    deliver(std::move(fired), state->owner);
}

}  // namespace

// ---- DatagramTransport ----------------------------------------------------------------

auto DatagramTransport::bind(const Endpoint &local, const DatagramOptions &options) -> Result<DatagramTransport> {
    auto state = make_datagram_state(local, options);
    if (!state) {
        return Result<DatagramTransport>{state.error()};
    }
    return Result<DatagramTransport>{detail::DatagramFactory::wrap(state.value())};
}

auto DatagramTransport::connect(const Endpoint &peer, const DatagramOptions &options) -> Result<DatagramTransport> {
    if (!peer.is_valid() || peer.is_local_path()) {
        return Result<DatagramTransport>{
            make_error(ErrorCode::NetEndpointInvalid, "a datagram peer needs an IPv4 or IPv6 endpoint")};
    }
    if (peer.port() == 0) {
        return Result<DatagramTransport>{
            make_error(ErrorCode::NetEndpointInvalid, "a datagram peer endpoint needs a non-zero port")};
    }
    // 固定对端不改变本地绑定形态：取对端族的通配地址与端口 0，由系统分配实端口。
    auto state = make_datagram_state(Endpoint::any(peer.family() != AddressFamily::IPv4, 0), options);
    if (!state) {
        return Result<DatagramTransport>{state.error()};
    }
    std::scoped_lock lock(state.value()->mutex);
    auto associated = detail::associate_datagram_peer(state.value()->handle, peer);
    if (!associated) {
        return Result<DatagramTransport>{associated.error()};
    }
    state.value()->peer = peer;
    return Result<DatagramTransport>{detail::DatagramFactory::wrap(state.value())};
}

auto DatagramTransport::is_open() const -> bool {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return !state_->closed;
}

auto DatagramTransport::native_handle() const -> std::intptr_t {
    if (!state_) {
        return -1;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->handle;
}

auto DatagramTransport::local_port() const -> std::uint16_t {
    if (!state_) {
        return 0U;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->port;
}

auto DatagramTransport::peer() const -> Endpoint {
    if (!state_) {
        return Endpoint{};
    }
    std::scoped_lock lock(state_->mutex);
    return state_->peer;
}

auto DatagramTransport::send_to(std::span<const char> data, const Endpoint &target, DatagramSendDone on_done) -> void {
    if (!on_done) {
        return;  // 没有回调就没有承诺：不建副本、不留悬挂的发送
    }
    if (!state_) {
        on_done(Result<std::size_t>{no_socket_reason("datagram send")});
        return;
    }
    queue_send(state_, data, target, std::move(on_done), false);
}

auto DatagramTransport::send(std::span<const char> data, DatagramSendDone on_done) -> void {
    if (!on_done) {
        return;
    }
    if (!state_) {
        on_done(Result<std::size_t>{no_socket_reason("datagram send")});
        return;
    }
    Endpoint fixed;
    {
        std::scoped_lock lock(state_->mutex);
        fixed = state_->peer;
    }
    // 「没有固定对端」的判据与入队在同一把锁里复核（这里的快照只用于取目标，队列腿自己会再判一次），
    // 故不存在「检查后被关闭或改状态」的窗口。
    queue_send(state_, data, fixed, std::move(on_done), true);
}

auto DatagramTransport::receive(std::span<char> buffer, DatagramDone on_done) -> void {
    if (!on_done) {
        return;
    }
    if (!state_) {
        on_done(Result<Datagram>{no_socket_reason("datagram receive")});
        return;
    }
    if (buffer.empty()) {
        // 零字节在数据报侧是「一条空消息」的形态，不能同时充当「没给缓冲」，故空缓冲只能是用法错误。
        on_done(Result<Datagram>{buffer_limit_reason("receive target buffer is empty")});
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        // 回调转入 Tasks 即被本检查判为「不应抛出」（口径同 send_to）。
        // NOLINTBEGIN(bugprone-exception-escape)
        if (state_->closed) {
            fired.emplace_back([done = std::move(on_done), reason = cancelled_reason("queued datagram receive")]() {
                done(Result<Datagram>{reason});
            });
        } else {
            detail::DatagramState::PendingReceive slot;
            slot.buffer = buffer;
            slot.done = std::move(on_done);
            state_->receives.push_back(std::move(slot));
            refresh_datagram_interest(state_, fired);
        }
        // NOLINTEND(bugprone-exception-escape)
    }
    deliver(std::move(fired), state_->owner);
}

auto DatagramTransport::join_multicast_group(const Endpoint &group) -> Result<void> {
    if (!state_) {
        return Result<void>{no_socket_reason("multicast membership")};
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return Result<void>{no_socket_reason("closed datagram socket")};
    }
    if (const auto problem = multicast_problem(*state_)) {
        return Result<void>{*problem};
    }
    auto joined = detail::set_multicast_membership(state_->handle, group, true);
    if (joined) {
        state_->groups.push_back(group);
    }
    return joined;
}

auto DatagramTransport::leave_multicast_group(const Endpoint &group) -> Result<void> {
    if (!state_) {
        return Result<void>{no_socket_reason("multicast membership")};
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return Result<void>{no_socket_reason("closed datagram socket")};
    }
    if (const auto problem = multicast_problem(*state_)) {
        return Result<void>{*problem};
    }
    const auto joined = std::find(state_->groups.begin(), state_->groups.end(), group);
    if (joined == state_->groups.end()) {
        // 本套接字没加入过这个组：让内核去猜「别的套接字加过」只会报出一条与现场无关的原生码。
        return Result<void>{make_error(
            ErrorCode::NetOptionFailed,
            ErrorParams{{"detail", "multicast group was never joined on this socket: " + group.to_string()}})};
    }
    auto left = detail::set_multicast_membership(state_->handle, group, false);
    if (left) {
        state_->groups.erase(joined);
    }
    return left;
}

auto DatagramTransport::set_multicast_loopback(bool enabled) -> Result<void> {
    if (!state_) {
        return Result<void>{no_socket_reason("multicast loopback option")};
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return Result<void>{no_socket_reason("closed datagram socket")};
    }
    auto applied = detail::set_multicast_loopback(state_->handle, enabled);
    if (applied) {
        state_->multicast_loopback = enabled;  // 口径只在成功时改：失败时 getters 仍报生效值
    }
    return applied;
}

auto DatagramTransport::multicast_loopback() const -> bool {
    if (!state_) {
        return true;  // 空态没有套接字可查，回报内核缺省值而不是伪造一次失败
    }
    std::scoped_lock lock(state_->mutex);
    return state_->multicast_loopback;
}

auto DatagramTransport::set_broadcast_allowed(bool enabled) -> Result<void> {
    if (!state_) {
        return Result<void>{no_socket_reason("broadcast option")};
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->closed) {
        return Result<void>{no_socket_reason("closed datagram socket")};
    }
    auto applied = detail::set_broadcast_allowed(state_->handle, enabled);
    if (applied) {
        state_->broadcast_allowed = enabled;
    }
    return applied;
}

auto DatagramTransport::broadcast_allowed() const -> bool {
    if (!state_) {
        return false;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->broadcast_allowed;
}

auto DatagramTransport::pending_send_bytes() const -> std::size_t {
    if (!state_) {
        return 0U;
    }
    std::scoped_lock lock(state_->mutex);
    return state_->pending_send_bytes;
}

auto DatagramTransport::close() -> void {
    if (!state_) {
        return;
    }
    Tasks fired;
    {
        std::scoped_lock lock(state_->mutex);
        close_datagram(*state_, cancelled_reason("datagram socket"), fired);
    }
    deliver(std::move(fired), state_->owner);
}

auto DatagramTransport::cancellation() const -> Cancellation {
    if (!state_) {
        return Cancellation{};
    }
    std::scoped_lock lock(state_->mutex);
    return state_->cancellation;
}

DatagramTransport::DatagramTransport(DatagramTransport &&other) noexcept : state_(std::move(other.state_)) {}

// 移动赋值与析构都要先关掉自有套接字：`close()` 的收尾链里有一次 Error 与若干 std::function 构造，
// 理论上能 bad_alloc。这两个函数不能按建议加 noexcept 了事（抛出即 terminate），也不能因抛出而跳过
// 关停——句柄泄漏比栈展开更糟，析构期传出异常同样是 terminate，故按库侧析构关停的既有口径逐点豁免。
// NOLINTBEGIN(bugprone-exception-escape)
auto DatagramTransport::operator=(DatagramTransport &&other) noexcept -> DatagramTransport & {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

DatagramTransport::~DatagramTransport() { close(); }
// NOLINTEND(bugprone-exception-escape)

}  // namespace aurora::net

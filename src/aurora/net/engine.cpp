// Aurora net — 自带 poll 线程引擎（CHG-010 底座切片：事件驱动形态的执行载体）。
//
// 本文件把 §8.8.3 的就绪基元 `wait_ready` 包成**常驻循环**，并接上主线程完成投递：
//   - 一条引擎线程按「跑已到期任务 → 快照事件源 → 等待就绪 → 投递回调」的节奏运转；
//   - 等待窗口由「最早到期定时」与空闲上限共同决定，唤醒通道负责把新任务/新事件源立刻送到
//     循环眼前（不在 UI 线程等待，也不忙等）；
//   - 就绪回调经 `app::EventSourceRegistry` 的 `Dispatcher` 投递，引擎启动时装入
//     `aurora::detail::post_to_main` 形态——即 §1 不变量所指「与 `Task<T>::set_main_poster`
//     同一条投递器」，宿主已装则不改。
//
// 唤醒通道刻意**不引入新的平台代码**：它是一对回环 TCP 流（`blocking::Listener` +
// `blocking::StreamSocket`，POSIX 与 Winsock 皆可 poll），因此本文件不出现 `poll.h` / `winsock2.h`
// ——CHG-006 的「平台 socket 头唯一落点是 `src/aurora/net/socket.cpp`」不变量继续成立。

#include "aurora/net/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/app/event_source_registry.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"
#include "aurora/net/blocking.h"
#include "aurora/net/detail/async_io.h"
#include "aurora/net/detail/engine_override.h"
#include "aurora/net/detail/runtime.h"
#include "aurora/net/detail/tls_backend.h"
#include "aurora/net/poll.h"
#include "aurora/state/async.h"

namespace aurora::net {
namespace {

/// @brief 空闲等待窗口：唤醒通道正常时本轮不会睡满它，它只是「通知丢失」的延迟上界。
constexpr std::chrono::milliseconds AURORA_NET_ENGINE_IDLE_QUANTUM{200};

/// @brief 最小等待窗口：最早到期时刻与 `now` 抖动时不退化成忙等。
constexpr std::chrono::milliseconds AURORA_NET_ENGINE_MIN_QUANTUM{1};

/// @brief 唤醒通道单轮读取字节数。
///
/// 只在 `POLLIN` 报可读后读，故本次读取必有数据、不会阻塞；残余字节让下一轮等待立刻返回可读，
/// 因此「一次读干净」不是必需项——不做计数也不会有丢唤醒或忙等。
constexpr std::size_t AURORA_NET_ENGINE_WAKE_CHUNK = 8;

/// @brief 唤醒通道上的字节内容（内容无意义，可读即足以打断等待）。
constexpr char AURORA_NET_ENGINE_WAKE_BYTE = 'n';

/// @brief 时钟别名：全部到期判定统一用单调钟，不受墙钟调整影响。
using EngineClock = std::chrono::steady_clock;

/// @brief 一条到期定时任务：到期时刻 + 执行体。
struct TimerEntry {
    EngineClock::time_point deadline;  ///< 到期时刻（steady_clock）
    std::function<void()> task;  ///< 到点执行体
};

/// @brief 由最早到期定时算出本轮等待窗口：夹进 [最小窗口, 空闲窗口]。
/// @param timers 当前登记的定时任务（调用方持锁）。
/// @param now 本轮起始时刻。
/// @return 传给 `wait_ready` 的时长；无定时即空闲上限。
auto compute_wait_window(const std::vector<TimerEntry> &timers, EngineClock::time_point now)
    -> std::chrono::milliseconds {
    if (timers.empty()) {
        return AURORA_NET_ENGINE_IDLE_QUANTUM;
    }
    auto earliest = timers.front().deadline;
    for (const auto &timer : timers) {
        if (timer.deadline < earliest) {
            earliest = timer.deadline;
        }
    }
    if (earliest <= now) {
        return AURORA_NET_ENGINE_MIN_QUANTUM;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);
    if (remaining <= AURORA_NET_ENGINE_MIN_QUANTUM) {
        return AURORA_NET_ENGINE_MIN_QUANTUM;
    }
    return remaining < AURORA_NET_ENGINE_IDLE_QUANTUM ? remaining : AURORA_NET_ENGINE_IDLE_QUANTUM;
}

/// @brief 把就绪方向从 net 形态搬到注册口形态（四向同名、逐值照抄）。
/// @param outcome 单个句柄的一次就绪结果。
/// @return 注册口的就绪回报。
auto registry_ready(const PollOutcome &outcome) -> app::SourceReady {
    return app::SourceReady{
        .readable = outcome.readable, .writable = outcome.writable, .error = outcome.error, .hangup = outcome.hangup};
}

/// @brief 自带 poll 线程引擎：一条常驻循环 + 任务队列 + 到期定时器 + 事件源驱动。
/// @note Thread: thread-safe（队列/定时/唤醒通道一律持锁访问；任务体只在引擎线程执行）
/// @note Side-effects: 启动即创建回环唤醒通道与一条线程，并写入进程级注册口接缝
class PollEngine final : public Engine {
  public:
    /// @brief 构造：仅经 `shared_engine()` 的单例存储使用（声明了拷贝删除，故默认构造需显式给出）。
    PollEngine() = default;

    PollEngine(const PollEngine &) = delete;
    auto operator=(const PollEngine &) -> PollEngine & = delete;
    PollEngine(PollEngine &&) = delete;
    auto operator=(PollEngine &&) -> PollEngine & = delete;

    /// @brief 析构：等价一次 `shutdown()`——**仅显式停机路径会走到这里**，单例存储刻意不析构（见
    ///        `shared_engine()`），故静态拆除阶段不会执行线程 join 与句柄回收。
    ~PollEngine() override { shutdown(); }

    [[nodiscard]] auto name() const -> std::string_view override { return "poll"; }

    [[nodiscard]] auto capabilities() const -> Capabilities override {
        Capabilities caps;
        caps.tcp = true;  // 本腿承载任意主机字节流（回环之外亦可；对外 API 随传输切片落地）
        caps.listens = true;
        caps.streaming = true;  // native 不受 ReadableStream 支持度约束（§18-6 的平台限制位）
        caps.custom_headers = true;  // native 无 CORS 预检约束
        caps.event_source = app::EventSourceRegistry::instance().event_source_supported();
        caps.udp = true;  // 数据报形态随 `DatagramTransport` 落地（能力位不先于实现点亮）
        caps.dns = true;  // 名称解析随 `net::Resolver` 落地（同一句「能力位不得先于实现点亮」的补账）
        // 本机 IPC 位由运行期探测决定：头文件提供 `AF_UNIX` 与真的能开出这个地址族是两件事，
        // Windows 上「编译过」只说明前一件。
        caps.local_ipc = detail::local_ipc_supported();
        // TLS 位与「`handshake()` 真的能成功」同源：由被编译进来的那个后端 TU 定义探测口，
        // 没有后端时 `tls_unavailable.cpp` 答 false，调用方据此降级而不必去分支一条不存在的路径。
        caps.tls = detail::tls_backend_available();
        return caps;
    }

    [[nodiscard]] auto post(std::function<void()> task) -> Result<void> override {
        if (!task) {
            return Result<void>{};  // 空任务：无事可做，不报错也不占队列
        }
        auto started = ensure_started();
        if (!started) {
            return started.error();
        }
        std::scoped_lock lock(mutex_);
        if (stop_requested_) {
            return make_error(ErrorCode::NetPlatformUnavailable, "post: engine already shutting down");
        }
        tasks_.push_back(std::move(task));
        return wake_locked();
    }

    [[nodiscard]] auto post_after(std::chrono::milliseconds delay, std::function<void()> task)
        -> Result<void> override {
        if (!task) {
            return Result<void>{};
        }
        auto started = ensure_started();
        if (!started) {
            return started.error();
        }
        std::scoped_lock lock(mutex_);
        if (stop_requested_) {
            return make_error(ErrorCode::NetPlatformUnavailable, "post_after: engine already shutting down");
        }
        const auto window = delay.count() > 0 ? delay : std::chrono::milliseconds{0};
        timers_.push_back(TimerEntry{.deadline = EngineClock::now() + window, .task = std::move(task)});
        return wake_locked();
    }

    [[nodiscard]] auto running() const -> bool override { return running_.load(); }

    auto shutdown() -> void override {
        std::thread worker;
        {
            std::scoped_lock lock(mutex_);
            if (!running_.load()) {
                return;  // 幂等：未启动
            }
            stop_requested_ = true;
            (void)wake_locked();  // 让阻塞在等待里的循环立刻走到停机腿
            worker = std::move(thread_);
        }
        if (worker.joinable()) {
            worker.join();  // 不持锁 join：循环收尾要取同一枚锁
        }
        std::scoped_lock lock(mutex_);
        // 接缝回收只清自己装的那一枚（`Dispatcher` 可能是宿主装的，不动）。
        app::EventSourceRegistry::instance().set_change_notify({});
        wake_handle_ = -1;
        wake_read_.close();
        wake_write_.close();
        session_.reset();
        // 并发第二调用者：线程已被上一位移走且不可 join，此处只做同样的幂等回收。
        running_.store(false);
    }

  private:
    /// @brief 惰性启动引擎线程：装配运行时会话、回环唤醒通道与常驻循环（已启动时为 no-op）。
    ///
    /// 整段持 `mutex_`：唤醒通道是单所有权 socket，构建期间不允许 `wake_locked()` / `drain_wake()`
    /// 并发改写。线程创建放在同一段锁内，循环体自然从「通道已就绪」的状态起步。
    /// @return 已就绪为 OK；唤醒通道建不起来为对应 `net-*` 码（不静默降级）。
    [[nodiscard]] auto ensure_started() -> Result<void> {
        std::scoped_lock lock(mutex_);
        if (running_.load()) {
            return Result<void>{};
        }

        session_.emplace();
        if (!session_->ok()) {
            session_.reset();
            return make_error(ErrorCode::NetRuntimeFailed, "engine: platform network runtime unavailable");
        }
        // 唤醒通道 = 一对回环 TCP 流：两平台的 poll 族都认得 socket 句柄，且不需要任何新平台代码。
        auto bound = blocking::Listener::listen_loopback(0, 4);
        if (!bound) {
            session_.reset();
            return bound.error();
        }
        auto listener = std::move(bound.value());
        auto connector = blocking::StreamSocket::connect_loopback(listener.port());
        if (!connector) {
            session_.reset();
            return connector.error();
        }
        auto accepted = listener.accept();
        if (!accepted) {
            session_.reset();
            return accepted.error();
        }
        wake_write_ = std::move(connector.value());
        wake_read_ = std::move(accepted.value());
        wake_handle_ = wake_read_.native_handle();

        stop_requested_ = false;
        running_.store(true);
        thread_ = std::thread([this] { run(); });

        // 接缝装配：投递器只在宿主未装时补上；变动通知归常驻等待方（即本引擎）。
        auto &registry = app::EventSourceRegistry::instance();
        if (!registry.has_dispatcher()) {
            registry.set_dispatcher([](std::function<void()> fn) { aurora::detail::post_to_main(std::move(fn)); });
        }
        registry.set_change_notify([this] { on_sources_changed(); });
        return Result<void>{};
    }

    /// @brief 事件源表变动的响应：唤醒循环立刻重取快照（登记与注销都需要，判据见注册口 §8.8.2）。
    auto on_sources_changed() -> void {
        std::scoped_lock lock(mutex_);
        if (running_.load() && !stop_requested_) {
            (void)wake_locked();
        }
    }

    /// @brief 写入一枚唤醒字节。**调用方须持 `mutex_`**（唤醒通道是单所有权 socket）。
    ///
    /// 唤醒失败不改写已受理的事实：任务仍在队列里，最迟在一个空闲窗口后被执行；但通道已坏，
    /// 调用方应当知情，故原样把错误上抛而非吞掉。
    /// @return 写入成功为 OK；否则为 `net-send-failed`。
    [[nodiscard]] auto wake_locked() -> Result<void> {
        const char nudge = AURORA_NET_ENGINE_WAKE_BYTE;
        return wake_write_.write_all(std::span<const char>(&nudge, 1));
    }

    /// @brief 排空唤醒通道上已可读的字节（单轮一块；残余字节由下一轮等待立刻回报）。
    auto drain_wake() -> void {
        std::array<char, AURORA_NET_ENGINE_WAKE_CHUNK> buffer{};
        (void)wake_read_.read_some(std::span(buffer));  // 字节内容无意义；失败（含竞态空读）一并忽略
    }

    /// @brief 引擎线程主体：到期任务 → 等待就绪 → 投递回调，直到收到停机请求。
    auto run() -> void {
        while (true) {
            std::vector<std::function<void()>> due;
            auto window = AURORA_NET_ENGINE_IDLE_QUANTUM;
            bool stopping = false;
            {
                std::scoped_lock lock(mutex_);
                // 已受理的任务与已到期的定时一律先跑完，停机也不例外（不丢已承诺的工作）。
                due.reserve(tasks_.size());
                for (auto &task : tasks_) {
                    due.push_back(std::move(task));
                }
                tasks_.clear();
                const auto now = EngineClock::now();
                for (auto it = timers_.begin(); it != timers_.end();) {
                    if (it->deadline <= now) {
                        due.push_back(std::move(it->task));
                        it = timers_.erase(it);
                    } else {
                        ++it;
                    }
                }
                stopping = stop_requested_;
                window = compute_wait_window(timers_, now);
            }
            for (auto &task : due) {
                task();
            }
            if (stopping) {
                return;
            }
            poll_once(window);
        }
    }

    /// @brief 一轮等待：唤醒通道排在快照首位，其余按注册口令牌一一关联，就绪项交回注册口投递。
    /// @param window 本轮等待上限。
    auto poll_once(std::chrono::milliseconds window) -> void {
        auto &registry = app::EventSourceRegistry::instance();
        // 宿主自带等待集在驱动时本引擎让位（§8.8.4 投递纪律 ③：一条事件源只由一个驱动方等待），
        // 否则同一次就绪会被两边各投一次。此处只等唤醒通道，任务与定时器照常按窗口推进。
        const auto sources = registry.host_driven() ? std::vector<app::WaitSource>{} : registry.sources();

        std::vector<PollDescriptor> want;
        want.reserve(sources.size() + 1);
        want.push_back(PollDescriptor{.handle = wake_handle_, .want_read = true, .want_write = false});
        for (const auto &source : sources) {
            want.push_back(PollDescriptor{
                .handle = source.handle, .want_read = source.interest.read, .want_write = source.interest.write});
        }

        auto polled = wait_ready(want, window);
        if (!polled) {
            // 瞬态失败：注销与等待同帧发生时，宿主可能已关闭快照里的句柄。下一轮重取快照即可，
            // 循环不因此终止（等待窗口有上限，不会变成紧转）。
            return;
        }
        const auto &outcomes = polled.value();
        if (outcomes[0].readable || outcomes[0].error || outcomes[0].hangup) {
            drain_wake();
        }

        std::vector<app::ReadySource> ready;
        for (std::size_t i = 1; i < outcomes.size(); ++i) {
            const auto &outcome = outcomes[i];
            if (outcome.readable || outcome.writable || outcome.error || outcome.hangup) {
                const auto slot = i - 1;  // 首位是唤醒通道，事件源整体后移一位
                ready.push_back(app::ReadySource{.token = sources[slot].token, .ready = registry_ready(outcome)});
            }
        }
        if (!ready.empty()) {
            registry.dispatch(ready);
        }
    }

    std::mutex mutex_;  ///< 保护队列、定时表、停机标志与唤醒通道
    std::deque<std::function<void()>> tasks_;  ///< 待执行任务（FIFO）
    std::vector<TimerEntry> timers_;  ///< 到期定时表（规模小，线性扫描）
    bool stop_requested_ = false;  ///< 停机请求（持锁读写）
    std::atomic<bool> running_{false};  ///< 引擎线程是否在跑（`running()` 无锁读）
    std::thread thread_;  ///< 常驻循环线程
    std::optional<detail::Session> session_;  ///< 引擎生命周期内的运行时引用（Windows 侧 WSAStartup 计数）
    blocking::StreamSocket wake_read_;  ///< 唤醒通道读端（引擎线程读）
    blocking::StreamSocket wake_write_;  ///< 唤醒通道写端（持锁写）
    std::intptr_t wake_handle_ = -1;  ///< 读端的原生句柄值（等待集合首位，构建期写、join 后清）
};

/// @brief 无 fd 语义腿（wasm）的引擎形态：能力全 false，投递一律报不可用，绝不假降级。
class NullEngine final : public Engine {
  public:
    [[nodiscard]] auto name() const -> std::string_view override { return "none"; }

    [[nodiscard]] auto capabilities() const -> Capabilities override { return Capabilities{}; }

    [[nodiscard]] auto post(std::function<void()> task) -> Result<void> override {
        (void)task;  // 本腿不受理任何任务：一律以不可用形态回报，不排队也不静默丢弃
        return make_error(ErrorCode::NetPlatformUnavailable, "post: this leg has no transport engine");
    }

    [[nodiscard]] auto post_after(std::chrono::milliseconds delay, std::function<void()> task)
        -> Result<void> override {
        (void)delay;  // 同 `post`：无引擎腿不登记定时
        (void)task;
        return make_error(ErrorCode::NetPlatformUnavailable, "post_after: this leg has no transport engine");
    }

    [[nodiscard]] auto running() const -> bool override { return false; }

    auto shutdown() -> void override {}
};

/// @brief 进程级引擎单例存储。
/// @return 本腿的引擎实例（native 为常驻 poll 线程引擎，无 fd 语义腿为不支持形态）。
///
/// 刻意不析构（泄漏一枚永不归还的对象），与 `app::EventSourceRegistry::instance()` 同口径：常驻线程
/// 的收敛要加锁、join、关唤醒 socket，而静态析构阶段这些原语已经不可依赖——winpthread 的互斥体与
/// 条件变量属它自己的静态区，可能先于本单例拆除。实测（MinGW，仅 `post` 一次即退出）在
/// `PollEngine::shutdown()` → `detail::Session::~Session()` 的加锁处直接段错误。泄漏的代价是一次
/// 未回收的进程内分配与一条随进程终止的线程；需要确定性收敛的调用方（宿主、探针、用例）显式调
/// `shutdown()`，该路径不依赖拆除次序。
auto shared_engine() -> Engine & {
#ifdef AURORA_PLATFORM_WASM
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static auto &engine = *new NullEngine();
#else
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static auto &engine = *new PollEngine();
#endif
    return engine;
}

}  // namespace

namespace detail {

/// @brief 替身指针的存储位（进程级单点；纪律见 `detail/engine_override.h`）。
/// @note 用 `nullptr` 表示「未装」，故读取一次即得到当时的完整事实，不需要与别处对齐。
///       函数局部静态与 `detail::main_poster_mutex()` 同形态：`std::atomic<Engine *>` 平凡可析构，
///       既无退出期顺序问题，也不留文件级可变全局。
auto engine_override_slot() -> std::atomic<Engine *> & {
    static std::atomic<Engine *> slot{nullptr};
    return slot;
}

auto set_engine_override(Engine *engine) -> void { engine_override_slot().store(engine, std::memory_order_release); }

auto clear_engine_override() -> void { engine_override_slot().store(nullptr, std::memory_order_release); }

auto engine_override() -> Engine * { return engine_override_slot().load(std::memory_order_acquire); }

}  // namespace detail

auto Engine::instance() -> Engine & {
    // 替身优先：测试装了脚本化引擎时，本层所有走 `instance()` 的路径（连接时限、竞速交错定时器、
    // 唤醒投递、解析排队）都改由用例推进；未装时代价只有一次原子加载。
    if (auto *replacement = detail::engine_override(); replacement != nullptr) {
        return *replacement;
    }
    return shared_engine();
}

}  // namespace aurora::net

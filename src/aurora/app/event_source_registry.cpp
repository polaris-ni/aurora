// Aurora app — 通用事件源注册口实现（CHG-010 底座切片：平台无关的就绪通知表）。
//
// 本文件**不含任何平台 socket/poll 头**：真正的「等待某些句柄就绪」经可注入的 Poller 接缝
// 委托给上层（net 引擎层用 WSAPoll/poll 安装、测试用脚本化实现安装）。这让 app 层保持在
// net 层之下且不触碰 CHG-006 的「socket 头唯一落点」不变量。

#include "aurora/app/event_source_registry.h"

#include <algorithm>
#include <utility>

#include "aurora/core/platform.h"

namespace aurora::app {

// ---- EventSourceRegistration ------------------------------------------------

EventSourceRegistration::~EventSourceRegistration() { unregister(); }

EventSourceRegistration::EventSourceRegistration(EventSourceRegistration &&other) noexcept
    : registry_(other.registry_), entry_(std::move(other.entry_)) {
    other.registry_ = nullptr;  // 源转空态：其析构不再触发注销（§18-1 移动不双重注销）
}

auto EventSourceRegistration::operator=(EventSourceRegistration &&other) noexcept -> EventSourceRegistration & {
    if (this != &other) {
        unregister();  // 先放手旧条目，再接管
        registry_ = other.registry_;
        entry_ = std::move(other.entry_);
        other.registry_ = nullptr;
    }
    return *this;
}

auto EventSourceRegistration::unregister() -> void {
    if (registry_ != nullptr) {
        registry_->unregister_source(entry_ ? entry_->token : 0U);
        registry_ = nullptr;
        entry_.reset();
    }
}

// ---- EventSourceRegistry ----------------------------------------------------

// 建进程级单例：无 fd 等待语义的腿在此把支持位置假（判据见 `event_source_supported()` 的声明处）。
// 置假落在构造期而非公共头，是为了让「本腿有没有 fd 语义」这一编译期事实只出现一处，
// 公共 API 面维持零 `#ifdef`（提案 §1 不变量）。
EventSourceRegistry::EventSourceRegistry() {
#ifdef AURORA_PLATFORM_WASM
    event_source_supported_.store(false);
#endif
}

auto EventSourceRegistry::instance() -> EventSourceRegistry & {
    // 函数内 static：首次调用才建，规避跨 TU 静态初始化顺序问题（与 async.h 投递器同口径）。
    //
    // 刻意不析构（泄漏一枚永不归还的对象）：本注册表的存续期必须覆盖**整个静态析构阶段**。自带
    // 引擎是「先构造完自己、再由 `ensure_started()` 首次触到本表」，按「构造完成逆序析构」的口径
    // 本表反而先死；而 `~PollEngine` 在退出路径上收敛线程时要经 `set_change_notify` 回收接缝，
    // 于是一把已析构的 mutex 被加锁（实测在 MinGW 上直接段错误退出）。表里只有 `std::function`
    // 与计数，没有任何需要归还给操作系统的资源，故泄漏没有代价；把顺序寄托在「谁先被碰到」上才是代价。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static auto &registry = *new EventSourceRegistry();
    return registry;
}

auto EventSourceRegistry::next_token() -> std::uint64_t { return ++token_counter_; }

auto EventSourceRegistry::set_poller(Poller poller) -> void {
    std::scoped_lock lock(mutex_);
    poller_ = std::move(poller);
}

auto EventSourceRegistry::set_dispatcher(Dispatcher dispatcher) -> void {
    std::scoped_lock lock(mutex_);
    dispatcher_ = std::move(dispatcher);
}

auto EventSourceRegistry::set_change_notify(ChangeNotify notify) -> void {
    std::scoped_lock lock(mutex_);
    change_notify_ = std::move(notify);
}

auto EventSourceRegistry::has_poller() -> bool {
    std::scoped_lock lock(mutex_);
    return static_cast<bool>(poller_);
}

auto EventSourceRegistry::has_dispatcher() -> bool {
    std::scoped_lock lock(mutex_);
    return static_cast<bool>(dispatcher_);
}

auto EventSourceRegistry::set_event_source_supported(bool supported) -> void {
    event_source_supported_.store(supported);
}

auto EventSourceRegistry::set_host_driven(bool driven) -> void {
    ChangeNotify notify;
    {
        std::scoped_lock lock(mutex_);
        host_driven_.store(driven);
        notify = change_notify_;  // 让位/接管都要常驻方立刻重取快照，故与表变动同一条通知
    }
    if (notify) {
        notify();
    }
}

auto EventSourceRegistry::host_driven() const -> bool { return host_driven_.load(); }

auto EventSourceRegistry::event_source_supported() const -> bool { return event_source_supported_.load(); }

auto EventSourceRegistry::register_source(std::intptr_t handle, WaitInterest interest,
                                          std::function<void(SourceReady)> callback)
    -> Result<EventSourceRegistration> {
    // 无 fd 等待语义的腿（wasm）一律拒绝，不默默收下永不被触发的条目（§2「不提供假降级」）。
    if (!event_source_supported_.load()) {
        return make_error(ErrorCode::NetPlatformUnavailable,
                          "register_source: this leg has no event-source wait semantics");
    }
    // 有效句柄判据：-1 恒无效（延续 CHG-006 口径）。
    if (handle == static_cast<std::intptr_t>(-1)) {
        return make_error(ErrorCode::NetPlatformUnavailable, "register_source: handle -1 is not a waitable descriptor");
    }
    auto entry = std::make_shared<detail::SourceEntry>();
    ChangeNotify notify;
    {
        std::scoped_lock lock(mutex_);
        entry->token = next_token();
        entry->handle = handle;
        entry->interest = interest;
        entry->callback = std::move(callback);
        entries_.push_back(entry);
        notify = change_notify_;
    }
    if (notify) {
        notify();  // 锁外通知：等待方据此重取快照，回调内再登记也不会死锁
    }
    return EventSourceRegistration{this, std::move(entry)};
}

auto EventSourceRegistry::unregister_source(std::uint64_t token) -> bool {
    ChangeNotify notify;
    bool removed = false;
    {
        std::scoped_lock lock(mutex_);
        const auto it =
            std::find_if(entries_.begin(), entries_.end(), [token](const auto &e) { return e->token == token; });
        if (it == entries_.end()) {
            return false;  // 幂等：已注销或从未登记
        }
        // 仅从快照移除：**不置 cancelled**——已入队的一次尾批仍会抵达（载荷由条目 shared_ptr 独立
        // 存活），符合 §18-1「注销 = 停止产生新回调」。作废尾批是 `cancel_pending()` 的独立职责。
        entries_.erase(it);
        removed = true;
        notify = change_notify_;
    }
    if (notify) {
        notify();  // 注销同样要通知：宿主随后关闭该句柄，滞留快照会让下一次系统调用撞上无效句柄
    }
    return removed;
}

auto EventSourceRegistry::sources() -> std::vector<WaitSource> {
    std::scoped_lock lock(mutex_);
    std::vector<WaitSource> out;
    out.reserve(entries_.size());
    for (const auto &e : entries_) {
        out.push_back(WaitSource{.handle = e->handle, .interest = e->interest, .token = e->token});
    }
    return out;
}

auto EventSourceRegistry::dispatch(const std::vector<ReadySource> &ready) -> void {
    // 锁内只快照待投递闭包（拷贝 shared_ptr 令条目独立于表存活），释放锁后再逐个投递，
    // 这样回调内再注册/注销都不会与本函数争锁，也不会因投递阻塞而拖慢快照。
    std::vector<std::function<void()>> pending;
    Dispatcher dispatcher;
    {
        std::scoped_lock lock(mutex_);
        dispatcher = dispatcher_;  // 空则下面就地执行
        for (const auto &rs : ready) {
            const auto it =
                std::find_if(entries_.begin(), entries_.end(), [&rs](const auto &e) { return e->token == rs.token; });
            if (it == entries_.end()) {
                continue;  // 已注销：不再产生新回调
            }
            const auto snapshot = rs.ready;
            // 按值捕获 `*it`（shared_ptr 拷贝）令条目独立于表存活，尾批即使被注销也能安全执行到投递。
            pending.emplace_back([entry = *it, snapshot]() {
                if (!entry->cancelled.load()) {
                    entry->callback(snapshot);
                }
            });
        }
    }
    for (auto &fn : pending) {
        if (dispatcher) {
            dispatcher(std::move(fn));
        } else {
            fn();
        }
    }
}

auto EventSourceRegistry::wait_and_dispatch(std::chrono::milliseconds timeout) -> Result<void> {
    Poller poller;
    std::vector<WaitSource> snapshot;
    {
        std::scoped_lock lock(mutex_);
        poller = poller_;
        snapshot.reserve(entries_.size());
        for (const auto &e : entries_) {
            snapshot.push_back(WaitSource{.handle = e->handle, .interest = e->interest, .token = e->token});
        }
    }
    if (!poller) {
        return make_error(ErrorCode::NetPlatformUnavailable, "wait_and_dispatch: no poller installed");
    }
    if (snapshot.empty()) {
        return Result<void>{};  // 无源可等：直接返回，不忙等
    }
    auto ready = poller(snapshot, timeout);
    if (!ready) {
        return ready.error();
    }
    dispatch(ready.value());
    return Result<void>{};
}

auto EventSourceRegistry::live_count() -> std::size_t {
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

}  // namespace aurora::app

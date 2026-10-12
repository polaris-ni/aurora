// Aurora app — 通用事件源注册口实现（CHG-008 底座切片：平台无关的就绪通知表）。
//
// 本文件**不含任何平台 socket/poll 头**：真正的「等待某些句柄就绪」经可注入的 Poller 接缝
// 委托给上层（net 引擎层用 WSAPoll/poll 安装、测试用脚本化实现安装）。这让 app 层保持在
// net 层之下且不触碰 CHG-006 的「socket 头唯一落点」不变量。

#include "aurora/app/event_source_registry.h"

#include <algorithm>
#include <utility>

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

auto EventSourceRegistry::instance() -> EventSourceRegistry & {
    // 函数内 static：首次调用才建，规避跨 TU 静态初始化顺序问题（与 async.h 投递器同口径）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static EventSourceRegistry registry;
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

auto EventSourceRegistry::set_event_source_supported(bool supported) -> void {
    event_source_supported_.store(supported);
}

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
    std::scoped_lock lock(mutex_);
    entry->token = next_token();
    entry->handle = handle;
    entry->interest = interest;
    entry->callback = std::move(callback);
    entries_.push_back(entry);
    return EventSourceRegistration{this, std::move(entry)};
}

auto EventSourceRegistry::unregister_source(std::uint64_t token) -> bool {
    std::scoped_lock lock(mutex_);
    const auto it =
        std::find_if(entries_.begin(), entries_.end(), [token](const auto &e) { return e->token == token; });
    if (it == entries_.end()) {
        return false;  // 幂等：已注销或从未登记
    }
    // 仅从快照移除：**不置 cancelled**——已入队的一次尾批仍会抵达（载荷由条目 shared_ptr 独立
    // 存活），符合 §18-1「注销 = 停止产生新回调」。作废尾批是 `cancel_pending()` 的独立职责。
    entries_.erase(it);
    return true;
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

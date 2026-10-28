// connectivity.cpp — 链路态公共面：无状态薄门面，全部委派到内部运行时。
//
// 状态（事实、订阅表、定时登记）都在 `detail/connectivity_state.h` 的 `ConnectivityRuntime`
// 里，本文件只负责三件事：句柄生命周期、旋钮与动作的合法性判断、把结果折成公共形态。
#include "aurora/net/connectivity.h"

#include "aurora/net/detail/connectivity_state.h"

namespace aurora::net {

struct ConnectivityRegistration::State {
    std::shared_ptr<detail::Subscription> entry;  ///< 订阅项；空 = 空句柄
};

ConnectivityRegistration::ConnectivityRegistration(std::shared_ptr<State> state, std::uint64_t token)
    : state_{std::move(state)}, token_{token} {}

ConnectivityRegistration::ConnectivityRegistration(ConnectivityRegistration &&other) noexcept
    : state_{std::move(other.state_)}, token_{other.token_} {
    // 逐字沿用 `EventSourceRegistration` 的空态口径：移走订阅即不再是句柄，令牌一并归零
    // （`= default` 会把 `token_` 原样拷贝，源句柄读起来仍像已登记）。
    other.state_ = nullptr;
    other.token_ = 0;
}

auto ConnectivityRegistration::operator=(ConnectivityRegistration &&other) noexcept -> ConnectivityRegistration & {
    if (this == &other) {
        return *this;
    }
    // 先注销本句柄现持订阅（移动赋值不得留下两枚都声称持有的句柄）。
    if (state_ && state_->entry) {
        detail::connectivity_unsubscribe(state_->entry);
    }
    state_ = std::move(other.state_);
    token_ = other.token_;
    other.state_ = nullptr;
    other.token_ = 0;
    return *this;
}

ConnectivityRegistration::~ConnectivityRegistration() {
    if (state_ && state_->entry) {
        detail::connectivity_unsubscribe(state_->entry);  // 幂等
    }
}

auto ConnectivityRegistration::is_registered() const -> bool {
    return state_ != nullptr && state_->entry != nullptr && *state_->entry->live;
}

auto ConnectivityRegistration::token() const -> std::uint64_t { return token_; }

auto ConnectivityRegistration::cancel_pending() -> void {
    if (state_ && state_->entry) {
        detail::connectivity_cancel_pending(state_->entry);
    }
}

auto ConnectivityMonitor::instance() -> ConnectivityMonitor & {
    // 进程级单例：函数内静态、刻意不析构（与 `shared_engine()` 同口径）。
    static ConnectivityMonitor value;
    return value;
}

auto ConnectivityMonitor::snapshot() const -> ConnectivityFact {
    // 恒可读：只加锁拷贝并重算 `stale`，绝不触发 I/O 或采样，也不发布通知。
    auto &self = detail::connectivity_runtime();
    const std::scoped_lock lock{self.mutex};
    return self.machine.fact();
}

auto ConnectivityMonitor::options() const -> ConnectivityOptions {
    auto &self = detail::connectivity_runtime();
    const std::scoped_lock lock{self.mutex};
    return self.machine.options();
}

auto ConnectivityMonitor::set_options(ConnectivityOptions options) -> void {
    {
        auto &self = detail::connectivity_runtime();
        const std::scoped_lock lock{self.mutex};
        self.machine.set_options(options);
    }
    // 有腿时安排一次 `HostRefresh` 采样（已有待采样轮次则合并）；无腿时只存值。
    if (detail::compiled_source() != ConnectivitySource::None) {
        (void)detail::connectivity_request_sample(ConnectivityChangeCause::HostRefresh);
    }
}

auto ConnectivityMonitor::defaults() -> ConnectivityOptions {
    return ConnectivityOptions{
        .sample_interval = std::chrono::milliseconds{5000}, .offline_evidence = 1U, .online_evidence = 2U};
}

auto ConnectivityMonitor::refresh() -> Result<void> {
    // 无源腿如实回报：不得回 OK 后永不通知。
    if (detail::compiled_source() == ConnectivitySource::None) {
        return make_error(ErrorCode::NetPlatformUnavailable,
                          ErrorParams{{"detail", "this build has no link-state collection source"}});
    }
    return detail::connectivity_request_sample(ConnectivityChangeCause::HostRefresh);
}

auto ConnectivityMonitor::subscribe(std::function<void(ConnectivityChange)> callback)
    -> Result<ConnectivityRegistration> {
    if (!callback) {
        return make_error(ErrorCode::NetOptionFailed, ErrorParams{{"detail", "subscribe callback must not be empty"}});
    }
    // 两类无源腿分开口径：无腿且无证据通路（wasm）回不支持；无腿但有旁证（BSD / Android 一类）
    // 允许登记、只由证据驱动。
    const bool has_source = detail::compiled_source() != ConnectivitySource::None;
    if (!has_source && !detail::has_evidence_path()) {
        return make_error(
            ErrorCode::NetPlatformUnavailable,
            ErrorParams{{"detail", "this build has neither a collection source nor a connect-evidence path"}});
    }

    auto registered = detail::connectivity_subscribe(std::move(callback));
    if (!registered) {
        return registered.error();
    }
    auto entry = registered.value().first;
    auto state = std::make_shared<ConnectivityRegistration::State>();
    state->entry = entry;
    auto registration = ConnectivityRegistration{std::move(state), entry->token};

    // 返回前一瞬才开启投递：并发发布不会抢跑（契约：`subscribe()` 返回前不回调）。
    *entry->live = true;
    if (has_source) {
        (void)detail::connectivity_request_sample(ConnectivityChangeCause::SubscriptionStart);
    }
    return Result<ConnectivityRegistration>{std::move(registration)};
}

auto network_state_text(NetworkState state) -> std::string_view {
    switch (state) {
        case NetworkState::Unknown:
            return "unknown";
        case NetworkState::Online:
            return "online";
        case NetworkState::Offline:
            return "offline";
    }
    return "unknown";  // 枚举值越界（外部强制转换）时的兜底，恒非空
}

auto connectivity_source_text(ConnectivitySource source) -> std::string_view {
    switch (source) {
        case ConnectivitySource::None:
            return "none";
        case ConnectivitySource::IpHelper:
            return "ip_helper";
        case ConnectivitySource::DynamicStore:
            return "dynamic_store";
        case ConnectivitySource::LinkFiles:
            return "link_files";
    }
    return "none";
}

auto connectivity_change_cause_text(ConnectivityChangeCause cause) -> std::string_view {
    switch (cause) {
        case ConnectivityChangeCause::SubscriptionStart:
            return "subscription_start";
        case ConnectivityChangeCause::PeriodicSample:
            return "periodic_sample";
        case ConnectivityChangeCause::FailureEvidence:
            return "failure_evidence";
        case ConnectivityChangeCause::SuccessfulConnect:
            return "successful_connect";
        case ConnectivityChangeCause::HostRefresh:
            return "host_refresh";
        case ConnectivityChangeCause::StalenessMarked:
            return "staleness_marked";
    }
    return "periodic_sample";
}

}  // namespace aurora::net

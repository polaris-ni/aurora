// connectivity_state.cpp — 链路态内部层实现：判据层状态机 + 进程级运行时。
//
// 锁纪律与投递纪律集中在本文件：全件只有 `ConnectivityRuntime::mutex` 一把互斥量，探测 I/O、
// 发布与订阅者回调一律在锁外；`post_after` 无撤销口，故每拍登记时递增 epoch，让旧闭包到期
// 自动作废（与 `http_client.cpp` 的 `StageTimer` 同法）。
#include "aurora/net/detail/connectivity_state.h"

#include <algorithm>
#include <utility>

#include "aurora/core/error_codes.gen.h"
#include "aurora/core/log.h"
#include "aurora/net/engine.h"
#include "aurora/net/http.h"
#include "aurora/state/async.h"

namespace aurora::net::detail {
namespace {

/// @brief 采样间隔下界：再快就是把引擎当轮询器使。
constexpr std::chrono::milliseconds AURORA_MIN_SAMPLE_INTERVAL{100};
/// @brief 采样间隔上界：再慢，`stale` 就失去「多久没确认过」的意义。
constexpr auto AURORA_MAX_SAMPLE_INTERVAL = std::chrono::milliseconds{std::chrono::hours{1}};
/// @brief 证据数下界：零枚即转态等于没有阈值。
constexpr std::uint32_t AURORA_MIN_EVIDENCE = 1U;
/// @brief 证据数上界：再多只是把掉线发现拖长。
constexpr std::uint32_t AURORA_MAX_EVIDENCE = 16U;
/// @brief 陈旧判定阈值相对采样周期的倍数（写定，不设第二枚旋钮）。
constexpr std::uint32_t AURORA_STALE_FACTOR = 3U;

/// @brief 把旋钮钳到合法区间（间隔 `[100ms, 1h]`、证据数 `[1, 16]`）。
[[nodiscard]] auto clamp_options(ConnectivityOptions options) -> ConnectivityOptions {
    options.sample_interval =
        std::clamp(options.sample_interval, AURORA_MIN_SAMPLE_INTERVAL, AURORA_MAX_SAMPLE_INTERVAL);
    options.offline_evidence = std::clamp(options.offline_evidence, AURORA_MIN_EVIDENCE, AURORA_MAX_EVIDENCE);
    options.online_evidence = std::clamp(options.online_evidence, AURORA_MIN_EVIDENCE, AURORA_MAX_EVIDENCE);
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// 判据层：`ConnectivityMachine`
// ---------------------------------------------------------------------------

struct ConnectivityMachine::State {
    ConnectivitySource source = ConnectivitySource::None;  ///< 编译期采集腿令牌
    ConnectivityOptions options{};  ///< 采样旋钮（恒为钳后值）
    ConnectivityClockFn now;  ///< 注入时钟
    std::chrono::steady_clock::time_point since;  ///< 当前 `state` 的起始时刻
    std::chrono::steady_clock::time_point last_evidence_at;  ///< 上次成功采样 / 合格正证时刻
    NetworkState state = NetworkState::Unknown;  ///< 当前三态
    std::uint64_t generation = 0;  ///< 已发布通知的序列计数
    bool has_evidence = false;  ///< 是否至少有过一次成功采样或合格正证
    bool stale_published = true;  ///< 当前证据期内是否已发布过陈旧通知（初值真 ⇒ 起始态不发布）
    std::optional<std::string> last_failure;  ///< 冻结错误码 slug（仅展示）
    std::uint32_t online_count = 0;  ///< 连续一致的正证数
    std::uint32_t offline_count = 0;  ///< 连续的合格探测读数
};

ConnectivityMachine::ConnectivityMachine(Config config) : state_{std::make_unique<State>()} {
    state_->source = config.source;
    state_->options = clamp_options(config.options);
    state_->now = std::move(config.now);
    // `since` 取 monitor 建立时刻而非 epoch 零点——它表的是「这条事实成立多久了」。
    state_->since = state_->now();
    state_->last_evidence_at = state_->since;
}

ConnectivityMachine::~ConnectivityMachine() = default;

auto ConnectivityMachine::source() const -> ConnectivitySource { return state_->source; }

auto ConnectivityMachine::options() const -> ConnectivityOptions { return state_->options; }

auto ConnectivityMachine::set_options(ConnectivityOptions options) -> void {
    // 只改旋钮，不动在途计数、不立即转换：`stale` 阈值随新周期派生，故旧读数在新口径下
    // 可能立刻转陈旧——改周期即承认新口径，这是刻意的。
    state_->options = clamp_options(options);
}

auto ConnectivityMachine::fact() const -> ConnectivityFact {
    ConnectivityFact out;
    out.state = state_->state;
    out.source = state_->source;
    out.since = state_->since;
    out.generation = state_->generation;
    out.stale = stale_now();
    out.last_failure = state_->last_failure;
    return out;
}

auto ConnectivityMachine::apply_probe(std::optional<bool> online, ConnectivityChangeCause cause)
    -> ConnectivityTransition {
    ConnectivityTransition result{};
    if (!online.has_value()) {
        // 读不到 ≠ 离线：不计数、不清零、不转换（`Unknown` 纪律的同源条款）。
        return result;
    }
    const auto previous = fact();  // 本拍之前订阅者可见的事实（含派生的 stale）
    const auto now = state_->now();
    state_->has_evidence = true;
    state_->last_evidence_at = now;
    state_->stale_published = false;
    if (*online) {
        state_->offline_count = 0U;  // 反向证据清零对方计数
        state_->online_count += 1U;
        if (state_->state == NetworkState::Online) {
            return result;  // 已在 Online：不发布、不插队
        }
        // `Unknown→Online` 一枚正证即可；`Offline→Online` 需 `online_evidence` 枚一致正证。
        const auto required = (state_->state == NetworkState::Unknown) ? 1U : state_->options.online_evidence;
        if (state_->online_count < required) {
            result.want_sample = true;  // 计票后仍未到 ⇒ 再采一次求第二枚一致读数
            return result;
        }
        result.change = apply_transition(NetworkState::Online, now, cause, previous);
        return result;
    }
    state_->online_count = 0U;  // 反向证据清零对方计数
    state_->offline_count += 1U;
    if (state_->state == NetworkState::Offline) {
        return result;  // 已在 Offline：不发布、不插队
    }
    if (state_->offline_count < state_->options.offline_evidence) {
        // 离线侧刻意不插队：合格的「无缺省路由」读数本身就是判决依据，等下一枚周期拍即可；
        // 正证侧才需要「再确认一次」——误判在线的代价远高于误判离线。
        return result;
    }
    result.change = apply_transition(NetworkState::Offline, now, cause, previous);
    return result;
}

auto ConnectivityMachine::note_connect_success(bool loopback_peer) -> ConnectivityTransition {
    ConnectivityTransition result{};
    if (loopback_peer) {
        return result;  // 本机自连不证明外链：不计正证、不采样
    }
    const auto now = state_->now();
    const auto previous = fact();
    state_->has_evidence = true;
    state_->last_evidence_at = now;
    state_->stale_published = false;
    state_->offline_count = 0U;
    state_->online_count += 1U;
    if (state_->state == NetworkState::Online) {
        return result;
    }
    const auto required = (state_->state == NetworkState::Unknown) ? 1U : state_->options.online_evidence;
    if (state_->online_count < required) {
        result.want_sample = true;
        return result;
    }
    result.change = apply_transition(NetworkState::Online, now, ConnectivityChangeCause::SuccessfulConnect, previous);
    return result;
}

auto ConnectivityMachine::note_connect_failure(ErrorCode code) -> ConnectivityTransition {
    ConnectivityTransition result{};
    const auto klass = http::error_class_of(code);
    if (klass == http::HttpErrorClass::Cancelled || klass == http::HttpErrorClass::Invalid) {
        return result;  // 取消与非法不属链路证据：不采样、不写展示字段
    }
    // 失败只作采样触发与展示字段，永不计负证。
    state_->last_failure = std::string{aurora::slug(code)};
    result.want_sample = true;
    return result;
}

auto ConnectivityMachine::mark_stale_if_due() -> ConnectivityTransition {
    ConnectivityTransition result{};
    if (state_->stale_published || !state_->has_evidence || !stale_now()) {
        return result;
    }
    // 陈旧位只在此处由 false→true 翻转一次：状态未变、代际推进、两侧证据计数清零。
    auto previous = fact();
    previous.stale = false;  // 翻转前的可见事实（本拍前已由证据刷新为不陈旧）
    state_->stale_published = true;
    state_->generation += 1U;
    state_->online_count = 0U;
    state_->offline_count = 0U;
    ConnectivityChange change;
    change.previous = previous;
    change.current = fact();
    change.cause = ConnectivityChangeCause::StalenessMarked;
    result.change = change;
    return result;
}

auto ConnectivityMachine::stale_now() const -> bool {
    if (!state_->has_evidence) {
        return true;  // 从未有过证据 ⇒ 无从谈新鲜
    }
    const auto budget = state_->options.sample_interval * AURORA_STALE_FACTOR;
    return (state_->now() - state_->last_evidence_at) > budget;
}

auto ConnectivityMachine::apply_transition(NetworkState next, std::chrono::steady_clock::time_point at,
                                           ConnectivityChangeCause cause, const ConnectivityFact &previous)
    -> ConnectivityChange {
    ConnectivityChange change;
    change.previous = previous;
    change.cause = cause;
    state_->state = next;
    state_->since = at;
    state_->generation += 1U;
    state_->online_count = 0U;  // 状态转换清零两侧
    state_->offline_count = 0U;
    if (next == NetworkState::Online) {
        state_->last_failure.reset();  // 只有转入 Online 才清空展示字段
    }
    change.current = fact();
    return change;
}

auto ConnectivityMachine::reset_for_test() -> void {
    state_->state = NetworkState::Unknown;
    state_->generation = 0U;
    state_->has_evidence = false;
    state_->stale_published = true;
    state_->last_failure.reset();
    state_->online_count = 0U;
    state_->offline_count = 0U;
    state_->since = state_->now();
    state_->last_evidence_at = state_->since;
}

namespace {

/// @brief 进程级运行时：函数内静态、刻意不析构（与 `shared_engine()` 同口径）。
auto runtime() -> ConnectivityRuntime & {
    static ConnectivityRuntime value;
    return value;
}

/// @brief 采集钩子槽（进程级单例；空函数 = 未装入，回落平台腿）。
auto probe_hook() -> ConnectivityProbeFn & {
    static ConnectivityProbeFn hook;
    return hook;
}

/// @brief 取一次探针读数：钩子优先，否则平台腿。
[[nodiscard]] auto take_reading() -> ProbeReading {
    const auto &hook = connectivity_probe_hook();
    if (hook) {
        return hook();
    }
    return probe_default_route();
}

/// @brief 把一枚变更并进订阅者的待投递项。
///
/// 首因保留（`cause` 已在待投递项里就不再改），`previous` 一律锚在订阅者**已见**值上——
/// 不是全局上一枚事实，否则合并投递会让订阅者看到一段它从未收到的历史。
auto enqueue(const std::shared_ptr<Subscription> &sub, const ConnectivityChange &change) -> void {
    if (!sub || !*sub->live) {
        return;
    }
    sub->pending.previous = sub->has_seen ? sub->seen : change.previous;
    sub->pending.current = change.current;
    if (!sub->has_pending) {
        sub->pending.cause = change.cause;
    }
    sub->has_pending = true;
    sub->seen = change.current;
    sub->has_seen = true;
}

/// @brief 执行投递：锁内取件、锁外回调。
///
/// 抛出型订阅者在此被摘除（`live` 转假 ⇒ `is_registered()` 转假）并留一条 WARN；采样循环
/// 与任何在途交换的结局都不受影响。
/// @brief 摘除一枚订阅（幂等；按共享 live 标志识别）。
auto drop_subscriber(const std::shared_ptr<Subscription> &entry) -> void {
    auto &subs = runtime().subscriptions;
    const std::scoped_lock lock{runtime().mutex};
    subs.erase(std::remove(subs.begin(), subs.end(), entry), subs.end());
    *entry->live = false;
}

auto deliver(const std::shared_ptr<Subscription> &sub) -> void {
    ConnectivityChange payload{};
    {
        const std::scoped_lock lock{runtime().mutex};
        if (!sub || !*sub->live || !sub->has_pending) {
            return;
        }
        payload = sub->pending;
        sub->has_pending = false;
    }
    try {
        if (sub->callback) {
            sub->callback(payload);
        }
    } catch (const std::exception &error) {
        drop_subscriber(sub);
        AURORA_LOG_WARN("net.connectivity", "subscription callback threw: ", error.what());
    } catch (...) {
        drop_subscriber(sub);
        AURORA_LOG_WARN("net.connectivity", "subscription callback threw (non-std exception)");
    }
}

/// @brief 把一枚已发布的变更投递给各订阅者（回调在锁外执行）。
auto publish_change(const ConnectivityChange &change) -> void {
    std::vector<std::shared_ptr<Subscription>> targets;
    {
        const std::scoped_lock lock{runtime().mutex};
        for (const auto &sub : runtime().subscriptions) {
            enqueue(sub, change);
        }
        targets = runtime().subscriptions;
    }
    for (const auto &sub : targets) {
        deliver(sub);
    }
}

}  // namespace

auto connectivity_runtime() -> ConnectivityRuntime & { return runtime(); }

auto connectivity_subscribe(std::function<void(ConnectivityChange)> callback)
    -> Result<std::pair<std::shared_ptr<Subscription>, ConnectivityFact>> {
    if (!callback) {
        return make_error(ErrorCode::NetOptionFailed, ErrorParams{{"detail", "subscribe callback must not be empty"}});
    }
    auto entry = std::make_shared<Subscription>();
    entry->callback = std::move(callback);
    // 登记时先关闭投递，返回前一瞬才开启 ⇒ 并发发布不会抢跑（契约：返回前绝不回调）。
    *entry->live = false;
    ConnectivityFact at_subscribe{};
    {
        const std::scoped_lock lock{runtime().mutex};
        entry->token = runtime().next_token++;
        at_subscribe = runtime().machine.fact();
        runtime().subscriptions.push_back(entry);
    }
    entry->seen = at_subscribe;  // 首条 `previous` = 订阅时刻的快照
    entry->has_seen = true;
    return Result<std::pair<std::shared_ptr<Subscription>, ConnectivityFact>>{
        std::make_pair(std::move(entry), at_subscribe)};
}

auto connectivity_unsubscribe(const std::shared_ptr<Subscription> &entry) -> void {
    if (!entry) {
        return;
    }
    drop_subscriber(entry);  // 幂等：重复调用只是把同一个标志再置一次假
}

auto connectivity_cancel_pending(const std::shared_ptr<Subscription> &entry) -> void {
    if (!entry) {
        return;
    }
    const std::scoped_lock lock{runtime().mutex};
    entry->has_pending = false;
}

auto connectivity_sample(std::uint64_t epoch, const std::shared_ptr<bool> &live, ConnectivityChangeCause cause)
    -> void {
    {
        const std::scoped_lock lock{runtime().mutex};
        if (epoch != runtime().epoch || !*live) {
            return;  // 旧拍已被新登记作废：`post_after` 无撤销口，epoch 是唯一闸门
        }
    }
    // 探测在锁外执行：读 OS 表 / 伪文件是 I/O，不得让 `snapshot()` 的加锁拷贝排在它后面
    // （「探测 I/O、发布与回调一律锁外」）。
    const auto reading = take_reading();
    std::vector<ConnectivityChange> published;
    {
        const std::scoped_lock lock{runtime().mutex};
        if (epoch != runtime().epoch || !*live) {
            return;  // 探测期间被新一拍作废
        }
        runtime().sample_pending = false;

        // 静止期先判：陈旧位翻转也是一次发布（false→true 恰好一次）。
        const auto stale_step = runtime().machine.mark_stale_if_due();
        if (stale_step.change) {
            published.push_back(*stale_step.change);
        }
        const auto step = runtime().machine.apply_probe(reading.online, cause);
        if (step.change) {
            published.push_back(*step.change);
        }
    }
    for (const auto &change : published) {
        publish_change(change);
    }
    // 采样后只登记下一枚周期拍；无订阅者时 `connectivity_request_sample` 自行 no-op。
    (void)connectivity_request_sample(ConnectivityChangeCause::PeriodicSample);
}

auto connectivity_request_sample(ConnectivityChangeCause cause) -> Result<void> {
    ConnectivityOptions options{};
    std::uint64_t epoch = 0;
    {
        const std::scoped_lock lock{runtime().mutex};
        // 周期拍只在至少一枚订阅存在时登记；证据拍（失败 / 宿主刷新）不受此限。
        if (cause == ConnectivityChangeCause::PeriodicSample && runtime().subscriptions.empty()) {
            return Result<void>{};
        }
        if (runtime().sample_pending) {
            return Result<void>{};  // 已有待采样轮次 ⇒ 合并，不另登记
        }
        runtime().sample_pending = true;
        epoch = ++runtime().epoch;  // 旧闭包见 epoch 不符即 no-op
        options = runtime().machine.options();
    }
    const auto live = runtime().sample_live;
    auto outcome = Engine::instance().post_after(options.sample_interval,
                                                 [epoch, live, cause] { connectivity_sample(epoch, live, cause); });
    if (!outcome) {
        // 登记失败必须清 pending：否则此后每一拍都合并进一枚不存在的定时器，永远不再采样。
        const std::scoped_lock lock{runtime().mutex};
        if (runtime().epoch == epoch) {
            runtime().sample_pending = false;
        }
    }
    return outcome;
}

auto note_connection_outcome(bool connected, bool loopback_peer, std::optional<ErrorCode> code) -> void {
    // 旁证入口：不阻塞、不做 I/O、不改交换结局，只据结局推进链路态。
    ConnectivityTransition step{};
    if (connected) {
        step = connectivity_runtime().machine.note_connect_success(loopback_peer);
    } else if (code.has_value()) {
        step = connectivity_runtime().machine.note_connect_failure(*code);
    } else {
        return;  // 既未成功也无错误码 ⇒ 无证据可落
    }
    if (step.want_sample) {
        (void)connectivity_request_sample(ConnectivityChangeCause::FailureEvidence);
    }
    if (step.change) {
        publish_change(*step.change);
    }
}

auto set_connectivity_probe_hook(ConnectivityProbeFn hook) -> void { probe_hook() = std::move(hook); }

auto connectivity_probe_hook() -> const ConnectivityProbeFn & { return probe_hook(); }

auto reset_connectivity_for_test() -> void {
    auto &self = runtime();
    const std::scoped_lock lock{self.mutex};
    self.machine.reset_for_test();
    self.sample_pending = false;
    self.epoch = 0U;
    self.sample_live = std::make_shared<bool>(true);
    for (const auto &sub : self.subscriptions) {
        sub->has_pending = false;
        sub->pending = ConnectivityChange{};
        sub->seen = ConnectivityFact{};
        sub->has_seen = false;
    }
}

}  // namespace aurora::net::detail

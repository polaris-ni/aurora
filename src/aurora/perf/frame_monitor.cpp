/// @brief 下游性能监控订阅门面的进程级状态与帧末投递实现。
/// @file frame_monitor.cpp

#include "aurora/perf/frame_monitor.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/json.h"
#include "aurora/perf/stopwatch.h"

namespace aurora {
namespace {

constexpr std::size_t AURORA_FRAME_MONITOR_MAX_BATCH_FRAMES = 64;

struct Subscriber {
    std::uint64_t handle = 0;
    std::function<void(const FrameBatch &)> callback;
};

struct Metric {
    std::uint64_t handle = 0;
    std::string name;
    double value = 0.0;
};

struct Scope {
    std::uint64_t handle = 0;
    std::string name;
    double start_ms = 0.0;
};

struct ScopeAggregate {
    std::string name;
    double total_ms = 0.0;
    std::uint64_t calls = 0;
};

struct State {
    std::vector<Subscriber> subscribers;
    std::vector<Metric> metrics;
    std::vector<Scope> scopes;
    std::vector<ScopeAggregate> aggregates;
    std::array<FrameSample, AURORA_FRAME_MONITOR_MAX_BATCH_FRAMES> batch{};
    std::size_t batch_count = 0;
    std::uint64_t next_handle = 1;
    std::uint64_t frame_index = 0;
    MonitorDiagnostics diag{};
    MetricError last_error = MetricError::None;
};

[[nodiscard]] auto state() -> State & {
    static State instance;
    return instance;
}

[[nodiscard]] auto valid_name(const std::string &name) -> bool {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [](const unsigned char ch) { return std::iscntrl(ch) == 0; });
}

}  // namespace

auto FrameMonitor::subscribe(std::function<void(const FrameBatch &)> callback) -> std::uint64_t {
    if (!callback) {
        return 0;
    }
    State &s = state();
    const std::uint64_t handle = s.next_handle++;
    s.subscribers.push_back(Subscriber{.handle = handle, .callback = std::move(callback)});
    s.diag.subscriber_count = static_cast<std::uint64_t>(s.subscribers.size());
    if (s.diag.subscriber_count == 1) {
        s.diag.hook_registrations = 1;
    }
    return handle;
}

auto FrameMonitor::unsubscribe(std::uint64_t subscription) -> bool {
    if (subscription == 0) {
        return false;
    }
    State &s = state();
    const auto it = std::find_if(s.subscribers.begin(), s.subscribers.end(),
                                 [subscription](const Subscriber &entry) { return entry.handle == subscription; });
    if (it == s.subscribers.end()) {
        return false;
    }
    s.subscribers.erase(it);
    s.diag.subscriber_count = static_cast<std::uint64_t>(s.subscribers.size());
    if (s.subscribers.empty()) {
        s.diag.hook_registrations = 0;
    }
    return true;
}

auto FrameMonitor::register_metric(const std::string &name) -> std::uint64_t {
    State &s = state();
    if (!valid_name(name)) {
        s.last_error = MetricError::InvalidName;
        return 0;
    }
    const bool duplicate =
        std::any_of(s.metrics.begin(), s.metrics.end(), [&name](const Metric &entry) { return entry.name == name; });
    if (duplicate) {
        s.last_error = MetricError::DuplicateName;
        return 0;
    }
    s.last_error = MetricError::None;
    const std::uint64_t handle = s.next_handle++;
    s.metrics.push_back(Metric{.handle = handle, .name = name, .value = 0.0});
    return handle;
}

auto FrameMonitor::last_metric_error() -> MetricError { return state().last_error; }

auto FrameMonitor::set_metric(std::uint64_t metric, double value) -> bool {
    State &s = state();
    const auto it = std::find_if(s.metrics.begin(), s.metrics.end(),
                                 [metric](const Metric &entry) { return entry.handle == metric; });
    if (it == s.metrics.end()) {
        return false;
    }
    it->value = value;
    return true;
}

auto FrameMonitor::metric_value(std::uint64_t metric) -> double {
    const State &s = state();
    const auto it = std::find_if(s.metrics.begin(), s.metrics.end(),
                                 [metric](const Metric &entry) { return entry.handle == metric; });
    return it == s.metrics.end() ? 0.0 : it->value;
}

auto FrameMonitor::unregister_metric(std::uint64_t metric) -> bool {
    if (metric == 0) {
        return false;
    }
    State &s = state();
    const auto it = std::find_if(s.metrics.begin(), s.metrics.end(),
                                 [metric](const Metric &entry) { return entry.handle == metric; });
    if (it == s.metrics.end()) {
        return false;
    }
    s.metrics.erase(it);
    return true;
}

auto FrameMonitor::begin_scope(const std::string &name) -> std::uint64_t {
    State &s = state();
    if (s.subscribers.empty()) {
        return 0;
    }
    ++s.diag.clock_reads;
    const double start_ms = Stopwatch::now_ms();
    const std::uint64_t handle = s.next_handle++;
    s.scopes.push_back(Scope{.handle = handle, .name = name, .start_ms = start_ms});
    return handle;
}

auto FrameMonitor::end_scope(std::uint64_t scope) -> void {
    if (scope == 0) {
        return;
    }
    State &s = state();
    const auto it =
        std::find_if(s.scopes.begin(), s.scopes.end(), [scope](const Scope &entry) { return entry.handle == scope; });
    if (it == s.scopes.end()) {
        return;
    }
    ++s.diag.clock_reads;
    const double elapsed_ms = Stopwatch::now_ms() - it->start_ms;
    const std::string name = it->name;
    s.scopes.erase(it);
    const auto agg = std::find_if(s.aggregates.begin(), s.aggregates.end(),
                                  [&name](const ScopeAggregate &entry) { return entry.name == name; });
    if (agg == s.aggregates.end()) {
        s.aggregates.push_back(ScopeAggregate{.name = name, .total_ms = elapsed_ms, .calls = 1});
    } else {
        agg->total_ms += elapsed_ms;
        ++agg->calls;
    }
}

auto FrameMonitor::snapshot_json() -> std::string {
    const State &s = state();
    auto root = json::Value::object();
    root.set("subscribers", json::Value{static_cast<std::int64_t>(s.diag.subscriber_count)});

    auto metrics = json::Value::object();
    for (const auto &entry : s.metrics) {
        auto item = json::Value::object();
        item.set("value", json::Value{entry.value});
        metrics.set(entry.name, std::move(item));
    }
    root.set("metrics", std::move(metrics));

    auto scopes = json::Value::array();
    for (const auto &entry : s.aggregates) {
        auto item = json::Value::object();
        item.set("name", json::Value{entry.name});
        item.set("total_ms", json::Value{entry.total_ms});
        item.set("calls", json::Value{static_cast<std::int64_t>(entry.calls)});
        scopes.push_back(std::move(item));
    }
    root.set("scopes", std::move(scopes));

    auto diag = json::Value::object();
    diag.set("hook_registrations", json::Value{static_cast<std::int64_t>(s.diag.hook_registrations)});
    diag.set("heap_allocations", json::Value{static_cast<std::int64_t>(s.diag.heap_allocations)});
    diag.set("clock_reads", json::Value{static_cast<std::int64_t>(s.diag.clock_reads)});
    diag.set("bytes_emitted", json::Value{static_cast<std::int64_t>(s.diag.bytes_emitted)});
    diag.set("dropped_batches", json::Value{static_cast<std::int64_t>(s.diag.dropped_batches)});
    diag.set("subscriber_count", json::Value{static_cast<std::int64_t>(s.diag.subscriber_count)});
    root.set("diagnostics", std::move(diag));

    const auto dumped = json::dump(root, json::DumpOptions{.indent = 2});
    return dumped ? dumped.value() : std::string{};
}

auto FrameMonitor::diagnostics() -> MonitorDiagnostics { return state().diag; }

auto FrameMonitor::max_batch_frames() -> std::size_t { return AURORA_FRAME_MONITOR_MAX_BATCH_FRAMES; }

namespace detail {

auto publish_frame_sample(double frame_delta, bool idle) -> void {
    State &s = state();
    if (s.subscribers.empty()) {
        return;
    }
    // 订阅者快照的容器分配属于帧投递路径；零订阅者不会走到这里。
    ++s.diag.heap_allocations;
    if (s.batch_count >= AURORA_FRAME_MONITOR_MAX_BATCH_FRAMES) {
        // 上一批因订阅者异常未能投递，积压到上限后整批丢弃，不阻塞帧也不改帧语义。
        ++s.diag.dropped_batches;
        s.batch_count = 0;
        return;
    }
    s.batch.at(s.batch_count) = FrameSample{.frame_delta = frame_delta, .index = s.frame_index++, .idle = idle};
    ++s.batch_count;

    const FrameBatch batch{.samples = s.batch.data(), .count = s.batch_count};
    const auto snapshot = s.subscribers;
    bool failed = false;
    for (const auto &entry : snapshot) {
        try {
            entry.callback(batch);
        } catch (...) {
            failed = true;
        }
    }
    if (!failed) {
        s.diag.bytes_emitted += static_cast<std::uint64_t>(s.batch_count) * sizeof(FrameSample);
        s.batch_count = 0;
    }
}

}  // namespace detail

}  // namespace aurora

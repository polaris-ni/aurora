#include "aurora/perf/profiler.h"

#include <algorithm>
#include <cstring>

#include "aurora/core/string_util.h"
#include "aurora/perf/trace_writer.h"

namespace aurora {

namespace detail {

auto on_frame_scope_end() -> void {
#ifdef AURORA_ENABLE_TRACING
    TraceWriter &tw = TraceWriter::instance();
    if (!tw.capturing()) {
        return;
    }
    const Profiler &prof = Profiler::instance();
    tw.capture_frame(prof);
    tw.capture_counters(prof.frame_index(), Stopwatch::now_ms(), RenderCounters::current());
#endif
}

}  // namespace detail

namespace {

/// @brief zone 名比较：允许同一名字来自不同 TU 的不同地址，故按内容比较。
[[nodiscard]] auto same_zone_name(const char *a, const char *b) -> bool {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return std::strcmp(a, b) == 0;
}

/// @brief 当帧长任务列表的预留容量（超出仅计数不存样本）。
constexpr std::size_t AURORA_LONG_TASK_CAPACITY = 64;

}  // namespace

Profiler::Profiler() : frame_start_ms_(Stopwatch::now_ms()) {
    zones_.reserve(capacity_);
    long_tasks_.reserve(AURORA_LONG_TASK_CAPACITY);
}

auto Profiler::instance() -> Profiler & {
    static Profiler s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// 帧协议
// ---------------------------------------------------------------------------

auto Profiler::begin_frame() -> void {
    if (depth_ != 0) {
        // 上一帧存在未闭合 zone：计入配对错误并强制复位，避免错误跨帧传播。
        unbalanced_ += static_cast<std::uint64_t>(depth_);
        depth_ = 0;
    }
    zones_.clear();
    long_tasks_.clear();
    frame_start_ms_ = Stopwatch::now_ms();
}

auto Profiler::end_frame() -> void {
    if (depth_ != 0) {
        unbalanced_ += static_cast<std::uint64_t>(depth_);
        depth_ = 0;
    }
    ++frame_index_;
}

auto Profiler::frame_index() const -> std::uint64_t { return frame_index_; }

auto Profiler::frame_start_ms() const -> double { return frame_start_ms_; }

// ---------------------------------------------------------------------------
// 作用域协议（热路径：无堆分配、无字符串操作）
// ---------------------------------------------------------------------------

auto Profiler::begin_zone(const char *name) -> void {
    if (!enabled_) {
        return;
    }
    const std::size_t idx = depth_++;
    if (idx >= AURORA_MAX_ZONE_DEPTH) {
        ++dropped_;  // 嵌套过深：丢弃样本，但仍维持深度以保证 end_zone 配对
        return;
    }
    OpenZone &z = stack_[idx];  // NOLINT(*-pro-bounds-constant-array-index)
    z.name = name;
    z.start_ms = Stopwatch::now_ms() - frame_start_ms_;
    z.watch.reset();
}

auto Profiler::end_zone() -> void {
    if (!enabled_) {
        return;
    }
    if (depth_ == 0) {
        ++unbalanced_;  // end 多于 begin
        return;
    }
    const std::size_t idx = --depth_;
    if (idx >= AURORA_MAX_ZONE_DEPTH) {
        return;  // 进入时已记 dropped
    }

    const OpenZone &z = stack_[idx];  // NOLINT(*-pro-bounds-constant-array-index)
    const double duration_ms = z.watch.elapsed_ms();
    const ZoneSample sample{
        .name = z.name, .start_ms = z.start_ms, .duration_ms = duration_ms, .depth = static_cast<std::uint16_t>(idx)};

    if (duration_ms >= long_task_threshold_ms_) {
        ++total_long_tasks_;
        if (long_tasks_.size() < long_tasks_.capacity()) {
            long_tasks_.push_back(sample);
        }
    }

    if (zones_.size() < capacity_) {
        zones_.push_back(sample);
    } else {
        ++dropped_;
    }
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

auto Profiler::frame_zones() const -> const std::vector<ZoneSample> & { return zones_; }

auto Profiler::aggregate(const char *name) const -> ZoneAggregate {
    ZoneAggregate agg{};
    agg.name = name;
    for (const auto &z : zones_) {
        if (!same_zone_name(z.name, name)) {
            continue;
        }
        ++agg.call_count;
        agg.total_ms += z.duration_ms;
        agg.max_ms = std::max(agg.max_ms, z.duration_ms);
    }
    return agg;
}

auto Profiler::aggregates() const -> std::vector<ZoneAggregate> {
    std::vector<ZoneAggregate> out;
    out.reserve(zones_.size());
    for (const auto &z : zones_) {
        auto it =
            std::ranges::find_if(out, [&](const ZoneAggregate &a) -> bool { return same_zone_name(a.name, z.name); });
        if (it == out.end()) {
            out.push_back(
                ZoneAggregate{.name = z.name, .call_count = 1, .total_ms = z.duration_ms, .max_ms = z.duration_ms});
        } else {
            ++it->call_count;
            it->total_ms += z.duration_ms;
            it->max_ms = std::max(it->max_ms, z.duration_ms);
        }
    }
    std::ranges::sort(out,
                      [](const ZoneAggregate &a, const ZoneAggregate &b) -> bool { return a.total_ms > b.total_ms; });
    return out;
}

// ---------------------------------------------------------------------------
// 长任务
// ---------------------------------------------------------------------------

auto Profiler::set_long_task_threshold_ms(double ms) -> void { long_task_threshold_ms_ = ms; }

auto Profiler::long_task_threshold_ms() const -> double { return long_task_threshold_ms_; }

auto Profiler::long_tasks() const -> const std::vector<ZoneSample> & { return long_tasks_; }

auto Profiler::total_long_task_count() const -> std::uint64_t { return total_long_tasks_; }

// ---------------------------------------------------------------------------
// 容量与诊断
// ---------------------------------------------------------------------------

auto Profiler::set_zone_capacity(std::size_t capacity) -> void {
    capacity_ = capacity == 0 ? 1 : capacity;
    zones_.clear();
    zones_.reserve(capacity_);
}

auto Profiler::zone_capacity() const -> std::size_t { return capacity_; }

auto Profiler::dropped_zones() const -> std::uint64_t { return dropped_; }

auto Profiler::unbalanced_zones() const -> std::uint64_t { return unbalanced_; }

// ---------------------------------------------------------------------------
// 开关与清理
// ---------------------------------------------------------------------------

auto Profiler::set_enabled(bool on) -> void { enabled_ = on; }

auto Profiler::is_enabled() const -> bool { return enabled_; }

auto Profiler::reset() -> void {
    zones_.clear();
    long_tasks_.clear();
    depth_ = 0;
    frame_index_ = 0;
    dropped_ = 0;
    unbalanced_ = 0;
    total_long_tasks_ = 0;
    frame_start_ms_ = Stopwatch::now_ms();
}

auto Profiler::report_text() const -> std::string {
    std::string out;
    out += "zone                                   calls      total_ms        max_ms\n";
    out += "-----------------------------------------------------------------------";
    for (const auto &agg : aggregates()) {
        out += aurora::internal::string_format("\n%-36s %7u %13.3f %13.3f", agg.name != nullptr ? agg.name : "<null>",
                                               agg.call_count, agg.total_ms, agg.max_ms);
    }
    return out;
}

}  // namespace aurora

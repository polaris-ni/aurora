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
    if (!tw.capturing()) return;
    const Profiler &prof = Profiler::instance();
    tw.capture_frame(prof);
    tw.capture_counters(prof.frame_index(), Stopwatch::now_ms(), RenderCounters::current());
#endif
}

} // namespace detail

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

} // namespace

Profiler::Profiler() : m_frame_start_ms(Stopwatch::now_ms()) {
    m_zones.reserve(m_capacity);
    m_long_tasks.reserve(AURORA_LONG_TASK_CAPACITY);
}

auto Profiler::instance() -> Profiler & {
    static Profiler s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// 帧协议
// ---------------------------------------------------------------------------

auto Profiler::begin_frame() -> void {
    if (m_depth != 0) {
        // 上一帧存在未闭合 zone：计入配对错误并强制复位，避免错误跨帧传播。
        m_unbalanced += static_cast<std::uint64_t>(m_depth);
        m_depth = 0;
    }
    m_zones.clear();
    m_long_tasks.clear();
    m_frame_start_ms = Stopwatch::now_ms();
}

auto Profiler::end_frame() -> void {
    if (m_depth != 0) {
        m_unbalanced += static_cast<std::uint64_t>(m_depth);
        m_depth = 0;
    }
    ++m_frame_index;
}

auto Profiler::frame_index() const -> std::uint64_t { return m_frame_index; }

auto Profiler::frame_start_ms() const -> double { return m_frame_start_ms; }

// ---------------------------------------------------------------------------
// 作用域协议（热路径：无堆分配、无字符串操作）
// ---------------------------------------------------------------------------

auto Profiler::begin_zone(const char *name) -> void {
    if (!m_enabled) {
        return;
    }
    const std::size_t idx = m_depth++;
    if (idx >= AURORA_MAX_ZONE_DEPTH) {
        ++m_dropped; // 嵌套过深：丢弃样本，但仍维持深度以保证 end_zone 配对
        return;
    }
    OpenZone &z = m_stack[idx]; // NOLINT(*-pro-bounds-constant-array-index)
    z.name = name;
    z.start_ms = Stopwatch::now_ms() - m_frame_start_ms;
    z.watch.reset();
}

auto Profiler::end_zone() -> void {
    if (!m_enabled) {
        return;
    }
    if (m_depth == 0) {
        ++m_unbalanced; // end 多于 begin
        return;
    }
    const std::size_t idx = --m_depth;
    if (idx >= AURORA_MAX_ZONE_DEPTH) {
        return; // 进入时已记 dropped
    }

    const OpenZone &z = m_stack[idx]; // NOLINT(*-pro-bounds-constant-array-index)
    const double duration_ms = z.watch.elapsed_ms();
    const ZoneSample sample{
        .name = z.name, .start_ms = z.start_ms, .duration_ms = duration_ms, .depth = static_cast<std::uint16_t>(idx)
    };

    if (duration_ms >= m_long_task_threshold_ms) {
        ++m_total_long_tasks;
        if (m_long_tasks.size() < m_long_tasks.capacity()) {
            m_long_tasks.push_back(sample);
        }
    }

    if (m_zones.size() < m_capacity) {
        m_zones.push_back(sample);
    } else {
        ++m_dropped;
    }
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

auto Profiler::frame_zones() const -> const std::vector<ZoneSample> & { return m_zones; }

auto Profiler::aggregate(const char *name) const -> ZoneAggregate {
    ZoneAggregate agg{};
    agg.name = name;
    for (const auto &z : m_zones) {
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
    out.reserve(m_zones.size());
    for (const auto &z : m_zones) {
        auto it =
            std::ranges::find_if(out, [&](const ZoneAggregate &a) -> bool { return same_zone_name(a.name, z.name); });
        if (it == out.end()) {
            out.push_back(
                ZoneAggregate{ .name = z.name, .call_count = 1, .total_ms = z.duration_ms, .max_ms = z.duration_ms });
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

auto Profiler::set_long_task_threshold_ms(double ms) -> void { m_long_task_threshold_ms = ms; }

auto Profiler::long_task_threshold_ms() const -> double { return m_long_task_threshold_ms; }

auto Profiler::long_tasks() const -> const std::vector<ZoneSample> & { return m_long_tasks; }

auto Profiler::total_long_task_count() const -> std::uint64_t { return m_total_long_tasks; }

// ---------------------------------------------------------------------------
// 容量与诊断
// ---------------------------------------------------------------------------

auto Profiler::set_zone_capacity(std::size_t capacity) -> void {
    m_capacity = capacity == 0 ? 1 : capacity;
    m_zones.clear();
    m_zones.reserve(m_capacity);
}

auto Profiler::zone_capacity() const -> std::size_t { return m_capacity; }

auto Profiler::dropped_zones() const -> std::uint64_t { return m_dropped; }

auto Profiler::unbalanced_zones() const -> std::uint64_t { return m_unbalanced; }

// ---------------------------------------------------------------------------
// 开关与清理
// ---------------------------------------------------------------------------

auto Profiler::set_enabled(bool on) -> void { m_enabled = on; }

auto Profiler::is_enabled() const -> bool { return m_enabled; }

auto Profiler::reset() -> void {
    m_zones.clear();
    m_long_tasks.clear();
    m_depth = 0;
    m_frame_index = 0;
    m_dropped = 0;
    m_unbalanced = 0;
    m_total_long_tasks = 0;
    m_frame_start_ms = Stopwatch::now_ms();
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

} // namespace aurora

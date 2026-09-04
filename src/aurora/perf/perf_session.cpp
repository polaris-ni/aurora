#include "aurora/perf/perf_session.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "aurora/core/string_util.h"

namespace aurora {

namespace {

/// @brief zone 名比较：同名字面量可能来自不同 TU 的不同地址，故按内容比较。
[[nodiscard]] auto same_zone_name(const char *a, const char *b) -> bool {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    return std::strcmp(a, b) == 0;
}

/**
 * @brief 线性插值分位数。
 * @param sorted 升序样本（非空）
 * @param p 分位 [0,1]
 */
[[nodiscard]] auto percentile(const std::vector<double> &sorted, double p) -> double {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double pos = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const std::size_t hi = lo + 1;
    if (hi >= sorted.size()) {
        return sorted.back();
    }
    const double frac = pos - static_cast<double>(lo);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    return (sorted[lo] * (1.0 - frac)) + (sorted[hi] * frac);
}

} // namespace

// ---------------------------------------------------------------------------
// PerfReport
// ---------------------------------------------------------------------------

auto PerfReport::over_budget_ratio() const -> double {
    if (frame_count == 0) {
        return 0.0;
    }
    return static_cast<double>(over_budget_frames) / static_cast<double>(frame_count);
}

auto PerfReport::avg_dirty_area_ratio() const -> double {
    if (frame_count == 0) {
        return 0.0;
    }
    return counters_sum.dirty_area_ratio / static_cast<double>(frame_count);
}

auto PerfReport::to_json() const -> std::string {
    std::string out;
    out.reserve(1024 + (zones.size() * 96));

    out += internal::string_format("{\"name\":\"%s\",\"frame_count\":%zu,\"total_ms\":%.3f,"
                                   "\"avg_frame_ms\":%.3f,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,"
                                   "\"best_ms\":%.3f,\"worst_ms\":%.3f,\"jitter_ms\":%.3f,"
                                   "\"frame_budget_ms\":%.2f,\"over_budget_frames\":%zu,\"over_budget_ratio\":%.4f,"
                                   "\"long_task_count\":%zu,\"full_redraw_frames\":%zu,\"avg_dirty_area_ratio\":%.4f,",
                                   name.c_str(), frame_count, total_ms, avg_frame_ms, p50_ms, p95_ms, p99_ms, best_ms,
                                   worst_ms, jitter_ms, frame_budget_ms, over_budget_frames, over_budget_ratio(),
                                   long_task_count, full_redraw_frames, avg_dirty_area_ratio());

    out += "\"counters_sum\":";
    out += counters_sum.to_json();
    out += ",\"counters_max\":";
    out += counters_max.to_json();

    out += ",\"zones\":[";
    bool first = true;
    for (const ZoneAggregate &z : zones) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += internal::string_format(R"({"name":"%s","calls":%u,"total_ms":%.3f,"max_ms":%.3f})",
                                       z.name != nullptr ? z.name : "<null>", z.call_count, z.total_ms, z.max_ms);
    }
    out += "]}";
    return out;
}

auto PerfReport::to_markdown() const -> std::string {
    std::string out;
    out.reserve(1024 + (zones.size() * 96));

    out += internal::string_format("### PerfReport · %s\n\n", name.c_str());

    out += "| 指标 | 值 |\n|------|----|\n";

    const auto row_f = [&](const char *k, double v, const char *unit) -> void {
        out += internal::string_format("| %s | %.3f %s |\n", k, v, unit);
    };
    const auto row_z = [&](const char *k, std::size_t v) -> void {
        out += internal::string_format("| %s | %zu |\n", k, v);
    };

    row_z("frames", frame_count);
    row_f("avg", avg_frame_ms, "ms");
    row_f("p50", p50_ms, "ms");
    row_f("p95", p95_ms, "ms");
    row_f("**p99**", p99_ms, "ms");
    row_f("worst", worst_ms, "ms");
    row_f("best", best_ms, "ms");
    row_f("**jitter**", jitter_ms, "ms");
    row_z("over budget", over_budget_frames);
    row_z("long tasks", long_task_count);
    row_z("**full redraw frames**", full_redraw_frames);
    row_f("avg dirty area", avg_dirty_area_ratio() * 100.0, "%");

    // 计数器峰值：跨帧汇总读数
    out += "\n| 计数器（峰值 / 累计） | 峰值 | 累计 |\n|------|------|------|\n";
    const auto row_c = [&](const char *k, unsigned long long mx, unsigned long long sum) -> void {
        out += internal::string_format("| %s | %llu | %llu |\n", k, mx, sum);
    };
    row_c("layout_nodes", counters_max.layout_nodes, counters_sum.layout_nodes);
    row_c("paint_nodes", counters_max.paint_nodes, counters_sum.paint_nodes);
    row_c("dl_records", counters_max.dl_records, counters_sum.dl_records);
    row_c("dl_replays", counters_max.dl_replays, counters_sum.dl_replays);
    row_c("draw_calls", counters_max.draw_calls, counters_sum.draw_calls);
    row_c("draw_texts", counters_max.draw_texts, counters_sum.draw_texts);
    row_c("glyphs_rendered", counters_max.glyphs_rendered, counters_sum.glyphs_rendered);
    row_c("pixels_filled", counters_max.pixels_filled, counters_sum.pixels_filled);
    row_c("scroll_buffer_bytes", counters_max.scroll_buffer_bytes, counters_sum.scroll_buffer_bytes);

    if (!zones.empty()) {
        out += "\n| zone | calls | total_ms | max_ms |\n|------|-------|----------|--------|\n";
        for (const ZoneAggregate &z : zones) {
            out += internal::string_format("| %s | %u | %.3f | %.3f |\n", z.name != nullptr ? z.name : "<null>",
                                           z.call_count, z.total_ms, z.max_ms);
        }
    }
    return out;
}

auto PerfReport::csv_header() -> std::string {
    return std::string("name,frame_count,avg_ms,p50_ms,p95_ms,p99_ms,best_ms,worst_ms,jitter_ms,"
                       "over_budget_frames,long_task_count,full_redraw_frames,avg_dirty_area_ratio,") +
           std::string(RenderCounters::csv_header());
}

auto PerfReport::to_csv_row() const -> std::string {
    return internal::string_format("%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu,%zu,%zu,%.4f,", name.c_str(),
                                   frame_count, avg_frame_ms, p50_ms, p95_ms, p99_ms, best_ms, worst_ms, jitter_ms,
                                   over_budget_frames, long_task_count, full_redraw_frames, avg_dirty_area_ratio()) +
           counters_max.to_csv_row();
}

// ---------------------------------------------------------------------------
// PerfSession
// ---------------------------------------------------------------------------

PerfSession::PerfSession(std::string name, std::size_t reserve_frames) : name_(std::move(name)) {
    frame_ms_.reserve(reserve_frames == 0 ? AURORA_DEFAULT_RESERVE_FRAMES : reserve_frames);
}

auto PerfSession::begin(std::size_t reserve_frames) -> void {
    clear();
    if (reserve_frames > 0) {
        frame_ms_.reserve(reserve_frames);
    }
}

auto PerfSession::record_frame(double frame_ms) -> void {
    record_frame(frame_ms, RenderCounters::current());
    // zone 与长任务只有在插桩开启时才有数据；关闭时上面已按空计数记账。
}

auto PerfSession::record_frame(double frame_ms, const RenderCounters &counters) -> void {
    frame_ms_.push_back(frame_ms);

    sum_.add(counters);
    max_.merge_max(counters);
    if (counters.full_redraw) {
        ++full_redraw_frames_;
    }

    if constexpr (profiling_enabled()) {
        const Profiler &prof = Profiler::instance();
        long_tasks_ += prof.long_tasks().size();
        merge_zones(prof.aggregates());
    }
}

auto PerfSession::merge_zones(const std::vector<ZoneAggregate> &frame_zones) -> void {
    for (const ZoneAggregate &z : frame_zones) {
        auto it = std::ranges::find_if(zones_,
                                       [&](const ZoneAggregate &a) -> bool { return same_zone_name(a.name, z.name); });
        if (it == zones_.end()) {
            zones_.push_back(z);
        } else {
            it->call_count += z.call_count;
            it->total_ms += z.total_ms;
            it->max_ms = std::max(it->max_ms, z.max_ms);
        }
    }
}

auto PerfSession::frame_count() const -> std::size_t { return frame_ms_.size(); }

auto PerfSession::frame_times_ms() const -> const std::vector<double> & { return frame_ms_; }

auto PerfSession::set_frame_budget_ms(double ms) -> void { frame_budget_ms_ = ms; }

auto PerfSession::frame_budget_ms() const -> double { return frame_budget_ms_; }

auto PerfSession::name() const -> const std::string & { return name_; }

auto PerfSession::report() const -> PerfReport {
    PerfReport r{};
    r.name = name_;
    r.frame_count = frame_ms_.size();
    r.frame_budget_ms = frame_budget_ms_;
    r.counters_sum = sum_;
    r.counters_max = max_;
    r.long_task_count = long_tasks_;
    r.full_redraw_frames = full_redraw_frames_;
    r.zones = zones_;
    std::ranges::sort(r.zones,
                      [](const ZoneAggregate &a, const ZoneAggregate &b) -> bool { return a.total_ms > b.total_ms; });

    if (frame_ms_.empty()) {
        return r;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    for (const double ms : frame_ms_) {
        sum += ms;
        sum_sq += ms * ms;
        if (ms > frame_budget_ms_) {
            ++r.over_budget_frames;
        }
    }
    const auto n = static_cast<double>(frame_ms_.size());
    r.total_ms = sum;
    r.avg_frame_ms = sum / n;

    const double var = (sum_sq / n) - (r.avg_frame_ms * r.avg_frame_ms);
    r.jitter_ms = var > 0.0 ? std::sqrt(var) : 0.0;

    std::vector<double> sorted = frame_ms_;
    std::ranges::sort(sorted);
    r.best_ms = sorted.front();
    r.worst_ms = sorted.back();
    r.p50_ms = percentile(sorted, 0.50);
    r.p95_ms = percentile(sorted, 0.95);
    r.p99_ms = percentile(sorted, 0.99);
    return r;
}

auto PerfSession::clear() -> void {
    frame_ms_.clear();
    zones_.clear();
    sum_ = RenderCounters{};
    max_ = RenderCounters{};
    long_tasks_ = 0;
    full_redraw_frames_ = 0;
}

} // namespace aurora
#include "aurora/perf/perf_log.h"

#include "aurora/app/perf_overlay.h"
#include "aurora/core/log.h"
#include "aurora/core/string_util.h"
#include "aurora/perf/counters.h"

namespace aurora {

namespace {

/// @brief `FrameStats` 部分的 CSV 列名（计数器列由 `RenderCounters::csv_header()` 追加）。
constexpr auto AURORA_FRAME_CSV_HEADER =
    "fps,avg_ms,p99_ms,jitter_ms,dropped,hitch,idle,layout_ms,paint_ms,present_ms,total_frames,profiling";

}  // namespace

auto PerfLog::enable(int interval_frames) -> void {
    s_enabled_ = true;
    s_interval_ = interval_frames;
    s_counter_ = 0;
}

auto PerfLog::disable() -> void { s_enabled_ = false; }

auto PerfLog::enabled() -> bool { return s_enabled_; }

auto PerfLog::on_frame_end() -> void {
    if (!s_enabled_) {
        return;
    }
    ++s_counter_;
    if (s_counter_ >= s_interval_) {
        s_counter_ = 0;
        log_summary();
    }
}

auto PerfLog::log_summary() -> void {
    const auto &s = FrameStats::instance();
    AURORA_LOG_INFO("perf", "FPS=", s.fps(), " avg=", s.avg_frame_ms(), "ms", " P99=", s.percentile_ms(0.99), "ms",
                    " jitter=", s.jitter_ms(), "ms", " dropped=", s.dropped_frame_count(), " hitch=", s.hitch_count(),
                    " layout=", s.avg_layout_ms(), "ms", " paint=", s.avg_paint_ms(), "ms",
                    " present=", s.avg_present_ms(), "ms");
    // 计数器仅在插桩开启时有意义；关闭时全零，输出徒增噪声。
    if constexpr (profiling_enabled()) {
        const RenderCounters &c = RenderCounters::current();
        AURORA_LOG_INFO("perf", "counters layout_nodes=", c.layout_nodes, " paint_nodes=", c.paint_nodes,
                        " dl_records=", c.dl_records, " dl_replays=", c.dl_replays, " draw_calls=", c.draw_calls,
                        " glyphs=", c.glyphs_rendered, " dirty=", c.dirty_rect_count,
                        " full_redraw=", c.full_redraw ? 1 : 0);
    }
}

auto PerfLog::snapshot_json() -> std::string {
    const auto &s = FrameStats::instance();
    const std::string prefix = aurora::internal::string_format(
        "{\"fps\":%.1f,\"avg_ms\":%.1f,\"p99_ms\":%.1f,\"jitter_ms\":%.1f,"
        "\"dropped\":%zu,\"hitch\":%zu,\"idle\":%zu,"
        "\"layout_ms\":%.2f,\"paint_ms\":%.2f,\"present_ms\":%.2f,"
        "\"total_frames\":%zu,\"profiling\":%s,\"counters\":",
        s.fps(), s.avg_frame_ms(), s.percentile_ms(0.99), s.jitter_ms(), s.dropped_frame_count(), s.hitch_count(),
        s.idle_frame_count(), s.avg_layout_ms(), s.avg_paint_ms(), s.avg_present_ms(), s.total_frames(),
        profiling_enabled() ? "true" : "false");
    return prefix + RenderCounters::current().to_json() + "}";
}

auto PerfLog::csv_header() -> std::string {
    return std::string(AURORA_FRAME_CSV_HEADER) + "," + std::string(RenderCounters::csv_header());
}

auto PerfLog::snapshot_csv() -> std::string {
    const auto &s = FrameStats::instance();
    const std::string row = aurora::internal::string_format(
        "%.1f,%.1f,%.1f,%.1f,%zu,%zu,%zu,%.2f,%.2f,%.2f,%zu,%d,", s.fps(), s.avg_frame_ms(), s.percentile_ms(0.99),
        s.jitter_ms(), s.dropped_frame_count(), s.hitch_count(), s.idle_frame_count(), s.avg_layout_ms(),
        s.avg_paint_ms(), s.avg_present_ms(), s.total_frames(), profiling_enabled() ? 1 : 0);
    return "# " + csv_header() + "\n" + row + RenderCounters::current().to_csv_row();
}

auto PerfLog::dump_json() -> void { AURORA_LOG_RAW("perf", snapshot_json(), "\n"); }

auto PerfLog::dump_csv() -> void { AURORA_LOG_RAW("perf", snapshot_csv(), "\n"); }

}  // namespace aurora

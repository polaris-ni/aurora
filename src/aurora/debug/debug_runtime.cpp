// 运行时信息导出门面实现（specification/06-app-platform.md §11.2）。
// 双门控：AURORA_ENABLE_DEBUG 下实现真实逻辑；Release 下所有函数返回 unavailable/空，零调试代码。

#include "aurora/debug/debug_runtime.h"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "aurora/app/perf_overlay.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/string_util.h"
#include "aurora/debug/debug_trace.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/perf/perf_log.h"
#include "aurora/widget/widget.h"

namespace aurora::debug {

namespace {

constexpr auto AURORA_UNAVAILABLE_REASON = "AURORA_ENABLE_DEBUG not enabled";

// ---- why_trace 采集缓冲（仅 DEBUG 下存在）----
#ifdef AURORA_ENABLE_DEBUG
struct DirtyTraceEntry {
    DirtyKind kind;
    const char *type_name; // type_name() 返回静态字符串（控件类型名在编译期/静态存储），无需拷贝分配
    std::uint64_t frame;
    bool propagated;
};

constexpr std::size_t AURORA_DIRTY_TRACE_CAP = 256;
// NOLINTBEGIN(cert-err58-cpp,cppcoreguidelines-avoid-non-const-global-variables,*-throwing-static-initialization):
// DEBUG 下 why_trace环形缓冲，需跨调用保持可变状态
std::deque<DirtyTraceEntry> g_dirty_trace;
std::uint64_t g_dirty_trace_total = 0;
// NOLINTEND(cert-err58-cpp,cppcoreguidelines-avoid-non-const-global-variables,*-throwing-static-initialization)

// ASCII flamegraph：按 layout/paint/present 占比画三段，总宽固定。
[[nodiscard]] auto build_flamegraph(double layout_ms, double paint_ms, double present_ms) -> std::string {
    const double total = layout_ms + paint_ms + present_ms;
    if (total <= 0.0) {
        return "[ no phase samples ]";
    }
    constexpr int width = 48;
    const int n_layout = static_cast<int>(std::lround(layout_ms / total * static_cast<double>(width)));
    const int n_paint = static_cast<int>(std::lround(paint_ms / total * static_cast<double>(width)));
    const int n_present = width - n_layout - n_paint;
    std::string bar;
    bar.append(n_layout, 'L');  // Layout
    bar.append(n_paint, 'P');   // Paint
    bar.append(n_present, 'R'); // pResent
    const std::string text = internal::string_format("frame phases (ms): layout=%.3f paint=%.3f present=%.3f\n"
                                                     " [%s]\n"
                                                     " L=layout  P=paint  R=present (width=%d, total=%.3fms)",
                                                     layout_ms, paint_ms, present_ms, bar.c_str(), width, total);
    return text;
}
#endif

} // namespace

// ───────────────────────────── widget_tree ─────────────────────────────

auto widget_tree(const Node &root) -> Json {
#ifdef AURORA_ENABLE_DEBUG
    return Inspector::tree_json_full(root);
#else
    (void)root;
    return Json{ { "available", false }, { "reason", AURORA_UNAVAILABLE_REASON } };
#endif
}

// ───────────────────────────── perf_snapshot ────────────────────────────

auto perf_snapshot() -> Json {
#ifdef AURORA_ENABLE_DEBUG
    Json j;
    const FrameStats &fs = FrameStats::instance();
    j["fps"] = fs.fps();
    j["avg_frame_ms"] = fs.avg_frame_ms();
    j["worst_frame_ms"] = fs.worst_frame_ms();
    j["jitter_ms"] = fs.jitter_ms();
    j["p50_ms"] = fs.percentile_ms(0.5);
    j["p99_ms"] = fs.percentile_ms(0.99);
    j["dropped_frames"] = fs.dropped_frame_count();
    j["dropped_ratio"] = fs.dropped_frame_ratio();
    j["hitches"] = fs.hitch_count();
    j["idle_frames"] = fs.idle_frame_count();
    j["total_frames"] = fs.total_frames();
    j["frame_budget_ms"] = fs.frame_budget_ms();
    try {
        j["perf_log"] = Json::parse(PerfLog::snapshot_json());
    } catch (...) {
        j["perf_log"] = Json{};
    }
    return j;
#else
    return Json{ { "available", false }, { "reason", AURORA_UNAVAILABLE_REASON } };
#endif
}

// ─────────────────────────── frame_phase_timeline ───────────────────────

auto frame_phase_timeline(std::size_t limit) -> Json {
#ifdef AURORA_ENABLE_DEBUG
    Json j;
    const FrameStats &fs = FrameStats::instance();
    j["avg_layout_ms"] = fs.avg_layout_ms();
    j["avg_paint_ms"] = fs.avg_paint_ms();
    j["avg_present_ms"] = fs.avg_present_ms();
    j["fps"] = fs.fps();
    j["avg_frame_ms"] = fs.avg_frame_ms();
    j["worst_frame_ms"] = fs.worst_frame_ms();
    j["dropped_frames"] = fs.dropped_frame_count();
    j["hitches"] = fs.hitch_count();
    Json frames = Json::array();
    const std::size_t n = fs.window_size();
    const std::size_t take = n < limit ? n : limit;
    for (std::size_t i = 0; i < take; ++i) {
        frames.push_back(fs.frame_at(i) * 1000.0); // 秒→毫秒
    }
    j["recent_frame_ms"] = frames;
    j["flamegraph"] = build_flamegraph(fs.avg_layout_ms(), fs.avg_paint_ms(), fs.avg_present_ms());
    return j;
#else
    (void)limit;
    return Json{ { "available", false }, { "reason", AURORA_UNAVAILABLE_REASON } };
#endif
}

// ─────────────────────────────── why_trace ──────────────────────────────

auto why_trace(std::size_t limit) -> Json {
#ifdef AURORA_ENABLE_DEBUG
    Json j;
    j["count"] = g_dirty_trace.size();
    j["total_recorded"] = g_dirty_trace_total;
    Json entries = Json::array();
    const std::size_t n = g_dirty_trace.size();
    const std::size_t take = n < limit ? n : limit;
    for (std::size_t i = 0; i < take; ++i) {
        const DirtyTraceEntry &e = g_dirty_trace[n - 1 - i]; // 最新在前
        Json o;
        o["kind"] = e.kind == DirtyKind::Layout ? "layout" : "paint";
        o["type"] = e.type_name;
        o["frame"] = e.frame;
        o["propagated"] = e.propagated;
        entries.push_back(o);
    }
    j["entries"] = entries;
    return j;
#else
    (void)limit;
    return Json{ { "available", false }, { "reason", AURORA_UNAVAILABLE_REASON } };
#endif
}

// ────────────────────────────── diagnostics ─────────────────────────────

auto diagnostics() -> Json {
#ifdef AURORA_ENABLE_DEBUG
    const std::vector<Diagnostic> &ds = Diagnostics::get_last_diagnostics();
    Json j;
    j["count"] = ds.size();
    Json arr = Json::array();
    for (const Diagnostic &d : ds) {
        Json e;
        e["severity"] = std::string(d.severity_str());
        e["category"] = std::string(d.category_str());
        e["message"] = d.message;
        e["where"] = d.where;
        e["code"] = d.code;
        if (d.fix) {
            Json f;
            f["code"] = d.fix->code;
            f["description"] = d.fix->description;
            e["fix"] = f;
        }
        arr.push_back(e);
    }
    j["diagnostics"] = arr;
    return j;
#else
    return Json{ { "available", false }, { "reason", AURORA_UNAVAILABLE_REASON } };
#endif
}

// ─────────────────────── detail::record_dirty（采集入口）─────────────────

namespace detail {

auto record_dirty(DirtyKind kind, const char *type_name, std::uint64_t frame, bool propagated) -> void {
#ifdef AURORA_ENABLE_DEBUG
    if (g_dirty_trace.size() >= AURORA_DIRTY_TRACE_CAP) {
        g_dirty_trace.pop_front();
    }
    g_dirty_trace.push_back({ .kind = kind, .type_name = type_name, .frame = frame, .propagated = propagated });
    ++g_dirty_trace_total;
#else
    (void)kind;
    (void)type_name;
    (void)frame;
    (void)propagated;
#endif
}

} // namespace detail
} // namespace aurora::debug

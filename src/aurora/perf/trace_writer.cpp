#include "aurora/perf/trace_writer.h"

#include <fstream>

#include "aurora/core/log.h"
#include "aurora/core/string_util.h"

namespace aurora {

namespace {

/// @brief Trace 事件的进程 / 线程 id（单线程 UI 模型，固定值即可）。
constexpr int AURORA_TRACE_PID = 1;
constexpr int AURORA_TRACE_TID = 1;

/// @brief 最小 JSON 字符串转义（zone 名为标识符，通常无需转义，此处仅作防御）。
auto append_escaped(std::string &out, const char *s) -> void {
    if (s == nullptr) {
        out += "<null>";
        return;
    }
    for (const char *p = s; *p != '\0'; ++p) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const char c = *p;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += internal::string_format("\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
            break;
        }
    }
}

/// @brief 毫秒 → 微秒（Trace Event 规范的时间单位）。
[[nodiscard]] constexpr auto to_us(double ms) -> double { return ms * 1000.0; }

} // namespace

TraceWriter::TraceWriter() {
    m_events.reserve(m_capacity);
    m_counters.reserve(m_counter_capacity);
}

auto TraceWriter::instance() -> TraceWriter & {
    static TraceWriter s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// 录制开关
// ---------------------------------------------------------------------------

auto TraceWriter::begin_capture() -> void {
    clear();
    m_capturing = true;
}

auto TraceWriter::end_capture() -> void { m_capturing = false; }

auto TraceWriter::capturing() const -> bool { return m_capturing; }

// ---------------------------------------------------------------------------
// 采集（热路径：仅 POD 拷贝，容量已预留）
// ---------------------------------------------------------------------------

auto TraceWriter::capture_frame(const Profiler &prof) -> void {
    if (!m_capturing) {
        return;
    }

    const double frame_start = prof.frame_start_ms();
    const std::uint64_t frame = prof.frame_index();
    const double threshold = prof.long_task_threshold_ms();

    for (const ZoneSample &z : prof.frame_zones()) {
        if (m_events.size() >= m_capacity) {
            ++m_dropped;
            continue;
        }
        m_events.push_back(TraceEvent{
            .name = z.name,
            .ts_ms = frame_start + z.start_ms,
            .dur_ms = z.duration_ms,
            .frame_index = frame,
            .depth = z.depth,
            .phase = TracePhase::Complete,
            .long_task = z.duration_ms >= threshold,
        });
    }
}

auto TraceWriter::capture_counters(std::uint64_t frame_index, double ts_ms, const RenderCounters &counters) -> void {
    if (!m_capturing) {
        return;
    }
    if (m_counters.size() >= m_counter_capacity) {
        ++m_dropped;
        return;
    }
    m_counters.push_back(TraceCounterSample{ .ts_ms = ts_ms, .frame_index = frame_index, .counters = counters });
}

auto TraceWriter::add_complete_event(const char *name, double ts_ms, double dur_ms, std::uint16_t depth,
                                     std::uint64_t frame_index) -> void {
    if (!m_capturing) {
        return;
    }
    if (m_events.size() >= m_capacity) {
        ++m_dropped;
        return;
    }
    m_events.push_back(TraceEvent{ .name = name,
                                   .ts_ms = ts_ms,
                                   .dur_ms = dur_ms,
                                   .frame_index = frame_index,
                                   .depth = depth,
                                   .phase = TracePhase::Complete,
                                   .long_task = false });
}

auto TraceWriter::add_instant_event(const char *name, double ts_ms, std::uint64_t frame_index) -> void {
    if (!m_capturing) {
        return;
    }
    if (m_events.size() >= m_capacity) {
        ++m_dropped;
        return;
    }
    m_events.push_back(TraceEvent{ .name = name,
                                   .ts_ms = ts_ms,
                                   .dur_ms = 0.0,
                                   .frame_index = frame_index,
                                   .depth = 0,
                                   .phase = TracePhase::Instant,
                                   .long_task = false });
}

// ---------------------------------------------------------------------------
// 查询与容量
// ---------------------------------------------------------------------------

auto TraceWriter::event_count() const -> std::size_t { return m_events.size(); }

auto TraceWriter::counter_sample_count() const -> std::size_t { return m_counters.size(); }

auto TraceWriter::dropped_events() const -> std::uint64_t { return m_dropped; }

auto TraceWriter::events() const -> const std::vector<TraceEvent> & { return m_events; }

auto TraceWriter::counter_samples() const -> const std::vector<TraceCounterSample> & { return m_counters; }

auto TraceWriter::set_capacity(std::size_t events, std::size_t counter_samples) -> void {
    m_capacity = events == 0 ? 1 : events;
    m_counter_capacity = counter_samples == 0 ? 1 : counter_samples;
    m_events.clear();
    m_counters.clear();
    m_events.reserve(m_capacity);
    m_counters.reserve(m_counter_capacity);
    m_dropped = 0;
}

auto TraceWriter::capacity() const -> std::size_t { return m_capacity; }

auto TraceWriter::clear() -> void {
    m_events.clear();
    m_counters.clear();
    m_dropped = 0;
}

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

auto TraceWriter::to_json() const -> std::string {
    std::string out;
    // 粗估：每条事件约 160 字节，计数事件约 400 字节，一次性预留避免反复扩容。
    out.reserve((m_events.size() * 160) + (m_counters.size() * 400) + 256);

    out += "[\n";

    bool first = true;
    const auto sep = [&]() -> void {
        if (!first) {
            out += ",\n";
        }
        first = false;
    };

    // ---- 元数据：进程 / 线程命名（Perfetto 里显示为可读轨道名）----
    sep();
    out += aurora::internal::string_format(
        R"({"name":"process_name","ph":"M","pid":%d,"tid":%d,"args":{"name":"aurora"}})", AURORA_TRACE_PID,
        AURORA_TRACE_TID);

    sep();
    out += aurora::internal::string_format(R"({"name":"thread_name","ph":"M","pid":%d,"tid":%d,"args":{"name":"ui"}})",
                                           AURORA_TRACE_PID, AURORA_TRACE_TID);

    // ---- zone / 瞬时事件 ----
    for (const TraceEvent &e : m_events) {
        sep();
        out += R"({"name":")";
        append_escaped(out, e.name);
        if (e.phase == TracePhase::Complete) {
            out +=
                aurora::internal::string_format(R"(","cat":"aurora","ph":"X","ts":%.3f,"dur":%.3f,"pid":%d,"tid":%d,)"
                                                R"("args":{"frame":%llu,"depth":%u,"long_task":%s}})",
                                                to_us(e.ts_ms), to_us(e.dur_ms), AURORA_TRACE_PID, AURORA_TRACE_TID,
                                                static_cast<unsigned long long>(e.frame_index),
                                                static_cast<unsigned>(e.depth), e.long_task ? "true" : "false");
        } else {
            out += aurora::internal::string_format(
                R"(","cat":"aurora","ph":"i","ts":%.3f,"pid":%d,"tid":%d,"s":"t","args":{"frame":%llu}})",
                to_us(e.ts_ms), AURORA_TRACE_PID, AURORA_TRACE_TID, static_cast<unsigned long long>(e.frame_index));
        }
    }

    // ---- 计数器轨道（每帧一条，Perfetto 中每个 key 渲染为一条曲线）----
    for (const TraceCounterSample &s : m_counters) {
        const RenderCounters &c = s.counters;
        sep();
        out += aurora::internal::string_format(
            R"({"name":"RenderCounters","cat":"aurora","ph":"C","ts":%.3f,"pid":%d,"tid":%d,"args":{)"
            R"("frame":%llu,"draw_calls":%u,"fill_rects":%u,"draw_texts":%u,"glyphs_rendered":%u,)"
            R"("layout_nodes":%u,"paint_nodes":%u,"dl_records":%u,"dl_replays":%u,)"
            R"("dirty_rect_count":%u,"dirty_area_ratio":%.4f,"full_redraw":%d}})",
            to_us(s.ts_ms), AURORA_TRACE_PID, AURORA_TRACE_TID, static_cast<unsigned long long>(s.frame_index),
            c.draw_calls, c.fill_rects, c.draw_texts, c.glyphs_rendered, c.layout_nodes, c.paint_nodes, c.dl_records,
            c.dl_replays, c.dirty_rect_count, c.dirty_area_ratio, c.full_redraw ? 1 : 0);
    }

    out += "\n]\n";
    return out;
}

auto TraceWriter::write_json(const char *path) const -> Result<bool> {
    if (path == nullptr || *path == '\0') {
        return make_error(ErrorCode::IOFileNotFound, "TraceWriter::write_json: empty path");
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("TraceWriter::write_json: cannot open ") + path);
    }
    const std::string json = to_json();
    f.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!f.good()) {
        return make_error(ErrorCode::IOFileNotFound, std::string("TraceWriter::write_json: write failed ") + path);
    }
    return Result<bool>{ true };
}

auto TraceWriter::dump_to_log() const -> void { AURORA_LOG_RAW("perf", to_json()); }

} // namespace aurora

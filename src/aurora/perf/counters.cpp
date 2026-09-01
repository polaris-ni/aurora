#include "aurora/perf/counters.h"

#include <algorithm>

#include "aurora/core/string_util.h"

namespace aurora {

auto RenderCounters::current() -> RenderCounters & {
    static RenderCounters s_current{};
    return s_current;
}

auto RenderCounters::reset() -> void { *this = RenderCounters{}; }

auto RenderCounters::add(const RenderCounters &other) -> void {
    draw_calls += other.draw_calls;
    fill_rects += other.fill_rects;
    draw_texts += other.draw_texts;
    glyphs_rendered += other.glyphs_rendered;
    pixels_filled += other.pixels_filled;

    glyph_cache_hits += other.glyph_cache_hits;
    glyph_cache_misses += other.glyph_cache_misses;
    shape_cache_hits += other.shape_cache_hits;
    shape_cache_misses += other.shape_cache_misses;
    dl_replays += other.dl_replays;
    dl_records += other.dl_records;

    layout_nodes += other.layout_nodes;
    paint_nodes += other.paint_nodes;
    relayout_boundaries_hit += other.relayout_boundaries_hit;

    dirty_rect_count += other.dirty_rect_count;
    dirty_area_ratio += other.dirty_area_ratio;
    full_redraw = full_redraw || other.full_redraw;

    scroll_buffer_bytes += other.scroll_buffer_bytes;
}

auto RenderCounters::merge_max(const RenderCounters &other) -> void {
    draw_calls = std::max(draw_calls, other.draw_calls);
    fill_rects = std::max(fill_rects, other.fill_rects);
    draw_texts = std::max(draw_texts, other.draw_texts);
    glyphs_rendered = std::max(glyphs_rendered, other.glyphs_rendered);
    pixels_filled = std::max(pixels_filled, other.pixels_filled);

    glyph_cache_hits = std::max(glyph_cache_hits, other.glyph_cache_hits);
    glyph_cache_misses = std::max(glyph_cache_misses, other.glyph_cache_misses);
    shape_cache_hits = std::max(shape_cache_hits, other.shape_cache_hits);
    shape_cache_misses = std::max(shape_cache_misses, other.shape_cache_misses);
    dl_replays = std::max(dl_replays, other.dl_replays);
    dl_records = std::max(dl_records, other.dl_records);

    layout_nodes = std::max(layout_nodes, other.layout_nodes);
    paint_nodes = std::max(paint_nodes, other.paint_nodes);
    relayout_boundaries_hit = std::max(relayout_boundaries_hit, other.relayout_boundaries_hit);

    dirty_rect_count = std::max(dirty_rect_count, other.dirty_rect_count);
    dirty_area_ratio = std::max(dirty_area_ratio, other.dirty_area_ratio);
    full_redraw = full_redraw || other.full_redraw;

    scroll_buffer_bytes = std::max(scroll_buffer_bytes, other.scroll_buffer_bytes);
}

auto RenderCounters::to_json() const -> std::string {
    std::string json = aurora::internal::string_format(
        "{\"draw_calls\":%u,\"fill_rects\":%u,\"draw_texts\":%u,\"glyphs_rendered\":%u,"
        "\"pixels_filled\":%llu,\"glyph_cache_hits\":%u,\"glyph_cache_misses\":%u,"
        "\"shape_cache_hits\":%u,\"shape_cache_misses\":%u,\"dl_replays\":%u,\"dl_records\":%u,"
        "\"layout_nodes\":%u,\"paint_nodes\":%u,\"relayout_boundaries_hit\":%u,"
        "\"dirty_rect_count\":%u,\"dirty_area_ratio\":%.4f,\"full_redraw\":%s,"
        "\"scroll_buffer_bytes\":%llu}",
        draw_calls, fill_rects, draw_texts, glyphs_rendered, static_cast<unsigned long long>(pixels_filled),
        glyph_cache_hits, glyph_cache_misses, shape_cache_hits, shape_cache_misses, dl_replays, dl_records,
        layout_nodes, paint_nodes, relayout_boundaries_hit, dirty_rect_count, dirty_area_ratio,
        full_redraw ? "true" : "false", static_cast<unsigned long long>(scroll_buffer_bytes));
    if (json.empty()) {
        return "{}";
    }
    return json;
}

auto RenderCounters::csv_header() -> std::string_view {
    return "draw_calls,fill_rects,draw_texts,glyphs_rendered,pixels_filled,"
           "glyph_cache_hits,glyph_cache_misses,shape_cache_hits,shape_cache_misses,"
           "dl_replays,dl_records,layout_nodes,paint_nodes,relayout_boundaries_hit,"
           "dirty_rect_count,dirty_area_ratio,full_redraw,scroll_buffer_bytes";
}

auto RenderCounters::to_csv_row() const -> std::string {
    std::string row = aurora::internal::string_format(
        "%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.4f,%d,%llu", draw_calls, fill_rects, draw_texts,
        glyphs_rendered, static_cast<unsigned long long>(pixels_filled), glyph_cache_hits, glyph_cache_misses,
        shape_cache_hits, shape_cache_misses, dl_replays, dl_records, layout_nodes, paint_nodes,
        relayout_boundaries_hit, dirty_rect_count, dirty_area_ratio, full_redraw ? 1 : 0,
        static_cast<unsigned long long>(scroll_buffer_bytes));
    if (row.empty()) {
        return {};
    }
    return row;
}

} // namespace aurora

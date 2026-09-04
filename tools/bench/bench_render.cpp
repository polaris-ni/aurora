// Rendering benchmark: use HeadlessSurface + aurora::Painter at multiple resolutions/scales to
// produce timing baselines for drawing primitives and end-to-end widget-tree whole-frame paint.
//
// Notes:
// - This program is a "benchmark/diagnostic" tool, not a unit test; it is not wired into CTest
//   (timings are affected by environment jitter, no stable assertions).
// - Output is a structured table (stdout, markdown) for cross-comparing draw cost across sizes/scales.
// - scale takes real effect through `aurora::Painter::set_scale` (logical dp x scale -> physical pixel
//   buffer), covering scale-sensitive paths such as high-DPI text rasterization rather than simply
//   upscaling the resolution.
// - Usage: ./bench_render
//
// Dimension matrix: logical sizes 1280x720 / 1920x1080 / 2560x1440; scales 1.0 / 1.5 / 2.0.
// Primitive scenes: full-screen opaque/translucent fill_rect, linear/radial gradient, shadow, blur
// (multiple radii), composite (rotate + scale), rounded-clip fill, text (incl. CJK); end-to-end:
// widget-tree whole frame, single-control dirtying (dirty-region clip path), single hit_test.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/event/dispatcher.h"
#include "aurora/perf/scroll_bench.h"
#include "aurora/render/font_engine.h"
#include "bench_common.h"

namespace {

// Create a fixed-size Headless window (set scale before begin_frame; physical buffer = logical x scale).
auto make_window(int w, int h, float scale) -> au::Window {
    auto surface = std::make_unique<aurora::HeadlessSurface>();
    surface->painter().set_scale(scale);
    (void)surface->begin_frame(w, h);
    return aurora::Window{std::move(surface)};
}

// Print one table row: scene | logical size | scale | physical size | ms/frame.
auto report(const char *scene, const std::string &size_label, float s, int dev_w, int dev_h, double ms) -> void {
    AURORA_LOG_RAW("bench", "| ", scene, " | ", size_label, " | ", aurora::bench::ffmt(1, s), " | ", dev_w, "x", dev_h,
                   " | ", aurora::bench::ffmt(3, ms), " |\n");
}

// End-to-end widget tree: 20 rows x 10 cols of aurora::Chip (with label text), covering layout +
// text + background drawing.
// out_probe returns one of the chips for the "single-control dirtying" scene to change color and mark dirty.
auto build_tree(std::shared_ptr<aurora::Chip> *out_probe) -> aurora::Node {
    std::vector<aurora::Node> rows;
    for (int r = 0; r < 20; ++r) {
        std::vector<aurora::Node> chips;
        for (int c = 0; c < 10; ++c) {
            auto chip = std::make_shared<aurora::Chip>();
            chip->set_label("item");
            if ((out_probe != nullptr) && r == 10 && c == 5) {
                *out_probe = chip;
            }
            chips.emplace_back(chip);
        }
        rows.emplace_back(std::make_shared<aurora::Row>(aurora::RowProps{.children = std::move(chips)}));
    }
    return aurora::Node{std::make_shared<aurora::Column>(aurora::ColumnProps{.children = std::move(rows)})};
}

// Representative text (Latin + digits + CJK) covering the fallback chain and atlas-cache paths.
constexpr auto AURORA_BENCH_TEXT = "The quick brown fox jumps 0123456789 灰狐跳过懒狗 こんにちは世界 안녕하세요";

// Scroll-scene content tree: `aurora::Scroll` wrapping a column of 200 aurora::Chip (with label
// text), content far taller than the viewport, for aurora::ScrollBenchHarness to run deterministic
// scroll sequences (local convention for time-based gates).
auto build_scroll_tree() -> aurora::Node {
    std::vector<aurora::Node> items;
    items.reserve(200);
    for (int i = 0; i < 200; ++i) {
        auto chip = std::make_shared<aurora::Chip>();
        chip->set_label("row " + std::to_string(i));
        items.emplace_back(std::move(chip));
    }
    auto col = std::make_shared<aurora::Column>(aurora::ColumnProps{.children = std::move(items)});
    return aurora::Node{std::make_shared<aurora::Scroll>(aurora::ScrollProps{.child = aurora::Node{std::move(col)}})};
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
auto main() -> int {
    const std::vector<std::pair<int, int>> bases = {{1280, 720}, {1920, 1080}, {2560, 1440}};
    const std::vector scales = {1.0F, 1.5F, 2.0F};
    constexpr int warmup = 3;
    constexpr int fast_iters = 20;  // fast scenes: fill/gradient/text/clip, etc.
    constexpr int slow_iters = 8;  // slow scenes: shadow/blur/composite/end-to-end, etc.

    AURORA_LOG_RAW("bench", "| scene | size (logical) | scale | device | ms/frame |\n");
    AURORA_LOG_RAW("bench", "|---|---|---|---|---|\n");

    for (const auto &[bw, bh] : bases) {
        for (const float s : scales) {
            const std::string size_label = std::to_string(bw) + "x" + std::to_string(bh);
            const auto w = static_cast<float>(bw);
            const auto h = static_cast<float>(bh);
            const aurora::Rect full{.origin = aurora::Point{.x = 0, .y = 0},
                                    .size = aurora::Size{.width = w, .height = h}};

            // Standalone aurora::Painter: logical bw x bh, physical x scale (primitive benchmark does
            // not go through the Window/widget layer).
            aurora::Painter p;
            p.set_scale(s);
            p.begin(bw, bh);
            const int dw = p.width();
            const int dh = p.height();

            // 1) full-screen opaque fill_rect (row-level fast path).
            report("fill_opaque_full", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.fill_rect(full, aurora::Color{30, 120, 200, 255}); },
                                          warmup, fast_iters));

            // 2) full-screen translucent fill_rect (per-pixel source-over).
            report("fill_alpha_full", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.fill_rect(full, aurora::Color{255, 200, 0, 128}); }, warmup,
                                          fast_iters));

            // 3) full-screen linear gradient.
            const std::vector gcolors = {aurora::Color{250, 250, 255, 255}, aurora::Color{30, 30, 60, 255}};
            const std::vector gstops = {0.0F, 1.0F};
            report("linear_gradient_full", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           p.draw_linear_gradient(full, aurora::Point{.x = 0, .y = 0}, aurora::Point{.x = w, .y = h},
                                                  gcolors, gstops);
                       },
                       warmup, fast_iters));

            // 4) full-screen radial gradient.
            report("radial_gradient_full", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           p.draw_radial_gradient(full, aurora::Point{.x = w * 0.5F, .y = h * 0.5F}, w * 0.5F, gcolors,
                                                  gstops);
                       },
                       warmup, fast_iters));

            // 5) shadow (400x300 card shape, blur 16).
            constexpr aurora::Rect card{.origin = aurora::Point{.x = 40, .y = 40},
                                        .size = aurora::Size{.width = 400, .height = 300}};
            report("shadow_card", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void { p.draw_shadow(card, 4.0F, 8.0F, 16.0F, aurora::Color{0, 0, 0, 96}); }, warmup,
                       slow_iters));

            // 6) blur_region at multiple radii (fixed 320x240 area, typical frosted-glass magnitude).
            constexpr aurora::Rect blur_area{.origin = aurora::Point{.x = 60, .y = 60},
                                             .size = aurora::Size{.width = 320, .height = 240}};
            report("blur_r4", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.blur_region(blur_area, 4.0F); }, warmup, slow_iters));
            report("blur_r16", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.blur_region(blur_area, 16.0F); }, warmup, slow_iters));

            // 7) composite: 256x256 offscreen subtree rotated 30 deg + scaled 1.2, composited (modifier-transform
            // path).
            aurora::Painter off;
            off.set_scale(s);
            off.begin(256, 256);
            off.fill_rect(aurora::Rect{.origin = aurora::Point{.x = 0, .y = 0},
                                       .size = aurora::Size{.width = 256, .height = 256}},
                          aurora::Color{200, 80, 40, 255});
            constexpr aurora::Point cc{.x = 128.0F, .y = 128.0F};
            const aurora::Matrix2D m = aurora::Matrix2D::from_rotate_about(30.0F, cc).compose(
                aurora::Matrix2D::from_scale_about(1.2F, 1.2F, cc));
            report("composite_rot_scale", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.composite(off, m); }, warmup, slow_iters));

            // 8) rounded clip + full-screen fill (SDF coverage anti-aliased clip path).
            report("rounded_clip_fill", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           p.push_clip_rounded(full, 24.0F);
                           p.fill_rect(full, aurora::Color{80, 160, 90, 255});
                           p.pop_clip();
                       },
                       warmup, fast_iters));

            // 9) text (incl. CJK): 12 lines of the representative string (steady-state draw cost after atlas hits).
            const aurora::Font tf{.size_pt = 14.0F};
            report(
                "text_cjk_12lines", size_label, s, dw, dh,
                aurora::bench::time_ms(
                    [&]() -> void {
                        for (int line = 0; line < 12; ++line) {
                            p.draw_text(
                                aurora::Rect{
                                    .origin = aurora::Point{.x = 8.0F, .y = 16.0F + (22.0F * static_cast<float>(line))},
                                    .size = aurora::Size{.width = w - 16.0F, .height = 22.0F}},
                                AURORA_BENCH_TEXT, tf, aurora::Color{20, 20, 20, 255});
                        }
                    },
                    warmup, fast_iters));

            // 10) end-to-end: widget-tree whole-frame present_root (layout + text + background).
            {
                aurora::Window win = make_window(bw, bh, s);
                std::shared_ptr<aurora::Chip> probe;
                aurora::Node root = build_tree(&probe);
                // Force a whole-frame redraw to measure the true draw cost (otherwise the static tree is
                // skipped by the idle frame-drop optimization, reading 0).
                report("tree_full_redraw", size_label, s, dw, dh,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               win.force_full_redraw();
                               (void)win.present_root(root);
                           },
                           warmup, slow_iters));

                // 11) single-control dirtying: only one aurora::Chip changes background color -> dirty-region
                // clip draw path (compare against whole-tree redraw cost).
                (void)win.present_root(root);  // ensure on_dirty is wired and the first frame is fully painted
                int flip = 0;
                report("tree_dirty_one_chip", size_label, s, dw, dh,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               ++flip;
                               probe->set_background((flip & 1) != 0 ? aurora::Color{220, 60, 60, 255}
                                                                     : aurora::Color{60, 60, 220, 255});
                               (void)win.present_root(root);
                           },
                           warmup, slow_iters));

                // 12) single hit_test (event hit cost, amortized over 1000 runs).
                aurora::Widget &rw = root.widget();
                const aurora::Point probe_pt{.x = w * 0.5F, .y = h * 0.5F};
                report("hit_test_x1000", size_label, s, dw, dh,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               for (int i = 0; i < 1000; ++i) {
                                   (void)aurora::EventDispatcher::hit_test(rw, probe_pt);
                               }
                           },
                           1, 5));
            }
        }
    }

    // 13) character-level hit (cost of hit-testing every Move event, amortized over 100 runs): line
    // length x scale in two dimensions -- after fullscreen the paragraph does not wrap and the
    // single-line code-point count doubles; if the hit is O(n^2) (recomputing the prefix at each
    // boundary) the cost grows quadratically.
    {
        const std::string seg = "The pale illimitable moonlit hills still fill the silent mill. ";
        for (const int reps : {1, 4}) {
            std::string line;
            for (int i = 0; i < reps; ++i) {
                line += seg;
            }
            for (const float s : {1.0F, 1.5F}) {
                const aurora::Font f{.size_pt = 15.0F};
                constexpr aurora::render::TextLayoutOpts o{};
                const float w = aurora::render::FontEngine::display_width(line, f, o, s);
                report(("char_hit_x100_n" + std::to_string(line.size())).c_str(), "-", s, 0, 0,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               for (int i = 0; i < 100; ++i) {
                                   (void)aurora::render::FontEngine::display_hit_test_char(line, w * 0.7F, f, o, s);
                               }
                           },
                           1, 5));
            }
        }
    }

    // 14) scroll scene: reuse aurora::ScrollBenchHarness to run a deterministic scroll sequence,
    // producing p99 / jitter / full_redraw_frames and RenderCounters baselines. Time-based gates are
    // affected by environment jitter and excluded from CTest; local trend comparison is in
    // tools/check/check_perf_gates.ps1. Counter-based gates are locked into CTest by
    // tests/test_scroll_regression.cpp (build-prof).
    {
        aurora::ScrollBenchHarness::Config cfg;
        cfg.name = "bench_render-scroll";
        const auto r =
            aurora::ScrollBenchHarness::run(build_scroll_tree(), aurora::Size{.width = 1100.0F, .height = 760.0F}, cfg);
        AURORA_LOG_RAW("bench", "\n## scroll scenario (aurora::ScrollBenchHarness, 1100x760 dp, 300 frames)\n\n");
        AURORA_LOG_RAW("bench", r.to_markdown(), "\n");
        AURORA_LOG_RAW("bench",
                       "> time-based gates (G-1~G-14) are affected by environment jitter and excluded from "
                       "CTest; local trend comparison see "
                       "tools/check/check_perf_gates.ps1.\n");
    }

    AURORA_LOG_RAW("bench", "\n", aurora::bench::AURORA_BENCH_DISCLAIMER, "\n");
    return 0;
}
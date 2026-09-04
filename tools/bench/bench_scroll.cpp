// Scroll performance benchmark: use `ScrollBenchHarness` under Headless to run deterministic scroll
// sequences against a "real workload tree", producing p99 / jitter / full_redraw_frames and
// RenderCounters baselines for "before-optimization / after-optimization" comparison tables.
//
// Notes:
// - This program is a "benchmark/diagnostic" tool, not a unit test; it is not wired into CTest
//   (timings are affected by environment jitter). But the counter readings are deterministic under
//   Headless; the regression assertions are owned by tests/test_scroll_bench.cpp.
// - Counters only have values in a build with `AURORA_ENABLE_PROFILING=ON`; timings should be
//   collected in Release + PROFILING=OFF (see codespec/BUILD_OPTIONS.md).
// - Output goes to stdout via AURORA_LOG_RAW (project hard rule #8: program product output uses the
//   raw channel).
//
// Usage:
//   bench_scroll [--scene google_play|synthetic|lazy|grid|all] [--frames N] [--warmup N]
//                [--delta DP] [--scale X] [--fling] [--repeat N] [--format md|json|csv]
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/widget/chip.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/lazy_list.h"
#include "aurora/widget/scroll.h"
#include "google_play_ui.h"

using aurora::Chip;
using aurora::Column;
using aurora::ColumnProps;
using aurora::GridView;
using aurora::LazyList;
using aurora::Node;
using aurora::profiling_enabled;
using aurora::Reactive;
using aurora::Row;
using aurora::RowProps;
using aurora::Scroll;
using aurora::ScrollBenchHarness;
using aurora::ScrollProps;
using aurora::Size;

namespace {

/// @brief Synthetic control scene: a stable long list decoupled from the demo, for trend comparison
/// in demo-less environments / CI.
/// 200 rows x 6 Chips; total content height far exceeds the viewport, so the whole scroll runs on
/// real content.
[[nodiscard]] auto build_synthetic_tree() -> Node {
    std::vector<Node> rows;
    rows.reserve(200);
    for (int r = 0; r < 200; ++r) {
        std::vector<Node> cells;
        cells.reserve(6);
        for (int c = 0; c < 6; ++c) {
            auto chip = std::make_shared<Chip>();
            chip->set_label("item " + std::to_string(r) + "-" + std::to_string(c));
            cells.emplace_back(chip);
        }
        rows.emplace_back(std::make_shared<Row>(RowProps{.children = std::move(cells)}));
    }
    auto col = std::make_shared<Column>(ColumnProps{.children = std::move(rows)});
    return Node{std::make_shared<Scroll>(ScrollProps{.child = Node{std::move(col)}})};
}

/// @brief Real-world scenario: the content tree of `demo_google_play` (AppShell, without the
/// NavigatorHost transition).
/// Holds Reactive and repo references statically to keep the nodes alive for the duration of the
/// harness run.
[[nodiscard]] auto build_google_play_tree(Reactive<bool> &dark) -> Node {
    auto &repo = gp::repository();
    auto on_open = [](const std::string &) -> void {};  // benchmark does not navigate
    return Node{std::make_shared<gp::ui::AppShell>(&repo, on_open, &dark)};
}

/// @brief L5-C isolated scenario: LazyList self-driven scroll path (not via the outer Scroll buffer).
/// 2000 rows x single-Chip cards; virtualization only instantiates the visible window, so the whole
/// scroll runs on real content.
[[nodiscard]] auto build_lazy_tree() -> Node {
    constexpr int items = 2000;
    auto builder = [](int i) -> Node {
        const auto chip = std::make_shared<Chip>();
        chip->set_label("lazy item " + std::to_string(i));
        return Node{chip};
    };
    return Node{std::make_shared<LazyList>(items, builder, 48.0F)};
}

/// @brief L5-C scenario: GridView self-driven scroll path (root-level GridView, not via the outer
/// Scroll).
/// As a root-level scrollable control, GridView receives a window-limited constraint; virtualization
/// only instantiates the visible rows, so the whole scroll runs on real content.
/// Note: under a loose (infinite) constraint GridView collapses to a 320x480 fallback size, so it
/// must not be wrapped in a Scroll for measurement (that would strip it of scrollable content); the
/// root-level usage here matches demo_google_play's make_grid.
/// 2000 items x 4 columns x single-Chip cards.
[[nodiscard]] auto build_grid_tree() -> Node {
    constexpr int items = 2000;
    constexpr int cols = 4;
    auto builder = [](int i) -> Node {
        const auto chip = std::make_shared<Chip>();
        chip->set_label("grid " + std::to_string(i));
        return Node{chip};
    };
    return Node{std::make_shared<GridView>(items, cols, builder, 96.0F)};
}

/// @brief Emit a scene report (rendering form selected by format).
auto emit(const std::string &format, const ScrollBenchHarness::Result &r, bool &csv_header_written) -> void {
    if (format == "json") {
        AURORA_LOG_RAW("bench", r.to_json(), "\n");
    } else if (format == "csv") {
        if (!csv_header_written) {
            AURORA_LOG_RAW("bench", ScrollBenchHarness::Result::csv_header(), "\n");
            csv_header_written = true;
        }
        AURORA_LOG_RAW("bench", r.to_csv_row(), "\n");
    } else {
        AURORA_LOG_RAW("bench", r.to_markdown(), "\n");
    }
}

/// @brief Fetch the next argument value, returning a default when missing (never throws; tool
/// tolerates faults first).
[[nodiscard]] auto arg_value(const std::vector<std::string_view> &args, int i, std::string_view fallback)
    -> std::string {
    const auto next = static_cast<std::size_t>(i) + 1U;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 基准测量热路径：.at()
    // 的边界检查开销会影响计时
    return (next < args.size()) ? std::string{args[next]} : std::string{fallback};
}

/// @brief Safely parse an integer argument (never throws; only the whole string being consumed is
/// accepted).
[[nodiscard]] auto parse_int(const std::string &s, int fallback) -> int {
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    if (v < static_cast<long>(std::numeric_limits<int>::min()) ||
        v > static_cast<long>(std::numeric_limits<int>::max())) {
        return fallback;
    }
    return static_cast<int>(v);
}

/// @brief Safely parse a float argument (never throws; only the whole string being consumed is
/// accepted).
[[nodiscard]] auto parse_float(const std::string &s, float fallback) -> float {
    char *end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<float>(v);
}

/// @brief Repeat sampling and take the best (the lowest p99).
///
/// **Why take min instead of average**: measured on 300 frames at the same config, `avg`/`p50`
/// stay within +/-2%, but p99 fluctuates +/-6~11% and `worst` can reach +/-65%. This noise is
/// **additive** -- OS preemption, page faults, and thermal throttling only make frames slower,
/// never faster. So the fastest sample across multiple runs is closest to "the true rendering cost
/// on this machine"; averaging writes the noise into the baseline and the threshold drifts with
/// machine load. The counters are deterministic, so any single run is equivalent.
///
/// **Known bias (must be aware)**: this function repeats **within the same process**. Measured in
/// the same process, the 1st p99 is ~11.8ms while the 2nd/3rd stabilize at ~18.7ms -- a 27MB-class
/// offscreen buffer being repeatedly allocated/freed causes heap fragmentation and page-fault
/// overhead unrelated to the rendering logic under test. The min statistic happens to select the 1st
/// run (the cleanest sample), so the conclusion direction is **conservative** (it only over-reports
/// cost, never under-reports). To get strictly comparable timings, call it multiple times in
/// **separate processes** and take the minimum:
/// `for i in 1 2 3; do bench_scroll --scene X --format csv; done` (separate-process sampling
/// protocol).
///
/// @param build    rebuild a brand-new tree on every rerun (avoid leftover cache/animation state
///                from the previous sample affecting the next)
/// @param viewport viewport size (logical dp), matching the target window resolution
/// @param cfg      scroll config: frames, warmup, delta, fling, etc.
/// @param repeat   repeat count; < 1 is treated as 1
[[nodiscard]] auto run_best_of(const std::function<Node()> &build, Size viewport, const ScrollBenchHarness::Config &cfg,
                               int repeat) -> std::pair<ScrollBenchHarness::Result, std::string> {
    const int n = repeat > 0 ? repeat : 1;
    ScrollBenchHarness::Result best;
    std::vector<double> p99s;
    p99s.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto r = ScrollBenchHarness::run(build(), viewport, cfg);
        p99s.push_back(r.p99_ms());
        if (i == 0 || r.p99_ms() < best.p99_ms()) {
            best = std::move(r);
        }
    }
    std::string note;
    if (n > 1) {
        auto sorted = p99s;
        std::ranges::sort(sorted);
        const double lo = sorted.front();
        const double hi = sorted.back();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 基准测量热路径：.at()
        // 的边界检查开销会影响计时
        const double mid = sorted[sorted.size() / 2];
        note = "> best-of-" + std::to_string(n) + ": p99 across runs " + std::to_string(lo) + " / " +
               std::to_string(mid) + " / " + std::to_string(hi) +
               " ms (min / median / max); the table below takes the lowest run.\n"
               "> Note: same-process repetition has a systematic bias (later runs are slower due to heap "
               "fragmentation); for strict sampling, call it multiple times in separate processes.\n\n";
    }
    return {std::move(best), std::move(note)};
}

}  // namespace

auto main(int argc, char **argv) -> int {  // NOLINT(*-function-cognitive-complexity,bugprone-exception-escape)
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);  // NOLINT(*-pro-bounds-pointer-arithmetic)
    }

    std::string scene = "all";
    std::string format = "md";
    int repeat = 1;
    ScrollBenchHarness::Config cfg;

    for (int i = 1; i < argc; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) 基准测量热路径：.at()
        // 的边界检查开销会影响计时
        const std::string a{args[static_cast<std::size_t>(i)]};
        if (a == "--scene") {
            scene = arg_value(args, i, "all");
            ++i;
        } else if (a == "--format") {
            format = arg_value(args, i, "md");
            ++i;
        } else if (a == "--frames") {
            cfg.frames = parse_int(arg_value(args, i, "300"), 300);
            ++i;
        } else if (a == "--warmup") {
            cfg.warmup_frames = parse_int(arg_value(args, i, "30"), 30);
            ++i;
        } else if (a == "--delta") {
            cfg.delta_per_frame = parse_float(arg_value(args, i, "12.0"), 12.0F);
            ++i;
        } else if (a == "--scale") {
            cfg.scale = parse_float(arg_value(args, i, "1.0"), 1.0F);
            ++i;
        } else if (a == "--repeat") {
            repeat = parse_int(arg_value(args, i, "1"), 1);
            ++i;
        } else if (a == "--fling") {
            cfg.fling = true;
        } else if (a == "--help" || a == "-h") {
            AURORA_LOG_RAW(
                "bench",
                "usage: bench_scroll [--scene google_play|synthetic|lazy|grid|all] [--frames N] [--warmup N]\n"
                "                    [--delta DP] [--scale X] [--fling] [--repeat N]\n"
                "                    [--format md|json|csv]\n"
                "\n"
                "  --scene    google_play / synthetic / lazy / grid / all (default all)\n"
                "              lazy / grid are L5-C isolated scenes: use LazyList / GridView\n"
                "              directly as roots to measure their self-driven scroll paths\n"
                "              (not via the outer Scroll offscreen buffer)\n"
                "  --delta DP   scroll distance per frame, in logical dp (default 12 ~ 720 dp/s @60fps)\n"
                "  --repeat N   repeat N times in the same process, take the lowest p99 (conservative,\n"
                "              only over-reports)\n"
                "              for strict sampling, call it multiple times in separate processes:\n"
                "                 for i in 1 2 3; do bench_scroll --scene X --format csv; done\n"
                "              counter readings are deterministic, one shot suffices; only timings need\n"
                "              repetition.\n");
            return 0;
        }
    }

    constexpr Size viewport{.width = 1100.0F,
                            .height = 760.0F};  // agreed convention: matches the demo_google_play window
    bool csv_header_written = false;

    if (format == "md") {
        AURORA_LOG_RAW("bench", "## scroll bench\n\n- viewport: 1100x760 dp @scale ",
                       std::to_string(static_cast<double>(cfg.scale)), "\n- frames: ", std::to_string(cfg.frames),
                       " (warmup ", std::to_string(cfg.warmup_frames), ")\n- mode: ", cfg.fling ? "fling" : "uniform",
                       ", delta/frame: ", std::to_string(static_cast<double>(cfg.delta_per_frame)), " dp",
                       "\n- repeat: ", std::to_string(repeat), repeat > 1 ? " (lowest p99 run)" : "", "\n- profiling: ",
                       profiling_enabled() ? "ON (counters valid)" : "OFF (counters always 0, time only)", "\n\n");
    }

    bool any_untrustworthy = false;

    if (scene == "synthetic" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "synthetic-fling" : "synthetic-uniform";
        auto [r, note] = run_best_of([]() -> Node { return build_synthetic_tree(); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (scene == "lazy" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "lazy-fling" : "lazy-uniform";
        auto [r, note] = run_best_of([]() -> Node { return build_lazy_tree(); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (scene == "grid" || scene == "all") {
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "grid-fling" : "grid-uniform";
        auto [r, note] = run_best_of([]() -> Node { return build_grid_tree(); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (scene == "google_play" || scene == "all") {
        // dark must outlive the whole sampling: AppShell only holds a pointer.
        Reactive<bool> dark{false};
        ScrollBenchHarness::Config c = cfg;
        c.name = cfg.fling ? "google_play-fling" : "google_play-uniform";
        auto [r, note] = run_best_of([&dark]() -> Node { return build_google_play_tree(dark); }, viewport, c, repeat);
        any_untrustworthy = any_untrustworthy || !r.trustworthy();
        if (format == "md" && !note.empty()) {
            AURORA_LOG_RAW("bench", note);
        }
        emit(format, r, csv_header_written);
    }

    if (any_untrustworthy) {
        // Non-zero exit code: the baseline collection script uses this to refuse writing untrusted
        // readings into the comparison table.
        AURORA_LOG_ERROR("bench",
                         "At least one scenario reading is unreliable (scroll container not found / idle "
                         "frame drops); non-zero exit code set");
        return 2;
    }
    return 0;
}
// 滚动性能基准：用 `ScrollBenchHarness` 在 Headless 下
// 对「真实业务树」跑确定性滚动序列，产出 p99 / jitter / full_redraw_frames 与
// RenderCounters 基线，供「优化前 / 优化后」对照表取数。
//
// 说明：
// - 本程序是「基准/诊断」工具，非单元测试，不接入 CTest（时间读数受环境抖动影响）。
//   但其中的**计数器读数在 Headless 下是确定的**，回归断言由 tests/test_scroll_bench.cpp 承担。
// - 计数器需 `AURORA_ENABLE_PROFILING=ON` 的构建才有值；时间读数应在 Release + PROFILING=OFF
//   下采集（见 codespec/BUILD_OPTIONS.md）。
// - 输出经 AURORA_LOG_RAW 走 stdout（项目硬规则第 8 条：程序产品输出用 raw 通道）。
//
// 用法：
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

/// @brief 合成对照场景：与 demo 解耦的稳定长列表，供无 demo 环境/CI 做趋势对照。
/// 200 行 × 6 个 Chip，内容总高远超视口，滚动全程都在真实内容上。
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
        rows.emplace_back(std::make_shared<Row>(RowProps{ .children = std::move(cells) }));
    }
    auto col = std::make_shared<Column>(ColumnProps{ .children = std::move(rows) });
    return Node{ std::make_shared<Scroll>(ScrollProps{ .child = Node{ std::move(col) } }) };
}

/// @brief 真实业务场景：`demo_google_play` 的内容树（AppShell，不含 NavigatorHost 转场）。
/// 静态持有 Reactive 与 repo 引用，保证节点在 harness 运行期间存活。
[[nodiscard]] auto build_google_play_tree(Reactive<bool> &dark) -> Node {
    auto &repo = gp::repository();
    auto on_open = [](const std::string &) -> void {}; // 基准不做导航跳转
    return Node{ std::make_shared<gp::ui::AppShell>(&repo, on_open, &dark) };
}

/// @brief L5-C 隔离场景：LazyList 自驱滚动路径（不经外层 Scroll 缓冲）。
/// 2000 行 × 单 Chip 卡片，虚拟化只实例化可见窗口，滚动全程都在真实内容上。
[[nodiscard]] auto build_lazy_tree() -> Node {
    constexpr int items = 2000;
    auto builder = [](int i) -> Node {
        const auto chip = std::make_shared<Chip>();
        chip->set_label("lazy item " + std::to_string(i));
        return Node{ chip };
    };
    return Node{ std::make_shared<LazyList>(items, builder, 48.0f) };
}

/// @brief L5-C 场景：GridView 自驱滚动路径（根级 GridView，不经外层 Scroll）。
/// GridView 作为根级可滚动控件接收窗口有限约束，虚拟化只实例化可见行，滚动全程都在真实内容上。
/// 注：GridView 在宽松（infinite）约束下会塌缩到 320×480 回退尺寸，故不可包在 Scroll 内测
/// （那会让它失去可滚内容）；这里的根级用法与 demo_google_play 的 make_grid 一致。
/// 2000 项 × 4 列 × 单 Chip 卡片。
[[nodiscard]] auto build_grid_tree() -> Node {
    constexpr int items = 2000;
    constexpr int cols = 4;
    auto builder = [](int i) -> Node {
        const auto chip = std::make_shared<Chip>();
        chip->set_label("grid " + std::to_string(i));
        return Node{ chip };
    };
    return Node{ std::make_shared<GridView>(items, cols, builder, 96.0f) };
}

/// @brief 打一段场景报告（按 format 选择渲染形态）。
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

/// @brief 取下一个参数值，缺失时返回默认值（不抛异常，工具容错优先）。
[[nodiscard]] auto arg_value(const std::vector<std::string_view> &args, int i, std::string_view fallback)
    -> std::string {
    const auto next = static_cast<std::size_t>(i) + 1U;
    return (next < args.size()) ? std::string{ args[next] } : std::string{ fallback };
}

/// @brief 安全解析整数参数（不抛异常，整串消耗才生效）。
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

/// @brief 安全解析浮点参数（不抛异常，整串消耗才生效）。
[[nodiscard]] auto parse_float(const std::string &s, float fallback) -> float {
    char *end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<float>(v);
}

/// @brief 重复采样取最优（p99 最小的一次）。
///
/// **为什么取 min 而不是平均**：同配置 300 帧实测，`avg`/`p50` 稳定在 ±2%，但 p99 波动
/// ±6~11%、`worst` 可达 ±65%。这些噪声是**加性**的——OS 抢占、缺页、热节流只会让帧变慢，
/// 不会让它变快。所以多次采样中最快的一次最接近「这台机器上真实的渲染成本」；取平均等于
/// 把噪声写进基线，门槛会随机器负载漂移。计数器是确定性的，取哪一次都一样。
///
/// **已知偏置（务必知情）**：本函数在**同一进程内**重复。实测同进程第 1 次 p99 ≈ 11.8ms，
/// 第 2/3 次稳定在 ≈ 18.7ms——27MB 级离屏缓冲反复申请/释放造成的堆碎片与缺页开销，与被测
/// 渲染逻辑无关。min 统计量恰好会选中第 1 次（最干净的样本），所以结论方向是**保守**的
/// （只会高报耗时，不会低报）。要拿严格可比的时间读数，请用**独立进程**多次调用取最小值：
/// `for i in 1 2 3; do bench_scroll --scene X --format csv; done`（独立进程取数协议）。
///
/// @param build    每次重跑都重建一棵全新的树（避免上次采样残留的缓存/动画状态影响下一次）
/// @param viewport 视口尺寸（逻辑 dp），与目标窗口分辨率一致
/// @param cfg      滚动配置：帧数、warmup、delta、fling 等
/// @param repeat   重复次数，< 1 视为 1
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
        const double mid = sorted[sorted.size() / 2];
        note = "> best-of-" + std::to_string(n) + "：p99 跨次 " + std::to_string(lo) + " / " + std::to_string(mid) +
               " / " + std::to_string(hi) +
               " ms（min / median / max），下表取最小的一次。\n"
               "> 注意同进程重复存在系统性偏置（后续次因堆碎片偏慢），严格取数请用独立进程多次调用。\n\n";
    }
    return { std::move(best), std::move(note) };
}

} // namespace

auto main(int argc, char **argv) -> int { // NOLINT(*-function-cognitive-complexity)
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]); // NOLINT(*-pro-bounds-pointer-arithmetic)
    }

    std::string scene = "all";
    std::string format = "md";
    int repeat = 1;
    ScrollBenchHarness::Config cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string a{ args[static_cast<std::size_t>(i)] };
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
            cfg.delta_per_frame = parse_float(arg_value(args, i, "12.0"), 12.0f);
            ++i;
        } else if (a == "--scale") {
            cfg.scale = parse_float(arg_value(args, i, "1.0"), 1.0f);
            ++i;
        } else if (a == "--repeat") {
            repeat = parse_int(arg_value(args, i, "1"), 1);
            ++i;
        } else if (a == "--fling") {
            cfg.fling = true;
        } else if (a == "--help" || a == "-h") {
            AURORA_LOG_RAW(
                "bench", "usage: bench_scroll [--scene google_play|synthetic|lazy|grid|all] [--frames N] [--warmup N]\n"
                         "                    [--delta DP] [--scale X] [--fling] [--repeat N]\n"
                         "                    [--format md|json|csv]\n"
                         "\n"
                         "  --scene    google_play / synthetic / lazy / grid / all（默认 all）\n"
                         "              lazy / grid 为 L5-C 隔离场景：直接以 LazyList / GridView 为根，\n"
                         "              测量二者自驱滚动路径（不经外层 Scroll 离屏缓冲）\n"
                         "  --delta DP   每帧滚动距离，单位逻辑 dp（默认 12 ≈ 720 dp/s @60fps）\n"
                         "  --repeat N   同进程重复 N 次取 p99 最小的一次（结论保守，只会高报）\n"
                         "               严格取数请用独立进程多次调用取最小值：\n"
                         "                 for i in 1 2 3; do bench_scroll --scene X --format csv; done\n"
                         "               计数器读数是确定性的，单次即可；仅时间读数需要重复。\n");
            return 0;
        }
    }

    constexpr Size viewport{ .width = 1100.0f, .height = 760.0f }; // 约定口径：与 demo_google_play 窗口一致
    bool csv_header_written = false;

    if (format == "md") {
        AURORA_LOG_RAW("bench", "## scroll bench\n\n- viewport: 1100x760 dp @scale ",
                       std::to_string(static_cast<double>(cfg.scale)), "\n- frames: ", std::to_string(cfg.frames),
                       " (warmup ", std::to_string(cfg.warmup_frames), ")\n- mode: ", cfg.fling ? "fling" : "uniform",
                       ", delta/frame: ", std::to_string(static_cast<double>(cfg.delta_per_frame)), " dp",
                       "\n- repeat: ", std::to_string(repeat), (repeat > 1 ? "（取 p99 最小的一次）" : ""),
                       "\n- profiling: ", profiling_enabled() ? "ON（计数器有效）" : "OFF（计数器恒为 0，仅看时间）",
                       "\n\n");
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
        // dark 必须活过整段采样：AppShell 只持有指针。
        Reactive<bool> dark{ false };
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
        // 非零退出码：基线采集脚本据此拒绝把不可信读数写进对照表。
        AURORA_LOG_ERROR("bench", "至少有一个场景的读数不可信（未找到滚动容器 / 存在 idle 跳帧），已置非零退出码");
        return 2;
    }
    return 0;
}

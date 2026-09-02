// 渲染基准：用 HeadlessSurface + aurora::Painter 在多种分辨率/缩放下，
// 对绘制原语与端到端 widget 树整帧 paint 做计时基线。
//
// 说明：
// - 本程序是「基准/诊断」工具，非单元测试，不接入 CTest（计时受环境抖动影响，无稳定断言）。
// - 输出结构化表格（stdout，markdown），便于横向比较不同尺寸/缩放下的绘制成本。
// - scale 经 `aurora::Painter::set_scale` 真实生效（逻辑 dp × scale → 物理像素缓冲），
//   覆盖高 DPI 文字光栅等 scale 敏感路径，而非单纯放大分辨率。
// - 用法：./bench_render
//
// 维度矩阵：逻辑尺寸 1280×720 / 1920×1080 / 2560×1440；缩放 1.0 / 1.5 / 2.0。
// 原语场景：全屏不透明/半透明 fill_rect、线性/径向渐变、阴影、blur（多半径）、
// composite（旋转+缩放）、圆角裁剪填充、文本（含 CJK）；端到端：widget 树整帧、
// 单控件变脏（脏区裁剪路径）、单次 hit_test。
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

// 创建已定尺寸的 Headless 窗口（先设 scale 再 begin_frame，物理缓冲 = 逻辑 × scale）。
auto make_window(int w, int h, float scale) -> au::Window {
    auto surface = std::make_unique<aurora::HeadlessSurface>();
    surface->painter().set_scale(scale);
    (void)surface->begin_frame(w, h);
    return aurora::Window{ std::move(surface) };
}

// 打一行表：场景 | 逻辑尺寸 | scale | 物理尺寸 | ms/帧。
auto report(const char *scene, const std::string &size_label, float s, int dev_w, int dev_h, double ms) -> void {
    AURORA_LOG_RAW("bench", "| ", scene, " | ", size_label, " | ", aurora::bench::ffmt(1, s), " | ", dev_w, "x", dev_h,
                   " | ", aurora::bench::ffmt(3, ms), " |\n");
}

// 端到端 widget 树：20 行 × 10 列 aurora::Chip（含标签文本），覆盖布局 + 文本 + 背景绘制。
// out_probe 返回其中一个 chip，供「单控件变脏」场景改色标脏。
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
        rows.emplace_back(std::make_shared<aurora::Row>(aurora::RowProps{ .children = std::move(chips) }));
    }
    return aurora::Node{ std::make_shared<aurora::Column>(aurora::ColumnProps{ .children = std::move(rows) }) };
}

// 代表性文本（拉丁 + 数字 + CJK 中日韩），覆盖回退链与图集缓存路径。
constexpr auto AURORA_BENCH_TEXT = "The quick brown fox jumps 0123456789 灰狐跳过懒狗 こんにちは世界 안녕하세요";

// 滚动场景内容树：`aurora::Scroll` 包一列 200 个 aurora::Chip（含标签文本），内容远高于视口，
// 供 aurora::ScrollBenchHarness 跑确定性滚动序列（时间类门槛的本机口径）。
auto build_scroll_tree() -> aurora::Node {
    std::vector<aurora::Node> items;
    items.reserve(200);
    for (int i = 0; i < 200; ++i) {
        auto chip = std::make_shared<aurora::Chip>();
        chip->set_label("row " + std::to_string(i));
        items.emplace_back(std::move(chip));
    }
    auto col = std::make_shared<aurora::Column>(aurora::ColumnProps{ .children = std::move(items) });
    return aurora::Node{ std::make_shared<aurora::Scroll>(
        aurora::ScrollProps{ .child = aurora::Node{ std::move(col) } }) };
}

} // namespace

auto main() -> int {
    const std::vector<std::pair<int, int>> bases = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
    const std::vector scales = { 1.0f, 1.5f, 2.0f };
    constexpr int warmup = 3;
    constexpr int fast_iters = 20; // fill/渐变/文本/裁剪等快场景
    constexpr int slow_iters = 8;  // 阴影/blur/composite/端到端等慢场景

    AURORA_LOG_RAW("bench", "| scene | size (logical) | scale | device | ms/frame |\n");
    AURORA_LOG_RAW("bench", "|---|---|---|---|---|\n");

    for (const auto &[bw, bh] : bases) {
        for (const float s : scales) {
            const std::string size_label = std::to_string(bw) + "x" + std::to_string(bh);
            const auto w = static_cast<float>(bw);
            const auto h = static_cast<float>(bh);
            const aurora::Rect full{ .origin = aurora::Point{ .x = 0, .y = 0 },
                                     .size = aurora::Size{ .width = w, .height = h } };

            // 独立 aurora::Painter：逻辑 bw×bh，物理 ×scale（原语基准不经 Window/widget 层）。
            aurora::Painter p;
            p.set_scale(s);
            p.begin(bw, bh);
            const int dw = p.width();
            const int dh = p.height();

            // 1) 全屏不透明 fill_rect（行级快路径）。
            report("fill_opaque_full", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.fill_rect(full, aurora::Color{ 30, 120, 200, 255 }); },
                                          warmup, fast_iters));

            // 2) 全屏半透明 fill_rect（逐像素 source-over）。
            report("fill_alpha_full", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.fill_rect(full, aurora::Color{ 255, 200, 0, 128 }); },
                                          warmup, fast_iters));

            // 3) 全屏线性渐变。
            const std::vector gcolors = { aurora::Color{ 250, 250, 255, 255 }, aurora::Color{ 30, 30, 60, 255 } };
            const std::vector gstops = { 0.0f, 1.0f };
            report("linear_gradient_full", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           p.draw_linear_gradient(full, aurora::Point{ .x = 0, .y = 0 },
                                                  aurora::Point{ .x = w, .y = h }, gcolors, gstops);
                       },
                       warmup, fast_iters));

            // 4) 全屏径向渐变。
            report("radial_gradient_full", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           p.draw_radial_gradient(full, aurora::Point{ .x = w * 0.5f, .y = h * 0.5f }, w * 0.5f,
                                                  gcolors, gstops);
                       },
                       warmup, fast_iters));

            // 5) 阴影（400×300 卡片形状，blur 16）。
            constexpr aurora::Rect card{ .origin = aurora::Point{ .x = 40, .y = 40 },
                                         .size = aurora::Size{ .width = 400, .height = 300 } };
            report("shadow_card", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void { p.draw_shadow(card, 4.0f, 8.0f, 16.0f, aurora::Color{ 0, 0, 0, 96 }); }, warmup,
                       slow_iters));

            // 6) blur_region 多半径（固定 320×240 区域，毛玻璃典型量级）。
            constexpr aurora::Rect blur_area{ .origin = aurora::Point{ .x = 60, .y = 60 },
                                              .size = aurora::Size{ .width = 320, .height = 240 } };
            report("blur_r4", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.blur_region(blur_area, 4.0f); }, warmup, slow_iters));
            report("blur_r16", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.blur_region(blur_area, 16.0f); }, warmup, slow_iters));

            // 7) composite：256×256 离屏子树旋转 30° + 缩放 1.2 合成（修饰变换路径）。
            aurora::Painter off;
            off.set_scale(s);
            off.begin(256, 256);
            off.fill_rect(aurora::Rect{ .origin = aurora::Point{ .x = 0, .y = 0 },
                                        .size = aurora::Size{ .width = 256, .height = 256 } },
                          aurora::Color{ 200, 80, 40, 255 });
            constexpr aurora::Point cc{ .x = 128.0f, .y = 128.0f };
            const aurora::Matrix2D m = aurora::Matrix2D::from_rotate_about(30.0f, cc).compose(
                aurora::Matrix2D::from_scale_about(1.2f, 1.2f, cc));
            report("composite_rot_scale", size_label, s, dw, dh,
                   aurora::bench::time_ms([&]() -> void { p.composite(off, m); }, warmup, slow_iters));

            // 8) 圆角裁剪 + 全屏填充（SDF coverage 抗锯齿裁剪路径）。
            report("rounded_clip_fill", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           p.push_clip_rounded(full, 24.0f);
                           p.fill_rect(full, aurora::Color{ 80, 160, 90, 255 });
                           p.pop_clip();
                       },
                       warmup, fast_iters));

            // 9) 文本（含 CJK）：12 行代表串（图集命中后的稳态绘制成本）。
            const aurora::Font tf{ .size_pt = 14.0f };
            report("text_cjk_12lines", size_label, s, dw, dh,
                   aurora::bench::time_ms(
                       [&]() -> void {
                           for (int line = 0; line < 12; ++line) {
                               p.draw_text(
                                   aurora::Rect{
                                       .origin =
                                           aurora::Point{ .x = 8.0f, .y = 16.0f + (22.0f * static_cast<float>(line)) },
                                       .size = aurora::Size{ .width = w - 16.0f, .height = 22.0f } },
                                   AURORA_BENCH_TEXT, tf, aurora::Color{ 20, 20, 20, 255 });
                           }
                       },
                       warmup, fast_iters));

            // 10) 端到端：widget 树整帧 present_root（布局 + 文本 + 背景）。
            {
                aurora::Window win = make_window(bw, bh, s);
                std::shared_ptr<aurora::Chip> probe;
                aurora::Node root = build_tree(&probe);
                // 强制整帧重绘以测量真实绘制成本（否则静态树被 idle 跳帧优化跳过，测得 0）。
                report("tree_full_redraw", size_label, s, dw, dh,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               win.force_full_redraw();
                               (void)win.present_root(root);
                           },
                           warmup, slow_iters));

                // 11) 单控件变脏：只有一个 aurora::Chip 改背景色 → 脏区裁剪绘制路径（对比整树重绘收益）。
                (void)win.present_root(root); // 确保接线 on_dirty 且首帧已全绘
                int flip = 0;
                report("tree_dirty_one_chip", size_label, s, dw, dh,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               ++flip;
                               // NOLINTNEXTLINE(*-signed-bitwise)
                               probe->set_background((flip & 1) != 0 ? aurora::Color{ 220, 60, 60, 255 }
                                                                     : aurora::Color{ 60, 60, 220, 255 });
                               (void)win.present_root(root);
                           },
                           warmup, slow_iters));

                // 12) 单次 hit_test（事件命中成本，均摊 1000 次）。
                aurora::Widget &rw = root.widget();
                const aurora::Point probe_pt{ .x = w * 0.5f, .y = h * 0.5f };
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

    // 13) 字符级命中（拖选每个 Move 事件的成本，均摊 100 次）：行长 × scale 两维——
    // 全屏后段落不折行、单行码点数翻倍，命中若为 O(n²)（逐边界重算前缀）则成本按平方涨。
    {
        const std::string seg = "The pale illimitable moonlit hills still fill the silent mill. ";
        for (const int reps : { 1, 4 }) {
            std::string line;
            for (int i = 0; i < reps; ++i) {
                line += seg;
            }
            for (const float s : { 1.0f, 1.5f }) {
                const aurora::Font f{ .size_pt = 15.0f };
                constexpr aurora::render::TextLayoutOpts o{};
                const float w = aurora::render::FontEngine::display_width(line, f, o, s);
                report(("char_hit_x100_n" + std::to_string(line.size())).c_str(), "-", s, 0, 0,
                       aurora::bench::time_ms(
                           [&]() -> void {
                               for (int i = 0; i < 100; ++i) {
                                   (void)aurora::render::FontEngine::display_hit_test_char(line, w * 0.7f, f, o, s);
                               }
                           },
                           1, 5));
            }
        }
    }

    // 14) 滚动场景：复用 aurora::ScrollBenchHarness 跑确定性滚动序列，产出 p99 / jitter /
    // full_redraw_frames 与 RenderCounters 基线。时间类门槛受环境抖动
    // 影响，不进 CTest；本机趋势对比见 check_perf_gates.ps1。计数类门槛由
    // tests/test_scroll_regression.cpp 锁进 CTest（build-prof）。
    {
        aurora::ScrollBenchHarness::Config cfg;
        cfg.name = "bench_render-scroll";
        const auto r = aurora::ScrollBenchHarness::run(build_scroll_tree(),
                                                       aurora::Size{ .width = 1100.0f, .height = 760.0f }, cfg);
        AURORA_LOG_RAW("bench", "\n## scroll scenario (aurora::ScrollBenchHarness, 1100x760 dp, 300 frames)\n\n");
        AURORA_LOG_RAW("bench", r.to_markdown(), "\n");
        AURORA_LOG_RAW("bench", "> 时间类门槛（G-1~G-14）受环境抖动影响，不进 CTest；本机趋势对比见 "
                                "tools/check/check_perf_gates.ps1。\n");
    }

    AURORA_LOG_RAW("bench", "\n", aurora::bench::AURORA_BENCH_DISCLAIMER, "\n");
    return 0;
}

/// 测试类型: integration
/// 目标单元: include/aurora/app/perf_overlay.h
/// 测试说明: 回归 PerfOverlay 被 DisplayList 缓存冻结（HUD 面板静止在「采样中」/全零）：
///           机制断言 PerfOverlay 禁 DL 缓存；行为断言在 headless 帧循环下两个时间点抓取
///           整帧像素，叠加层实时刷新则两帧像素必显著不同（被 DL 冻结则 diff=0）。
/// 覆盖说明: 两阶段以不同均值的合成 dt 驱动 FrameStats 滑动窗口（统计差异确定性成立，
///           不依赖墙钟节奏）；每阶段保留 >500ms 墙钟跨度仅为触发 HUD 2Hz 刷新节流。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/core/log.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/widget.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_perf_overlay_refresh {

namespace {

// 抓取整帧像素缓冲的副本（RGBA）。
void capture_surface(const Surface& s, std::vector<std::uint8_t>& out) {
    const int w = static_cast<int>(s.size().width);
    const int h = static_cast<int>(s.size().height);
    const std::uint8_t* buf = s.data();
    // 整帧 RGBA 缓冲拷贝：Surface::data() 返回裸指针，buf+len 首尾区间是必要写法（长度已按 w*h*4 核算）。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    out.assign(buf, buf + (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4));
}

// 两帧像素差异字节数（RGBA）。
auto pixel_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) -> long {
    long d = 0;
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (a.at(i) != b.at(i)) {
            ++d;
        }
    }
    return d;
}

// 整帧亮像素数（叠加层文本为浅色，应存在）。
auto bright_count(const std::vector<std::uint8_t>& buf) -> long {
    long c = 0;
    for (std::size_t i = 0; i + 3 < buf.size(); i += 4) {
        if (buf.at(i) + buf.at(i + 1) + buf.at(i + 2) > 300) {
            ++c;
        }
    }
    return c;
}

// 驱动 [from, to) 帧：force_full_redraw + present_root + 手动喂入正弦扰动的 dt。
// 关键：直接调 present_root（非 app.run）时 FrameStats 不会被自动 record，必须手动喂入
// 变化的 dt —— 否则 window_size 恒 0、叠加层永驻「采样中」、两帧像素完全一致（diff=0）。
// mean_dt 可在两阶段间取不同均值，使滑动窗口统计确定性地移位（不依赖墙钟节奏）。
void drive_frames(Window& win, Node& root, int from, int to, double mean_dt) {
    for (int i = from; i < to; ++i) {
        win.force_full_redraw();
        (void)win.present_root(root);
        const double dt = mean_dt + (0.010 * std::sin(static_cast<double>(i) * 0.31));
        FrameStats::instance().record(dt);
        // 8ms 间隔保证阶段墙钟跨度 > 500ms：足以触发 HUD 层的 2Hz 离屏重绘节流。
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

}  // namespace

AURORA_TEST_CASE(overlay_disables_display_list_cache) {
    // 机制断言：PerfOverlay 必须禁 DL 缓存，否则首帧「采样中」画面会被录进 DL 永久冻结。
    const PerfOverlay ov;
    AURORA_TEST_CHECK_MSG(!ov.can_cache_display_list(),
                          "PerfOverlay must disable DisplayList cache: otherwise first frame 'sampling' freezes "
                          "and HUD never refreshes");
}

AURORA_TEST_CASE(overlay_refreshes_live_not_frozen) {
    FrameStats::instance().reset();

    Scene scene{Node{std::make_shared<Column>()}};
    WindowOptions opts;
    opts.size = Size{.width = 1100.0F, .height = 760.0F};
    auto win_res = create_window(HeadlessOptions{opts});
    AURORA_TEST_REQUIRE(static_cast<bool>(win_res));
    Application app{std::move(scene), std::move(win_res.value()), opts};

    // 叠加层（保留引用以便断言机制修复）。
    auto ov = std::make_shared<PerfOverlay>();
    app.set_overlay(ov);

    Window* win = app.window();
    AURORA_TEST_REQUIRE_NOT_NULL(win);
    Node& root = app.scene().root_node();

    // 预热 ~0.72s：驱动帧循环使 FrameStats 累积真实样本（window_size ≫ 2），
    // 叠加层脱离「采样中」。force_full_redraw 模拟「活跃应用」：避免静态场景触发
    // present_root 的 idle 跳过（idle 跳过会在抵达叠加层合成段前提前 return）。
    drive_frames(*win, root, 0, 90, 0.016);

    std::vector<std::uint8_t> snap_a;
    capture_surface(win->surface(), snap_a);
    AURORA_TEST_PRINTF("[diag] after warmup window_size=%zu fps=%.1f bright_pixels_A=%ld\n",
                       FrameStats::instance().window_size(), FrameStats::instance().fps(), bright_count(snap_a));

    // 再驱动 ~0.72s：叠加层应持续刷新（HUD 2Hz 重绘，文本随实时统计变化）。
    // 本阶段均值 dt 取 0.028（预热阶段 0.016）：滑动窗口统计确定性地移位
    // （FPS 约 62 → 约 42），快照文本必不同 —— 这是「未冻结」的铁证，且不依赖墙钟节奏。
    drive_frames(*win, root, 90, 180, 0.028);

    std::vector<std::uint8_t> snap_b;
    capture_surface(win->surface(), snap_b);
    AURORA_TEST_PRINTF("[diag] second capture window_size=%zu fps=%.1f bright_pixels_B=%ld\n",
                       FrameStats::instance().window_size(), FrameStats::instance().fps(), bright_count(snap_b));

    const long diff = pixel_diff(snap_a, snap_b);
    AURORA_TEST_PRINTF(
        "[overlay] two-frame full-frame pixel diff=%ld bytes (>0 proves overlay refreshes live, not DL-frozen)\n",
        diff);

    // 行为断言：叠加层未被冻结——两次抓取的像素应显著不同。
    AURORA_TEST_CHECK_MSG(diff > 500,
                          "PerfOverlay should refresh live: two-frame pixel diff should be significant "
                          "(>500B). If 0, frozen at first frame by DL");
}

}  // namespace aurora::test_cases::itest_perf_overlay_refresh

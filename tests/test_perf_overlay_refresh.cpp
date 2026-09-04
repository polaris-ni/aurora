// 回归：修复 PerfOverlay 被 DisplayList 缓存冻结（HUD 面板静止在「采样中」/全零）。
//
// 根因：PerfOverlay 实时文本每帧由 on_paint 从 FrameStats::instance() 重读，属于「内容每帧变动」
// 控件；但 can_cache_display_list() 默认 true，导致首帧（window_size<2，显示「采样中」）被录进
// DL，之后 Widget::paint 在 bounds 不变时直接 replay 首帧缓存、再不调用 on_paint，HUD 冻结。
// 修复：PerfOverlay 覆写 can_cache_display_list() 返回 false（见 perf_overlay.h）。
//
// 验证（headless）：构造空场景 + PerfOverlay 叠加层，驱动 ~3s 帧循环使 FrameStats 累积真实样本，
// 在两个时间点各抓取整帧像素并比对——若叠加层实时刷新，两次抓取的像素必不同（差异来自变化的
// FPS/P99 文本）；若被 DL 冻结，两次抓取像素完全一致（diff=0）。整帧比对与设备 scale 无关，
// 且空场景除叠加层外无变化源，故差异即叠加层实时刷新的铁证。
// ── API 覆盖映射 ─────────────────────────────
// app/perf_overlay.h（PerfOverlay 实时刷新；FrameStats 定义于此头）。

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/app/perf_overlay.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/widget.h"
#include "test_harness.h"

namespace au = aurora;

// 抓取整帧像素缓冲的副本
static auto capture_surface(const au::Surface &s, std::vector<std::uint8_t> &out) -> void {
    const int w = static_cast<int>(s.size().width);
    const int h = static_cast<int>(s.size().height);
    const std::uint8_t *buf = s.data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    out.assign(buf, buf + (static_cast<size_t>(w) * h * 4));
}

// 两帧像素差异字节数（RGBA）
static auto pixel_diff(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) -> long {
    long d = 0;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        if (a.at(i) != b.at(i)) {
            ++d;
        }
    }
    return d;
}

AURORA_TEST() {
    au::FrameStats::instance().reset();

    au::Scene scene{au::Node{std::make_shared<au::Column>()}};
    au::WindowOptions opts;
    opts.size = au::Size{.width = 1100.0F, .height = 760.0F};
    auto win_res = au::create_window(au::HeadlessOptions{opts});
    AURORA_TEST_CHECK(static_cast<bool>(win_res));
    au::Application app{std::move(scene), std::move(win_res.value()), opts};

    // 叠加层（保留引用以便断言机制修复）
    auto ov = std::make_shared<au::PerfOverlay>();
    app.set_overlay(ov);

    auto *win = app.window();
    auto root = app.scene().root_node();

    // 机制断言：PerfOverlay 必须禁 DL 缓存，否则首帧会被冻结
    AURORA_TEST_CHECK_MSG(
        !ov->can_cache_display_list(),
        "PerfOverlay must disable DisplayList cache: otherwise first frame 'sampling' freezes and HUD never refreshes");

    // 预热 ~1.5s：驱动帧循环使 FrameStats 累积真实样本（window_size 显著），叠加层脱离「采样中」。
    // force_full_redraw 模拟「活跃应用」：避免静态场景触发 present_root 的 idle 跳过
    // （idle 跳过会在抵达叠加层合成段前提前 return，使 HUD 不被重绘）。
    //
    // 关键：测试直接调 present_root（非 app.run），FrameStats 不会被自动 record，故必须手动喂入
    // 变化的 dt —— 否则 window_size 恒 0、叠加层永驻「采样中」、两帧像素完全一致（diff=0）。
    // dt 用正弦扰动（非定常），使滑动窗口内的 FPS/P99/jitter 随帧变化，叠加层文本随之刷新，
    // 从而 snap_a 与 snap_b 的像素产生差异——这是「未冻结」的铁证。
    for (int i = 0; i < 90; ++i) {
        win->force_full_redraw();
        (void)win->present_root(root);
        // 帧时 6ms~26ms 正弦扰动 → 1/dt ∈ [38,166] FPS，整数 FPS 在快照间必变
        const double dt = 0.016 + (0.010 * std::sin(static_cast<double>(i) * 0.31));
        au::FrameStats::instance().record(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    std::vector<std::uint8_t> snap_a;
    capture_surface(win->surface(), snap_a);

    // 诊断：FrameStats 状态 + 整帧亮像素数（叠加层文本为浅色，应存在）
    const auto bright_count = [&](const std::vector<std::uint8_t> &buf) -> long {
        long c = 0;
        for (size_t i = 0; i + 3 < buf.size(); i += 4) {
            if (buf.at(i) + buf.at(i + 1) + buf.at(i + 2) > 300) {
                ++c;
            }
        }
        return c;
    };
    AURORA_TEST_PRINTF("[diag] after warmup window_size=%zu fps=%.1f bright_pixels_A=%ld\n",
                       au::FrameStats::instance().window_size(), au::FrameStats::instance().fps(),
                       bright_count(snap_a));

    // 再驱动 ~1.5s：叠加层应持续刷新（每 500ms HUD 重绘，文本随实时 FPS 变化）。
    // 同一正弦扰动继续推进相位，窗口（128 帧）已移位 → snap_a 与 snap_b 的滚动统计不同 → 文本不同。
    for (int i = 90; i < 180; ++i) {
        win->force_full_redraw();
        (void)win->present_root(root);
        const double dt = 0.016 + (0.010 * std::sin(static_cast<double>(i) * 0.31));
        au::FrameStats::instance().record(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    std::vector<std::uint8_t> snap_b;
    capture_surface(win->surface(), snap_b);
    AURORA_TEST_PRINTF("[diag] second capture window_size=%zu fps=%.1f bright_pixels_B=%ld\n",
                       au::FrameStats::instance().window_size(), au::FrameStats::instance().fps(),
                       bright_count(snap_b));

    const long diff = pixel_diff(snap_a, snap_b);
    AURORA_TEST_PRINTF(
        "[overlay] two-frame full-frame pixel diff=%ld bytes (>0 proves overlay refreshes live, not DL-frozen)\n",
        diff);

    // 行为断言：叠加层未被冻结——两次抓取的像素应显著不同
    AURORA_TEST_CHECK_MSG(diff > 500,
                          "PerfOverlay should refresh live: two-frame pixel diff should be significant "
                          "(>500B). If 0, frozen at first frame by DL");
}
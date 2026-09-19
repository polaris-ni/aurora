/// 测试类型: integration
/// 目标单元: include/aurora/window/wgpu_surface.h + include/aurora/window/wgpu_x11_surface.h
///           + src/aurora/window/window_factory.cpp
/// 测试说明: wgpu GPU 栅格真实窗口 smoke——create_window(WgpuOptions) 开窗后经
///           Window::present_root 帧调度连续出帧（gpu_backend 标识 "gpu-wgpu"、
///           gpu_active 恒真不回退、frame_count 递增）；RendererPreference::GpuWgpu
///           经平台宿主选项（Win32Options/X11Options）强制路由（不可用时报
///           RendererUnavailable 不静默降级）。宿主类型按编译口径别名切换。
///           依赖桌面会话（HWND/X Display）+ wgpu adapter，任一缺失 SKIP。

#include <chrono>
#include <memory>
#include <thread>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#if defined(AURORA_BACKEND_GPU_WGPU) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_X11))

#include "aurora/aurora.h"
#include "aurora/window/native_surfaces.h"
#include "aurora/window/window.h"

namespace aurora::test_cases::itest_wgpu_present {

// 宿主切换：Windows → WgpuSurface(Win32Options)；Linux/X11 → WgpuX11Surface(X11Options)。
// 两宿主宏互斥（BACKEND_WIN32 仅 Windows、BACKEND_X11 仅 Linux），任一构建至多命中一种。
#ifdef AURORA_BACKEND_WIN32
using HostWgpuSurface = au::WgpuSurface;
using HostOptions = au::Win32Options;
#else
using HostWgpuSurface = au::WgpuX11Surface;
using HostOptions = au::X11Options;
#endif

AURORA_TEST_CASE(wgpu_surface_present_frames_and_no_fallback) {
    au::WgpuOptions opts;
    opts.size = au::Size{.width = 320.0F, .height = 240.0F};
    opts.title = "itest_wgpu_present";
    auto created = au::create_window(opts);
    if (!created) {
        AURORA_TEST_SKIP("开窗失败（无桌面会话或无 wgpu adapter），GPU 上屏链路无实例可验");
    }
    auto win = std::move(created.value());
    auto &surface = win->surface();

    auto *sink = surface.gpu_backend();
    AURORA_TEST_REQUIRE(sink != nullptr);
    AURORA_TEST_CHECK_EQ(sink->name(), std::string_view("gpu-wgpu"));

    au::Node page = au::Text{au::TextProps{.content = au::LocalizedString{"Wgpu Aa 01"}}};
    // 开窗后的首批 map/configure/expose 事件会触发宿主「同步重绘」（present_request_ → 额外
    // 一次 present），属真实窗口语义而非回退；先短 settle 泵掉该突发，保持下方逐帧帧数口径
    // 为精确相等。
    for (int k = 0; k < 5; ++k) {
        surface.poll_platform_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const int base = surface.frame_count();
    for (int i = 1; i <= 3; ++i) {
        win->force_full_redraw();  // 绕过 idle 跳帧：逐帧走完整 GPU begin→replay→end→present
        const auto r = win->present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK_EQ(surface.frame_count(), base + i);
        surface.poll_platform_events();
    }
    // 全程无运行期失效（sink.begin_frame 恒成功 → 不触发永久软件回退）。
    auto *ws = dynamic_cast<HostWgpuSurface *>(&surface);
    AURORA_TEST_REQUIRE(ws != nullptr);
    AURORA_TEST_CHECK_TRUE(ws->gpu_active());
}

AURORA_TEST_CASE(gpu_wgpu_preference_routing) {
    HostOptions opts;
    opts.size = au::Size{.width = 200.0F, .height = 160.0F};
    opts.title = "itest_wgpu_routing";
    opts.renderer = au::RendererPreference::GpuWgpu;
    auto created = au::create_window(opts);
    if (!created) {
        // 强制 GPU 栅格不可用：必须报 renderer-unavailable（不静默降级为软件/其他 GPU 路径）。
        AURORA_TEST_CHECK_EQ(created.error().code, std::string{"renderer-unavailable"});
        AURORA_TEST_SKIP("无 wgpu adapter，强制路由的错误分支已由上方 code 断言覆盖");
    }
    auto win = std::move(created.value());
    AURORA_TEST_CHECK(dynamic_cast<HostWgpuSurface *>(&win->surface()) != nullptr);

    // Auto 偏好不受本路径影响（不隐式选 wgpu 栅格，保持既有平台优先序）。
    HostOptions auto_opts;
    auto_opts.size = au::Size{.width = 200.0F, .height = 160.0F};
    auto_opts.title = "itest_wgpu_routing_auto";
    auto auto_win = au::create_window(auto_opts);
    if (auto_win) {
        AURORA_TEST_CHECK(dynamic_cast<HostWgpuSurface *>(&auto_win.value()->surface()) == nullptr);
    }
}

}  // namespace aurora::test_cases::itest_wgpu_present

#else  // 后端未编译

namespace aurora::test_cases::itest_wgpu_present {

AURORA_TEST_CASE(wgpu_surface_present_frames_and_no_fallback) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或宿主宏（WIN32/X11）未开启，Wgpu*Surface 整体被宏剔除");
}
AURORA_TEST_CASE(gpu_wgpu_preference_routing) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或宿主宏（WIN32/X11）未开启");
}

}  // namespace aurora::test_cases::itest_wgpu_present

#endif  // AURORA_BACKEND_GPU_WGPU && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_X11)

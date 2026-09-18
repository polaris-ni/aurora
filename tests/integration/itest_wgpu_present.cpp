/// 测试类型: integration
/// 目标单元: include/aurora/window/wgpu_surface.h + src/aurora/window/window_factory.cpp
/// 测试说明: wgpu GPU 栅格真实窗口 smoke——create_window(WgpuOptions) 开窗后经
///           Window::present_root 帧调度连续出帧（gpu_backend 标识 "gpu-wgpu"、
///           gpu_active 恒真不回退、frame_count 递增）；RendererPreference::GpuWgpu
///           经 Win32Options 强制路由（不可用时报 RendererUnavailable 不静默降级）。
///           依赖 Windows 桌面会话 + wgpu adapter，任一缺失 SKIP。

#include <memory>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#if defined(AURORA_BACKEND_GPU_WGPU) && defined(AURORA_BACKEND_WIN32)

#include "aurora/aurora.h"
#include "aurora/window/native_surfaces.h"
#include "aurora/window/window.h"

namespace aurora::test_cases::itest_wgpu_present {

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
    for (int i = 1; i <= 3; ++i) {
        win->force_full_redraw();  // 绕过 idle 跳帧：逐帧走完整 GPU begin→replay→end→present
        const auto r = win->present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK_EQ(surface.frame_count(), i);
        surface.poll_platform_events();
    }
    // 全程无运行期失效（sink.begin_frame 恒成功 → 不触发永久软件回退）。
    auto *ws = dynamic_cast<au::WgpuSurface *>(&surface);
    AURORA_TEST_REQUIRE(ws != nullptr);
    AURORA_TEST_CHECK_TRUE(ws->gpu_active());
}

AURORA_TEST_CASE(gpu_wgpu_preference_routing) {
    au::Win32Options opts;
    opts.size = au::Size{.width = 200.0F, .height = 160.0F};
    opts.title = "itest_wgpu_routing";
    opts.renderer = au::RendererPreference::GpuWgpu;
    auto created = au::create_window(opts);
    if (!created) {
        // 强制 GPU 栅格不可用：必须报 renderer-unavailable（不静默降级为 GDI/D3D11）。
        AURORA_TEST_CHECK_EQ(created.error().code, std::string{"renderer-unavailable"});
        AURORA_TEST_SKIP("无 wgpu adapter，强制路由的错误分支已由上方 code 断言覆盖");
    }
    auto win = std::move(created.value());
    AURORA_TEST_CHECK(dynamic_cast<au::WgpuSurface *>(&win->surface()) != nullptr);

    // Auto 偏好不受本路径影响（不隐式选 WgpuSurface，保持既有 D3D11/GDI 优先序）。
    au::Win32Options auto_opts;
    auto_opts.size = au::Size{.width = 200.0F, .height = 160.0F};
    auto_opts.title = "itest_wgpu_routing_auto";
    auto auto_win = au::create_window(auto_opts);
    if (auto_win) {
        AURORA_TEST_CHECK(dynamic_cast<au::WgpuSurface *>(&auto_win.value()->surface()) == nullptr);
    }
}

}  // namespace aurora::test_cases::itest_wgpu_present

#else  // 后端未编译

namespace aurora::test_cases::itest_wgpu_present {

AURORA_TEST_CASE(wgpu_surface_present_frames_and_no_fallback) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或 AURORA_BACKEND_WIN32 未开启，WgpuSurface 整体被宏剔除");
}
AURORA_TEST_CASE(gpu_wgpu_preference_routing) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或 AURORA_BACKEND_WIN32 未开启");
}

}  // namespace aurora::test_cases::itest_wgpu_present

#endif  // AURORA_BACKEND_GPU_WGPU && AURORA_BACKEND_WIN32

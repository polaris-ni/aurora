/// 测试类型: integration
/// 目标单元: include/aurora/window/wgpu_win32_surface.h + wgpu_x11_surface.h + wgpu_wayland_surface.h
///           + src/aurora/window/window_factory.cpp
/// 测试说明: wgpu GPU 栅格真实窗口 smoke——create_window(WgpuOptions) 开窗后经
///           Window::present_root 帧调度连续出帧（gpu_backend 标识 "gpu-wgpu"、
///           gpu_active 恒真不回退、frame_count 递增、software_present_count 恒 0
///           ——含开窗初期系统重绘突发，该计数非零即「app 帧绕过 GPU 通道上屏软件缓冲」
///           的白闪签名）；RendererPreference::GpuWgpu
///           经平台宿主选项（Win32Options/X11Options/WaylandOptions）强制路由（不可用
///           时报 RendererUnavailable 不静默降级）；Wayland 侧另有强制 `ClientSide` 装饰
///           的用例，把「自绘 CSD 装饰回放进 GPU 帧」锁成逐帧递增的确定断言。宿主类型按
///           编译口径别名切换（与工厂择一序 Win32 → X11 → Wayland 同序）。
///           依赖桌面会话（HWND/X Display/Wayland compositor）+ wgpu adapter，任一缺失 SKIP。
///
///           `WgpuOptions` 一例的建窗与帧推进统一经 E2E 内核（`e2e::Session`）；三个「宿主选项 +
///           `RendererPreference::GpuWgpu`」路由用例保持直接调工厂——内核的 `WindowSpec` 描述的是
///           **目标后端**，不描述「宿主选项 × 渲染器偏好」这一组合，强行并入会把内核的规格面
///           撑成宿主矩阵，与内核「只回答用哪个后端」的定位相悖。

#include <chrono>
#include <memory>
#include <thread>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#if defined(AURORA_BACKEND_GPU_WGPU) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_X11) || defined(AURORA_BACKEND_WAYLAND))

#include "aurora/aurora.h"
#include "aurora/window/native_surfaces.h"
#include "aurora/window/window.h"
#include "e2e/harness.h"

namespace aurora::test_cases::itest_wgpu_present {

// 宿主切换（与 create_window(WgpuOptions) 的编译期择一序一致）：Windows → WgpuWin32Surface
// (Win32Options)；Linux/X11 → WgpuX11Surface(X11Options)；仅 Wayland → WgpuWaylandSurface
// (WaylandOptions)。Linux 下 X11/Wayland 宏可并开，本别名取 X11；Wayland 宿主的强制路由
// 由 create_native_window 会话选择与专属工厂覆盖（X11+Wayland 并开构建下亦可经
// WaylandOptions+GpuWgpu 直达，见 gpu_wgpu_preference_routing 的宿主注）。
#ifdef AURORA_BACKEND_WIN32
using HostWgpuSurface = au::WgpuWin32Surface;
using HostOptions = au::Win32Options;
#elif defined(AURORA_BACKEND_X11)
using HostWgpuSurface = au::WgpuX11Surface;
using HostOptions = au::X11Options;
#else
using HostWgpuSurface = au::WgpuWaylandSurface;
using HostOptions = au::WaylandOptions;
#endif

AURORA_TEST_CASE(wgpu_win32_surface_present_frames_and_no_fallback) {
    e2e::WindowSpec spec;
    spec.backend = e2e::Backend::Wgpu;
    spec.width = 320;
    spec.height = 240;
    spec.title = "itest_wgpu_present";
    auto session = e2e::open(spec);
    if (!session.ok()) {
        AURORA_TEST_SKIP("开窗失败（无桌面会话或无 wgpu adapter），GPU 上屏链路无实例可验");
    }
    auto &surface = session.surface();

    auto *sink = surface.gpu_backend();
    AURORA_TEST_REQUIRE(sink != nullptr);
    AURORA_TEST_CHECK_EQ(sink->name(), std::string_view("gpu-wgpu"));

    auto *ws = dynamic_cast<HostWgpuSurface *>(&surface);
    AURORA_TEST_REQUIRE(ws != nullptr);

    session.mount(au::Text{au::TextProps{.content = au::LocalizedString{"Wgpu Aa 01"}}});
    // 开窗后的首批 map/configure/expose 事件会触发宿主「同步重绘」（present_request_ → 对缓存
    // 根再渲染一帧），属真实窗口语义而非回退；先短 settle 泵掉该突发，保持下方逐帧帧数口径为
    // 精确相等。该路径若绕过 GPU 帧通道直接 present()，软件缓冲上屏即白闪——故此处一并断言
    // 软件上屏帧数为 0（各宿主 software_present_count 的口径）。
    for (int k = 0; k < 5; ++k) {
        session.pump_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AURORA_TEST_CHECK_EQ(ws->software_present_count(), 0);
    const int base = surface.frame_count();
    for (int i = 1; i <= 3; ++i) {
        session.window().force_full_redraw();  // 绕过 idle 跳帧：逐帧走完整 GPU begin→replay→end→present
        const auto r = session.present();
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK_EQ(surface.frame_count(), base + i);
        session.pump_events();
    }
    // 全程无运行期失效（sink.begin_frame 恒成功 → 不触发永久软件回退）。
    AURORA_TEST_CHECK_TRUE(ws->gpu_active());
    AURORA_TEST_CHECK_EQ(ws->software_present_count(), 0);
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

// Wayland 宿主专属：X11/Wayland 并开构建里上方别名取 X11，本用例不经别名、直接用
// WaylandOptions+GpuWgpu 直达 WgpuWaylandSurface，验证装配 + 出帧不回退（Wayland-only
// 构建与上方 routing 用例同语义，仍成立）。
#ifdef AURORA_BACKEND_WAYLAND
AURORA_TEST_CASE(wayland_host_gpu_routing_and_frames) {
    au::WaylandOptions opts;
    opts.size = au::Size{.width = 320.0F, .height = 240.0F};
    opts.title = "itest_wgpu_wayland";
    opts.renderer = au::RendererPreference::GpuWgpu;
    auto created = au::create_window(opts);
    if (!created) {
        AURORA_TEST_CHECK_EQ(created.error().code, std::string{"renderer-unavailable"});
        AURORA_TEST_SKIP("无 Wayland 会话或无 wgpu adapter，Wayland GPU 宿主无实例可验");
    }
    auto win = std::move(created.value());
    auto *ws = dynamic_cast<au::WgpuWaylandSurface *>(&win->surface());
    AURORA_TEST_REQUIRE(ws != nullptr);
    AURORA_TEST_CHECK_TRUE(ws->gpu_active());
    auto *sink = ws->gpu_backend();
    AURORA_TEST_REQUIRE(sink != nullptr);
    AURORA_TEST_CHECK_EQ(sink->name(), std::string_view("gpu-wgpu"));

    au::Node page = au::Text{au::TextProps{.content = au::LocalizedString{"Wgpu Wayland Aa 01"}}};
    // 同首用例口径：泵掉 map/configure 触发的同步重绘突发（该路径必须走 GPU 帧通道，不得以
    // 软件 wl_shm 帧兜底），保持逐帧帧数精确相等。
    for (int k = 0; k < 5; ++k) {
        ws->poll_platform_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AURORA_TEST_CHECK_EQ(ws->software_present_count(), 0);
    const int base = ws->frame_count();
    for (int i = 1; i <= 3; ++i) {
        win->force_full_redraw();
        const auto r = win->present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        AURORA_TEST_CHECK_EQ(ws->frame_count(), base + i);
        ws->poll_platform_events();
    }
    AURORA_TEST_CHECK_TRUE(ws->gpu_active());
    AURORA_TEST_CHECK_EQ(ws->software_present_count(), 0);
    // CSD 装饰合成进 GPU 帧：SSD 合成器（Weston/KDE 等）下内嵌宿主不绘装饰、恒 0；
    // CSD 兜底合成器（GNOME 等）下逐帧递增（settle 突发与 3 帧已保证 ≥ 总帧数 - 1）。
    // 两态皆符合契约，故只锁「非 0 即覆盖几乎全部呈现帧」，不锁具体值。
    const int deco = ws->decoration_replay_count();
    AURORA_TEST_CHECK_TRUE(deco == 0 || deco >= ws->frame_count() - 3);
}

// CSD 装饰合成进 GPU 帧的确定性验证：上方用例在 SSD 合成器（Weston/KDE）下只会命中
// `deco == 0` 分支，「装饰合成进 GPU 帧」这条实路径拿不到真机证据。本用例按规格 §4.1
// 强制 `DecorationPolicy::ClientSide`（即便合成器支持 SSD 也自绘），于是安全区 top 必为
// 标题栏高、每帧 `Sink::end_frame` 必录放一份装饰 → decoration_replay_count 逐帧递增。
AURORA_TEST_CASE(wayland_csd_decoration_replays_into_gpu_frame) {
    au::WaylandOptions opts;
    opts.size = au::Size{.width = 320.0F, .height = 240.0F};
    opts.title = "itest_wgpu_wayland_csd";
    opts.renderer = au::RendererPreference::GpuWgpu;
    opts.style.decoration = au::DecorationPolicy::ClientSide;
    auto created = au::create_window(opts);
    if (!created) {
        AURORA_TEST_CHECK_EQ(created.error().code, std::string{"renderer-unavailable"});
        AURORA_TEST_SKIP("无 Wayland 会话或无 wgpu adapter，CSD 装饰合成无实例可验");
    }
    auto win = std::move(created.value());
    auto *ws = dynamic_cast<au::WgpuWaylandSurface *>(&win->surface());
    AURORA_TEST_REQUIRE(ws != nullptr);
    // 强制 CSD 确实生效的判据（否则下方「装饰回放递增」会因 csd_title=false 恒 0 而空转）。
    AURORA_TEST_CHECK_GT(ws->content_inset().top, 0.0F);

    au::Node page = au::Text{au::TextProps{.content = au::LocalizedString{"Wgpu CSD Aa 01"}}};
    for (int k = 0; k < 5; ++k) {
        ws->poll_platform_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AURORA_TEST_CHECK_EQ(ws->software_present_count(), 0);
    const int deco_base = ws->decoration_replay_count();
    for (int i = 1; i <= 3; ++i) {
        win->force_full_redraw();
        const auto r = win->present_root(page);
        AURORA_TEST_CHECK(static_cast<bool>(r));
        ws->poll_platform_events();
    }
    // 每帧一份装饰回放（settle 泵期间若另有呈现帧则只多不少，故取下界）。
    AURORA_TEST_CHECK_GE(ws->decoration_replay_count(), deco_base + 3);
    AURORA_TEST_CHECK_TRUE(ws->gpu_active());
    AURORA_TEST_CHECK_EQ(ws->software_present_count(), 0);
}
#else
AURORA_TEST_CASE(wayland_host_gpu_routing_and_frames) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WAYLAND 未开启，Wayland GPU 宿主整体被宏剔除");
}
AURORA_TEST_CASE(wayland_csd_decoration_replays_into_gpu_frame) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WAYLAND 未开启，Wayland GPU 宿主整体被宏剔除");
}
#endif

}  // namespace aurora::test_cases::itest_wgpu_present

#else  // 后端未编译

namespace aurora::test_cases::itest_wgpu_present {

AURORA_TEST_CASE(wgpu_win32_surface_present_frames_and_no_fallback) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或宿主宏（WIN32/X11/WAYLAND）未开启，Wgpu*Surface 整体被宏剔除");
}
AURORA_TEST_CASE(gpu_wgpu_preference_routing) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或宿主宏（WIN32/X11/WAYLAND）未开启");
}
AURORA_TEST_CASE(wayland_host_gpu_routing_and_frames) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或宿主宏（WIN32/X11/WAYLAND）未开启");
}
AURORA_TEST_CASE(wayland_csd_decoration_replays_into_gpu_frame) {
    AURORA_TEST_SKIP("AURORA_BACKEND_GPU_WGPU 或宿主宏（WIN32/X11/WAYLAND）未开启");
}

}  // namespace aurora::test_cases::itest_wgpu_present

#endif  // AURORA_BACKEND_GPU_WGPU && (AURORA_BACKEND_WIN32 || AURORA_BACKEND_X11 || AURORA_BACKEND_WAYLAND)

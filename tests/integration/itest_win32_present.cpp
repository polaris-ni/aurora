/// 测试类型: integration
/// 目标单元: include/aurora/window/win32_surface.h
/// 测试说明: Win32/GDI 真实窗口的上屏计数口径——`frame_count()` 逐次 `present()` 自增
///           （帧循环上屏即计数，与系统几何变化无关），且与宿主观测器 `present_count()`
///           （仅 WM_SIZE/WM_PAINT 触发的同步重渲染次数）相互独立。无显示环境建窗失败时 SKIP
#include <memory>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#if defined(AURORA_PLATFORM_WINDOWS) && defined(AURORA_BACKEND_WIN32)

#include "aurora/render/painter.h"
#include "aurora/window/win32_surface.h"

namespace aurora::test_cases::itest_win32_present {

AURORA_TEST_CASE(frame_count_counts_every_presented_frame) {
    auto surf = std::make_unique<au::Win32Surface>(320, 240, "win32 present itest", au::WindowStyleOptions{});
    if (surf->native_handle() == nullptr) {
        // 无桌面会话（服务/容器环境）：建窗失败属环境问题，跳过而非失败。
        AURORA_TEST_SKIP("无法创建真实 Win32 窗口（无显示环境）");
    }

    // 建窗初期的系统重绘（首帧 WM_SIZE/WM_PAINT）会让宿主计数先天非零，故只断言增量。
    const int host_before = surf->present_count();
    const int frames_before = surf->frame_count();

    for (int i = 1; i <= 3; ++i) {
        const auto bf = surf->begin_frame(320, 240);
        AURORA_TEST_CHECK(bf.ok());
        surf->painter().fill_rect(
            au::Rect{.origin = au::Point{.x = 10.0F, .y = 10.0F}, .size = au::Size{.width = 100.0F, .height = 100.0F}},
            au::Color{255, 0, 0, 255});
        const auto pr = surf->present();
        AURORA_TEST_CHECK(pr.ok());
        // 每帧几何未变、无系统重绘，仍必须计一帧（缺陷回归点：曾转发宿主同步重渲染计数而恒 0）。
        AURORA_TEST_CHECK_EQ(surf->frame_count(), frames_before + i);
    }

    // 两个观测器语义不同源：帧循环上屏不推进宿主的同步重渲染计数。
    AURORA_TEST_CHECK_EQ(surf->present_count(), host_before);
}

}  // namespace aurora::test_cases::itest_win32_present

#else  // 未开启 Win32 后端

namespace aurora::test_cases::itest_win32_present {

AURORA_TEST_CASE(frame_count_counts_every_presented_frame) {
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32 未开启（非 Windows 平台或未编译该后端）");
}

}  // namespace aurora::test_cases::itest_win32_present

#endif  // AURORA_PLATFORM_WINDOWS && AURORA_BACKEND_WIN32

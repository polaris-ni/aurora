/// 测试类型: unit
/// 目标单元: include/aurora/window/win32_surface.h
/// 测试说明: Win32/GDI 后端类型契约与 win32_media_query 纯映射逻辑（经 HeadlessSurface 注入
/// scale/size）；真实 HWND 创建与 GDI 上屏路径不测。头整体被 AURORA_BACKEND_WIN32 门控

#ifdef AURORA_BACKEND_WIN32
#include <type_traits>

#include "aurora/window/win32_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_win32_surface {

AURORA_TEST_CASE(win32_surface_type_contract) {
#if defined(AURORA_BACKEND_WIN32)
    static_assert(std::is_base_of_v<aurora::Surface, aurora::Win32Surface>);
    static_assert(std::is_final_v<aurora::Win32Surface>);
    static_assert(!std::is_copy_constructible_v<aurora::Win32Surface>);
    static_assert(!std::is_move_constructible_v<aurora::Win32Surface>);
    static_assert(!std::is_default_constructible_v<aurora::Win32Surface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::Win32Surface>);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32 未开启，头文件整体被宏剔除");
#endif
}

AURORA_TEST_CASE(win32_media_query_maps_headless_surface) {
#if defined(AURORA_BACKEND_WIN32) && defined(AURORA_BACKEND_HEADLESS)
    // win32_media_query 是纯映射函数：只读 Surface 的 scale_factor()/size()，
    // 叠加系统 DPI/屏幕度量与减弱动效设置——不创建窗口，可用 HeadlessSurface 注入。
    HeadlessSurface surface("", Size{.width = 800.0F, .height = 600.0F});
    const MediaQuery mq = win32_media_query(surface);

    // Headless 未覆写 scale_factor（基类默认 1.0），mq.size 即注入的逻辑尺寸。
    AURORA_TEST_CHECK_NEAR(mq.scale_factor, 1.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(mq.size.width, 800.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(mq.size.height, 600.0F, 1e-4F);

    // 平台/设备归类是硬编码契约。
    AURORA_TEST_CHECK(mq.platform == PlatformKind::Windows);
    AURORA_TEST_CHECK(mq.device == DeviceKind::Desktop);

    // 屏幕物理度量经 GetSystemMetrics 获取，须为正值。
    AURORA_TEST_CHECK_TRUE(mq.screen_size.width > 0.0F);
    AURORA_TEST_CHECK_TRUE(mq.screen_size.height > 0.0F);
    // 方向由屏幕宽高比推导（与函数体内同一规则保持一致）。
    const ScreenOrientation expected = (mq.screen_size.width >= mq.screen_size.height)
                                           ? ScreenOrientation::Landscape
                                           : ScreenOrientation::Portrait;
    AURORA_TEST_CHECK(mq.orientation == expected);
#else
    AURORA_TEST_SKIP("需要 AURORA_BACKEND_WIN32 与 AURORA_BACKEND_HEADLESS 同时开启");
#endif
}

AURORA_TEST_CASE(win32_surface_window_creation_skipped) {
#if defined(AURORA_BACKEND_WIN32)
    // Win32Surface 构造即创建真实 HWND（RegisterClass/CreateWindow），present 走 GDI
    // BitBlt 上屏；WM_SIZE/WM_PAINT 同步重渲染与白闪修复依赖真实消息泵，属集成层覆盖范围。
    AURORA_TEST_SKIP("Win32Surface 构造会创建真实 HWND 并依赖消息泵，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32 未开启，头文件整体被宏剔除");
#endif
}

}  // namespace aurora::test_cases::utest_win32_surface

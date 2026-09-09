/// 测试类型: unit
/// 目标单元: include/aurora/window/x11_surface.h
/// 测试说明: X11/Xlib 后端类型契约（Surface 派生、final、不可复制/移动/默认构造，#if 分支内
/// static_assert）；真实 X 连接与窗口上屏不触碰。头整体被 AURORA_PLATFORM_LINUX &&
/// AURORA_BACKEND_X11 门控，非 Linux / 未开后端时用例恒注册并 SKIP

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)
#include <type_traits>

#include "aurora/window/x11_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_x11_surface {

AURORA_TEST_CASE(x11_surface_type_contract) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)
    static_assert(std::is_base_of_v<aurora::Surface, aurora::X11Surface>);
    static_assert(std::is_final_v<aurora::X11Surface>);
    static_assert(!std::is_copy_constructible_v<aurora::X11Surface>);
    static_assert(!std::is_move_constructible_v<aurora::X11Surface>);
    static_assert(!std::is_default_constructible_v<aurora::X11Surface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::X11Surface>);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_X11 未开启（非 Linux 平台），头文件整体被宏剔除");
#endif
}

AURORA_TEST_CASE(x11_surface_window_creation_skipped) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)
    // X11Surface 构造即建立 X 连接并创建真实窗口（pimpl 持有 Display/Window/GC），
    // 事件翻译 / XPutImage 上屏依赖真实 X server，属集成层覆盖范围；单测不触碰 OS 资源。
    AURORA_TEST_SKIP("X11Surface 构造依赖 X server 连接，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_X11 未开启（非 Linux 平台），头文件整体被宏剔除");
#endif
}

}  // namespace aurora::test_cases::utest_x11_surface

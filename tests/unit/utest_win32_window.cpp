/// 测试类型: unit
/// 目标单元: include/aurora/window/win32_window.h
/// 测试说明: 共享 Win32 窗口宿主的类型契约（pimpl 封装、非 Surface 派生、不可拷贝/移动）；
/// 构造会创建真实 HWND，消息泵/事件翻译/DPI 路径不测。头整体被 AURORA_BACKEND_WIN32 门控

#ifdef AURORA_BACKEND_WIN32
#include <type_traits>

#include "aurora/window/win32_window.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_win32_window {

AURORA_TEST_CASE(win32_window_type_contract) {
#ifdef AURORA_BACKEND_WIN32
    // 契约：Win32Window 是独立窗口宿主（不含像素上屏），并非 Surface 派生类；
    // 与 Win32Surface(GDI)/D3D11Surface(GPU) 组合而非继承。
    static_assert(!std::is_base_of_v<aurora::Surface, aurora::Win32Window>);
    static_assert(std::is_class_v<aurora::Win32Window>);
    static_assert(!std::is_copy_constructible_v<aurora::Win32Window>);
    static_assert(!std::is_move_constructible_v<aurora::Win32Window>);
    static_assert(!std::is_default_constructible_v<aurora::Win32Window>);
    AURORA_TEST_CHECK_TRUE(std::is_class_v<aurora::Win32Window>);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32 未开启，头文件整体被宏剔除");
#endif
}

AURORA_TEST_CASE(win32_window_creation_skipped) {
#ifdef AURORA_BACKEND_WIN32
    // 构造函数立即注册窗口类并 CreateWindowW 创建真实 HWND；wait_events/poll_platform_events
    // 依赖消息队列，事件翻译/DPI 属集成层覆盖范围，单元测试不触碰 OS 资源。
    AURORA_TEST_SKIP("Win32Window 构造会注册窗口类并创建真实 HWND，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_WIN32 未开启，头文件整体被宏剔除");
#endif
}

}  // namespace aurora::test_cases::utest_win32_window

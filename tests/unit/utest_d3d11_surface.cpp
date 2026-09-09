/// 测试类型: unit
/// 目标单元: include/aurora/window/d3d11_surface.h
/// 测试说明: D3D11 后端类型契约（宏门控、继承 Surface、不可拷贝/构造签名）；
/// 构造与帧管线会创建真实 Win32 窗口与 D3D11 设备，单元测试不触碰 OS 资源

#ifdef AURORA_BACKEND_D3D11
#include <type_traits>

#include "aurora/window/d3d11_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_d3d11_surface {

AURORA_TEST_CASE(d3d11_surface_type_contract) {
#ifdef AURORA_BACKEND_D3D11
    static_assert(std::is_base_of_v<aurora::Surface, aurora::D3D11Surface>);
    static_assert(!std::is_abstract_v<aurora::D3D11Surface>);
    static_assert(!std::is_copy_constructible_v<aurora::D3D11Surface>);
    static_assert(!std::is_move_constructible_v<aurora::D3D11Surface>);
    static_assert(!std::is_default_constructible_v<aurora::D3D11Surface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::D3D11Surface>);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_D3D11 未开启（默认 OFF），头文件整体被宏剔除");
#endif
}

AURORA_TEST_CASE(d3d11_surface_os_dependent_paths_skipped) {
#ifdef AURORA_BACKEND_D3D11
    // 构造函数会创建真实 HWND + D3D11Device/SwapChain；vsync/增量上屏/device-lost
    // 恢复均依赖真实设备与消息泵，属集成层覆盖范围，单元测试不触碰 OS 资源。
    AURORA_TEST_SKIP("D3D11Surface 构造会创建真实 Win32 窗口与 D3D11 设备/交换链，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_D3D11 未开启（默认 OFF），头文件整体被宏剔除");
#endif
}

}  // namespace aurora::test_cases::utest_d3d11_surface

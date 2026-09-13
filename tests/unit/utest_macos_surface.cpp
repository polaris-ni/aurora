/// 测试类型: unit
/// 目标单元: include/aurora/window/macos_surface.h
/// 测试说明: macOS 后端类型契约 skip 桩——头整体被
/// AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS 门控，非 Apple 平台无法编译验证

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
#include <type_traits>

#include "aurora/window/macos_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_macos_surface {

AURORA_TEST_CASE(macos_surface_type_contract) {
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    static_assert(std::is_base_of_v<aurora::Surface, aurora::MacOSSurface>);
    static_assert(!std::is_copy_constructible_v<aurora::MacOSSurface>);
    static_assert(!std::is_move_constructible_v<aurora::MacOSSurface>);
    static_assert(!std::is_default_constructible_v<aurora::MacOSSurface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::MacOSSurface>);
#else
    AURORA_TEST_SKIP("macOS 后端仅在 AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS 下编译，当前平台未开启");
#endif
}

AURORA_TEST_CASE(macos_surface_os_dependent_paths_skipped) {
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    // 构造会创建真实 NSWindow/NSView 并依赖 AppKit 主线程运行循环，单元测试不触碰 OS 资源。
    AURORA_TEST_SKIP("MacOSSurface 构造会创建真实 AppKit 窗口，单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("macOS 后端仅在 AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS 下编译，当前平台未开启");
#endif
}

}  // namespace aurora::test_cases::utest_macos_surface

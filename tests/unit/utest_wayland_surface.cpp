/// 测试类型: unit
/// 目标单元: include/aurora/window/wayland_surface.h
/// 测试说明: Wayland 后端类型契约 skip 桩——头整体被
/// AURORA_PLATFORM_LINUX && !AURORA_PLATFORM_ANDROID && AURORA_BACKEND_WAYLAND 门控，非 Linux 平台无法编译

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
#include <type_traits>

#include "aurora/window/wayland_surface.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_wayland_surface {

AURORA_TEST_CASE(wayland_surface_type_contract) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
    static_assert(std::is_base_of_v<aurora::Surface, aurora::WaylandSurface>);
    static_assert(std::is_final_v<aurora::WaylandSurface>);
    static_assert(!std::is_copy_constructible_v<aurora::WaylandSurface>);
    static_assert(!std::is_move_constructible_v<aurora::WaylandSurface>);
    static_assert(!std::is_default_constructible_v<aurora::WaylandSurface>);
    AURORA_TEST_CHECK_TRUE(std::is_base_of_v<aurora::Surface, aurora::WaylandSurface>);
#else
    AURORA_TEST_SKIP("Wayland 后端仅在 Linux + AURORA_BACKEND_WAYLAND 下编译，当前平台未开启");
#endif
}

AURORA_TEST_CASE(wayland_surface_os_dependent_paths_skipped) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_WAYLAND)
    // 构造需要 WAYLAND_DISPLAY 连接（wl_display/registry/xdg-shell 握手）；
    // is_available()==false 分支由工厂返回 Result 错误，需真实合成器或 headless 组合器
    // （如 wlheadless）驱动，属集成层覆盖范围。
    AURORA_TEST_SKIP("WaylandSurface 构造依赖真实合成器连接（wl_display/xdg-shell），单测不触碰 OS 资源");
#else
    AURORA_TEST_SKIP("Wayland 后端仅在 Linux + AURORA_BACKEND_WAYLAND 下编译，当前平台未开启");
#endif
}

}  // namespace aurora::test_cases::utest_wayland_surface

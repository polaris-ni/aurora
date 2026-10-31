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
    // 四枚运行期开关必须真覆写：`&MacOSSurface::set_visible` 的类型只在类内声明过成员时才是
    // `MacOSSurface::*`，漏写即继承基类空实现、类型退回 `Surface::*`——此处把「声明了」钉成
    // 编译期判据，否则 `is_visible()` 恒真而能力位仍声称已接线。
    using VisibleSetter = void (aurora::MacOSSurface::*)(bool);
    using VisibleGetter = bool (aurora::MacOSSurface::*)() const;
    static_assert(std::is_same_v<decltype(&aurora::MacOSSurface::set_visible), VisibleSetter>);
    static_assert(std::is_same_v<decltype(&aurora::MacOSSurface::is_visible), VisibleGetter>);
    static_assert(std::is_same_v<decltype(&aurora::MacOSSurface::set_always_on_top), VisibleSetter>);
    static_assert(std::is_same_v<decltype(&aurora::MacOSSurface::is_always_on_top), VisibleGetter>);
    // 无障碍两枚覆写同理：漏写即落回基类的 nullptr / no-op 默认实现，桥永不接线。
    using A11yProviderGetter = aurora::a11y::Provider *(aurora::MacOSSurface::*)() const;
    using A11yRootSetter = void (aurora::MacOSSurface::*)(aurora::Widget *);
    static_assert(std::is_same_v<decltype(&aurora::MacOSSurface::accessibility_provider), A11yProviderGetter>);
    static_assert(std::is_same_v<decltype(&aurora::MacOSSurface::set_accessibility_root), A11yRootSetter>);
#else
    AURORA_TEST_SKIP("macOS backend builds only under AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS; not enabled");
#endif
}

AURORA_TEST_CASE(macos_surface_os_dependent_paths_skipped) {
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
    // 构造会创建真实 NSWindow/NSView 并依赖 AppKit 主线程运行循环，单元测试不触碰 OS 资源。
    AURORA_TEST_SKIP("MacOSSurface construction creates a real AppKit window; unit tests do not touch OS resources");
#else
    AURORA_TEST_SKIP("macOS backend builds only under AURORA_PLATFORM_MACOS && AURORA_BACKEND_MACOS; not enabled");
#endif
}

}  // namespace aurora::test_cases::utest_macos_surface

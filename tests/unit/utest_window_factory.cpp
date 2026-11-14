/// 测试类型: unit
/// 目标单元: src/aurora/window/window_factory.cpp（声明见 include/aurora/window/window.h）
/// 测试说明: 工厂三个公共出口的**无头可判面**——注入式入口的空 Surface 报错、所有权与标题交接、
///           auto_detect_surface 的后端选择次序；真实窗口创建（create_native_window）按
///           utest_win32_surface 的既定口径不在此覆盖
/// 覆盖说明: 各后端的专属工厂（Win32/X11/Wayland/D3D11/Glfw/MacOS/Wasm）需要真实窗口与显示服务，
///           归 tools/verify 的真机探针；此处只钉与平台无关的判定与交接

#include <cstdlib>
#include <memory>
#include <string>

#include "aurora/core/result.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_factory {

namespace {

/// 按规范声明的次序推导「当前构建 + 当前会话」应选的后端，用作 `auto_detect_surface()` 的独立基准。
///
/// 这不是把实现抄一遍：次序本身是 `window.h` 对外的承诺（Wayland > X11 > MacOS > Wasm > Win32 >
/// Glfw > Headless，Linux 双后端按会话类型），若实现改了次序而规格没改，本函数就是那条基准。
/// @return 期望的 SurfaceKind。
auto expected_surface_kind() -> SurfaceKind {
#if defined(AURORA_BACKEND_WAYLAND) && defined(AURORA_BACKEND_X11)
    return std::getenv("WAYLAND_DISPLAY") != nullptr ? SurfaceKind::Wayland : SurfaceKind::X11;
#elif defined(AURORA_BACKEND_WAYLAND)
    return SurfaceKind::Wayland;
#elif defined(AURORA_BACKEND_X11)
    return SurfaceKind::X11;
#elif defined(AURORA_BACKEND_MACOS)
    return SurfaceKind::MacOS;
#elif defined(AURORA_BACKEND_WASM)
    return SurfaceKind::Wasm;
#elif defined(AURORA_BACKEND_WIN32)
    return SurfaceKind::Win32;
#elif defined(AURORA_BACKEND_GLFW)
    return SurfaceKind::Glfw;
#else
    // 含 `AURORA_BACKEND_HEADLESS` 与「全部被剪裁」两种情形：实现两条分支都回 Headless。
    return SurfaceKind::Headless;
#endif
}

}  // namespace

AURORA_TEST_CASE(create_window_rejects_null_surface_with_platform_error) {
    // 注入式入口是第三方 Surface 的唯一通路：空指针必须返回错误而不是崩溃或造出野窗口。
    const Result<std::unique_ptr<Window>> made = create_window(std::unique_ptr<Surface>{}, WindowOptions{});

    AURORA_TEST_CHECK_FALSE(made.ok());
    AURORA_TEST_CHECK_MSG(made.error().code_enum == ErrorCode::PlatformUnavailable,
                          "a null surface is a platform-unavailable failure, not a generic internal one");
    AURORA_TEST_CHECK_MSG(!made.error().message.empty(), "the error explains what was wrong");
    AURORA_TEST_CHECK_MSG(made.error().message.find("null surface") != std::string::npos,
                          "the message names the actual defect (null surface)");
    AURORA_TEST_CHECK_MSG(!made.error().suggestion.empty(), "the error tells the caller what to pass instead");
}

AURORA_TEST_CASE(create_window_takes_ownership_of_the_injected_surface) {
    // 工厂接管所有权并套用跨后端共享的标题/尺寸：用 Headless 表面走完整条 make_window 出口。
    auto owned = std::make_unique<HeadlessSurface>("", Size{.width = 320.0F, .height = 200.0F});
    const HeadlessSurface *const raw = owned.get();

    WindowOptions opts;
    opts.title = "aurora-factory-probe";
    const Result<std::unique_ptr<Window>> made = create_window(std::move(owned), opts);
    AURORA_TEST_REQUIRE_MSG(made.ok(), "create_window(std::unique_ptr<Surface>) must accept a HeadlessSurface");

    const std::unique_ptr<Window> &win = made.value();
    AURORA_TEST_REQUIRE(win != nullptr);
    // 同一个对象：工厂移动接管，不复制也不替换。
    AURORA_TEST_CHECK_MSG(&win->surface() == raw, "the factory adopts the exact surface it was handed");
    AURORA_TEST_CHECK(std::string{win->title()} == "aurora-factory-probe");
    AURORA_TEST_CHECK_NEAR(win->size().width, 320.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(win->size().height, 200.0F, 1e-4F);
}

AURORA_TEST_CASE(auto_detect_surface_follows_the_documented_backend_order) {
    // 探测结果必须与规格声明的次序一致；Windows 构建恒为 Win32，无桌面后端时回退 Headless。
    const SurfaceKind detected = auto_detect_surface();
    AURORA_TEST_CHECK_MSG(detected == expected_surface_kind(),
                          "auto_detect_surface must pick the backend the documented order names");
    // D3D11 是渲染偏好（RendererPreference），不是可探测的表面种类：探测入口不得返回它。
    AURORA_TEST_CHECK_MSG(detected != SurfaceKind::D3D11,
                          "D3D11 is a renderer preference, not an auto-detected surface kind");
}

AURORA_TEST_CASE(enable_dpi_awareness_is_safe_before_any_window) {
    // 契约是「在 init_console 与 create_window 之前调用」，且幂等（进程级 static 守卫）。
    // 这里钉两件事：重复调用不出错、调用后无头窗口仍能建起来（不污染后续创建路径）。
    enable_dpi_awareness();
    enable_dpi_awareness();

    auto surf = std::make_unique<HeadlessSurface>("", Size{.width = 64.0F, .height = 64.0F});
    const Result<std::unique_ptr<Window>> made = create_window(std::move(surf), WindowOptions{});
    AURORA_TEST_CHECK_MSG(made.ok(), "a window must still come up after enabling DPI awareness");
    AURORA_TEST_CHECK_NEAR(made.value()->size().width, 64.0F, 1e-4F);
}

}  // namespace aurora::test_cases::utest_window_factory

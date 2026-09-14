/// 测试类型: unit
/// 目标单元: include/aurora/window/native_surfaces.h
/// 测试说明: 聚合包含入口的类型可见性与 Surface 抽象契约，窗口样式默认值、
/// DecorationPolicy/WindowResizeEdge 枚举值序的跨后端映射契约

#include <cstdint>
#include <type_traits>

#include "aurora/window/native_surfaces.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_native_surfaces {

AURORA_TEST_CASE(window_style_options_defaults) {
    constexpr WindowStyleOptions opts;
    AURORA_TEST_CHECK_FALSE(opts.always_on_top);
    AURORA_TEST_CHECK_FALSE(opts.frameless);
    AURORA_TEST_CHECK(opts.decoration == DecorationPolicy::Auto);
    AURORA_TEST_CHECK_FALSE(opts.transparent);
    AURORA_TEST_CHECK_TRUE(opts.resizable);
    // 0 = 不限（最小/最大尺寸默认不限）。
    AURORA_TEST_CHECK_NEAR(opts.min_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.min_size.height, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.max_size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(opts.max_size.height, 0.0F, 1e-4F);
    // CSD 标题栏样式默认 36dp 高。
    AURORA_TEST_CHECK_NEAR(opts.title_bar.height, 36.0F, 1e-4F);
}

AURORA_TEST_CASE(decoration_policy_enum_order_contract) {
    // surface.h 契约：枚举值序是各后端映射表的公共契约，不得重排。
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::Auto) == 0);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::ServerSide) == 1);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::ClientSide) == 2);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::Borderless) == 3);
    static_assert(static_cast<std::uint8_t>(DecorationPolicy::Frameless) == 4);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(DecorationPolicy::Frameless), 4);
}

AURORA_TEST_CASE(window_resize_edge_enum_order_contract) {
    // 契约：None 为下标 0 哨兵（后端据此拒绝），其余按 上/下/左/右/四角 排列。
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::None) == 0);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Top) == 1);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Bottom) == 2);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Left) == 3);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::Right) == 4);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::TopLeft) == 5);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::TopRight) == 6);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::BottomLeft) == 7);
    static_assert(static_cast<std::uint8_t>(WindowResizeEdge::BottomRight) == 8);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(WindowResizeEdge::BottomRight), 8);
}

AURORA_TEST_CASE(aggregate_header_exposes_surface_contract) {
    // 聚合头至少暴露抽象层：Surface 抽象、不可拷贝/移动。
    static_assert(std::is_abstract_v<Surface>);
    static_assert(!std::is_copy_constructible_v<Surface>);
    static_assert(!std::is_move_constructible_v<Surface>);
    AURORA_TEST_CHECK_TRUE(std::is_abstract_v<Surface>);

#ifdef AURORA_BACKEND_HEADLESS
    // 聚合头注释契约：Headless 零三方依赖、默认可用。
    HeadlessSurface surface;
    AURORA_TEST_CHECK_EQ(surface.frame_count(), 0);
    AURORA_TEST_CHECK_NEAR(surface.size().width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(surface.size().height, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NULL(surface.data());
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_HEADLESS 未开启，聚合头不暴露 HeadlessSurface");
#endif
}

AURORA_TEST_CASE(backend_surface_set_cursor_contract) {
    // 各真实窗口后端须覆写 Surface::set_cursor（平台光标 API）。
    // 判定为类型级、无须创建真实窗口：T 自身声明 set_cursor 时 `&T::set_cursor` 的类型是
    // `void (T::*)(CursorShape)`；仅继承基类默认空实现时是 `void (Surface::*)(CursorShape)`。
    // （不用「成员函数指针比较」——虚函数取址比较结果未指定，见 [expr.eq]。）
    // 后端专属：本仓库默认无头构建下无任何真实后端宏 → 整例 SKIP；真机构建由 static_assert 守门。
#if defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_GLFW) || defined(AURORA_BACKEND_X11) || \
    defined(AURORA_BACKEND_WAYLAND) || defined(AURORA_BACKEND_MACOS) || defined(AURORA_BACKEND_D3D11)
#ifdef AURORA_BACKEND_WIN32
    static_assert(!std::is_same_v<decltype(&Win32Surface::set_cursor), void (Surface::*)(CursorShape)>,
                  "Win32Surface 必须覆写 set_cursor");
#endif
#ifdef AURORA_BACKEND_D3D11
    static_assert(!std::is_same_v<decltype(&D3D11Surface::set_cursor), void (Surface::*)(CursorShape)>,
                  "D3D11Surface 必须覆写 set_cursor（复用 detail::set_win32_cursor）");
#endif
#ifdef AURORA_BACKEND_GLFW
    static_assert(!std::is_same_v<decltype(&GlfwSurface::set_cursor), void (Surface::*)(CursorShape)>,
                  "GlfwSurface 必须覆写 set_cursor");
#endif
#ifdef AURORA_BACKEND_X11
    static_assert(!std::is_same_v<decltype(&X11Surface::set_cursor), void (Surface::*)(CursorShape)>,
                  "X11Surface 必须覆写 set_cursor");
#endif
#ifdef AURORA_BACKEND_WAYLAND
    static_assert(!std::is_same_v<decltype(&WaylandSurface::set_cursor), void (Surface::*)(CursorShape)>,
                  "WaylandSurface 必须覆写 set_cursor");
#endif
#ifdef AURORA_BACKEND_MACOS
    static_assert(!std::is_same_v<decltype(&MacOSSurface::set_cursor), void (Surface::*)(CursorShape)>,
                  "MacOSSurface 必须覆写 set_cursor");
#endif
    AURORA_TEST_CHECK_TRUE(true);
#else
    AURORA_TEST_SKIP("无任何真实窗口后端开启（默认无头构建），后端 set_cursor 覆写契约无法判定");
#endif
}

AURORA_TEST_CASE(windows_family_native_handle_contract) {
    // Win32 家族（GDI 上屏与 D3D11 GPU 上屏）共用同一个 `Win32Window` 宿主，故「原生窗口
    // 句柄」访问器必须两路都覆写：`Surface::native_handle()` 的默认实现恒返回 nullptr，
    // 一旦漏覆写，`aurora::debug::surface_state()`（src/aurora/debug/debug_backend.cpp）的
    // `has_native_window` 就会对**真实窗口后端**误报 false（`D3D11Surface` 曾如此）。
    // 判定为类型级（同 set_cursor 契约用法），无须创建真实窗口。
    // 后端专属：默认无头构建下两个宏皆未定义 → 整例 SKIP；真机构建由 static_assert 守门。
#if defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11)
#ifdef AURORA_BACKEND_WIN32
    static_assert(!std::is_same_v<decltype(&Win32Surface::native_handle), void *(Surface::*)() const>,
                  "Win32Surface 必须覆写 native_handle()（返回宿主 HWND）");
    static_assert(std::is_same_v<decltype(&Win32Surface::hwnd), void *(Win32Surface::*)() const>,
                  "Win32Surface::hwnd() 须为 const 且返回 void*");
#endif
#ifdef AURORA_BACKEND_D3D11
    static_assert(!std::is_same_v<decltype(&D3D11Surface::native_handle), void *(Surface::*)() const>,
                  "D3D11Surface 必须覆写 native_handle()（与 Win32Surface 同宿主）");
    static_assert(std::is_same_v<decltype(&D3D11Surface::hwnd), void *(D3D11Surface::*)() const>,
                  "D3D11Surface::hwnd() 须为 const 且返回 void*");
#endif
    AURORA_TEST_CHECK_TRUE(true);
#else
    AURORA_TEST_SKIP("Win32/D3D11 均未开启（默认无头构建），Win32 家族 native_handle 契约无法判定");
#endif
}

AURORA_TEST_CASE(windows_family_capture_window_contract) {
    // Win32 家族（GDI 上屏与 D3D11 GPU 上屏）共用同一个 `Win32Window` 宿主，须都覆写
    // `Surface::capture_window` 走共享 `detail::capture_window_by_hwnd` 的 PrintWindow 路径；
    // 漏覆写会回落基类默认（unsupported）错误，导致 Ctrl+Shift+S 窗口截图在 D3D11 后端失效。
    // 判定为类型级（同 native_handle 契约），无须创建真实窗口。
    // 后端专属：默认无头构建下两宏未定义 → 整例 SKIP；真机构建由 static_assert 守门。
#if defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11)
#ifdef AURORA_BACKEND_WIN32
    static_assert(
        !std::is_same_v<decltype(&Win32Surface::capture_window), Result<bool> (Surface::*)(const std::string &)>,
        "Win32Surface 必须覆写 capture_window（走 detail::capture_window_by_hwnd）");
#endif
#ifdef AURORA_BACKEND_D3D11
    static_assert(
        !std::is_same_v<decltype(&D3D11Surface::capture_window), Result<bool> (Surface::*)(const std::string &)>,
        "D3D11Surface 必须覆写 capture_window（与 Win32Surface 共用 Win32Window 宿主）");
#endif
    AURORA_TEST_CHECK_TRUE(true);
#else
    AURORA_TEST_SKIP("Win32/D3D11 均未开启（默认无头构建），Win32 家族 capture_window 契约无法判定");
#endif
}

}  // namespace aurora::test_cases::utest_native_surfaces

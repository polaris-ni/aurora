/// 测试类型: unit
/// 目标单元: include/aurora/window/x11_surface.h
/// 测试说明: X11/Xlib 后端类型契约（Surface 派生、final、不可复制/移动/默认构造，#if 分支内
/// static_assert）；真实 X 连接与窗口上屏默认不触碰。头整体被 AURORA_PLATFORM_LINUX &&
/// AURORA_BACKEND_X11 门控，非 Linux / 未开后端时用例恒注册并 SKIP。
///
/// 另含一个**显式选择加入**的真机用例（光标收尾）：置 `AURORA_LIVE_X11=1` 时连接真实
/// X server、创建真实窗口，对 11 个 `CursorShape` 走两轮 `set_cursor`，并以进程级 X 错误
/// 处理器断言服务器未拒绝任何 `XCreateFontCursor`/`XDefineCursor`（不依赖指针 hover，
/// 故在 Wayland/Xwayland 会话下同样可判定）。
///
/// 注：Xlib 头按仓库约定放在文件级 `#if` 内（非 Linux / 未开后端时不能引入 X11 依赖）；
/// 而**用例本身必须无条件注册**（`TEST-R6` / `check_test_registry`：`runner --list` 的用例集
/// 必须与测试源字面量一致），故守卫写在**用例体内**。

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)
#include <type_traits>

#include "aurora/window/cursor_map.h"
#include "aurora/window/x11_surface.h"
#endif

#include "framework/aurora_test.h"

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

// Xlib 头必须在所有 aurora 头之后引入：Xlib 会把 None/Bool/Status/True/False 等通用词定义为
// 宏，先引入会污染 core/enums.h（例如 ModifierKey::None）。本 TU 已在文件开头引入 aurora 头。
#include <X11/Xlib.h>

#include <cstdlib>

// <X11/X.h>（经 Xlib.h 引入）**无条件**定义 `#define CursorShape 0`，与本项目公共类型名
// `aurora::CursorShape` 硬碰撞：不解除时任何 `CursorShape` 记号都被展开为 `0`，报出
// `expected ')' before 'shape'`。src/aurora/window/x11_surface.cpp 有同款处置。
#undef CursorShape

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX

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
    // 默认不触碰真实 X 资源（事件翻译 / XPutImage 上屏依赖真实 X server，属集成层范围）；
    // 需要在真机上验证窗口与光标时，改用 `x11_surface_live_real_window_and_cursor_sweep`。
    AURORA_TEST_SKIP("X11Surface 构造依赖 X server 连接，默认不触碰 OS 资源（真机验证见 AURORA_LIVE_X11=1）");
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_X11 未开启（非 Linux 平台），头文件整体被宏剔除");
#endif
}

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

namespace {

/// 进程级 X 协议错误计数（Xlib 的错误处理器是**进程全局**的，与连接无关）。
/// 借此捕获 aurora 自身 X 连接上的 BadCursor / BadWindow 等错误——无需指针 hover，
/// 在 Wayland/Xwayland 会话下同样可判定「光标资源是否被真实服务器接受」。
int g_x_error_count = 0;
unsigned char g_last_error_code = 0;

auto count_x_error(Display * /*display*/, XErrorEvent *event) -> int {
    ++g_x_error_count;
    g_last_error_code = event->error_code;
    return 0;  // 已处理：不打印、不终止（默认处理器会打印后 exit）
}

}  // namespace

#endif  // AURORA_BACKEND_X11 / AURORA_PLATFORM_LINUX

AURORA_TEST_CASE(x11_surface_live_real_window_and_cursor_sweep) {
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)
    const char *opt_in = std::getenv("AURORA_LIVE_X11");
    if (opt_in == nullptr || *opt_in == '\0') {
        AURORA_TEST_SKIP("需显式置 AURORA_LIVE_X11=1：本用例会连接真实 X server 并创建真实窗口");
    }

    const XErrorHandler previous = XSetErrorHandler(count_x_error);
    g_x_error_count = 0;
    g_last_error_code = 0;

    {
        // 构造即建立真实 X 连接 + 创建真实窗口。
        aurora::X11Surface surface(160, 120, "aurora-live-i1-cursor");
        AURORA_TEST_REQUIRE_TRUE(surface.is_available());

        Display *dpy = XOpenDisplay(nullptr);
        AURORA_TEST_REQUIRE_TRUE(dpy != nullptr);

        // 独立连接也认得该 XID ⇒ 它确实是 X 服务器侧的真实窗口（而非客户端假象）。
        const auto win = static_cast<Window>(reinterpret_cast<std::uintptr_t>(surface.native_handle()));
        AURORA_TEST_CHECK_TRUE(win != 0);
        XWindowAttributes attrs{};
        AURORA_TEST_CHECK_EQ(XGetWindowAttributes(dpy, win, &attrs), 1);
        AURORA_TEST_CHECK_TRUE(attrs.width > 0);
        AURORA_TEST_CHECK_TRUE(attrs.height > 0);

        // 两轮全形状：第 2 轮命中 Impl 的「按形状缓存句柄」分支（首轮才 XCreateFontCursor）。
        for (int round = 0; round < 2; ++round) {
            for (int i = 0; i < static_cast<int>(aurora::AURORA_CURSOR_SHAPE_COUNT); ++i) {
                surface.set_cursor(static_cast<aurora::CursorShape>(i));
            }
        }
        // wait_events 阻塞在 X 连接 fd 上：给服务器处理请求、并让 Xlib 读回异步错误的窗口。
        surface.wait_events(50.0);
        AURORA_TEST_CHECK_EQ(g_x_error_count, 0);

        // 光标下发后窗口仍可用（标题、几何未受影响）。
        AURORA_TEST_CHECK_TRUE(surface.size().width > 0.0F);
        surface.set_title("aurora-live-i1-cursor");
        surface.wait_events(20.0);
        AURORA_TEST_CHECK_EQ(g_x_error_count, 0);

        XCloseDisplay(dpy);
    }

    XSetErrorHandler(previous);
#else
    AURORA_TEST_SKIP("AURORA_BACKEND_X11 未开启（非 Linux 平台），头文件整体被宏剔除");
#endif
}

}  // namespace aurora::test_cases::utest_x11_surface

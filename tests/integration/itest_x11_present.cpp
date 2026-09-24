/// 测试类型: integration
/// 目标单元: include/aurora/window/x11_surface.h
/// 测试说明: X11/Xlib 真实窗口的上屏计数口径——`frame_count()` 逐次 `present()` 走完
///           XPutImage 自增（几何不变的连续帧同样计数），与 `Surface::frame_count()`
///           契约及其余后端口径一致。无 DISPLAY（纯 TTY）或后端未开启时 SKIP
#include <memory>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID) && defined(AURORA_BACKEND_X11)

#include "aurora/render/painter.h"
#include "aurora/window/x11_surface.h"

namespace aurora::test_cases::itest_x11_present {

AURORA_TEST_CASE(frame_count_counts_every_presented_frame) {
    auto surf = std::make_unique<au::X11Surface>(320, 240, "x11 present itest");
    if (!surf->is_available()) {
        // 无 DISPLAY（纯 TTY / 容器）：建窗失败属环境问题，跳过而非失败。
        AURORA_TEST_SKIP("无 DISPLAY 或 X 连接失败");
    }

    const int base = surf->frame_count();
    for (int i = 1; i <= 3; ++i) {
        const auto bf = surf->begin_frame(320, 240);
        AURORA_TEST_CHECK(bf.ok());
        surf->painter().fill_rect(
            au::Rect{.origin = au::Point{.x = 10.0F, .y = 10.0F}, .size = au::Size{.width = 100.0F, .height = 100.0F}},
            au::Color{255, 0, 0, 255});
        const auto pr = surf->present();
        AURORA_TEST_CHECK(pr.ok());
        // 连续三帧几何未变，仍必须逐帧计数（缺陷回归点：Linux 后端曾未覆写而恒 0）。
        AURORA_TEST_CHECK_EQ(surf->frame_count(), base + i);
    }
}

}  // namespace aurora::test_cases::itest_x11_present

#else  // 未开启 X11 后端

namespace aurora::test_cases::itest_x11_present {

AURORA_TEST_CASE(frame_count_counts_every_presented_frame) {
    AURORA_TEST_SKIP("AURORA_BACKEND_X11 未开启（非 Linux 或未编译该后端）");
}

}  // namespace aurora::test_cases::itest_x11_present

#endif  // AURORA_PLATFORM_LINUX && AURORA_BACKEND_X11

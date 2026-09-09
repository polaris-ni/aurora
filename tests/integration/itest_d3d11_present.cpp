/// 测试类型: integration
/// 目标单元: include/aurora/window/d3d11_surface.h
/// 测试说明: D3D11 真实设备帧管线——首帧整帧上传、次帧脏矩形增量上传、
///           device-lost 模拟后 present 报错不崩溃、poll_platform_events 重建恢复、
///           恢复后全量重渲染继续出帧。无适配器/宏未开启时 SKIP

#include <memory>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

namespace au = aurora;

#ifdef AURORA_BACKEND_D3D11

#include "aurora/render/painter.h"
#include "aurora/window/d3d11_surface.h"

namespace aurora::test_cases::itest_d3d11_present {

AURORA_TEST_CASE(present_frames_dirty_upload_and_device_lost_recovery) {
    auto surf = std::make_unique<au::D3D11Surface>(320, 240, "d3d11 present itest", au::WindowStyleOptions{});
    if (!surf->is_available()) {
        // 无 D3D11 适配器（CI / 无头环境）：跳过而非失败。
        AURORA_TEST_SKIP("no D3D11 adapter available in this environment");
    }

    // 首帧：脏矩形空 → 整帧上传。
    const auto bf = surf->begin_frame(320, 240);
    AURORA_TEST_CHECK(bf.ok());
    surf->painter().fill_rect(
        au::Rect{.origin = au::Point{.x = 10.0F, .y = 10.0F}, .size = au::Size{.width = 100.0F, .height = 100.0F}},
        au::Color{255, 0, 0, 255});
    const auto pr = surf->present();
    AURORA_TEST_CHECK(pr.ok());
    AURORA_TEST_CHECK_EQ(surf->frame_count(), 1);

    // 第二帧：局部重绘 → 增量上传路径（脏矩形非空）。
    const auto bf2 = surf->begin_frame(320, 240);
    AURORA_TEST_CHECK(bf2.ok());
    surf->painter().fill_rect(
        au::Rect{.origin = au::Point{.x = 50.0F, .y = 50.0F}, .size = au::Size{.width = 80.0F, .height = 80.0F}},
        au::Color{0, 0, 255, 255});
    const auto pr2 = surf->present();
    AURORA_TEST_CHECK(pr2.ok());
    AURORA_TEST_CHECK_EQ(surf->frame_count(), 2);

    // device-lost 恢复：模拟设备丢失 → present 报错（不崩溃）→
    // 下次 poll_platform_events 在 present_root 外重建 device/swapchain。
    surf->simulate_device_lost();
    AURORA_TEST_CHECK_FALSE(surf->is_available());
    const auto pr_lost = surf->present();
    AURORA_TEST_CHECK_FALSE(pr_lost.ok());
    surf->poll_platform_events();
    AURORA_TEST_CHECK_TRUE(surf->is_available());

    // 恢复后继续正常出帧（全量上传，无残留增量脏区）。
    const auto bf3 = surf->begin_frame(320, 240);
    AURORA_TEST_CHECK(bf3.ok());
    surf->painter().fill_rect(
        au::Rect{.origin = au::Point{.x = 0.0F, .y = 0.0F}, .size = au::Size{.width = 60.0F, .height = 60.0F}},
        au::Color{0, 255, 0, 255});
    const auto pr3 = surf->present();
    AURORA_TEST_CHECK(pr3.ok());
}

}  // namespace aurora::test_cases::itest_d3d11_present

#else  // !AURORA_BACKEND_D3D11

namespace aurora::test_cases::itest_d3d11_present {

AURORA_TEST_CASE(present_frames_dirty_upload_and_device_lost_recovery) {
    AURORA_TEST_SKIP("AURORA_BACKEND_D3D11 未开启（默认 OFF），头文件整体被宏剔除");
}

}  // namespace aurora::test_cases::itest_d3d11_present

#endif  // AURORA_BACKEND_D3D11

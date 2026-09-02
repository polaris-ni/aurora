#include "aurora/core/log.h"

#include "test_harness.h"
// 无可用适配器（如 CI/无头）时跳过，不计入失败。
#ifdef AURORA_BACKEND_D3D11

#include <memory>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/window/d3d11_surface.h"

namespace au = aurora;

AURORA_TEST() {
    auto surf = std::make_unique<au::D3D11Surface>(320, 240, "d3d11 present test", au::WindowStyleOptions{});
    if (!surf->is_available()) {
        // 无 D3D11 适配器：跳过（不计入失败）。
        AURORA_LOG_INFO("test", "skip: no D3D11 adapter available in this environment");
        return;
    }

    // 首帧：整帧上传（脏矩形空）。
    auto bf = surf->begin_frame(320, 240);
    AURORA_TEST_CHECK(bf.ok());
    au::Painter &p = surf->painter();
    p.fill_rect(au::Rect{ .origin = au::Point{ .x = 10.0f, .y = 10.0f },
                          .size = au::Size{ .width = 100.0f, .height = 100.0f } },
                au::Color{ 255, 0, 0, 255 });
    auto pr = surf->present();
    AURORA_TEST_CHECK(pr.ok());
    AURORA_TEST_CHECK(surf->frame_count() == 1);

    // 第二帧：局部重绘 → 增量上传路径（脏矩形非空）。
    auto bf2 = surf->begin_frame(320, 240);
    AURORA_TEST_CHECK(bf2.ok());
    p.fill_rect(
        au::Rect{ .origin = au::Point{ .x = 50.0f, .y = 50.0f }, .size = au::Size{ .width = 80.0f, .height = 80.0f } },
        au::Color{ 0, 0, 255, 255 });
    auto pr2 = surf->present();
    AURORA_TEST_CHECK(pr2.ok());
    AURORA_TEST_CHECK(surf->frame_count() == 2);

    // device-lost 恢复：模拟设备丢失 → present 报错 → 下次 poll 重建后恢复可用。
    surf->simulate_device_lost();
    AURORA_TEST_CHECK_FALSE(surf->is_available());
    auto pr_lost = surf->present();
    AURORA_TEST_CHECK_FALSE(pr_lost.ok()); // 设备不可用：present 报错而非崩溃
    surf->poll_platform_events();          // 恢复时机：present_root 外重建 device/swapchain
    AURORA_TEST_CHECK_TRUE(surf->is_available());
    // 恢复后可继续正常出帧（全量上传，无残留增量脏区）。
    auto bf3 = surf->begin_frame(320, 240);
    AURORA_TEST_CHECK(bf3.ok());
    surf->painter().fill_rect(
        au::Rect{ .origin = au::Point{ .x = 0.0f, .y = 0.0f }, .size = au::Size{ .width = 60.0f, .height = 60.0f } },
        au::Color{ 0, 255, 0, 255 });
    auto pr3 = surf->present();
    AURORA_TEST_CHECK(pr3.ok());

    AURORA_LOG_INFO("test", "d3d11 present OK (frames=", surf->frame_count(), ")");
}

#else // !AURORA_BACKEND_D3D11

AURORA_TEST_SKIP(AURORA_BACKEND_D3D11)

#endif // AURORA_BACKEND_D3D11

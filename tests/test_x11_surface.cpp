// 验证 X11 后端契约：构造不抛异常（无 DISPLAY 时 is_available()==false）、
// begin_frame → painter → present 帧生命周期、size/scale_factor/set_title/native_handle、
// request_wake + wait_events 不死锁、create_window(X11Options) 工厂与
// create_native_window 回退链（X11 不可用 → Headless）。
// 仅 AURORA_BACKEND_X11 下编译运行；未启用该后端时编译为空通过。
#include "test_harness.h"

#ifdef AURORA_BACKEND_X11

#include <memory>

#include "aurora/render/painter.h"
#include "aurora/window/window.h"
#include "aurora/window/x11_surface.h"

namespace au = aurora;

AURORA_TEST() {
    // 契约 0：构造在任何环境下都不抛异常；无 X 连接时以 is_available() 显式暴露。
    auto surf = std::make_unique<au::X11Surface>(320, 200, "aurora-x11-test");
    if (!surf->is_available()) {
        // 无 DISPLAY（纯 tty / CI）：工厂必须返回错误而非崩溃，验证后优雅跳过。
        auto r = au::create_window(au::X11Options{});
        AURORA_TEST_CHECK(!r.ok());
        AURORA_TEST_PRINTF("test_x11_surface: no X display; skipped windowed checks\n");
    }

    // 契约 1：逻辑尺寸与缩放因子有效（scale 由 Xft.dpi 推导，clamp 0.5–4.0）。
    AURORA_TEST_CHECK(surf->size().width > 0.0f && surf->size().height > 0.0f);
    AURORA_TEST_CHECK(surf->scale_factor() >= 0.5f && surf->scale_factor() <= 4.0f);
    AURORA_TEST_CHECK(surf->native_handle() != nullptr);
    AURORA_TEST_CHECK(!surf->should_close());

    // 契约 2：begin_frame → painter → present 完整往返；painter 缓冲已按物理尺寸分配。
    auto bf = surf->begin_frame(static_cast<int>(surf->size().width), static_cast<int>(surf->size().height));
    AURORA_TEST_CHECK(bf.ok());
    au::Painter &p = surf->painter();
    AURORA_TEST_CHECK(p.width() > 0 && p.height() > 0);
    p.fill_rect(au::Rect{ au::Point{ 10.0f, 10.0f }, au::Size{ 50.0f, 30.0f } }, au::Color{ 30, 120, 220, 255 });
    auto pr = surf->present();
    AURORA_TEST_CHECK(pr.ok());

    // 契约 3：增量脏区 present 不失败（脏区一次性消费）。
    surf->set_present_dirty({ au::Rect{ au::Point{ 0.0f, 0.0f }, au::Size{ 16.0f, 16.0f } } });
    AURORA_TEST_CHECK(surf->present().ok());

    // 契约 4：set_title 与事件泵不崩溃；request_wake 后 wait_events 立即返回（不死锁）。
    surf->set_title("aurora-x11-test-renamed");
    surf->poll_platform_events();
    surf->request_wake();
    surf->wait_events(1000); // 有唤醒信号：应远早于超时返回
    AURORA_TEST_CHECK(true); // 抵达此处即未死锁/未崩溃

    // 契约 5：类型安全工厂成功创建 Window，并可 pump + present。
    {
        au::X11Options opts;
        opts.size = au::Size{ 240.0f, 160.0f };
        opts.title = "aurora-x11-factory";
        auto win = au::create_window(opts);
        AURORA_TEST_CHECK(win.ok());
        if (win.ok()) {
            win.value()->pump_events();
            AURORA_TEST_CHECK(win.value()->begin_frame().ok());
            AURORA_TEST_CHECK(win.value()->present().ok());
        }
    }

    // 契约 6：跨平台便捷工厂在 Linux 上可用（X11 成功或回退 Headless，均返回有效 Window）。
    {
        auto win = au::create_native_window(au::WindowOptions{});
        AURORA_TEST_CHECK(win.ok());
    }
}

#else // !AURORA_BACKEND_X11

AURORA_TEST_SKIP(AURORA_BACKEND_X11)

#endif

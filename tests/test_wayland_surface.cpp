// 验证原生 Wayland 后端契约：构造不抛异常（无 WAYLAND_DISPLAY 时 is_available()==false）、
// begin_frame → painter → present 帧生命周期（wl_shm 双缓冲槽）、size/scale_factor/
// set_title/native_handle、request_wake + wait_events 不死锁、create_window(WaylandOptions)
// 工厂与 create_native_window 回退链（Wayland → X11 → Headless）。
// 仅 AURORA_BACKEND_WAYLAND 下编译运行；未启用该后端时编译为空通过。
#include "test_harness.h"

#ifdef AURORA_BACKEND_WAYLAND

#include <memory>

#include "aurora/render/painter.h"
#include "aurora/window/wayland_surface.h"
#include "aurora/window/window.h"

namespace au = aurora;

AURORA_TEST() {
    // 契约 0：构造在任何环境下都不抛异常；无 Wayland 合成器时以 is_available() 显式暴露。
    auto surf = std::make_unique<au::WaylandSurface>(320, 200, "aurora-wayland-test");
    if (!surf->is_available()) {
        // 无 WAYLAND_DISPLAY（纯 tty / X11 会话 / CI）：工厂必须返回错误而非崩溃，验证后优雅跳过。
        auto r = au::create_window(au::WaylandOptions{});
        AURORA_TEST_CHECK(!r.ok());
        AURORA_TEST_PRINTF("test_wayland_surface: no Wayland compositor; skipped windowed checks\n");
    }

    // 契约 1：逻辑尺寸与缩放因子有效（scale 由 wl_output 整数缩放推导）。
    AURORA_TEST_CHECK(surf->size().width > 0.0f && surf->size().height > 0.0f);
    AURORA_TEST_CHECK(surf->scale_factor() >= 1.0f && surf->scale_factor() <= 4.0f);
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

    // 契约 3：连续 present 不失败（双缓冲槽轮换；槽 busy 时 roundtrip 等 release）。
    surf->set_present_dirty({ au::Rect{ au::Point{ 0.0f, 0.0f }, au::Size{ 16.0f, 16.0f } } });
    AURORA_TEST_CHECK(surf->present().ok());
    AURORA_TEST_CHECK(surf->present().ok());

    // 契约 4：set_title 与事件泵不崩溃；request_wake 后 wait_events 立即返回（不死锁）。
    surf->set_title("aurora-wayland-test-renamed");
    surf->poll_platform_events();
    surf->request_wake();
    surf->wait_events(1000); // 有唤醒信号：应远早于超时返回
    AURORA_TEST_CHECK(true); // 抵达此处即未死锁/未崩溃

    // 契约 5：类型安全工厂成功创建 Window，并可 pump + present。
    {
        au::WaylandOptions opts;
        opts.size = au::Size{ 240.0f, 160.0f };
        opts.title = "aurora-wayland-factory";
        auto win = au::create_window(opts);
        AURORA_TEST_CHECK(win.ok());
        if (win.ok()) {
            win.value()->pump_events();
            AURORA_TEST_CHECK(win.value()->begin_frame().ok());
            AURORA_TEST_CHECK(win.value()->present().ok());
        }
    }

    // 契约 6：跨平台便捷工厂在 Linux 上可用（Wayland 成功或回退 X11/Headless，均返回有效 Window）。
    {
        auto win = au::create_native_window(au::WindowOptions{});
        AURORA_TEST_CHECK(win.ok());
    }

    // 契约 7：装饰策略机制（DecorationPolicy）——「无标题栏也能移动/缩放/关闭」。
    // ClientSide 强制自绘标题栏（content_inset.top == 标题栏高）；Borderless 无标题栏但保留边框；
    // Frameless 完全无装饰。三者不依赖 compositor 是否支持 xdg-decoration，故断言确定。
    {
        au::WindowStyleOptions cs;
        cs.decoration = au::DecorationPolicy::ClientSide;
        au::WaylandSurface w_cs(200, 120, "csd-client", cs);
        AURORA_TEST_CHECK(w_cs.is_available());
        AURORA_TEST_CHECK(w_cs.content_inset().top == 36.0f); // 自绘标题栏高度
        AURORA_TEST_CHECK(w_cs.content_inset().left == 0.0f && w_cs.content_inset().bottom == 0.0f);

        au::WindowStyleOptions bl;
        bl.decoration = au::DecorationPolicy::Borderless;
        au::WaylandSurface w_bl(200, 120, "csd-borderless", bl);
        AURORA_TEST_CHECK(w_bl.is_available());
        AURORA_TEST_CHECK(w_bl.content_inset().top == 0.0f);  // 无标题栏
        AURORA_TEST_CHECK(w_bl.content_inset().left == 6.0f); // 保留可缩放边框
        AURORA_TEST_CHECK(w_bl.content_inset().right == 6.0f && w_bl.content_inset().bottom == 6.0f);

        au::WindowStyleOptions fl;
        fl.decoration = au::DecorationPolicy::Frameless;
        au::WaylandSurface w_fl(200, 120, "csd-frameless", fl);
        AURORA_TEST_CHECK(w_fl.is_available());
        auto ins = w_fl.content_inset();
        AURORA_TEST_CHECK(ins.top == 0.0f && ins.left == 0.0f && ins.right == 0.0f && ins.bottom == 0.0f);

        // 程序化 close：置 should_close（等价点 ×）；minimize/maximize/fullscreen 不崩溃。
        w_fl.close();
        AURORA_TEST_CHECK(w_fl.should_close());
        w_cs.minimize();
        w_cs.toggle_maximize();
        w_cs.set_fullscreen(true);
        w_cs.set_fullscreen(false);
        AURORA_TEST_CHECK(true);
    }
}

#else // !AURORA_BACKEND_WAYLAND

AURORA_TEST_SKIP(AURORA_BACKEND_WAYLAND)

#endif

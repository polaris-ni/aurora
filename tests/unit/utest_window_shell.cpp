/// 测试类型: unit
/// 目标单元: include/aurora/app/window_host.h, include/aurora/window/surface.h
/// 测试说明: 窗口外壳能力（specification/06-app-platform.md §2.4）——模态窗口对 owner 的输入屏蔽与
/// 恢复、OS 层 owner 从属关系、z 序提升/激活转发、所在显示器 id 透传、DPI 缩放变化触发整帧重排重绘、
/// 无窗口宿主的显示器迁移安全 no-op。全部以 Headless 多实例驱动（seam 记录调用而非产生副作用）。

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "aurora/app/application.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_shell {

namespace {

auto make_scene(const std::string &label) -> Scene { return Scene{Node{std::make_shared<Text>(label)}}; }

/// @brief 窗口选项：角色 / owner / 模态 / 帧上限（帧上限保证 `run()` 必然收敛）。
auto make_opts(std::string title, WindowRole role = WindowRole::Main, WindowId owner = AURORA_INVALID_WINDOW_ID,
               bool modal = false, int frames = -1) -> WindowOptions {
    WindowOptions o;
    o.title = std::move(title);
    o.size = Size{.width = 320.0F, .height = 240.0F};
    o.role = role;
    o.owner = owner;
    o.modal = modal;
    o.max_frames = frames;
    o.power_saving = false;
    return o;
}

auto make_window(const WindowOptions &o) -> std::unique_ptr<Window> {
    HeadlessOptions h;
    static_cast<WindowOptions &>(h) = o;
    auto res = create_window(h);
    return res ? std::move(res.value()) : nullptr;
}

auto surface_of(WindowHost *host) -> HeadlessSurface * {
    if (host == nullptr || host->window() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<HeadlessSurface *>(&host->window()->surface());
}

}  // namespace

AURORA_TEST_CASE(modal_window_disables_owner_input_until_closed) {
    const auto oa = make_opts("owner", WindowRole::Main, AURORA_INVALID_WINDOW_ID, false, 3);
    Application app{make_scene("owner"), make_window(oa), oa};
    WindowHost *const owner = app.windows().front();
    HeadlessSurface *const owner_surface = surface_of(owner);
    AURORA_TEST_REQUIRE_NOT_NULL(owner_surface);
    AURORA_TEST_CHECK_TRUE(owner_surface->enabled());  // 初始可接收输入

    // 打开模态窗口：owner 输入被屏蔽，且建立 OS 层从属关系（子窗浮于 owner 之上）。
    const auto om = make_opts("modal", WindowRole::Transient, app.main_window(), true);
    const WindowId id_modal = app.open_window(make_window(om), make_scene("modal"), om);
    HeadlessSurface *const modal_surface = surface_of(app.window_host(id_modal));
    AURORA_TEST_REQUIRE_NOT_NULL(modal_surface);
    AURORA_TEST_CHECK_FALSE(owner_surface->enabled());
    AURORA_TEST_CHECK_EQ(modal_surface->owner_surface(), static_cast<const Surface *>(owner_surface));

    // 关闭模态窗口（经帧循环回收）→ owner 输入自动恢复，否则将永久停在禁用态。
    int frames = 0;
    app.set_on_frame([&frames, &app, id_modal]() -> void {
        ++frames;
        if (frames == 1) {
            app.close_window(id_modal);
        }
    });
    app.run();

    AURORA_TEST_CHECK_NULL(app.window_host(id_modal));
    AURORA_TEST_CHECK_TRUE(owner_surface->enabled());
}

AURORA_TEST_CASE(non_modal_window_keeps_owner_enabled) {
    const auto oa = make_opts("owner");
    Application app{make_scene("owner"), make_window(oa), oa};
    HeadlessSurface *const owner_surface = surface_of(app.windows().front());
    AURORA_TEST_REQUIRE_NOT_NULL(owner_surface);

    // 普通辅助窗口：不得屏蔽 owner 输入。
    const auto ob = make_opts("aux", WindowRole::Auxiliary);
    (void)app.open_window(make_window(ob), make_scene("aux"), ob);
    AURORA_TEST_CHECK_TRUE(owner_surface->enabled());

    // 声明了 owner 但**非模态**：同样不屏蔽输入、不建立从属。
    const auto oc = make_opts("child", WindowRole::Transient, app.main_window(), false);
    (void)app.open_window(make_window(oc), make_scene("child"), oc);
    AURORA_TEST_CHECK_TRUE(owner_surface->enabled());
    AURORA_TEST_CHECK_NULL(app.window_host(static_cast<WindowId>(100000)));  // 未知 id 安全
}

AURORA_TEST_CASE(host_forwards_raise_and_focus_to_surface) {
    const auto oa = make_opts("A");
    Application app{make_scene("A"), make_window(oa), oa};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    AURORA_TEST_CHECK_EQ(surface->raise_count(), 0);
    AURORA_TEST_CHECK_EQ(surface->focus_count(), 0);
    host->raise();
    host->raise();
    host->focus_window();
    AURORA_TEST_CHECK_EQ(surface->raise_count(), 2);
    AURORA_TEST_CHECK_EQ(surface->focus_count(), 1);
}

AURORA_TEST_CASE(host_reports_display_id_and_survives_migration_noop) {
    const auto oa = make_opts("A");
    Application app{make_scene("A"), make_window(oa), oa};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    // 无头后端默认未知显示器 → -1；经 seam 设定后透传（与 app::Display::id 同一 id 空间）。
    AURORA_TEST_CHECK_EQ(host->display_id(), -1);
    surface->set_display_id(2);
    AURORA_TEST_CHECK_EQ(host->display_id(), 2);

    // 迁移请求在无头后端是安全 no-op（不崩溃、不改变宿主状态）。
    host->move_to_display(9);
    AURORA_TEST_CHECK_EQ(host->display_id(), 2);
}

AURORA_TEST_CASE(scale_change_marks_window_for_full_redraw) {
    const auto oa = make_opts("A");
    Application app{make_scene("A"), make_window(oa), oa};
    WindowHost *const host = app.windows().front();
    HeadlessSurface *const surface = surface_of(host);
    AURORA_TEST_REQUIRE_NOT_NULL(surface);

    (void)host->render_frame(0.016);  // 先渲染一帧，使脏区状态进入稳定态
    // DPI 变化必须触发整帧重排重绘：否则沿用旧布局缓存/帧缓冲会内容错位或发虚。
    surface->emit_scale_change(2.0F);
    AURORA_TEST_CHECK_TRUE(host->window()->has_pending_dirty());
}

AURORA_TEST_CASE(headless_host_shell_calls_are_safe) {
    Application app{make_scene("headless"), 200, 150};
    WindowHost *const host = app.windows().front();

    // 无 OS 窗口的宿主：显示器 / 层级操作全部安全 no-op（不得崩溃）。
    AURORA_TEST_CHECK_EQ(host->display_id(), -1);
    host->move_to_display(1);
    host->raise();
    host->focus_window();
    AURORA_TEST_CHECK_FALSE(host->has_window());
}

}  // namespace aurora::test_cases::utest_window_shell

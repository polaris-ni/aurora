/// 测试类型: unit
/// 目标单元: include/aurora/app/application.h, include/aurora/window/window.h
/// 测试说明: 窗口角色与退出策略（specification/06-app-platform.md §2.4）——三种 ExitPolicy 的收敛行为、
/// quit() 的强制退出、主窗口切换与访问器跟随、Transient 从属窗口随 owner 连带关闭、关闭回调触发。
/// 全部以 Headless 多实例驱动（无 OS 窗口），不依赖任何真实后端。

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_lifecycle {

namespace {

auto make_scene(const std::string &label) -> Scene { return Scene{Node{std::make_shared<Text>(label)}}; }

/// @brief 窗口选项：角色 / owner / 帧上限。
///
/// ⚠️ `open_window` 不会从 `Window` 反推选项——角色与 owner 必须显式传入，
/// 否则宿主拿到的是默认选项（Main + 无 owner），连带关闭等语义将失效。
auto make_opts(std::string title, WindowRole role = WindowRole::Main, WindowId owner = AURORA_INVALID_WINDOW_ID,
               int frames = -1) -> WindowOptions {
    WindowOptions o;
    o.title = std::move(title);
    o.size = Size{.width = 200.0F, .height = 150.0F};
    o.role = role;
    o.owner = owner;
    o.max_frames = frames;
    o.power_saving = false;  // 无 OS 等待通道：退回忙轮询，帧数确定
    return o;
}

/// @brief 按选项产出一个 Headless 窗口（多实例安全，无 OS 状态）。
auto make_window(const WindowOptions &o) -> std::unique_ptr<Window> {
    HeadlessOptions h;
    static_cast<WindowOptions &>(h) = o;
    auto res = create_window(h);
    return res ? std::move(res.value()) : nullptr;
}

}  // namespace

AURORA_TEST_CASE(default_policy_exits_when_last_window_closes) {
    const auto oa = make_opts("A", WindowRole::Main, AURORA_INVALID_WINDOW_ID, 5);
    Application app{make_scene("A"), make_window(oa), oa};
    AURORA_TEST_CHECK_EQ(app.exit_policy(), ExitPolicy::LastWindowClosed);

    int frames = 0;
    app.set_on_frame([&frames, &app]() -> void {
        ++frames;
        if (frames == 2) {
            app.close_window(app.main_window());  // 唯一的窗口关闭 → 默认策略下即退出
        }
    });
    app.run();

    // 未跑满 max_frames 即退出。
    AURORA_TEST_CHECK_LT(frames, 5);
}

AURORA_TEST_CASE(explicit_only_policy_runs_until_quit) {
    const auto oa = make_opts("A", WindowRole::Main, AURORA_INVALID_WINDOW_ID, 4);
    Application app{make_scene("A"), make_window(oa), oa};
    app.set_exit_policy(ExitPolicy::ExplicitOnly);
    AURORA_TEST_CHECK_EQ(app.exit_policy(), ExitPolicy::ExplicitOnly);

    int frames = 0;
    app.set_on_frame([&frames, &app]() -> void {
        ++frames;
        if (frames == 1) {
            app.close_window(app.main_window());
        }
    });
    app.run();

    // 窗口全关也不退出：跑满 max_frames（常驻型应用语义）。
    AURORA_TEST_CHECK_EQ(frames, 4);
}

AURORA_TEST_CASE(quit_exits_regardless_of_policy) {
    const auto oa = make_opts("A", WindowRole::Main, AURORA_INVALID_WINDOW_ID, 6);
    Application app{make_scene("A"), make_window(oa), oa};
    app.set_exit_policy(ExitPolicy::ExplicitOnly);  // 最严格的策略下 quit() 依然生效

    int frames = 0;
    app.set_on_frame([&frames, &app]() -> void {
        ++frames;
        if (frames == 2) {
            app.quit();
        }
    });
    app.run();

    AURORA_TEST_CHECK_EQ(frames, 2);
}

AURORA_TEST_CASE(main_window_closed_policy_closes_every_window) {
    const auto oa = make_opts("main", WindowRole::Main, AURORA_INVALID_WINDOW_ID, 6);
    Application app{make_scene("main"), make_window(oa), oa};
    app.set_exit_policy(ExitPolicy::MainWindowClosed);

    const auto ob = make_opts("aux", WindowRole::Auxiliary);
    const WindowId id_b = app.open_window(make_window(ob), make_scene("aux"), ob);
    AURORA_TEST_CHECK_EQ(app.window_count(), 2U);

    int frames = 0;
    app.set_on_frame([&frames, &app]() -> void {
        ++frames;
        if (frames == 1) {
            app.close_window(app.main_window());  // 主窗关闭 → 策略要求连带关闭全部
        }
    });
    app.run();

    // 提前退出；辅助窗口被连带关闭（宿主已回收），仅保留主窗口对象以维持访问器有效。
    AURORA_TEST_CHECK_LT(frames, 6);
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_EQ(app.main_window(), app.windows().front()->id());
}

AURORA_TEST_CASE(transient_window_follows_its_owner) {
    const auto oa = make_opts("owner", WindowRole::Main, AURORA_INVALID_WINDOW_ID, 4);
    Application app{make_scene("owner"), make_window(oa), oa};
    const WindowId id_owner = app.main_window();

    // 从属窗口：owner = 主窗口；另开一个无关的辅助窗口作对照。
    const auto ob = make_opts("transient", WindowRole::Transient, id_owner);
    const WindowId id_b = app.open_window(make_window(ob), make_scene("transient"), ob);
    const auto oc = make_opts("aux", WindowRole::Auxiliary);
    const WindowId id_c = app.open_window(make_window(oc), make_scene("aux"), oc);
    AURORA_TEST_REQUIRE_EQ(app.window_count(), 3U);

    int frames = 0;
    app.set_on_frame([&frames, &app, id_owner]() -> void {
        ++frames;
        if (frames == 1) {
            app.close_window(id_owner);
        }
    });
    app.run();

    // owner 关闭 → 从属窗口连带关闭并被回收；无关的辅助窗口继续存活。
    AURORA_TEST_CHECK_NULL(app.window_host(id_owner));
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_NOT_NULL(app.window_host(id_c));
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
    // 仍有窗口存活：默认策略下不退出，跑满 max_frames。
    AURORA_TEST_CHECK_EQ(frames, 4);
}

AURORA_TEST_CASE(on_window_closed_reports_reaped_window) {
    const auto oa = make_opts("A", WindowRole::Main, AURORA_INVALID_WINDOW_ID, 3);
    Application app{make_scene("A"), make_window(oa), oa};
    const auto ob = make_opts("B", WindowRole::Auxiliary);
    const WindowId id_b = app.open_window(make_window(ob), make_scene("B"), ob);

    std::vector<WindowId> closed;
    app.set_on_window_closed([&closed](WindowId id) -> void { closed.push_back(id); });

    int frames = 0;
    app.set_on_frame([&frames, &app, id_b]() -> void {
        ++frames;
        if (frames == 1) {
            app.close_window(id_b);
        }
    });
    app.run();

    // 回调在宿主回收**之后**触发一次，且只报被关闭的那个窗口。
    AURORA_TEST_CHECK_EQ(closed.size(), 1U);
    if (closed.size() == 1U) {
        AURORA_TEST_CHECK_EQ(closed.front(), id_b);
    }
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));
    AURORA_TEST_CHECK_EQ(app.window_count(), 1U);
}

AURORA_TEST_CASE(set_main_window_switches_accessors) {
    const auto oa = make_opts("A");
    Application app{make_scene("A"), make_window(oa), oa};
    const auto ob = make_opts("B", WindowRole::Auxiliary);
    const WindowId id_b = app.open_window(make_window(ob), make_scene("B"), ob);

    WindowHost *const a = app.windows().front();
    WindowHost *const b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(b);
    AURORA_TEST_CHECK_EQ(&app.scene(), &a->scene());

    // 切换主窗口：访问器随之改指向；未知 id 为 no-op。
    app.set_main_window(id_b);
    AURORA_TEST_CHECK_EQ(app.main_window(), id_b);
    AURORA_TEST_CHECK_EQ(&app.scene(), &b->scene());
    AURORA_TEST_CHECK_EQ(&app.focus(), &b->focus());
    app.set_main_window(static_cast<WindowId>(31337));
    AURORA_TEST_CHECK_EQ(app.main_window(), id_b);
}

}  // namespace aurora::test_cases::utest_window_lifecycle

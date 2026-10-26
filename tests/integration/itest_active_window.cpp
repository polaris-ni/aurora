/// @file itest_active_window.cpp
/// 测试类型: integration
/// 目标单元: include/aurora/app/application.h（`Application::active_window()`）
/// 测试说明: 两扇无头窗注册一条 Global 快捷键，逐窗派发断言动作落点随来源移动；
///           关掉一扇后另一扇仍拿得到自己那一格（回收清号后回落存活宿主）。
///           确定性由 power_saving=false + max_frames 固定帧预算保证，不依赖 wall-clock。

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/widget/text.h"
#include "aurora/window/surface.h"
#include "aurora/window/window.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_active_window {

namespace {

auto make_scene(const std::string &label) -> Scene { return Scene{Node{std::make_shared<Text>(label)}}; }

/// @brief 产出一枚无头窗口（多实例安全，与 utest_multi_window 同款底座）。
auto make_window(std::string title, int frames = -1) -> std::unique_ptr<Window> {
    HeadlessOptions opts;
    static_cast<WindowOptions &>(opts) = WindowOptions{};
    opts.title = std::move(title);
    opts.size = Size{.width = 200.0F, .height = 150.0F};
    opts.max_frames = frames;
    auto res = create_window(opts);
    return res ? std::move(res.value()) : nullptr;
}

/// @brief 构造一枚 Ctrl+O 按下事件（快捷键命中 + 属记账范围的输入意图类事件）。
auto make_ctrl_o_down() -> KeyEvent {
    KeyEvent e;
    e.key = static_cast<int>(KeyCode::O);
    e.action = KeyAction::Down;
    e.modifiers = ModifierKey::Control;
    return e;
}

}  // namespace

AURORA_TEST_CASE(global_shortcut_action_follows_dispatch_source) {
    WindowOptions opts;
    opts.max_frames = 2;
    opts.power_saving = false;

    auto wa = make_window("A");
    AURORA_TEST_REQUIRE_NOT_NULL(wa.get());
    Application app{make_scene("A"), std::move(wa), opts};
    auto wb = make_window("B");
    AURORA_TEST_REQUIRE_NOT_NULL(wb.get());
    const WindowId id_b = app.open_window(std::move(wb), make_scene("B"));
    const WindowId id_a = app.main_window();
    AURORA_TEST_CHECK_NE(id_a, id_b);

    auto *host_a = app.window_host(id_a);
    auto *host_b = app.window_host(id_b);
    AURORA_TEST_REQUIRE_NOT_NULL(host_a);
    AURORA_TEST_REQUIRE_NOT_NULL(host_b);

    // 一条 Global 快捷键：闭包内读 `active_window()` 记录「本次派发落在哪一扇」。
    std::vector<WindowId> landings;
    app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O},
                        [&app, &landings]() -> void { landings.push_back(app.active_window()); });
    KeyEvent e = make_ctrl_o_down();

    // 逐窗派发：动作落点随来源移动（记账早于匹配，闭包读到的是本次来源）。
    host_a->dispatch(e);
    host_b->dispatch(e);
    AURORA_TEST_REQUIRE_EQ(landings.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(landings[0], id_a);
    AURORA_TEST_CHECK_EQ(landings[1], id_b);

    // 关掉 B：回收清号 → 回落存活宿主 A，另一扇仍拿得到自己那一格。
    app.close_window(id_b);
    app.run();
    AURORA_TEST_CHECK_NULL(app.window_host(id_b));

    landings.clear();
    host_a->dispatch(e);
    AURORA_TEST_REQUIRE_EQ(landings.size(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(landings[0], id_a);
    AURORA_TEST_CHECK_EQ(app.active_window(), id_a);
}

}  // namespace aurora::test_cases::itest_active_window

/// 测试类型: unit
/// 目标单元: include/aurora/app/application.h
/// 测试说明: 覆盖 Application 无头构造的默认状态（无 Window、严格模式、窗口可见性/几何态
/// 响应式初值）、子系统句柄（scene/focus/scheduler/shortcuts）、快捷键优先派发（消费/落空/禁用）、
/// 空场景同步派发安全性与 App 流式构建器链式配置（不进入真实帧循环）

#include <memory>
#include <string>
#include <vector>

#include "aurora/app/application.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_application {

namespace {

auto make_scene() -> Scene { return Scene{Node{std::make_shared<Text>("hi")}}; }

}  // namespace

AURORA_TEST_CASE(headless_application_defaults) {
    Application app{make_scene(), 320, 240};

    // 无头构造：不持有后端 Window。
    AURORA_TEST_CHECK_NULL(app.window());
    // 严格模式默认 Off；窗口可见性/几何态响应式初值为 Visible / Normal。
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::Off);
    AURORA_TEST_CHECK_EQ(app.window_state().get(), WindowState::Visible);
    AURORA_TEST_CHECK_EQ(app.window_mode().get(), WindowMode::Normal);

    // 命令式窗口状态回调可注册（无 Window 时不会触发）。
    bool state_cb_called = false;
    bool mode_cb_called = false;
    app.set_on_window_state([&state_cb_called](WindowState) -> void { state_cb_called = true; });
    app.set_on_window_mode([&mode_cb_called](WindowMode) -> void { mode_cb_called = true; });
    AURORA_TEST_CHECK_FALSE(state_cb_called);
    AURORA_TEST_CHECK_FALSE(mode_cb_called);

    // set_on_frame / set_overlay 仅注册/转发，无头路径不触发。
    app.set_on_frame([]() -> void {});
    app.set_overlay(nullptr);
    AURORA_TEST_CHECK_NULL(app.window());
}

AURORA_TEST_CASE(application_exposes_subsystem_handles) {
    Application app{make_scene(), 100, 80};

    // 场景根为构造时注入的 widget。
    AURORA_TEST_CHECK_EQ(std::string{app.scene().root().type_name()}, "Text");
    // 空场景下焦点管理器无焦点。
    AURORA_TEST_CHECK_NULL(app.focus().focused());
    // 快捷键注册表初始为空。
    AURORA_TEST_CHECK_EQ(app.shortcuts().count(), 0U);
    // 子系统句柄可取引用且稳定。
    AURORA_TEST_CHECK_EQ(&app.scheduler(), &app.scheduler());
    AURORA_TEST_CHECK_EQ(&app.shortcuts(), &app.shortcuts());
}

AURORA_TEST_CASE(strict_mode_roundtrip) {
    Application app{make_scene(), 100, 80};
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::Off);

    app.set_strict_mode(StrictMode::On);
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::On);

    app.set_strict_mode(StrictMode::Off);
    AURORA_TEST_CHECK_EQ(app.strict_mode(), StrictMode::Off);
}

AURORA_TEST_CASE(dispatch_key_consumed_by_registered_shortcut) {
    Application app{make_scene(), 320, 240};
    int fired = 0;
    app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O}, [&fired]() -> void { ++fired; });

    KeyEvent e;
    e.key = static_cast<int>(KeyCode::O);
    e.action = KeyAction::Down;
    e.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_TRUE(app.dispatch_key(e));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(dispatch_key_falls_through_when_unmatched) {
    Application app{make_scene(), 320, 240};
    int fired = 0;
    app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::O}, [&fired]() -> void { ++fired; });

    // 无修饰键的同键不匹配 → 快捷键不消费；空场景无人处理 → 返回 false。
    KeyEvent plain;
    plain.key = static_cast<int>(KeyCode::O);
    plain.action = KeyAction::Down;
    plain.modifiers = ModifierKey::None;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(plain));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 匹配组合但为抬起事件 → 不触发快捷键。
    KeyEvent up;
    up.key = static_cast<int>(KeyCode::O);
    up.action = KeyAction::Up;
    up.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(up));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 注册表外的键同样落空。
    KeyEvent other;
    other.key = static_cast<int>(KeyCode::S);
    other.action = KeyAction::Down;
    other.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(other));
    AURORA_TEST_CHECK_EQ(fired, 0);
}

AURORA_TEST_CASE(disabled_shortcut_not_consumed) {
    Application app{make_scene(), 320, 240};
    int fired = 0;
    const int id = app.shortcuts().add(KeyCombo{ModifierKey::Control, KeyCode::S}, [&fired]() -> void { ++fired; });

    app.shortcuts().set_enabled(id, false);
    KeyEvent e;
    e.key = static_cast<int>(KeyCode::S);
    e.action = KeyAction::Down;
    e.modifiers = ModifierKey::Control;
    AURORA_TEST_CHECK_FALSE(app.dispatch_key(e));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 重新启用后恢复消费。
    app.shortcuts().set_enabled(id, true);
    AURORA_TEST_CHECK_TRUE(app.dispatch_key(e));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(plain_scene_dispatches_are_safe_noops) {
    Application app{make_scene(), 320, 240};

    // 空场景（纯 Text 叶根）上的同步派发不命中任何控件，应安全无害。
    AURORA_TEST_CHECK_NO_THROW(app.dispatch_click(5.0F, 5.0F));
    AURORA_TEST_CHECK_NO_THROW(app.dispatch_pointer(6.0F, 6.0F, MouseAction::Move));
    AURORA_TEST_CHECK_NO_THROW(app.tick());
    // TEST_TEMP_EXEMPT: 模拟拖入的假路径字符串，非真实临时目录。
    const std::vector<std::string> dropped_paths{"C:/tmp/a.txt"};
    AURORA_TEST_CHECK_NO_THROW(app.dispatch_file_drop(dropped_paths, 1.0F, 2.0F));

    // 文本输入无人处理 → 返回 false。
    TextInputEvent text;
    text.text = "x";
    AURORA_TEST_CHECK_FALSE(app.dispatch_text(text));
}

AURORA_TEST_CASE(app_builder_chains_fluently) {
    // 流式构建器：各 setter 返回同一实例引用（可链式）。
    // 注：命名空间里自由函数 App() 会隐藏类名 App，类型语境用 auto 绕开。
    auto builder = aurora::App::make();
    auto& chained =
        builder.title("utest").size(320, 240).frames(1).on_frame([]() -> void {}).strict_mode(StrictMode::Off);
    AURORA_TEST_CHECK_EQ(&chained, &builder);

    // 文档形态的自由函数工厂 `au::App()` 可用。
    auto factory = aurora::App();
    AURORA_TEST_CHECK_NO_THROW(factory.title("factory"));

    // 显式 Node 构造与 view() 替换（仅配置，不进入帧循环）；elaborated 类型名绕开函数隐藏。
    class aurora::App with_view{Node{std::make_shared<Text>("root")}};
    AURORA_TEST_CHECK_NO_THROW(with_view.view(Node{std::make_shared<Text>("replaced")}));
}

}  // namespace aurora::test_cases::utest_application

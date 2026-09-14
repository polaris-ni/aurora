/// 测试类型: unit
/// 目标单元: include/aurora/commands.h（bind_shortcuts / to_menu_items 三源投影）
/// 测试说明: 覆盖命令默认快捷键接入 ShortcutRegistry（命中触发 invoke、无绑定不产生绑定、
/// 重复绑定幂等、解绑与清空连带撤销绑定、启用条件拦住已消费的快捷键），
/// 以及 to_menu_items 的字段投影与点击出口

#include <memory>
#include <string>
#include <utility>

#include "aurora/app/application.h"
#include "aurora/app/scene.h"
#include "aurora/commands.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_commands_shortcut {

namespace {

auto make_command(const std::string &id, const std::string &title) -> Command {
    Command cmd;
    cmd.id = id;
    cmd.title = title;
    return cmd;
}

auto key_event(KeyCode code, ModifierKey mods = ModifierKey::None) -> KeyEvent {
    KeyEvent e;
    e.action = KeyAction::Down;
    e.key = static_cast<int>(code);
    e.modifiers = mods;
    return e;
}

}  // namespace

AURORA_TEST_CASE(shortcut_binding_consumes_key_and_invokes_command) {
    CommandRegistry reg;
    int calls = 0;
    Command open = make_command("file.open", "Open");
    open.default_binding = KeyCombo{ModifierKey::Control, KeyCode::O};
    open.action = [&calls]() -> void { ++calls; };
    reg.add(std::move(open));
    reg.add(make_command("plain", "Plain"));  // 无默认快捷键

    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{1});
    AURORA_TEST_CHECK(reg.shortcuts() == &shortcuts);

    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::O, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(calls, 1);

    // 未绑定键不消费、不执行。
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::P, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(shortcut_binding_is_idempotent) {
    CommandRegistry reg;
    Command a = make_command("a", "A");
    a.default_binding = KeyCombo{KeyCode::A};
    reg.add(std::move(a));

    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{1});
    reg.bind_shortcuts(shortcuts);  // 重复绑定：先撤旧再重建
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{1});
}

AURORA_TEST_CASE(shortcut_binding_survives_but_invoke_respects_enabled_state) {
    CommandRegistry reg;
    int calls = 0;
    Command a = make_command("a", "A");
    a.default_binding = KeyCombo{KeyCode::A};
    a.action = [&calls]() -> void { ++calls; };
    reg.add(std::move(a));

    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);
    reg.set_enabled("a", false);

    // 快捷键绑定仍存在并消费事件，但调用被启用条件拦住。
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::A)));
    AURORA_TEST_CHECK_EQ(calls, 0);
}

AURORA_TEST_CASE(remove_and_clear_revoke_bound_shortcuts) {
    CommandRegistry reg;
    Command a = make_command("a", "A");
    a.default_binding = KeyCombo{KeyCode::A};
    int calls = 0;
    a.action = [&calls]() -> void { ++calls; };
    reg.add(std::move(a));
    Command b = make_command("b", "B");
    b.default_binding = KeyCombo{KeyCode::B};
    reg.add(std::move(b));

    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{2});

    // 解绑单条命令：连带撤销其快捷键绑定。
    AURORA_TEST_CHECK_TRUE(reg.remove("a"));
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::A)));
    AURORA_TEST_CHECK_EQ(calls, 0);

    // 清空注册表：连带撤销全部绑定。
    reg.clear();
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{0});
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::B)));
}

AURORA_TEST_CASE(menu_items_project_command_fields_and_share_invoke_exit) {
    CommandRegistry reg;
    Command open = make_command("file.open", "Open");
    open.icon = "folder";
    open.default_binding = KeyCombo{ModifierKey::Control, KeyCode::O};
    int calls = 0;
    open.action = [&calls]() -> void { ++calls; };
    reg.add(std::move(open));

    Command gated = make_command("gated", "Gated");
    gated.enabled = []() -> bool { return false; };
    reg.add(std::move(gated));

    const auto items = reg.to_menu_items();
    AURORA_TEST_CHECK_EQ(items.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(items[0].label, std::string{"Open"});
    AURORA_TEST_CHECK_EQ(items[0].icon, std::string{"folder"});
    AURORA_TEST_CHECK_EQ(items[0].shortcut_text, (KeyCombo{ModifierKey::Control, KeyCode::O}).to_string());
    AURORA_TEST_CHECK_TRUE(items[0].enabled);
    AURORA_TEST_CHECK_EQ(items[1].label, std::string{"Gated"});
    AURORA_TEST_CHECK_FALSE(items[1].enabled);
    AURORA_TEST_CHECK_EQ(items[1].shortcut_text, std::string{});

    // 菜单项点击与快捷键收敛到同一 invoke 出口。
    items[0].on_click();
    AURORA_TEST_CHECK_EQ(calls, 1);
}

AURORA_TEST_CASE(application_hosts_command_registry) {
    Scene scene{Node{std::make_shared<Text>("host")}};
    Application app{std::move(scene), 320, 240};

    int calls = 0;
    Command ping = make_command("app.ping", "Ping");
    ping.default_binding = KeyCombo{ModifierKey::Alt, KeyCode::P};
    ping.action = [&calls]() -> void { ++calls; };
    app.commands().add(std::move(ping));
    app.commands().bind_shortcuts(app.shortcuts());

    AURORA_TEST_CHECK_EQ(app.commands().count(), std::size_t{1});
    AURORA_TEST_CHECK_EQ(app.shortcuts().count(), std::size_t{1});
    AURORA_TEST_CHECK_TRUE(app.shortcuts().handle(key_event(KeyCode::P, ModifierKey::Alt)));
    AURORA_TEST_CHECK_EQ(calls, 1);
}

}  // namespace aurora::test_cases::utest_commands_shortcut

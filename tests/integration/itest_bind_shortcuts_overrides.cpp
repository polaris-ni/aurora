/// 测试类型: integration
/// 目标单元: include/aurora/commands.h
/// 测试说明: 宿主启动期的快捷键覆盖表重放——「command id → 组合键文本」先经 `KeyCombo::from` 解析、
/// 再由 `set_binding` 回灌注册表、最后 `bind_shortcuts` 投射进 `ShortcutRegistry`；断派发语义按覆盖表
/// 生效（被覆盖者换键生效且旧键失效、未覆盖者仍走 default_binding），且畸形文本与未注册 id
/// 只跳过本条、不牵连其余条目、不影响启动。

#include <map>
#include <string>

#include "aurora/app/shortcuts.h"
#include "aurora/commands.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_bind_shortcuts_overrides {

namespace {

auto make_command(const std::string &id, const std::string &title, KeyCombo combo) -> Command {
    Command cmd;
    cmd.id = id;
    cmd.title = title;
    cmd.default_binding = combo;
    return cmd;
}

auto key_event(KeyCode code, ModifierKey mods = ModifierKey::None) -> KeyEvent {
    KeyEvent e;
    e.action = KeyAction::Down;
    e.key = static_cast<int>(code);
    e.modifiers = mods;
    return e;
}

/// @brief 装配层的重放循环：解析失败与未注册 id 各记一条（此处计 `skipped`），其余照旧。
auto replay_overrides(CommandRegistry &reg, const std::map<std::string, std::string> &overrides) -> int {
    int skipped = 0;
    for (const auto &kv : overrides) {
        const auto parsed = KeyCombo::from(kv.second);
        if (!parsed.has_value()) {
            ++skipped;
            continue;
        }
        if (!reg.set_binding(kv.first, *parsed)) {
            ++skipped;
        }
    }
    return skipped;
}

}  // namespace

AURORA_TEST_CASE(startup_replay_applies_overrides_and_skips_only_bad_entries) {
    CommandRegistry reg;
    int opened = 0;
    int found = 0;
    int zoomed = 0;
    Command open = make_command("file.open", "Open", KeyCombo{ModifierKey::Control, KeyCode::O});
    open.action = [&opened]() -> void { ++opened; };
    Command find = make_command("edit.find", "Find", KeyCombo{ModifierKey::Control, KeyCode::F});
    find.action = [&found]() -> void { ++found; };
    Command zoom = make_command("view.zoom", "Zoom", KeyCombo{ModifierKey::Control, KeyCode::Equal});
    zoom.action = [&zoomed]() -> void { ++zoomed; };
    reg.add(std::move(open));
    reg.add(std::move(find));
    reg.add(std::move(zoom));

    const std::map<std::string, std::string> overrides = {
        {"file.open", "Ctrl+Alt+O"},  // 合法覆盖
        {"edit.find", "Shift+F5"},  // 合法覆盖
        {"view.zoom", "Ctrl+"},  // 畸形文本：只该条被跳过
        {"app.quit", "F5"},  // 未注册 id：只该条被跳过
    };
    AURORA_TEST_CHECK_EQ(replay_overrides(reg, overrides), 2);

    ShortcutRegistry shortcuts;
    reg.bind_shortcuts(shortcuts);
    AURORA_TEST_CHECK_EQ(shortcuts.count(), std::size_t{3});  // 三条命令各一条，未注册 id 不产生绑定

    // 被覆盖的两条：新键生效、旧键失效。
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Alt)));
    AURORA_TEST_CHECK_EQ(opened, 1);
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::O, ModifierKey::Control)));

    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::F5, ModifierKey::Shift)));
    AURORA_TEST_CHECK_EQ(found, 1);
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::F, ModifierKey::Control)));

    // 畸形覆盖条目不牵连其余：该命令仍走自己的 default_binding。
    AURORA_TEST_CHECK_TRUE(shortcuts.handle(key_event(KeyCode::Equal, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(zoomed, 1);

    // 未注册 id 的覆盖不会凭空产生绑定。
    AURORA_TEST_CHECK_FALSE(shortcuts.handle(key_event(KeyCode::F5)));
    AURORA_TEST_CHECK_EQ(reg.count(), std::size_t{3});
}

}  // namespace aurora::test_cases::itest_bind_shortcuts_overrides

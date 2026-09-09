/// 测试类型: unit
/// 目标单元: include/aurora/app/shortcuts.h
/// 测试说明: 覆盖 KeyCombo 匹配语义（Down+键码+修饰键完全一致）与 to_string 格式、
/// ShortcutRegistry 的注册/ID 分配/枚举/解绑/启停、作用域（Global/Focus）与焦点标记、
/// 冲突时先注册先赢、空动作绑定的消费语义

#include <string>

#include "aurora/app/shortcuts.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_shortcuts {

namespace {

auto key_event(KeyCode k, ModifierKey mods = ModifierKey::None, KeyAction action = KeyAction::Down) -> KeyEvent {
    KeyEvent e;
    e.key = static_cast<int>(k);
    e.action = action;
    e.modifiers = mods;
    return e;
}

}  // namespace

AURORA_TEST_CASE(key_combo_matches_exact_down_event) {
    const KeyCombo combo{ModifierKey::Control, KeyCode::O};

    // 精确匹配：按下 + 同键码 + 修饰键一致。
    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::O, ModifierKey::Control)));
    // 抬起事件不匹配。
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::O, ModifierKey::Control, KeyAction::Up)));
    // 键码不同不匹配。
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::S, ModifierKey::Control)));
    // 事件多带 Shift 不匹配（修饰键须完全一致）。
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift)));
    // 组合带修饰键而事件没有，同样不匹配。
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::O)));
}

AURORA_TEST_CASE(key_combo_matches_without_modifiers) {
    const KeyCombo combo{KeyCode::F5};

    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::F5)));
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::F5, ModifierKey::Shift)));
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::F4)));
}

AURORA_TEST_CASE(key_combo_to_string_orders_modifiers) {
    // lhs 含花括号逗号需加括号包裹（CHECK_EQ(lhs, ...) 首逗号切参）。
    AURORA_TEST_CHECK_EQ((KeyCombo{ModifierKey::Control, KeyCode::O}.to_string()), std::string{"Ctrl+O"});
    AURORA_TEST_CHECK_EQ((KeyCombo{ModifierKey::Control | ModifierKey::Shift, KeyCode::S}.to_string()),
                         std::string{"Ctrl+Shift+S"});
    AURORA_TEST_CHECK_EQ((KeyCombo{ModifierKey::Alt | ModifierKey::Meta, KeyCode::X}.to_string()),
                         std::string{"Alt+Meta+X"});
    // 无修饰键：仅键名。
    AURORA_TEST_CHECK_EQ((KeyCombo{KeyCode::A}.to_string()), std::string{"A"});
    AURORA_TEST_CHECK_EQ((KeyCombo{ModifierKey::None, KeyCode::F5}.to_string()), std::string{"F5"});
    AURORA_TEST_CHECK_EQ((KeyCombo{KeyCode::D1}.to_string()), std::string{"1"});
}

AURORA_TEST_CASE(registry_add_assigns_ids_and_enumerates) {
    ShortcutRegistry reg;
    AURORA_TEST_CHECK_EQ(reg.count(), 0U);

    const int id1 = reg.add(KeyCombo{ModifierKey::Control, KeyCode::O}, [] {}, ShortcutScope::Global, "打开");
    const int id2 = reg.add(KeyCombo{ModifierKey::Control, KeyCode::S}, [] {});
    // ID 自 1 起单调递增。
    AURORA_TEST_CHECK_EQ(id1, 1);
    AURORA_TEST_CHECK_EQ(id2, 2);
    AURORA_TEST_CHECK_EQ(reg.count(), 2U);

    // bindings() 枚举全部绑定（含描述元数据）。
    const auto bindings = reg.bindings();
    AURORA_TEST_REQUIRE_EQ(bindings.size(), 2U);
    AURORA_TEST_CHECK_EQ(bindings[0].description, std::string{"打开"});
    AURORA_TEST_CHECK_EQ(bindings[0].scope, ShortcutScope::Global);
    AURORA_TEST_CHECK_TRUE(bindings[0].enabled);
    AURORA_TEST_CHECK_EQ(bindings[1].combo.key, KeyCode::S);
    AURORA_TEST_CHECK_TRUE(bindings[1].description.empty());

    reg.clear();
    AURORA_TEST_CHECK_EQ(reg.count(), 0U);
    AURORA_TEST_CHECK_TRUE(reg.bindings().empty());
}

AURORA_TEST_CASE(handle_runs_action_and_consumes_event) {
    ShortcutRegistry reg;
    int fired = 0;
    reg.add(KeyCombo{ModifierKey::Control, KeyCode::O}, [&fired] { ++fired; });

    // 命中：执行动作并消费事件。
    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::O, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(fired, 1);

    // 未命中：返回 false，动作不执行。
    AURORA_TEST_CHECK_FALSE(reg.handle(key_event(KeyCode::O)));
    AURORA_TEST_CHECK_FALSE(reg.handle(key_event(KeyCode::S, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(disabled_binding_is_skipped) {
    ShortcutRegistry reg;
    int fired = 0;
    const int id = reg.add(KeyCombo{ModifierKey::Control, KeyCode::P}, [&fired] { ++fired; });

    reg.set_enabled(id, false);
    AURORA_TEST_CHECK_FALSE(reg.handle(key_event(KeyCode::P, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(fired, 0);

    // 重新启用后恢复触发。
    reg.set_enabled(id, true);
    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::P, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(remove_unbinds_and_unknown_id_is_noop) {
    ShortcutRegistry reg;
    int fired = 0;
    const int id = reg.add(KeyCombo{ModifierKey::Control, KeyCode::N}, [&fired] { ++fired; });

    // 未知 ID 解绑为 no-op。
    reg.remove(999);
    AURORA_TEST_CHECK_EQ(reg.count(), 1U);

    reg.remove(id);
    AURORA_TEST_CHECK_EQ(reg.count(), 0U);
    AURORA_TEST_CHECK_FALSE(reg.handle(key_event(KeyCode::N, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(fired, 0);
}

AURORA_TEST_CASE(focus_scope_requires_focused_widget) {
    ShortcutRegistry reg;
    int fired = 0;
    reg.add(KeyCombo{ModifierKey::None, KeyCode::F1}, [&fired] { ++fired; }, ShortcutScope::Focus);

    // Focus 作用域：无焦点控件时不触发。
    AURORA_TEST_CHECK_FALSE(reg.handle(key_event(KeyCode::F1), false));
    AURORA_TEST_CHECK_EQ(fired, 0);
    // 有焦点控件时触发。
    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::F1), true));
    AURORA_TEST_CHECK_EQ(fired, 1);

    // Global 作用域与焦点标记无关。
    int global_fired = 0;
    reg.add(KeyCombo{ModifierKey::None, KeyCode::F2}, [&global_fired] { ++global_fired; }, ShortcutScope::Global);
    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::F2), false));
    AURORA_TEST_CHECK_EQ(global_fired, 1);
}

AURORA_TEST_CASE(first_matching_binding_wins_on_conflict) {
    // 冲突（同键组合注册两次）：先注册者触发并消费，后者不再执行。
    ShortcutRegistry reg;
    int fired_a = 0;
    int fired_b = 0;
    reg.add(KeyCombo{ModifierKey::Control, KeyCode::K}, [&fired_a] { ++fired_a; });
    reg.add(KeyCombo{ModifierKey::Control, KeyCode::K}, [&fired_b] { ++fired_b; });

    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::K, ModifierKey::Control)));
    AURORA_TEST_CHECK_EQ(fired_a, 1);
    AURORA_TEST_CHECK_EQ(fired_b, 0);
}

AURORA_TEST_CASE(binding_without_action_still_consumes) {
    // 动作为空的绑定：命中即消费事件（返回 true），不执行任何动作。
    ShortcutRegistry reg;
    reg.add(KeyCombo{ModifierKey::Alt, KeyCode::Enter}, nullptr);

    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::Enter, ModifierKey::Alt)));
}

}  // namespace aurora::test_cases::utest_shortcuts

/// 测试类型: unit
/// 目标单元: include/aurora/app/shortcuts.h
/// 测试说明: 覆盖 KeyCombo 匹配语义（Down+键码+**可按住的**修饰位完全一致）与 to_string 格式、
/// ShortcutRegistry 的注册/ID 分配/枚举/解绑/启停、作用域（Global/Focus）与焦点标记、
/// 冲突时先注册先赢、空动作绑定的消费语义
///           另覆盖锁定态修饰位（NumLock）与匹配的关系：事件侧多带未登记的锁定位仍匹配、
///           锁定位不把「相等」变成「包含」、注册侧锁定位被忽略、两个位掩码常量对
///           ModifierKey 全部已定义位的无余划分（新增锁定位时该用例要求同步）

#include <cstdint>
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
    // CHECK_EQ 在首个顶层逗号处切参，而预处理器的配对只认圆括号、不认花括号；
    // 含「花括号 + 逗号」的表达式若加圆括号保护，又会被 clang-format 判为冗余括号剥掉，
    // 故先落 KeyCombo 局部量再断言（改回内联写法会在下次格式化后编译失败）。
    const KeyCombo ctrl_o{ModifierKey::Control, KeyCode::O};
    const KeyCombo ctrl_shift_s{ModifierKey::Control | ModifierKey::Shift, KeyCode::S};
    const KeyCombo alt_meta_x{ModifierKey::Alt | ModifierKey::Meta, KeyCode::X};
    const KeyCombo none_f5{ModifierKey::None, KeyCode::F5};
    AURORA_TEST_CHECK_EQ(ctrl_o.to_string(), std::string{"Ctrl+O"});
    AURORA_TEST_CHECK_EQ(ctrl_shift_s.to_string(), std::string{"Ctrl+Shift+S"});
    AURORA_TEST_CHECK_EQ(alt_meta_x.to_string(), std::string{"Alt+Meta+X"});
    // 无修饰键：仅键名。
    AURORA_TEST_CHECK_EQ(KeyCombo{KeyCode::A}.to_string(), std::string{"A"});
    AURORA_TEST_CHECK_EQ(none_f5.to_string(), std::string{"F5"});
    AURORA_TEST_CHECK_EQ(KeyCombo{KeyCode::D1}.to_string(), std::string{"1"});
}

AURORA_TEST_CASE(registry_add_assigns_ids_and_enumerates) {
    ShortcutRegistry reg;
    AURORA_TEST_CHECK_EQ(reg.count(), 0U);

    const int id1 = reg.add(KeyCombo{ModifierKey::Control, KeyCode::O}, []() -> void {}, ShortcutScope::Global, "Open");
    const int id2 = reg.add(KeyCombo{ModifierKey::Control, KeyCode::S}, []() -> void {});
    // ID 自 1 起单调递增。
    AURORA_TEST_CHECK_EQ(id1, 1);
    AURORA_TEST_CHECK_EQ(id2, 2);
    AURORA_TEST_CHECK_EQ(reg.count(), 2U);

    // bindings() 枚举全部绑定（含描述元数据）。
    const auto bindings = reg.bindings();
    AURORA_TEST_REQUIRE_EQ(bindings.size(), 2U);
    AURORA_TEST_CHECK_EQ(bindings[0].description, std::string{"Open"});
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
    reg.add(KeyCombo{ModifierKey::Control, KeyCode::O}, [&fired]() -> void { ++fired; });

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
    const int id = reg.add(KeyCombo{ModifierKey::Control, KeyCode::P}, [&fired]() -> void { ++fired; });

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
    const int id = reg.add(KeyCombo{ModifierKey::Control, KeyCode::N}, [&fired]() -> void { ++fired; });

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
    reg.add(KeyCombo{ModifierKey::None, KeyCode::F1}, [&fired]() -> void { ++fired; }, ShortcutScope::Focus);

    // Focus 作用域：无焦点控件时不触发。
    AURORA_TEST_CHECK_FALSE(reg.handle(key_event(KeyCode::F1), false));
    AURORA_TEST_CHECK_EQ(fired, 0);
    // 有焦点控件时触发。
    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::F1), true));
    AURORA_TEST_CHECK_EQ(fired, 1);

    // Global 作用域与焦点标记无关。
    int global_fired = 0;
    reg.add(
        KeyCombo{ModifierKey::None, KeyCode::F2}, [&global_fired]() -> void { ++global_fired; }, ShortcutScope::Global);
    AURORA_TEST_CHECK_TRUE(reg.handle(key_event(KeyCode::F2), false));
    AURORA_TEST_CHECK_EQ(global_fired, 1);
}

AURORA_TEST_CASE(first_matching_binding_wins_on_conflict) {
    // 冲突（同键组合注册两次）：先注册者触发并消费，后者不再执行。
    ShortcutRegistry reg;
    int fired_a = 0;
    int fired_b = 0;
    reg.add(KeyCombo{ModifierKey::Control, KeyCode::K}, [&fired_a]() -> void { ++fired_a; });
    reg.add(KeyCombo{ModifierKey::Control, KeyCode::K}, [&fired_b]() -> void { ++fired_b; });

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

// ---------------------------------------------------------------------------
// 锁定态修饰位（NumLock）与匹配的关系
//
// 背景：`ModifierKey` 把「可按住的修饰位」与「键盘锁定态位」放进同一个字节，四个后端都会在
// 事件上盖章锁定位，而注册侧从不登记它。若 `matches` 按整字节相等比较，`NumLock` 开着时任何
// 未登记该位的应用内快捷键恒不匹配——整层静默失效，且与焦点、作用域无关。
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(lock_bit_on_the_event_does_not_break_matching) {
    // 判据 A 的单元形态：注册侧只带可按住位，事件侧多带一个未登记的锁定位，仍须匹配。
    const KeyCombo combo{ModifierKey::Control | ModifierKey::Shift, KeyCode::O};

    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift)));
    // 事件侧带 NumLock（NumLock 开着，用户没按它）——必须照样命中。
    AURORA_TEST_CHECK_TRUE(
        combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift | ModifierKey::NumLock)));
    // 锁定位在 NumLock 关着时同样不参与：缺该位也命中（否则等于要求「必须关」）。
    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift)));
}

AURORA_TEST_CASE(lock_bit_never_turns_equality_into_containment) {
    // 反向守卫，专防「把相等改成包含」：Ctrl+O 不得匹配 Ctrl+Shift+O。
    // 这条是刻意的语义（Qt QKeySequence / WPF KeyGesture / Flutter SingleActivator 共识）：
    // 改成包含会让「更具体组合优先」的注册表失去区分度。
    const KeyCombo combo{ModifierKey::Control, KeyCode::O};

    AURORA_TEST_CHECK_FALSE(
        combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift | ModifierKey::NumLock)));
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift)));
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::O, ModifierKey::NumLock)));
    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::NumLock)));
}

AURORA_TEST_CASE(lock_bit_on_the_registered_side_is_ignored) {
    // 注册侧的锁定位不可表达：KeyCombo{Control|NumLock, K} 里的锁定位被忽略，等价于 Control+K。
    // matches 不加断言/日志/Result（它在每条按键消息上被调用，热路径噪声比它想防的错更贵）。
    const KeyCombo combo{ModifierKey::Control | ModifierKey::NumLock, KeyCode::K};

    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::K, ModifierKey::Control)));
    AURORA_TEST_CHECK_TRUE(combo.matches(key_event(KeyCode::K, ModifierKey::Control | ModifierKey::NumLock)));
    // 锁定位被忽略不意味着「可按住位」也被忽略：多带 Shift 仍不匹配。
    AURORA_TEST_CHECK_FALSE(combo.matches(key_event(KeyCode::K, ModifierKey::Control | ModifierKey::Shift)));
    // to_string() 本就不列锁定位，与该口径一致。
    AURORA_TEST_CHECK_EQ(combo.to_string(), std::string("Ctrl+K"));
}

AURORA_TEST_CASE(registry_handle_matches_while_numlock_is_on) {
    // 判据 A 的注册表形态：整条 pre-handler 链路在 NumLock 开着时命中、执行动作、消费事件。
    ShortcutRegistry reg;
    int fired = 0;
    reg.add(KeyCombo{ModifierKey::Control | ModifierKey::Shift, KeyCode::O}, [&fired]() -> void { ++fired; });

    AURORA_TEST_CHECK_TRUE(
        reg.handle(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift | ModifierKey::NumLock)));
    AURORA_TEST_CHECK_EQ(fired, 1);

    // 同一绑定 enabled=false 时不消费——锁定位不该把「禁用」这条路径也带偏。
    reg.set_enabled(1, false);
    AURORA_TEST_CHECK_FALSE(
        reg.handle(key_event(KeyCode::O, ModifierKey::Control | ModifierKey::Shift | ModifierKey::NumLock)));
    AURORA_TEST_CHECK_EQ(fired, 1);
}

AURORA_TEST_CASE(pressable_and_lock_masks_partition_the_modifier_bits) {
    // 两个新常量的位集自证：并集覆盖 ModifierKey 全部已定义位，且两者交集为 0。
    // 「新增锁定位时这条会要求同步」正是它存在的目的——漏并入 AURORA_MODIFIER_LOCK_MASK 的位会落进
    // 两掩码之外的缝隙，在按位比较中继续污染结果（那正是本次缺陷的形态）。
    constexpr std::uint8_t all_defined =
        static_cast<std::uint8_t>(ModifierKey::Shift) | static_cast<std::uint8_t>(ModifierKey::Control) |
        static_cast<std::uint8_t>(ModifierKey::Alt) | static_cast<std::uint8_t>(ModifierKey::Meta) |
        static_cast<std::uint8_t>(ModifierKey::NumLock);

    // 交集为 0：一个位不能既是「按住」又是「锁定」。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(AURORA_MODIFIER_PRESSABLE_MASK & AURORA_MODIFIER_LOCK_MASK),
                         static_cast<std::uint8_t>(0));
    // 并集无余位：没有位落在两个掩码之外。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(AURORA_MODIFIER_PRESSABLE_MASK | AURORA_MODIFIER_LOCK_MASK),
                         all_defined);
    // 锁定位只落在 LOCK 侧（PRESSABLE 侧取子集后必须为 0，否则 matches 屏蔽不掉它）。
    AURORA_TEST_CHECK_EQ(
        static_cast<std::uint8_t>(static_cast<std::uint8_t>(ModifierKey::NumLock) & AURORA_MODIFIER_PRESSABLE_MASK),
        static_cast<std::uint8_t>(0));
    AURORA_TEST_CHECK_EQ(
        static_cast<std::uint8_t>(static_cast<std::uint8_t>(ModifierKey::NumLock) & AURORA_MODIFIER_LOCK_MASK),
        static_cast<std::uint8_t>(ModifierKey::NumLock));
}

}  // namespace aurora::test_cases::utest_shortcuts

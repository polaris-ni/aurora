/// @file utest_glfw_modifiers.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/glfw_modifiers.h
/// 测试说明: GLFW 修饰键位两个入口的折算表 —— ① `glfw_mods_to_aurora`（回调形参掩码，
///           键盘 / `on_mouse_button` 走它）；② `glfw_key_states_to_modifiers`（八个键码的
///           左右归并纯映射，`on_cursor_pos` / `on_scroll` 经 `glfw_cached_modifiers` 走它）。
///           另钉一条**同源契约**：两个入口对同一物理状态必须产出同一位集——它们若各写一份，
///           就会长出「按住 Shift 点按钮有位、拖动时没位」这类只在真机显形的分叉。
/// 平台门控: 依赖 `AURORA_BACKEND_GLFW`（头随门控裁掉）；未开启时每条用例落 SKIP 桩
///           （声明无条件可见，满足 `runner --list` 与测试源字面量一致）

#include <cstdint>

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#ifdef AURORA_BACKEND_GLFW
#include <GLFW/glfw3.h>

#include "aurora/window/detail/glfw_modifiers.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_glfw_modifiers {

#ifdef AURORA_BACKEND_GLFW

/// @brief 断言辅助：位集与期望逐位比对（枚举位掩码按 uint8_t 比）。
auto check_mods(ModifierKey got, ModifierKey want) -> void {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(got), static_cast<std::uint8_t>(want));
}

#endif

#define AURORA_GLFW_MODIFIERS_SKIP \
    AURORA_TEST_SKIP("AURORA_BACKEND_GLFW is not enabled: detail/glfw_modifiers.h is compiled out")

// GLFW_MOD_* 掩码值与位宽：折算表逐位正确的地基。仓库固定 GLFW 3.5.1（第三方源码内置），
// 这些值随上游定义而变，故写死断言——错位后果是「某个组合永不生效」且极难归因。
AURORA_TEST_CASE(glfw_mod_mask_values_match_the_sdk) {
#ifdef AURORA_BACKEND_GLFW
    AURORA_TEST_CHECK_EQ(static_cast<int>(GLFW_MOD_SHIFT), 0x0001);
    AURORA_TEST_CHECK_EQ(static_cast<int>(GLFW_MOD_CONTROL), 0x0002);
    AURORA_TEST_CHECK_EQ(static_cast<int>(GLFW_MOD_ALT), 0x0004);
    AURORA_TEST_CHECK_EQ(static_cast<int>(GLFW_MOD_SUPER), 0x0008);
    // 掩码互不重叠，否则两个语义位会被同一位点亮。
    AURORA_TEST_CHECK_EQ(static_cast<int>(GLFW_MOD_SHIFT) & static_cast<int>(GLFW_MOD_CONTROL), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(GLFW_MOD_ALT) & static_cast<int>(GLFW_MOD_SUPER), 0);
#else
    AURORA_GLFW_MODIFIERS_SKIP;
#endif
}

// 八个修饰键码常量：指针回调入口按这些码读缓存态，码值写错则某个键永远读不到。
AURORA_TEST_CASE(glfw_modifier_key_tokens_match_the_sdk) {
#ifdef AURORA_BACKEND_GLFW
    AURORA_TEST_CHECK_EQ(GLFW_KEY_LEFT_SHIFT, 340);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_RIGHT_SHIFT, 344);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_LEFT_CONTROL, 341);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_RIGHT_CONTROL, 345);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_LEFT_ALT, 342);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_RIGHT_ALT, 346);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_LEFT_SUPER, 343);
    AURORA_TEST_CHECK_EQ(GLFW_KEY_RIGHT_SUPER, 347);
    // 左右两侧必须是不同码，否则「左右归并」退化成读同一个键、失去存在意义。
    AURORA_TEST_CHECK_NE(GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT);
    AURORA_TEST_CHECK_NE(GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL);
    AURORA_TEST_CHECK_NE(GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT);
    AURORA_TEST_CHECK_NE(GLFW_KEY_LEFT_SUPER, GLFW_KEY_RIGHT_SUPER);
#else
    AURORA_GLFW_MODIFIERS_SKIP;
#endif
}

// 入口①：回调形参掩码 → 位集。逐位 + 组合 + 空掩码。
AURORA_TEST_CASE(callback_mod_mask_folds_to_the_four_pointer_bits) {
#ifdef AURORA_BACKEND_GLFW
    check_mods(detail::glfw_mods_to_aurora(0), ModifierKey::None);
    check_mods(detail::glfw_mods_to_aurora(GLFW_MOD_SHIFT), ModifierKey::Shift);
    check_mods(detail::glfw_mods_to_aurora(GLFW_MOD_CONTROL), ModifierKey::Control);
    check_mods(detail::glfw_mods_to_aurora(GLFW_MOD_ALT), ModifierKey::Alt);
    check_mods(detail::glfw_mods_to_aurora(GLFW_MOD_SUPER), ModifierKey::Meta);

    const auto ctrl_shift = detail::glfw_mods_to_aurora(GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(ctrl_shift), std::uint8_t{3});
    const auto all_four =
        detail::glfw_mods_to_aurora(GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(all_four), std::uint8_t{15});

    // CapsLock / NumLock 两个掩码**不被折**：锁定位与指针手势无语义关系（无小键盘参与），
    // 且它们只在开启 GLFW_LOCK_KEY_MODS 输入模式时才随事件上报。消费方不应在指针事件上
    // 看到这两位——「不静默假报开」在此处就是「不折」。
    check_mods(detail::glfw_mods_to_aurora(GLFW_MOD_CAPS_LOCK), ModifierKey::None);
    check_mods(detail::glfw_mods_to_aurora(GLFW_MOD_NUM_LOCK), ModifierKey::None);
    // 即便掩码里混进了锁定位，四位判定也不受影响。
    const auto shifted = detail::glfw_mods_to_aurora(GLFW_MOD_SHIFT | GLFW_MOD_NUM_LOCK);
    AURORA_TEST_CHECK_NE(shifted & ModifierKey::Shift, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(shifted & ModifierKey::NumLock, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(shifted & ModifierKey::Meta, std::uint8_t{0});
#else
    AURORA_GLFW_MODIFIERS_SKIP;
#endif
}

// 入口②：八个键码的按下态 → 位集，**左右归并**。归并的必要性同 Win32 侧：不归并则左右
// 各占一位，消费方拿到「左 Shift」这种值还得自己再折，且不配对的左右变体会留下幻影位。
AURORA_TEST_CASE(key_states_fold_left_and_right_into_one_bit_each) {
#ifdef AURORA_BACKEND_GLFW
    using detail::glfw_key_states_to_modifiers;
    // 全 false → None。
    check_mods(glfw_key_states_to_modifiers(false, false, false, false, false, false, false, false), ModifierKey::None);
    // 逐位：左侧单独按下即置位（右侧不该被要求同时按下）。
    check_mods(glfw_key_states_to_modifiers(true, false, false, false, false, false, false, false), ModifierKey::Shift);
    check_mods(glfw_key_states_to_modifiers(false, false, true, false, false, false, false, false),
               ModifierKey::Control);
    check_mods(glfw_key_states_to_modifiers(false, false, false, false, true, false, false, false), ModifierKey::Alt);
    check_mods(glfw_key_states_to_modifiers(false, false, false, false, false, false, true, false), ModifierKey::Meta);
    // 逐位：右侧单独按下同样置位（这正是「归并」的含义）。
    check_mods(glfw_key_states_to_modifiers(false, true, false, false, false, false, false, false), ModifierKey::Shift);
    check_mods(glfw_key_states_to_modifiers(false, false, false, true, false, false, false, false),
               ModifierKey::Control);
    check_mods(glfw_key_states_to_modifiers(false, false, false, false, false, true, false, false), ModifierKey::Alt);
    check_mods(glfw_key_states_to_modifiers(false, false, false, false, false, false, false, true), ModifierKey::Meta);
    // 左右同时按下仍是同一位（不得叠出第二位）。
    check_mods(glfw_key_states_to_modifiers(true, true, false, false, false, false, false, false), ModifierKey::Shift);
    // 四位齐按 = 15；参数顺序错位（把右当左传）时结果必须不同，否则顺序无意义。
    check_mods(glfw_key_states_to_modifiers(true, true, true, true, true, true, true, true),
               ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt | ModifierKey::Meta);
    // 只有左 Alt 时不得点亮 Meta：位与位之间不能串扰。
    const auto left_alt_only = glfw_key_states_to_modifiers(false, false, false, false, true, false, false, false);
    AURORA_TEST_CHECK_EQ(left_alt_only & ModifierKey::Meta, std::uint8_t{0});
    AURORA_TEST_CHECK_EQ(left_alt_only & ModifierKey::Control, std::uint8_t{0});
    AURORA_TEST_CHECK_NE(left_alt_only & ModifierKey::Alt, std::uint8_t{0});
#else
    AURORA_GLFW_MODIFIERS_SKIP;
#endif
}

// 同源契约：同一物理状态经两个入口必须产出**同一**位集。两个入口服务不同回调
// （`on_mouse_button` 有掩码形参、`on_cursor_pos` / `on_scroll` 没有只能读缓存态），
// 若折算规则各写一份，就会长出「按住 Shift 点按钮有位、拖动时没位」这类只在真机显形的分叉。
AURORA_TEST_CASE(both_entry_points_agree_on_the_same_physical_state) {
#ifdef AURORA_BACKEND_GLFW
    using detail::glfw_key_states_to_modifiers;
    using detail::glfw_mods_to_aurora;
    // 三种组合各比一次：单 Shift、Ctrl+Alt、四位齐按。
    check_mods(glfw_key_states_to_modifiers(true, true, false, false, false, false, false, false),
               glfw_mods_to_aurora(GLFW_MOD_SHIFT));
    check_mods(glfw_key_states_to_modifiers(false, false, true, true, true, true, false, false),
               glfw_mods_to_aurora(GLFW_MOD_CONTROL | GLFW_MOD_ALT));
    check_mods(glfw_key_states_to_modifiers(true, true, true, true, true, true, true, true),
               glfw_mods_to_aurora(GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER));
    // 全不按：两入口同得 None。
    check_mods(glfw_key_states_to_modifiers(false, false, false, false, false, false, false, false),
               glfw_mods_to_aurora(0));
#else
    AURORA_GLFW_MODIFIERS_SKIP;
#endif
}

}  // namespace aurora::test_cases::utest_glfw_modifiers

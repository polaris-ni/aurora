/// @file utest_win32_modifiers.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/win32_modifiers.h
/// 测试说明: Win32 宿主修饰键跟踪器 —— 位翻转随消息推进、左右与通用码归并、失焦清空、激活
///           播种、AltGr 两段序列。它取代了原先「派发时刻 `GetAsyncKeyState` 采样」，故这些
///           消息序列正是热键能否命中的判据本体（竞态实测见 `manual-test/21-debug.md` TC-DEBUG-004）
/// 平台门控: 依赖 `<windows.h>` 的 VK 码与后端宏；宏未开启时每条用例落 SKIP 桩（声明无条件可见）

#include <cstdint>

#include "aurora/core/platform.h"
#include "framework/aurora_test.h"

#if defined(AURORA_PLATFORM_WINDOWS) && (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11))
// 平台可用性开关，供下方各用例体内的 #ifdef 使用，无 constexpr 等价物
// NOLINTNEXTLINE(*-macro-usage)
#define AURORA_WIN32_MODIFIERS_AVAILABLE 1
#endif

#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
#include <windows.h>

#include "aurora/event/event.h"
#include "aurora/window/detail/win32_modifiers.h"
#endif

namespace aurora::test_cases::utest_win32_modifiers {

#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
using detail::ModifierKeyTracker;

/// @brief 断言辅助：把 `ModifierKey` 位集与期望逐位比对（枚举位掩码按 uint8_t 比）。
auto check_mods(const ModifierKeyTracker &t, ModifierKey want) -> void {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(t.get()), static_cast<std::uint8_t>(want));
}
#endif

#define AURORA_WIN32_MODIFIERS_SKIP \
    AURORA_TEST_SKIP("non-Windows or Win32/D3D11 backend off: tracker needs VK_* codes from <windows.h>")

AURORA_TEST_CASE(down_sets_bit_and_up_clears_it) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, true));
    check_mods(t, ModifierKey::Control);
    // 自动重复（按住不放连发 WM_KEYDOWN）不得叠出第二个状态
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, true));
    check_mods(t, ModifierKey::Control);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_CONTROL, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(left_right_and_generic_vk_share_one_bit) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 归并到同一位的收益：按下与抬起给出**不同变体**（注入与某些布局会这样）也不残留幻影。
    ModifierKeyTracker t;
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LSHIFT, true));
    check_mods(t, ModifierKey::Shift);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_SHIFT, false));  // 抬起给的是通用码，不是 L 变体
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_RSHIFT, true));
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LSHIFT, false));  // 抬起给另一侧变体
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(chord_carries_the_modifier_still_down_in_the_queue) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 被修掉的回归本体：`Ctrl+S` 里 Ctrl 的抬起消息排在 S 之后，则无论泵送多晚，派发 S 时
    // 跟踪器都还带着 Control。物理采样做不到这点——抬起一旦落地读数即翻 0。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    AURORA_TEST_CHECK_FALSE(t.apply(0x53 /* 'S' */, true));  // 非修饰键：不改状态、回 false
    check_mods(t, ModifierKey::Control);
    t.apply(0x53, false);
    t.apply(VK_CONTROL, false);
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(altgr_two_stage_sequence_yields_alt_and_control) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // AltGr 在 Windows 上是 `VK_RMENU` + `VK_LCONTROL` 两段（抬起反序），逐位翻转天然覆盖。
    ModifierKeyTracker t;
    t.apply(VK_LCONTROL, true);
    t.apply(VK_RMENU, true);
    check_mods(t, ModifierKey::Control | ModifierKey::Alt);
    t.apply(VK_LCONTROL, false);
    check_mods(t, ModifierKey::Alt);
    t.apply(VK_RMENU, false);
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(blur_clears_every_held_bit) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 交出前台/焦点后的抬起消息属于别的窗口，跟踪器再也收不到 → 必须整体归零。
    ModifierKeyTracker t;
    t.apply(VK_LMENU, true);
    t.apply(VK_SHIFT, true);
    check_mods(t, ModifierKey::Alt | ModifierKey::Shift);
    t.clear();
    check_mods(t, ModifierKey::None);
    // 清空后真正的抬起落地（切回来时系统补发）也不该把它「抬」回来
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LMENU, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(seed_replaces_state_wholesale) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    // 激活基线播种：Alt+Tab 切进来时 Alt 已按下，那条按下属于旧前台窗口。
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    t.clear();  // 交出前台
    t.seed(ModifierKey::Alt);
    check_mods(t, ModifierKey::Alt);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_MENU, false));  // 用户随后松开 Alt
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(meta_maps_both_win_keys) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LWIN, true));
    check_mods(t, ModifierKey::Meta);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_LWIN, false));
    check_mods(t, ModifierKey::None);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_RWIN, true));
    check_mods(t, ModifierKey::Meta);
    AURORA_TEST_CHECK_TRUE(t.apply(VK_RWIN, false));
    check_mods(t, ModifierKey::None);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

AURORA_TEST_CASE(non_modifier_keys_never_touch_the_state) {
#ifdef AURORA_WIN32_MODIFIERS_AVAILABLE
    ModifierKeyTracker t;
    t.apply(VK_CONTROL, true);
    for (const int vk : {VK_TAB, VK_LEFT, 0x41 /* 'A' */, VK_SPACE, VK_ESCAPE}) {
        AURORA_TEST_CHECK_FALSE(t.apply(vk, true));
        AURORA_TEST_CHECK_FALSE(t.apply(vk, false));
        check_mods(t, ModifierKey::Control);
    }
    // 大写锁定既不是修饰位也不该污染状态（`VK_CAPITAL` 不在映射内）
    AURORA_TEST_CHECK_FALSE(t.apply(VK_CAPITAL, true));
    check_mods(t, ModifierKey::Control);
#else
    AURORA_WIN32_MODIFIERS_SKIP;
#endif
}

#undef AURORA_WIN32_MODIFIERS_SKIP

}  // namespace aurora::test_cases::utest_win32_modifiers

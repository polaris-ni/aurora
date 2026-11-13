/// @file utest_macos_keymap.cpp
/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/macos_keymap.h
/// 测试说明: macOS `NSEvent.keyCode`（Carbon 虚拟键码）与 `NSEvent.modifierFlags` 的折算表——
///           字母/数字行/标点/方向与编辑键/功能键/小键盘逐值钉死，未收录键回 `Unknown`，
///           小键盘回车并入 `Enter`、小键盘等号并入 `Equal` 的并档口径；
///           修饰位 Shift/Control/Option→Alt/Command→Meta 的逐位与组合折算，CapsLock/Function
///           位如实丢弃。原生常量按 Carbon/AppKit ABI 写死，防「头内手抄常量」静默漂移。
/// 平台门控: 依赖 `AURORA_PLATFORM_MACOS` ∧ `AURORA_BACKEND_MACOS`（与后端同门控）；
///           未开时每条用例落 SKIP 桩

#include "aurora/core/platform.h"  // 守卫求值前必须先有平台宏（TU 自包含，不依赖 PCH 伞头带入）
#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)
#include <cstdint>
#include <span>

#include "aurora/window/detail/macos_keymap.h"
#endif

#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_macos_keymap {

#if defined(AURORA_PLATFORM_MACOS) && defined(AURORA_BACKEND_MACOS)

namespace {

using aurora::detail::macos_keycode_to_keycode;
using aurora::detail::macos_modifiers_from_flags;

/// @brief 键码对照项：原生 Carbon 虚拟键码 → 期望逻辑键码。
struct KeyEntry {
    unsigned short native;
    KeyCode expected;
};

/// @brief 逐项比对一张表；失败时套件只报告用例失败，表内容在源码里即判据。
/// @param entries 对照表（数组自动退化为 span，长度随视图带入，不做裸下标）。
auto check_table(std::span<const KeyEntry> entries) -> void {
    for (const auto &entry : entries) {
        AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(entry.native)),
                             static_cast<int>(entry.expected));
    }
}

/// @brief 修饰位比对（枚举位掩码按底层整值比）。
auto check_mods(ModifierKey got, ModifierKey want) -> void {
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(got), static_cast<std::uint8_t>(want));
}

}  // namespace

// 原生修饰位与 AppKit 公开常量一致（1<<17..1<<20）。头刻意不引 AppKit，常量靠手抄，
// 故这里把值钉死；抄错会让某个组合永远不生效，且真机上极难归因。
AURORA_TEST_CASE(macos_modifier_flag_values_match_appkit) {
    AURORA_TEST_CHECK_EQ(aurora::detail::AURORA_MACOS_MOD_SHIFT, 1ULL << 17U);
    AURORA_TEST_CHECK_EQ(aurora::detail::AURORA_MACOS_MOD_CONTROL, 1ULL << 18U);
    AURORA_TEST_CHECK_EQ(aurora::detail::AURORA_MACOS_MOD_OPTION, 1ULL << 19U);
    AURORA_TEST_CHECK_EQ(aurora::detail::AURORA_MACOS_MOD_COMMAND, 1ULL << 20U);
}

AURORA_TEST_CASE(macos_modifiers_map_each_flag_to_its_own_bit) {
    check_mods(macos_modifiers_from_flags(0U), ModifierKey::None);
    check_mods(macos_modifiers_from_flags(aurora::detail::AURORA_MACOS_MOD_SHIFT), ModifierKey::Shift);
    check_mods(macos_modifiers_from_flags(aurora::detail::AURORA_MACOS_MOD_CONTROL), ModifierKey::Control);
    // Option 是 macOS 的 Alt 语义位；Command 是 Meta 语义位。
    check_mods(macos_modifiers_from_flags(aurora::detail::AURORA_MACOS_MOD_OPTION), ModifierKey::Alt);
    check_mods(macos_modifiers_from_flags(aurora::detail::AURORA_MACOS_MOD_COMMAND), ModifierKey::Meta);
    // CapsLock（1<<16）与 Function（1<<23）不属 ModifierKey，如实丢弃、不污染其它位。
    check_mods(macos_modifiers_from_flags(1ULL << 16U), ModifierKey::None);
    check_mods(macos_modifiers_from_flags(1ULL << 23U), ModifierKey::None);
    // macOS 无 NumLock。
    check_mods(macos_modifiers_from_flags(
                   ~0ULL & ~(aurora::detail::AURORA_MACOS_MOD_SHIFT | aurora::detail::AURORA_MACOS_MOD_CONTROL |
                             aurora::detail::AURORA_MACOS_MOD_OPTION | aurora::detail::AURORA_MACOS_MOD_COMMAND)),
               ModifierKey::None);
}

AURORA_TEST_CASE(macos_modifiers_combine_without_crosstalk) {
    constexpr auto shift = aurora::detail::AURORA_MACOS_MOD_SHIFT;
    constexpr auto ctrl = aurora::detail::AURORA_MACOS_MOD_CONTROL;
    constexpr auto alt = aurora::detail::AURORA_MACOS_MOD_OPTION;
    constexpr auto meta = aurora::detail::AURORA_MACOS_MOD_COMMAND;
    check_mods(macos_modifiers_from_flags(shift | ctrl), ModifierKey::Shift | ModifierKey::Control);
    check_mods(macos_modifiers_from_flags(meta | alt), ModifierKey::Meta | ModifierKey::Alt);
    check_mods(macos_modifiers_from_flags(ctrl | alt | shift),
               ModifierKey::Control | ModifierKey::Alt | ModifierKey::Shift);
    check_mods(macos_modifiers_from_flags(shift | ctrl | alt | meta),
               ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt | ModifierKey::Meta);
}

AURORA_TEST_CASE(macos_letter_keys_map_to_their_physical_positions) {
    // 美式布局的物理键位（NSEvent.keyCode 按位置而非字符），逐个钉死。
    constexpr KeyEntry entries[] = {
        {.native = 0, .expected = KeyCode::A},  {.native = 11, .expected = KeyCode::B},
        {.native = 8, .expected = KeyCode::C},  {.native = 2, .expected = KeyCode::D},
        {.native = 14, .expected = KeyCode::E}, {.native = 3, .expected = KeyCode::F},
        {.native = 5, .expected = KeyCode::G},  {.native = 4, .expected = KeyCode::H},
        {.native = 34, .expected = KeyCode::I}, {.native = 38, .expected = KeyCode::J},
        {.native = 40, .expected = KeyCode::K}, {.native = 37, .expected = KeyCode::L},
        {.native = 46, .expected = KeyCode::M}, {.native = 45, .expected = KeyCode::N},
        {.native = 31, .expected = KeyCode::O}, {.native = 35, .expected = KeyCode::P},
        {.native = 12, .expected = KeyCode::Q}, {.native = 15, .expected = KeyCode::R},
        {.native = 1, .expected = KeyCode::S},  {.native = 17, .expected = KeyCode::T},
        {.native = 32, .expected = KeyCode::U}, {.native = 9, .expected = KeyCode::V},
        {.native = 13, .expected = KeyCode::W}, {.native = 7, .expected = KeyCode::X},
        {.native = 16, .expected = KeyCode::Y}, {.native = 6, .expected = KeyCode::Z},
    };
    check_table(entries);
}

AURORA_TEST_CASE(macos_digit_row_maps_in_layout_order) {
    // 数字行在 ANSI 布局里 1..9 的键码不连续（5/6、7/8 互换），逐项写死。
    constexpr KeyEntry entries[] = {
        {.native = 18, .expected = KeyCode::D1}, {.native = 19, .expected = KeyCode::D2},
        {.native = 20, .expected = KeyCode::D3}, {.native = 21, .expected = KeyCode::D4},
        {.native = 23, .expected = KeyCode::D5}, {.native = 22, .expected = KeyCode::D6},
        {.native = 26, .expected = KeyCode::D7}, {.native = 28, .expected = KeyCode::D8},
        {.native = 25, .expected = KeyCode::D9}, {.native = 29, .expected = KeyCode::D0},
    };
    check_table(entries);
}

AURORA_TEST_CASE(macos_punctuation_and_editing_keys) {
    constexpr KeyEntry entries[] = {
        {.native = 24, .expected = KeyCode::Equal},        {.native = 27, .expected = KeyCode::Minus},
        {.native = 30, .expected = KeyCode::RightBracket}, {.native = 33, .expected = KeyCode::LeftBracket},
        {.native = 39, .expected = KeyCode::Quote},        {.native = 41, .expected = KeyCode::Semicolon},
        {.native = 42, .expected = KeyCode::Backslash},    {.native = 43, .expected = KeyCode::Comma},
        {.native = 44, .expected = KeyCode::Slash},        {.native = 47, .expected = KeyCode::Period},
        {.native = 50, .expected = KeyCode::Backquote},    {.native = 36, .expected = KeyCode::Enter},
        {.native = 48, .expected = KeyCode::Tab},          {.native = 49, .expected = KeyCode::Space},
        {.native = 51, .expected = KeyCode::Backspace},    {.native = 53, .expected = KeyCode::Escape},
        {.native = 117, .expected = KeyCode::Delete},
    };
    check_table(entries);
}

AURORA_TEST_CASE(macos_navigation_and_function_keys) {
    constexpr KeyEntry entries[] = {
        {.native = 123, .expected = KeyCode::ArrowLeft}, {.native = 124, .expected = KeyCode::ArrowRight},
        {.native = 125, .expected = KeyCode::ArrowDown}, {.native = 126, .expected = KeyCode::ArrowUp},
        {.native = 115, .expected = KeyCode::Home},      {.native = 119, .expected = KeyCode::End},
        {.native = 116, .expected = KeyCode::PageUp},    {.native = 121, .expected = KeyCode::PageDown},
        {.native = 122, .expected = KeyCode::F1},        {.native = 120, .expected = KeyCode::F2},
        {.native = 99, .expected = KeyCode::F3},         {.native = 118, .expected = KeyCode::F4},
        {.native = 96, .expected = KeyCode::F5},         {.native = 97, .expected = KeyCode::F6},
        {.native = 98, .expected = KeyCode::F7},         {.native = 100, .expected = KeyCode::F8},
        {.native = 101, .expected = KeyCode::F9},        {.native = 109, .expected = KeyCode::F10},
        {.native = 103, .expected = KeyCode::F11},       {.native = 111, .expected = KeyCode::F12},
    };
    check_table(entries);
}

AURORA_TEST_CASE(macos_modifier_keys_map_to_left_right_merged_codes) {
    constexpr KeyEntry entries[] = {
        {.native = 54, .expected = KeyCode::Meta},    {.native = 55, .expected = KeyCode::Meta},  // Command 左/右
        {.native = 56, .expected = KeyCode::Shift},   {.native = 60, .expected = KeyCode::Shift},  // Shift 左/右
        {.native = 58, .expected = KeyCode::Alt},     {.native = 61, .expected = KeyCode::Alt},  // Option 左/右
        {.native = 59, .expected = KeyCode::Control}, {.native = 62, .expected = KeyCode::Control},  // Control 左/右
    };
    check_table(entries);
}

AURORA_TEST_CASE(macos_keypad_maps_to_kp_codes_with_documented_merges) {
    constexpr KeyEntry entries[] = {
        {.native = 82, .expected = KeyCode::KP_0},
        {.native = 83, .expected = KeyCode::KP_1},
        {.native = 84, .expected = KeyCode::KP_2},
        {.native = 85, .expected = KeyCode::KP_3},
        {.native = 86, .expected = KeyCode::KP_4},
        {.native = 87, .expected = KeyCode::KP_5},
        {.native = 88, .expected = KeyCode::KP_6},
        {.native = 89, .expected = KeyCode::KP_7},
        {.native = 91, .expected = KeyCode::KP_8},
        {.native = 92, .expected = KeyCode::KP_9},
        {.native = 65, .expected = KeyCode::KP_Decimal},
        {.native = 67, .expected = KeyCode::KP_Multiply},
        {.native = 69, .expected = KeyCode::KP_Add},
        {.native = 75, .expected = KeyCode::KP_Divide},
        {.native = 78, .expected = KeyCode::KP_Subtract},
        // 并档口径：小键盘回车并入 Enter，小键盘等号并入主键盘 Equal（框架未单列 KP_Equal）。
        {.native = 76, .expected = KeyCode::Enter},
        {.native = 81, .expected = KeyCode::Equal},
    };
    check_table(entries);
}

AURORA_TEST_CASE(macos_unmapped_keys_are_unknown_not_guesswork) {
    // ISO 节键（10）、F13+（105）、媒体键（72/73/74）等不在框架键面内，一律 Unknown。
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(10)), static_cast<int>(KeyCode::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(105)), static_cast<int>(KeyCode::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(72)), static_cast<int>(KeyCode::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(999)), static_cast<int>(KeyCode::Unknown));
}

#else

#define AURORA_MACOS_KEYMAP_SKIP                                                    \
    AURORA_TEST_SKIP(                                                               \
        "non-macOS or AURORA_BACKEND_MACOS off: detail/macos_keymap.h is compiled " \
        "out; this case requires a real macOS backend build")

AURORA_TEST_CASE(macos_modifier_flag_values_match_appkit) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_modifiers_map_each_flag_to_its_own_bit) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_modifiers_combine_without_crosstalk) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_letter_keys_map_to_their_physical_positions) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_digit_row_maps_in_layout_order) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_punctuation_and_editing_keys) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_navigation_and_function_keys) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_modifier_keys_map_to_left_right_merged_codes) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_keypad_maps_to_kp_codes_with_documented_merges) { AURORA_MACOS_KEYMAP_SKIP; }
AURORA_TEST_CASE(macos_unmapped_keys_are_unknown_not_guesswork) { AURORA_MACOS_KEYMAP_SKIP; }

#endif

}  // namespace aurora::test_cases::utest_macos_keymap

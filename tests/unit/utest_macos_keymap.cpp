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
auto check_table(const KeyEntry *entries, std::size_t count) -> void {
    for (std::size_t i = 0; i < count; ++i) {
        AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(entries[i].native)),
                             static_cast<int>(entries[i].expected));
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
    check_mods(macos_modifiers_from_flags(~0ULL & ~(aurora::detail::AURORA_MACOS_MOD_SHIFT |
                                                    aurora::detail::AURORA_MACOS_MOD_CONTROL |
                                                    aurora::detail::AURORA_MACOS_MOD_OPTION |
                                                    aurora::detail::AURORA_MACOS_MOD_COMMAND)),
               ModifierKey::None);
}

AURORA_TEST_CASE(macos_modifiers_combine_without_crosstalk) {
    constexpr auto kShift = aurora::detail::AURORA_MACOS_MOD_SHIFT;
    constexpr auto kCtrl = aurora::detail::AURORA_MACOS_MOD_CONTROL;
    constexpr auto kAlt = aurora::detail::AURORA_MACOS_MOD_OPTION;
    constexpr auto kMeta = aurora::detail::AURORA_MACOS_MOD_COMMAND;
    check_mods(macos_modifiers_from_flags(kShift | kCtrl),
               ModifierKey::Shift | ModifierKey::Control);
    check_mods(macos_modifiers_from_flags(kMeta | kAlt), ModifierKey::Meta | ModifierKey::Alt);
    check_mods(macos_modifiers_from_flags(kCtrl | kAlt | kShift),
               ModifierKey::Control | ModifierKey::Alt | ModifierKey::Shift);
    check_mods(macos_modifiers_from_flags(kShift | kCtrl | kAlt | kMeta),
               ModifierKey::Shift | ModifierKey::Control | ModifierKey::Alt | ModifierKey::Meta);
}

AURORA_TEST_CASE(macos_letter_keys_map_to_their_physical_positions) {
    // 美式布局的物理键位（NSEvent.keyCode 按位置而非字符），逐个钉死。
    constexpr KeyEntry entries[] = {
        {0, KeyCode::A},  {11, KeyCode::B}, {8, KeyCode::C},  {2, KeyCode::D},  {14, KeyCode::E},
        {3, KeyCode::F},  {5, KeyCode::G},  {4, KeyCode::H},  {34, KeyCode::I}, {38, KeyCode::J},
        {40, KeyCode::K}, {37, KeyCode::L}, {46, KeyCode::M}, {45, KeyCode::N}, {31, KeyCode::O},
        {35, KeyCode::P}, {12, KeyCode::Q}, {15, KeyCode::R}, {1, KeyCode::S},  {17, KeyCode::T},
        {32, KeyCode::U}, {9, KeyCode::V},  {13, KeyCode::W}, {7, KeyCode::X},  {16, KeyCode::Y},
        {6, KeyCode::Z},
    };
    check_table(entries, sizeof(entries) / sizeof(entries[0]));
}

AURORA_TEST_CASE(macos_digit_row_maps_in_layout_order) {
    // 数字行在 ANSI 布局里 1..9 的键码不连续（5/6、7/8 互换），逐项写死。
    constexpr KeyEntry entries[] = {
        {18, KeyCode::D1}, {19, KeyCode::D2}, {20, KeyCode::D3}, {21, KeyCode::D4}, {23, KeyCode::D5},
        {22, KeyCode::D6}, {26, KeyCode::D7}, {28, KeyCode::D8}, {25, KeyCode::D9}, {29, KeyCode::D0},
    };
    check_table(entries, sizeof(entries) / sizeof(entries[0]));
}

AURORA_TEST_CASE(macos_punctuation_and_editing_keys) {
    constexpr KeyEntry entries[] = {
        {24, KeyCode::Equal},     {27, KeyCode::Minus},      {30, KeyCode::RightBracket},
        {33, KeyCode::LeftBracket}, {39, KeyCode::Quote},    {41, KeyCode::Semicolon},
        {42, KeyCode::Backslash}, {43, KeyCode::Comma},      {44, KeyCode::Slash},
        {47, KeyCode::Period},    {50, KeyCode::Backquote},  {36, KeyCode::Enter},
        {48, KeyCode::Tab},       {49, KeyCode::Space},      {51, KeyCode::Backspace},
        {53, KeyCode::Escape},    {117, KeyCode::Delete},
    };
    check_table(entries, sizeof(entries) / sizeof(entries[0]));
}

AURORA_TEST_CASE(macos_navigation_and_function_keys) {
    constexpr KeyEntry entries[] = {
        {123, KeyCode::ArrowLeft}, {124, KeyCode::ArrowRight}, {125, KeyCode::ArrowDown},
        {126, KeyCode::ArrowUp},   {115, KeyCode::Home},       {119, KeyCode::End},
        {116, KeyCode::PageUp},    {121, KeyCode::PageDown},   {122, KeyCode::F1},
        {120, KeyCode::F2},        {99, KeyCode::F3},          {118, KeyCode::F4},
        {96, KeyCode::F5},         {97, KeyCode::F6},          {98, KeyCode::F7},
        {100, KeyCode::F8},        {101, KeyCode::F9},         {109, KeyCode::F10},
        {103, KeyCode::F11},       {111, KeyCode::F12},
    };
    check_table(entries, sizeof(entries) / sizeof(entries[0]));
}

AURORA_TEST_CASE(macos_modifier_keys_map_to_left_right_merged_codes) {
    constexpr KeyEntry entries[] = {
        {54, KeyCode::Meta},  {55, KeyCode::Meta},    // Command 左/右
        {56, KeyCode::Shift}, {60, KeyCode::Shift},   // Shift 左/右
        {58, KeyCode::Alt},   {61, KeyCode::Alt},     // Option 左/右
        {59, KeyCode::Control}, {62, KeyCode::Control},  // Control 左/右
    };
    check_table(entries, sizeof(entries) / sizeof(entries[0]));
}

AURORA_TEST_CASE(macos_keypad_maps_to_kp_codes_with_documented_merges) {
    constexpr KeyEntry entries[] = {
        {82, KeyCode::KP_0},     {83, KeyCode::KP_1},     {84, KeyCode::KP_2},
        {85, KeyCode::KP_3},     {86, KeyCode::KP_4},     {87, KeyCode::KP_5},
        {88, KeyCode::KP_6},     {89, KeyCode::KP_7},     {91, KeyCode::KP_8},
        {92, KeyCode::KP_9},     {65, KeyCode::KP_Decimal}, {67, KeyCode::KP_Multiply},
        {69, KeyCode::KP_Add},   {75, KeyCode::KP_Divide}, {78, KeyCode::KP_Subtract},
        // 并档口径：小键盘回车并入 Enter，小键盘等号并入主键盘 Equal（框架未单列 KP_Equal）。
        {76, KeyCode::Enter},
        {81, KeyCode::Equal},
    };
    check_table(entries, sizeof(entries) / sizeof(entries[0]));
}

AURORA_TEST_CASE(macos_unmapped_keys_are_unknown_not_guesswork) {
    // ISO 节键（10）、F13+（105）、媒体键（72/73/74）等不在框架键面内，一律 Unknown。
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(10)), static_cast<int>(KeyCode::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(105)), static_cast<int>(KeyCode::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(72)), static_cast<int>(KeyCode::Unknown));
    AURORA_TEST_CHECK_EQ(static_cast<int>(macos_keycode_to_keycode(999)), static_cast<int>(KeyCode::Unknown));
}

#else

#define AURORA_MACOS_KEYMAP_SKIP                                                                  \
    AURORA_TEST_SKIP("non-macOS or AURORA_BACKEND_MACOS off: detail/macos_keymap.h is compiled " \
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

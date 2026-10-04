/// 测试类型: unit
/// 目标单元: src/aurora/window/keysym_map.h
/// 测试说明: X11 / Wayland 共用的 keysym → KeyCode 映射全表。字母 / 数字 / 功能键 / 导航 / 标点的
///           既有条目逐个锁定，以及**数字小键盘**（KP_0-9 连号区间 + 导航六键 + 运算键 +
///           小数点 / 分隔符）的新增条目、`KP_Enter` 并入 `KeyCode::Enter` 的既有决定、
///           `KP_Equal` 因无对应码位而落 Unknown
/// 平台门控: 无（该头是纯常量表，不含任何平台头依赖，故任何平台都可编译与断言）

#include "aurora/window/keysym_map.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_keysym_map {

namespace ks = aurora::detail::keysym;

AURORA_TEST_CASE(letter_and_digit_ranges_map_by_offset) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_a) == KeyCode::A);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_z) == KeyCode::Z);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_A) == KeyCode::A);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Z) == KeyCode::Z);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_0) == KeyCode::D0);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_9) == KeyCode::D9);
}

AURORA_TEST_CASE(function_and_navigation_keys_map) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_F1) == KeyCode::F1);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_F12) == KeyCode::F12);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Left) == KeyCode::ArrowLeft);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Right) == KeyCode::ArrowRight);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Up) == KeyCode::ArrowUp);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Down) == KeyCode::ArrowDown);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Home) == KeyCode::Home);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_End) == KeyCode::End);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Prior) == KeyCode::PageUp);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Next) == KeyCode::PageDown);
}

// 主键盘 Insert（0xFF63）与小键盘 `KP_Insert`（0xFF9E）在 X11 / Wayland 上各占独立码点，故本后端
// 是**唯一**能把两区分开的后端——主档因此单列一档、不与 `KP_Insert` 合并（Win32 / GLFW 分不开，
// 恒给主档）。两条互串断言是这条的本体：任意一方被并掉都会转红。
AURORA_TEST_CASE(main_keyboard_insert_maps_to_insert) {
    AURORA_TEST_CHECK(ks::AURORA_KEYSYM_Insert == 0xFF63UL);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Insert) == KeyCode::Insert);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Insert) != KeyCode::KP_Insert);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Insert) == KeyCode::KP_Insert);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Insert) != KeyCode::Insert);
}

// KP_Enter 并入 Enter 是既有决定（头注释已落定），此处锁住它不被某次「补全小键盘」改动推翻。
AURORA_TEST_CASE(keypad_enter_shares_the_main_enter_code) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Return) == KeyCode::Enter);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Enter) == KeyCode::Enter);
}

// KP_0-9 在 X11 是连号区间（0xFFB0..0xFFB9），与主键盘数字行同形走区间判。
// 逐值锁死是为了守住「区间首项 + 偏移」这条映射在两端都被钉住：删掉任一端都会静默丢键。
AURORA_TEST_CASE(keypad_digits_map_by_contiguous_range) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_0) == KeyCode::KP_0);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_0 + 1UL) == KeyCode::KP_1);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_0 + 5UL) == KeyCode::KP_5);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_9) == KeyCode::KP_9);
    // 区间的上界不得越界：KP_9 的下一位（0xFFBA）在 X11 未定义，落 Unknown。
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_9 + 1UL) == KeyCode::Unknown);
    // 区间的下界紧邻 `KP_Divide`（0xFFAF）——它**不是**越界值，而是另一个已映射键。
    // 这条同时守住「KP_0-9 的区间判不得把紧邻的运算键吞进来」。
    AURORA_TEST_CHECK(ks::AURORA_KEYSYM_KP_0 - 1UL == ks::AURORA_KEYSYM_KP_Divide);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_0 - 1UL) == KeyCode::KP_Divide);
}

AURORA_TEST_CASE(keypad_navigation_and_operator_keys_map) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Insert) == KeyCode::KP_Insert);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Delete) == KeyCode::KP_Delete);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Begin) == KeyCode::KP_Begin);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Home) == KeyCode::KP_Home);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_End) == KeyCode::KP_End);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Prior) == KeyCode::KP_Prior);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Next) == KeyCode::KP_Next);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Add) == KeyCode::KP_Add);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Subtract) == KeyCode::KP_Subtract);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Multiply) == KeyCode::KP_Multiply);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Divide) == KeyCode::KP_Divide);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Decimal) == KeyCode::KP_Decimal);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Separator) == KeyCode::KP_Separator);
}

// 小键盘导航键与主键盘导航键在 X11 上是**不同 keysym**，故本后端天然可区分——
// 这正是 Win32 侧做不到、必须借 lParam 扫描码的那一段。两组映射必须各自成立、互不串码。
AURORA_TEST_CASE(keypad_navigation_does_not_alias_mainboard_keys) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Home) !=
                      aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Home));
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_End) !=
                      aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_End));
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Prior) !=
                      aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Prior));
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Next) !=
                      aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Next));
    // Insert 同理：本后端分得开，故主键盘 Insert 与 `KP_Insert` 必须各归各档。
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Insert) !=
                      aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_Insert));
    // `KP_Begin`（0xFF9D）与 `KP_Home`（0xFF95）是**两个不同**的 keysym：前者是小键盘 5 无
    // NumLock 时的 Home 位，后者是小键盘 Home 键位。两者不得混为同一码。
    AURORA_TEST_CHECK(ks::AURORA_KEYSYM_KP_Begin != ks::AURORA_KEYSYM_KP_Home);
}

// KP_Equal（0xFFBD）在 KeyCode 里无对应码位（小键盘等号无独立 GUI 语义），故落 Unknown，
// 而不是臆造一个码位。这条锁住「不建模」这个决定。
AURORA_TEST_CASE(keypad_equal_has_no_code_and_falls_back_to_unknown) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(ks::AURORA_KEYSYM_KP_Equal) == KeyCode::Unknown);
}

AURORA_TEST_CASE(unmapped_keysym_falls_back_to_unknown) {
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(0UL) == KeyCode::Unknown);
    AURORA_TEST_CHECK(aurora::detail::keysym_to_keycode(0xFFFFFFFFUL) == KeyCode::Unknown);
}

}  // namespace aurora::test_cases::utest_keysym_map

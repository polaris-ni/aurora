/// 测试类型: unit
/// 目标单元: include/aurora/event/keycode.h
/// 测试说明: key_name 对字母/数字/导航/修饰/标点/功能键的全覆盖、未知与越界键码回退 Unknown、KeyCode
/// 分组连续性与相对次序

#include <type_traits>

#include "aurora/event/keycode.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_keycode {

AURORA_TEST_CASE(key_name_covers_letters_and_digits) {
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::A), "A");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::E), "E");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Z), "Z");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::D0), "0");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::D5), "5");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::D9), "9");
}

AURORA_TEST_CASE(key_name_covers_navigation_and_modifier_keys) {
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Escape), "Escape");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Enter), "Enter");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Tab), "Tab");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Backspace), "Backspace");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Delete), "Delete");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Space), "Space");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::ArrowLeft), "ArrowLeft");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::ArrowRight), "ArrowRight");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::ArrowUp), "ArrowUp");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::ArrowDown), "ArrowDown");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Shift), "Shift");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Control), "Control");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Alt), "Alt");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Meta), "Meta");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Home), "Home");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::End), "End");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::PageUp), "PageUp");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::PageDown), "PageDown");
}

AURORA_TEST_CASE(key_name_covers_punctuation_and_function_keys) {
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Minus), "Minus");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Equal), "Equal");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::LeftBracket), "LeftBracket");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::RightBracket), "RightBracket");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Backslash), "Backslash");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Semicolon), "Semicolon");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Quote), "Quote");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Comma), "Comma");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Period), "Period");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Slash), "Slash");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Backquote), "Backquote");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::F1), "F1");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::F5), "F5");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::F12), "F12");
}

AURORA_TEST_CASE(key_name_falls_back_to_unknown) {
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Unknown), "Unknown");
    // 越界值不在 switch 枚举列表内：走函数尾部的兜底返回
    // 越界取值正是本用例被测目标（验证 key_name 兜底返回 Unknown），不可改为合法枚举值。
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto bogus = static_cast<KeyCode>(999);
    AURORA_TEST_CHECK_STREQ(key_name(bogus), "Unknown");
}

AURORA_TEST_CASE(key_code_groups_are_contiguous_and_ordered) {
    static_assert(std::is_same_v<std::underlying_type_t<KeyCode>, int>);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Unknown), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::A), 1);  // 字母紧随 Unknown
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Z) - static_cast<int>(KeyCode::A), 25);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::D0) - static_cast<int>(KeyCode::Z), 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::D9) - static_cast<int>(KeyCode::D0), 9);
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::ArrowLeft), static_cast<int>(KeyCode::ArrowRight));
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::ArrowRight), static_cast<int>(KeyCode::ArrowUp));
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::ArrowUp), static_cast<int>(KeyCode::ArrowDown));
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::Shift), static_cast<int>(KeyCode::Control));
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::Control), static_cast<int>(KeyCode::Alt));
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::Alt), static_cast<int>(KeyCode::Meta));
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::F12) - static_cast<int>(KeyCode::F1), 11);
}

}  // namespace aurora::test_cases::utest_keycode

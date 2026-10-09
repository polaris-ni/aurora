/// 测试类型: unit
/// 目标单元: include/aurora/event/keycode.h
/// 测试说明: key_name 对字母/数字/导航/修饰/标点/功能键的全覆盖、未知与越界键码回退 Unknown、KeyCode
/// 分组连续性与相对次序

#include <string>
#include <string_view>
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
    constexpr auto bogus = static_cast<KeyCode>(999);
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

// 枚举值稳定性：逐个锁死既有项与新增的 KP_* 项。
//
// 为什么必须逐值断言（而不是只断言「分组连续」）：消费方普遍持有「KeyCode → 平台原生值」的
// **逐值对齐**映射表（如 `KeyCode::D5 → GLFW_KEY_5`），任何在枚举中间插入新项的改动都会让那些
// 表**静默错位**——不报错、只是全盘对不上。`KP_*` 段因此只能追加到末尾并写死显式数值，
// 这条用例就是那个「只能追加」纪律的守门。
AURORA_TEST_CASE(enum_values_are_pinned_for_cross_backend_alignment) {
    // 既有项：Unknown=0 与各分组起点锁死（中间项由「分组连续性」那条覆盖）。
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Unknown), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::A), 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Z), 26);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::D0), 27);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::D9), 36);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Escape), 37);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Enter), 38);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Tab), 39);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::F1), 66);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::F12), 77);

    // KP_* 段：从 100 起、连号、逐值锁死。首项写死 100 是「与既有段留出明确间隔」的可测形态。
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Insert), 100);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Delete), 101);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Begin), 102);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_End), 103);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Home), 104);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Prior), 105);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Next), 106);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Add), 107);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Subtract), 108);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Multiply), 109);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Divide), 110);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Decimal), 111);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_Separator), 112);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_0), 113);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_9), 122);

    // KP_0-9 必须连号：三后端都按「首项 + 偏移」做区间映射（Win32 的 VK_NUMPAD0、keysym 的
    // 0xFFB0、GLFW 的 GLFW_KEY_KP_0 各自连号），一旦在中间插项，这三处区间映射会一起失准。
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::KP_9) - static_cast<int>(KeyCode::KP_0), 9);
    // KP_* 段整体在既有项之后：追加而非插入。
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::F12), static_cast<int>(KeyCode::KP_Insert));
}

AURORA_TEST_CASE(key_name_covers_keypad_keys) {
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Insert), "KP_Insert");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Delete), "KP_Delete");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Begin), "KP_Begin");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_End), "KP_End");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Home), "KP_Home");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Prior), "KP_Prior");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Next), "KP_Next");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Add), "KP_Add");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Subtract), "KP_Subtract");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Multiply), "KP_Multiply");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Divide), "KP_Divide");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Decimal), "KP_Decimal");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_Separator), "KP_Separator");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_0), "KP_0");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_5), "KP_5");
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::KP_9), "KP_9");
}

// 主键盘 Insert：数值钉死 + 键名唯一。
//
// 数值为什么钉死在 123：它语义上属「编辑 / 导航」段，却**不**插回该段——既有段（0..94）靠隐式
// 连号，插一项会让其后全部取值整体位移，而消费方的「键码 → 平台原生值」逐值表会**静默错位**。
// 故取 `KP_9`（122）之后的下一个显式初值位，与 `KP_Insert = 100` 同一条「只许往后接」的纪律。
AURORA_TEST_CASE(insert_has_a_pinned_numeric_value) {
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Insert), 123);
    AURORA_TEST_CHECK_EQ(static_cast<int>(KeyCode::Insert) - static_cast<int>(KeyCode::KP_9), 1);
    // 追加而非插入：不得落在 KP_* 段内、也不得落在任何隐式连号段内。
    AURORA_TEST_CHECK_LT(static_cast<int>(KeyCode::KP_9), static_cast<int>(KeyCode::Insert));
}

// 键名唯一性为什么走「全枚举遍历」而不是只对拍 `KP_Insert`：只断言 `Insert != "KP_Insert"`
// 挡不住「新键复用了某个既有档的名字」——那种错位在逐值对齐的消费方里表现为某个键静默失效。
// 遍历把「name 恰好出现一次」钉成全局性质，任何重名都转红。
AURORA_TEST_CASE(key_name_of_insert_is_unique_across_all_codes) {
    AURORA_TEST_CHECK_STREQ(key_name(KeyCode::Insert), "Insert");
    int insert_name_count = 0;
    for (int i = 0; i <= static_cast<int>(KeyCode::Insert); ++i) {
        // 遍历会经过未使用的枚举取值（如 78..99 的间隔段），那正是本用例要覆盖的兜底形态。
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        const auto k = static_cast<KeyCode>(i);
        if (std::string_view{key_name(k)} == "Insert") {
            ++insert_name_count;
        }
    }
    AURORA_TEST_CHECK_EQ(insert_name_count, 1);
}

// key_code_from_name 与 key_name 同源：反查不另列键名表，而是逐项取 key_name 的输出比对。
// 因此「两边漂移」这类缺陷不可能发生——新增键位只改 key_name 一处，反查自动跟上；本用例钉住这一点。
AURORA_TEST_CASE(key_code_from_name_inverts_key_name_for_every_name) {
    int checked = 0;
    for (int i = 0; i < aurora::detail::AURORA_KEY_NAME_TABLE_SIZE; ++i) {
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        const auto k = static_cast<KeyCode>(i);
        const std::string_view name{key_name(k)};
        if (name == "Unknown") {
            continue;  // 枚举空洞与越界取值：key_name 的兜底串，不是键名
        }
        AURORA_TEST_TRACE(std::string(name));
        const KeyCode parsed = aurora::testing::require_value(key_code_from_name(name));
        AURORA_TEST_CHECK_EQ(parsed, k);
        ++checked;
    }
    // 守卫：0 命中也会「通过」，那样本用例就恒真了——必须真的扫到键名。
    AURORA_TEST_CHECK_GE(checked, 90);
}

AURORA_TEST_CASE(key_code_from_name_rejects_non_key_names) {
    // 占位名 "Unknown" 不可绑定：认下它等于把「解析不出来」假报成「绑定到了某个键」。
    AURORA_TEST_CHECK_FALSE(key_code_from_name("Unknown").has_value());
    AURORA_TEST_CHECK_FALSE(key_code_from_name("").has_value());
    AURORA_TEST_CHECK_FALSE(key_code_from_name("ctrl").has_value());  // 区分大小写
    AURORA_TEST_CHECK_FALSE(key_code_from_name("F13").has_value());  // 无此键位
    AURORA_TEST_CHECK_FALSE(key_code_from_name("ArrowLeft ").has_value());  // 尾随空格
    AURORA_TEST_CHECK_FALSE(key_code_from_name("Ctrl").has_value());  // 修饰位字面量不是键名
    const std::string non_ascii = "Entr\xC3\xA9";  // 非 ASCII 恒不命中（主键名一律 ASCII）
    AURORA_TEST_CHECK_FALSE(key_code_from_name(non_ascii).has_value());
}

}  // namespace aurora::test_cases::utest_keycode

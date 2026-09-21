/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/ime_composition.h
/// 测试说明: 平台组合索引 → Aurora 码点下标的纯折算——ASCII/CJK 串长度与光标、代理对内部向下夹紧、
/// 孤立代理替换计数、GCS_COMPATTR 目标段（含跨代理对边界）、属性数组缺尾/多余、
/// 无目标段时 sel_end 落哨兵、make_preedit_state 端到端字段；
/// UTF-8 口径（X11 XIM / Wayland text-input-v3）：字节下标→码点、多字节码点内部夹紧、
/// 半开区间选区折算、cursor=-1 退化串尾、越界夹紧

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/window/detail/ime_composition.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_ime_composition {

using aurora::ime::Attr;
using aurora::ime::CpRange;
using aurora::ime::make_preedit_state;
using aurora::ime::make_preedit_state_utf8;
using aurora::ime::target_selection;
using aurora::ime::utf16_index_to_cp_index;
using aurora::ime::utf8_byte_to_cp_index;

namespace {

/// @brief 由码点序列构造 UTF-16 串（BMP 直填，非 BMP 拆代理对），避免测试里手写 0xD83D。
auto u16(const std::vector<std::uint32_t> &cps) -> std::u16string {
    std::u16string out;
    for (const std::uint32_t cp : cps) {
        if (cp > 0xFFFFU) {
            const std::uint32_t v = cp - 0x10000U;
            out.push_back(static_cast<char16_t>(0xD800U + (v >> 10U)));
            out.push_back(static_cast<char16_t>(0xDC00U + (v & 0x3FFU)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

constexpr std::uint8_t AURORA_TARGET = static_cast<std::uint8_t>(Attr::TargetNotConverted);
constexpr std::uint8_t AURORA_DONE = static_cast<std::uint8_t>(Attr::Converted);

}  // namespace

AURORA_TEST_CASE(ascii_preedit_indices_pass_through) {
    // 纯 ASCII：单元数 == 码点数 == 字节数，索引原样。
    const auto e = make_preedit_state(u16({'n', 'i', 'h', 'a', 'o'}), 3, {});
    AURORA_TEST_CHECK_EQ(e.preedit, std::string("nihao"));
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(3));
    AURORA_TEST_CHECK_FALSE(e.has_preedit_selection());
    AURORA_TEST_CHECK(e.committed.empty());
}

AURORA_TEST_CASE(cjk_preedit_is_utf8_and_cursor_is_cp_index) {
    // 「你好」在 UTF-16 里 2 单元、UTF-8 里 6 字节；契约要求 UTF-8 串 + 码点下标。
    const auto e = make_preedit_state(u16({0x4F60U, 0x597DU}), 2, {});
    AURORA_TEST_CHECK_EQ(e.preedit, std::string("\xE4\xBD\xA0\xE5\xA5\xBD"));
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(2));
}

AURORA_TEST_CASE(surrogate_pair_caret_clamps_to_codepoint_start) {
    // 「你😀好」：单元布局 [你, 高, 低, 好]，码点布局 [0=你, 1=😀, 2=好]。
    const std::u16string s = u16({0x4F60U, 0x1F600U, 0x597DU});
    AURORA_TEST_CHECK_EQ(utf16_index_to_cp_index(s, 0), std::size_t(0));
    AURORA_TEST_CHECK_EQ(utf16_index_to_cp_index(s, 1), std::size_t(1));
    AURORA_TEST_CHECK_EQ(utf16_index_to_cp_index(s, 2), std::size_t(1));  // 代理对内部 → 夹紧到起点
    AURORA_TEST_CHECK_EQ(utf16_index_to_cp_index(s, 3), std::size_t(2));
    AURORA_TEST_CHECK_EQ(utf16_index_to_cp_index(s, 4), std::size_t(3));
    AURORA_TEST_CHECK_EQ(utf16_index_to_cp_index(s, 99), std::size_t(3));  // 越界夹紧到总数
}

AURORA_TEST_CASE(orphan_surrogate_becomes_replacement_char) {
    // 孤立高代理（无跟随低代理）不得产出 CESU-8 非法序列：与 U+FFFD 一一对应计 1 码点。
    const std::u16string s{u'\x4F60', u'\xD800'};
    const auto e = make_preedit_state(s, 2, {});
    AURORA_TEST_CHECK_EQ(e.preedit, std::string("\xE4\xBD\xA0\xEF\xBF\xBD"));
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(2));
}

AURORA_TEST_CASE(target_selection_maps_attr_run) {
    // 「nihao 你好」式混合：目标段落在单元 1..2（码点 1..2）。
    const std::u16string s = u16({'a', 'b', 'c', 'd'});
    const CpRange r = target_selection(s, {AURORA_DONE, AURORA_TARGET, AURORA_TARGET, AURORA_DONE});
    AURORA_TEST_CHECK_TRUE(r.has_selection());
    AURORA_TEST_CHECK_EQ(r.start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(r.end, std::size_t(2));
}

AURORA_TEST_CASE(target_selection_accepts_both_target_attrs) {
    // TargetConverted（已转换待替换）与 TargetNotConverted（待转换）同属目标段。
    const std::u16string s = u16({'a', 'b'});
    const CpRange r = target_selection(
        s, {static_cast<std::uint8_t>(Attr::TargetConverted), static_cast<std::uint8_t>(Attr::InputError)});
    AURORA_TEST_CHECK_TRUE(r.has_selection());
    AURORA_TEST_CHECK_EQ(r.start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(r.end, std::size_t(0));
}

AURORA_TEST_CASE(target_selection_without_target_has_no_range) {
    const std::u16string s = u16({'a', 'b', 'c'});
    AURORA_TEST_CHECK_FALSE(target_selection(s, {}).has_selection());
    AURORA_TEST_CHECK_FALSE(target_selection(s, {AURORA_DONE, AURORA_DONE, AURORA_DONE}).has_selection());
    const CpRange none = target_selection(s, {AURORA_DONE, AURORA_DONE, AURORA_DONE});
    AURORA_TEST_CHECK_EQ(none.end, TextCompositionEvent::AURORA_NO_SELECTION);
}

AURORA_TEST_CASE(target_selection_spanning_surrogate_pair_collapses_to_one_cp) {
    // 目标段覆盖 😀 的两个单元：起讫同属一个码点（含尾口径），不得产出区间倒置。
    const std::u16string s = u16({0x1F600U, 'x'});
    const CpRange r = target_selection(s, {AURORA_TARGET, AURORA_TARGET, AURORA_DONE});
    AURORA_TEST_CHECK_TRUE(r.has_selection());
    AURORA_TEST_CHECK_EQ(r.start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(r.end, std::size_t(0));
}

AURORA_TEST_CASE(target_selection_honours_shorter_or_longer_attr_array) {
    const std::u16string s = u16({'a', 'b', 'c'});
    // 缺尾（attrs 短于串）：按较短者生效，不越界读。
    const CpRange short_run = target_selection(s, {AURORA_TARGET});
    AURORA_TEST_CHECK_EQ(short_run.start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(short_run.end, std::size_t(0));
    // 多余（attrs 长于串）：越界部分忽略。
    const CpRange long_run = target_selection(s, {AURORA_DONE, AURORA_TARGET, AURORA_DONE, AURORA_DONE, AURORA_DONE});
    AURORA_TEST_CHECK_EQ(long_run.start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(long_run.end, std::size_t(1));
}

AURORA_TEST_CASE(empty_comp_means_composition_cancelled) {
    // 空组合串：preedit 空、光标 0、无选区（控件据此走 cancel 分支）。
    const auto e = make_preedit_state(u16({}), 0, {AURORA_TARGET});
    AURORA_TEST_CHECK(e.preedit.empty());
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(0));
    AURORA_TEST_CHECK_FALSE(e.has_preedit_selection());
}

AURORA_TEST_CASE(preedit_state_end_to_end_with_selection) {
    // 端到端：已转换段 + 待转换段，光标落在目标段起点。
    const std::u16string s = u16({0x4E00U, 0x4E8CU, 0x4E09U});  // 一二三
    const auto e = make_preedit_state(s, 1, {AURORA_DONE, AURORA_TARGET, AURORA_TARGET});
    AURORA_TEST_CHECK_EQ(e.preedit, std::string("\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89"));
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(1));
    AURORA_TEST_CHECK_TRUE(e.has_preedit_selection());
    AURORA_TEST_CHECK_EQ(e.sel_start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(e.sel_end, std::size_t(2));
}

// ---- UTF-8 口径（X11 XIM 组合串 / Wayland text-input-v3 index_in_text）----

AURORA_TEST_CASE(utf8_byte_to_cp_index_passes_through_and_clamps) {
    // 「你a好」：你=字节 0..2，a=3，好=4..6，总 7 字节、3 码点。
    constexpr std::string_view s =
        "\xE4\xBD\xA0"
        "a"
        "\xE5\xA5\xBD";
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 0), std::size_t(0));  // 「你」起点
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 3), std::size_t(1));  // a（ASCII）
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 4), std::size_t(2));  // 「好」起点
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 7), std::size_t(3));  // 串尾 = 总码点数
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 99), std::size_t(3));  // 越界夹紧
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 1), std::size_t(0));  // 切进「你」中部 → 夹紧起点
    AURORA_TEST_CHECK_EQ(utf8_byte_to_cp_index(s, 5), std::size_t(2));  // 切进「好」中部 → 夹紧起点
}

AURORA_TEST_CASE(utf8_preedit_cursor_and_no_selection) {
    // Wayland 形态：preedit 携 UTF-8 游标字节、无选区（v3 的 preedit_string 不带选区端点）。
    const auto e = make_preedit_state_utf8("\xE4\xBD\xA0\xE5\xA5\xBD", 3, -1, -1);  // 「你好」游标在第 2 字前
    AURORA_TEST_CHECK_EQ(e.preedit, std::string("\xE4\xBD\xA0\xE5\xA5\xBD"));
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(1));
    AURORA_TEST_CHECK_FALSE(e.has_preedit_selection());
    AURORA_TEST_CHECK(e.committed.empty());
}

AURORA_TEST_CASE(utf8_preedit_cursor_minus_one_falls_back_to_end) {
    // cursor=-1（输入法未给游标）→ 退化到串尾码点下标。
    const auto e = make_preedit_state_utf8("ni hao", -1, -1, -1);
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(6));
}

AURORA_TEST_CASE(utf8_preedit_half_open_selection_maps_to_inclusive_cp) {
    // 「一二三」每字 3 字节。半开区间 [3,6) 恰含「二」→ 含尾码点区间 [1,1]。
    constexpr std::string_view s = "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89";
    const auto one = make_preedit_state_utf8(s, 6, 3, 6);
    AURORA_TEST_CHECK_TRUE(one.has_preedit_selection());
    AURORA_TEST_CHECK_EQ(one.sel_start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(one.sel_end, std::size_t(1));
    // [0,6) 含「一二」→ 含尾 [0,1]。
    const auto two = make_preedit_state_utf8(s, 0, 0, 6);
    AURORA_TEST_CHECK_EQ(two.sel_start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(two.sel_end, std::size_t(1));
}

AURORA_TEST_CASE(utf8_preedit_empty_means_cancelled) {
    const auto e = make_preedit_state_utf8("", -1, -1, -1);
    AURORA_TEST_CHECK(e.preedit.empty());
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(0));
    AURORA_TEST_CHECK_FALSE(e.has_preedit_selection());
}

AURORA_TEST_CASE(utf8_preedit_selection_end_clamped_to_last_cp) {
    // 尾端点越界（半开区间 end ≥ 串长）→ 含尾码点夹紧到最后一个码点。
    constexpr std::string_view s = "\xE4\xBD\xA0\xE5\xA5\xBD";  // 「你好」2 码点
    const auto e = make_preedit_state_utf8(s, 0, 3, 99);
    AURORA_TEST_CHECK_EQ(e.sel_start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(e.sel_end, std::size_t(1));  // total-1
}

}  // namespace aurora::test_cases::utest_ime_composition

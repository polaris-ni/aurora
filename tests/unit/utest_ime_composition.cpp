/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/ime_composition.h
/// 测试说明: 平台组合索引 → Aurora 码点下标的纯折算——ASCII/CJK 串长度与光标、代理对内部向下夹紧、
/// 孤立代理替换计数、GCS_COMPATTR 目标段（含跨代理对边界）、属性数组缺尾/多余、
/// 无目标段时 sel_end 落哨兵、make_preedit_state 端到端字段

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/window/detail/ime_composition.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_ime_composition {

using aurora::ime::Attr;
using aurora::ime::CpRange;
using aurora::ime::make_preedit_state;
using aurora::ime::target_selection;
using aurora::ime::utf16_index_to_cp_index;

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

constexpr std::uint8_t kTarget = static_cast<std::uint8_t>(Attr::kTargetNotConverted);
constexpr std::uint8_t kDone = static_cast<std::uint8_t>(Attr::kConverted);

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
    const CpRange r = target_selection(s, {kDone, kTarget, kTarget, kDone});
    AURORA_TEST_CHECK_TRUE(r.has_selection());
    AURORA_TEST_CHECK_EQ(r.start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(r.end, std::size_t(2));
}

AURORA_TEST_CASE(target_selection_accepts_both_target_attrs) {
    // kTargetConverted（已转换待替换）与 kTargetNotConverted（待转换）同属目标段。
    const std::u16string s = u16({'a', 'b'});
    const CpRange r = target_selection(s, {static_cast<std::uint8_t>(Attr::kTargetConverted),
                                           static_cast<std::uint8_t>(Attr::kInputError)});
    AURORA_TEST_CHECK_TRUE(r.has_selection());
    AURORA_TEST_CHECK_EQ(r.start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(r.end, std::size_t(0));
}

AURORA_TEST_CASE(target_selection_without_target_has_no_range) {
    const std::u16string s = u16({'a', 'b', 'c'});
    AURORA_TEST_CHECK_FALSE(target_selection(s, {}).has_selection());
    AURORA_TEST_CHECK_FALSE(target_selection(s, {kDone, kDone, kDone}).has_selection());
    const CpRange none = target_selection(s, {kDone, kDone, kDone});
    AURORA_TEST_CHECK_EQ(none.end, TextCompositionEvent::AURORA_NO_SELECTION);
}

AURORA_TEST_CASE(target_selection_spanning_surrogate_pair_collapses_to_one_cp) {
    // 目标段覆盖 😀 的两个单元：起讫同属一个码点（含尾口径），不得产出区间倒置。
    const std::u16string s = u16({0x1F600U, 'x'});
    const CpRange r = target_selection(s, {kTarget, kTarget, kDone});
    AURORA_TEST_CHECK_TRUE(r.has_selection());
    AURORA_TEST_CHECK_EQ(r.start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(r.end, std::size_t(0));
}

AURORA_TEST_CASE(target_selection_honours_shorter_or_longer_attr_array) {
    const std::u16string s = u16({'a', 'b', 'c'});
    // 缺尾（attrs 短于串）：按较短者生效，不越界读。
    const CpRange short_run = target_selection(s, {kTarget});
    AURORA_TEST_CHECK_EQ(short_run.start, std::size_t(0));
    AURORA_TEST_CHECK_EQ(short_run.end, std::size_t(0));
    // 多余（attrs 长于串）：越界部分忽略。
    const CpRange long_run = target_selection(s, {kDone, kTarget, kDone, kDone, kDone});
    AURORA_TEST_CHECK_EQ(long_run.start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(long_run.end, std::size_t(1));
}

AURORA_TEST_CASE(empty_comp_means_composition_cancelled) {
    // 空组合串：preedit 空、光标 0、无选区（控件据此走 cancel 分支）。
    const auto e = make_preedit_state(u16({}), 0, {kTarget});
    AURORA_TEST_CHECK(e.preedit.empty());
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(0));
    AURORA_TEST_CHECK_FALSE(e.has_preedit_selection());
}

AURORA_TEST_CASE(preedit_state_end_to_end_with_selection) {
    // 端到端：已转换段 + 待转换段，光标落在目标段起点。
    const std::u16string s = u16({0x4E00U, 0x4E8CU, 0x4E09U});  // 一二三
    const auto e = make_preedit_state(s, 1, {kDone, kTarget, kTarget});
    AURORA_TEST_CHECK_EQ(e.preedit, std::string("\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89"));
    AURORA_TEST_CHECK_EQ(e.cursor_index, std::size_t(1));
    AURORA_TEST_CHECK_TRUE(e.has_preedit_selection());
    AURORA_TEST_CHECK_EQ(e.sel_start, std::size_t(1));
    AURORA_TEST_CHECK_EQ(e.sel_end, std::size_t(2));
}

}  // namespace aurora::test_cases::utest_ime_composition

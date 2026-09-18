/// 测试类型: unit
/// 目标单元: include/aurora/core/a11y_text.h
/// 测试说明: UTF-8↔UTF-16 偏移映射（含非 BMP 代理对的向下夹紧 G9）、双向转换往返、
///           按码点/UTF-16 单元前后移动、Word/Line/Character/Document 单位展开

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "aurora/core/a11y_text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_a11y_text {

namespace m = aurora::testing::matchers;

namespace {

using aurora::a11y::TextUnit;
using aurora::a11y::UtfOffsetMap;

/// @brief ASCII 文本：UTF-8 偏移与 UTF-16 偏移逐位相等（映射退化为恒等）。
constexpr std::string_view kAscii = "hello";

/// @brief 3 字节字符（CJK「中」）：1 个 UTF-16 单元、3 个 UTF-8 字节。
constexpr std::string_view kCjk = "中文";

/// @brief 非 BMP：emoji U+1F600 占 4 字节 UTF-8 / 2 个 UTF-16 单元。
constexpr std::string_view kEmoji = "\xF0\x9F\x98\x80";

}  // namespace

AURORA_TEST_CASE(ascii_offsets_are_identity) {
    const UtfOffsetMap map{kAscii};
    AURORA_TEST_CHECK_EQ(map.utf8_length(), std::size_t{5});
    AURORA_TEST_CHECK_EQ(map.utf16_length(), std::size_t{5});
    for (std::size_t i = 0; i <= 5; ++i) {
        AURORA_TEST_CHECK_EQ(map.to_utf16(i), i);
        AURORA_TEST_CHECK_EQ(map.to_utf8(i), i);
    }
}

AURORA_TEST_CASE(cjk_maps_bytes_to_single_utf16_unit) {
    const UtfOffsetMap map{kCjk};
    AURORA_TEST_CHECK_EQ(map.utf8_length(), std::size_t{6});
    AURORA_TEST_CHECK_EQ(map.utf16_length(), std::size_t{2});

    AURORA_TEST_CHECK_EQ(map.to_utf16(0), std::size_t{0});
    AURORA_TEST_CHECK_EQ(map.to_utf16(3), std::size_t{1});
    AURORA_TEST_CHECK_EQ(map.to_utf8(1), std::size_t{3});
    AURORA_TEST_CHECK_EQ(map.to_utf8(2), std::size_t{6});  // 末尾哨兵
}

AURORA_TEST_CASE(emoji_low_surrogate_index_clamps_to_codepoint_start) {
    // G9：UIA MoveEndpointByUnit(Character) 会把端点落到代理对第二单元（utf16==1），
    // 该索引在 UTF-8 侧无码点起点 —— 必须夹紧回 0，绝不产生指向码点中部的偏移。
    const UtfOffsetMap map{kEmoji};
    AURORA_TEST_CHECK_EQ(map.utf8_length(), std::size_t{4});
    AURORA_TEST_CHECK_EQ(map.utf16_length(), std::size_t{2});

    AURORA_TEST_CHECK_EQ(map.to_utf8(0), std::size_t{0});
    AURORA_TEST_CHECK_EQ(map.to_utf8(1), std::size_t{0});  // 低代理 → 夹紧
    AURORA_TEST_CHECK_EQ(map.to_utf8(2), std::size_t{4});  // 文末
    AURORA_TEST_CHECK_EQ(map.to_utf8(99), std::size_t{4});  // 越界 → 文末
}

AURORA_TEST_CASE(advance_utf8_walks_codepoints_and_clamps) {
    const UtfOffsetMap map{kCjk};
    AURORA_TEST_CHECK_EQ(map.advance_utf8(0, 1), std::size_t{3});
    AURORA_TEST_CHECK_EQ(map.advance_utf8(3, 1), std::size_t{6});
    AURORA_TEST_CHECK_EQ(map.advance_utf8(3, -1), std::size_t{0});
    AURORA_TEST_CHECK_EQ(map.advance_utf8(0, -1), std::size_t{0});  // 下界夹紧
    AURORA_TEST_CHECK_EQ(map.advance_utf8(0, 99), std::size_t{6});  // 上界夹紧
    AURORA_TEST_CHECK_EQ(map.advance_utf8(3, 0), std::size_t{3});  // count==0 原样返回
}

AURORA_TEST_CASE(advance_utf16_returns_utf8_after_clamping) {
    const UtfOffsetMap map{kEmoji};
    AURORA_TEST_CHECK_EQ(map.advance_utf16(0, 1), std::size_t{0});  // 落到低代理 → 夹紧
    AURORA_TEST_CHECK_EQ(map.advance_utf16(0, 2), std::size_t{4});
    AURORA_TEST_CHECK_EQ(map.advance_utf16(2, -2), std::size_t{0});
    AURORA_TEST_CHECK_EQ(map.advance_utf16(0, -5), std::size_t{0});  // 负向下溢夹紧
}

AURORA_TEST_CASE(utf8_utf16_roundtrip_preserves_text) {
    const std::string mixed = std::string{"a"} + std::string{kCjk} + std::string{kEmoji} + "z";
    const std::u16string wide = aurora::a11y::utf8_to_utf16(mixed);
    AURORA_TEST_CHECK_EQ(wide.size(), std::size_t{6});  // a 中 文 emoji(2) z
    AURORA_TEST_CHECK_STREQ(aurora::a11y::utf16_to_utf8(wide).c_str(), mixed.c_str());
}

AURORA_TEST_CASE(utf8_to_utf16_emits_surrogate_pair) {
    const std::u16string wide = aurora::a11y::utf8_to_utf16(kEmoji);
    AURORA_TEST_REQUIRE_EQ(wide.size(), std::size_t{2});
    AURORA_TEST_CHECK_EQ(static_cast<int>(wide[0]), 0xD83D);
    AURORA_TEST_CHECK_EQ(static_cast<int>(wide[1]), 0xDE00);
}

AURORA_TEST_CASE(utf16_to_utf8_replaces_lone_surrogates) {
    const std::u16string lone_high{static_cast<char16_t>(0xD83D)};
    AURORA_TEST_CHECK_STREQ(aurora::a11y::utf16_to_utf8(lone_high).c_str(), "\xEF\xBF\xBD");  // U+FFFD
    const std::u16string lone_low{static_cast<char16_t>(0xDE00)};
    AURORA_TEST_CHECK_STREQ(aurora::a11y::utf16_to_utf8(lone_low).c_str(), "\xEF\xBF\xBD");
}

AURORA_TEST_CASE(utf16_length_of_counts_surrogates_as_two) {
    AURORA_TEST_CHECK_EQ(aurora::a11y::utf16_length_of(kAscii), std::size_t{5});
    AURORA_TEST_CHECK_EQ(aurora::a11y::utf16_length_of(kCjk), std::size_t{2});
    AURORA_TEST_CHECK_EQ(aurora::a11y::utf16_length_of(kEmoji), std::size_t{2});
}

AURORA_TEST_CASE(expand_document_covers_whole_text) {
    const std::string text = "abc";
    const auto [s, e] = aurora::a11y::expand_to_unit(text, 1, TextUnit::Document);
    AURORA_TEST_CHECK_EQ(s, std::size_t{0});
    AURORA_TEST_CHECK_EQ(e, std::size_t{3});
}

AURORA_TEST_CASE(expand_line_splits_on_newline) {
    const std::string text = "ab\ncd\n ef";
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 3, TextUnit::Line);
        AURORA_TEST_CHECK_STREQ(text.substr(s, e - s).c_str(), "cd");
    }
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 0, TextUnit::Line);
        AURORA_TEST_CHECK_STREQ(text.substr(s, e - s).c_str(), "ab");
    }
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 7, TextUnit::Line);
        AURORA_TEST_CHECK_STREQ(text.substr(s, e - s).c_str(), " ef");
    }
}

AURORA_TEST_CASE(expand_word_splits_on_ascii_punctuation_and_space) {
    const std::string text = "foo bar,baz";
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 1, TextUnit::Word);
        AURORA_TEST_CHECK_STREQ(text.substr(s, e - s).c_str(), "foo");
    }
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 5, TextUnit::Word);
        AURORA_TEST_CHECK_STREQ(text.substr(s, e - s).c_str(), "bar");
    }
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 9, TextUnit::Word);
        AURORA_TEST_CHECK_STREQ(text.substr(s, e - s).c_str(), "baz");
    }
}

AURORA_TEST_CASE(expand_character_spans_one_codepoint) {
    const std::string text = std::string{kCjk} + "z";
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 0, TextUnit::Character);
        AURORA_TEST_CHECK_EQ(s, std::size_t{0});
        AURORA_TEST_CHECK_EQ(e, std::size_t{3});  // 「中」3 字节
    }
    {
        const auto [s, e] = aurora::a11y::expand_to_unit(text, 6, TextUnit::Character);
        AURORA_TEST_CHECK_EQ(s, std::size_t{6});
        AURORA_TEST_CHECK_EQ(e, std::size_t{7});
    }
}

AURORA_TEST_CASE(is_word_char_separates_whitespace_and_ascii_punct) {
    AURORA_TEST_CHECK_FALSE(aurora::a11y::is_word_char(' '));
    AURORA_TEST_CHECK_FALSE(aurora::a11y::is_word_char('\n'));
    AURORA_TEST_CHECK_FALSE(aurora::a11y::is_word_char(','));
    AURORA_TEST_CHECK_FALSE(aurora::a11y::is_word_char(0));
    AURORA_TEST_CHECK_TRUE(aurora::a11y::is_word_char('a'));
    AURORA_TEST_CHECK_TRUE(aurora::a11y::is_word_char(0x4E2D));  // 「中」按词内处理
}

}  // namespace aurora::test_cases::utest_a11y_text

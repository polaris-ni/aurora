/// 测试类型: unit
/// 目标单元: include/aurora/render/bitmap_font.h
/// 测试说明: 覆盖内置位图字形的网格不变量（8×8、行宽一致）、小写到大写的自动映射、
/// 未知字符降级为空格字形、数字/字母/常用标点具备前景像素，以及空格字形恒为空白

#include <array>
#include <cstddef>
#include <string>

#include "aurora/render/bitmap_font.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_bitmap_font {

namespace {
/// @brief 统计字形中的前景像素数（'#'）。
[[nodiscard]] auto ink_count(const render::Glyph &g) -> int {
    int count = 0;
    for (const char *row : g.rows) {
        for (const char cell : std::string_view{row}) {
            if (cell == '#') {
                ++count;
            }
        }
    }
    return count;
}
}  // namespace

AURORA_TEST_CASE(glyph_grid_is_eight_by_eight) {
    // 设计网格固定 8×8：绘制端按 CELL 步进，行宽不一致会越界读。
    AURORA_TEST_CHECK_EQ(render::BitmapFont::AURORA_CELL, 8);
    const render::Glyph &g = render::BitmapFont::glyph('A');
    AURORA_TEST_CHECK_EQ(g.rows.size(), 8U);
    for (const char *row : g.rows) {
        AURORA_TEST_CHECK_EQ(std::string{row}.size(), 8U);
    }
}

AURORA_TEST_CASE(lowercase_maps_to_uppercase_glyph) {
    // 无独立小写字形数据：查询时归一到大写，保证可读性且节省数据。
    const render::Glyph &lower = render::BitmapFont::glyph('a');
    const render::Glyph &upper = render::BitmapFont::glyph('A');
    AURORA_TEST_CHECK_EQ(lower.rows, upper.rows);
    AURORA_TEST_CHECK_EQ(ink_count(lower), ink_count(upper));
}

AURORA_TEST_CASE(letters_and_digits_have_ink) {
    for (const char c : std::string{"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"}) {
        AURORA_TEST_TRACE(std::string{"glyph "} + c);
        AURORA_TEST_CHECK_GT(ink_count(render::BitmapFont::glyph(c)), 0);
    }
}

AURORA_TEST_CASE(common_punctuation_has_ink) {
    for (const char c : std::string{".,:-_/()[]{}<>="}) {
        AURORA_TEST_TRACE(std::string{"glyph "} + c);
        AURORA_TEST_CHECK_GT(ink_count(render::BitmapFont::glyph(c)), 0);
    }
}

AURORA_TEST_CASE(space_glyph_is_blank) {
    AURORA_TEST_CHECK_EQ(ink_count(render::BitmapFont::glyph(' ')), 0);
}

AURORA_TEST_CASE(unknown_character_degrades_to_space) {
    // 未覆盖字符（含控制字符与非 ASCII 字节）降级为空格，避免出现豆腐块。
    AURORA_TEST_CHECK_EQ(render::BitmapFont::glyph('\x01').rows, render::BitmapFont::glyph(' ').rows);
    AURORA_TEST_CHECK_EQ(ink_count(render::BitmapFont::glyph('\x7F')), 0);
}

}  // namespace aurora::test_cases::utest_bitmap_font

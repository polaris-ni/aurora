/// 测试类型: unit
/// 目标单元: include/aurora/render/bitmap_font.h
/// 测试说明: 内置 8×8 位图字体（小写转大写、未知降级空格、pixel_size 缩放、measure 宽高）单元测试

#include <string>

#include "aurora/render/bitmap_font.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_bitmap_font {

using render::BitmapFont;

AURORA_TEST() {
    // ---- 1. 设计网格边长为 8 ----
    AURORA_TEST_CHECK_EQ(BitmapFont::AURORA_CELL, 8);

    // ---- 2. 小写字母映射到大写字形（同一静态字形对象） ----
    {
        AURORA_TEST_CHECK(&BitmapFont::glyph('a') == &BitmapFont::glyph('A'));
        AURORA_TEST_CHECK(&BitmapFont::glyph('z') == &BitmapFont::glyph('Z'));
    }

    // ---- 3. 未覆盖字符降级为空格字形（与 ' ' 同一对象） ----
    {
        AURORA_TEST_CHECK(&BitmapFont::glyph('\x01') == &BitmapFont::glyph(' '));
        // CJK 字节序列同样落到 default 分支 → 空格
        AURORA_TEST_CHECK(&BitmapFont::glyph(static_cast<char>(0xE4)) == &BitmapFont::glyph(' '));
    }

    // ---- 4. 字形数据：8 行，每行 8 列；'A' 首行为网格图案 ----
    {
        const render::Glyph &a = BitmapFont::glyph('A');
        AURORA_TEST_CHECK_EQ(std::string(a.rows[0]), std::string("  ####  "));
        bool all_eight = true;
        for (const char *row : a.rows) {
            all_eight = all_eight && (std::string(row).size() == 8);
        }
        AURORA_TEST_CHECK(all_eight);
        // 空格字形全空白
        const render::Glyph &sp = BitmapFont::glyph(' ');
        AURORA_TEST_CHECK_EQ(std::string(sp.rows[3]), std::string("        "));
    }

    // ---- 5. pixel_size：size_pt / 12 四舍五入，下限 1 ----
    {
        AURORA_TEST_CHECK_EQ(BitmapFont::pixel_size(12.0F), 1);
        AURORA_TEST_CHECK_EQ(BitmapFont::pixel_size(24.0F), 2);
        AURORA_TEST_CHECK_EQ(BitmapFont::pixel_size(36.0F), 3);
        AURORA_TEST_CHECK_EQ(BitmapFont::pixel_size(0.0F), 1);   // 下限保护
        AURORA_TEST_CHECK_EQ(BitmapFont::pixel_size(6.0F), 1);    // round(0.5)=1
    }

    // ---- 6. measure_width / measure_height ----
    {
        AURORA_TEST_CHECK_NEAR(BitmapFont::measure_width("ABC", 12.0F), 24.0F, 1e-3F);  // 3 * 8 * 1
        AURORA_TEST_CHECK_NEAR(BitmapFont::measure_width("AB", 24.0F), 32.0F, 1e-3F);   // 2 * 8 * 2
        AURORA_TEST_CHECK_NEAR(BitmapFont::measure_width("", 12.0F), 0.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(BitmapFont::measure_height(12.0F), 8.0F, 1e-3F);
        AURORA_TEST_CHECK_NEAR(BitmapFont::measure_height(24.0F), 16.0F, 1e-3F);
    }
}

}  // namespace aurora::test_cases::utest_bitmap_font

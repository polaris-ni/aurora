/// 测试类型: unit
/// 目标单元: include/aurora/render/noto_font_data.h
/// 测试说明: 覆盖内嵌 Noto Sans 数据的可访问性：span 非空且规模符合整包字体、sfnt 魔数为合法 TrueType 标识、
/// 多次调用返回同一数据视图（单一定义、不产生 TU 副本）

#include <cstddef>
#include <cstdint>

#include "aurora/render/noto_font_data.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_noto_font_data {

AURORA_TEST_CASE(embedded_font_is_non_empty_and_sized_like_a_full_font) {
    // 整包 Noto Sans Regular 约 431 KB：下界用于发现「数据被误裁/未链接」。
    const auto data = render::noto_sans_ttf();
    AURORA_TEST_CHECK_FALSE(data.empty());
    AURORA_TEST_CHECK_GT(data.size(), 100U * 1024U);
}

AURORA_TEST_CASE(embedded_font_carries_sfnt_magic) {
    // sfnt 版本：0x00010000（TrueType）或 'true' / 'ttcf'。魔数校验可拦住数据错位/字节序问题。
    const auto data = render::noto_sans_ttf();
    AURORA_TEST_REQUIRE_GE(data.size(), 4U);

    const bool truetype = data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00;
    const bool apple_true = data[0] == 't' && data[1] == 'r' && data[2] == 'u' && data[3] == 'e';
    const bool collection = data[0] == 't' && data[1] == 't' && data[2] == 'c' && data[3] == 'f';
    AURORA_TEST_CHECK_TRUE(truetype || apple_true || collection);
}

AURORA_TEST_CASE(repeated_access_returns_same_view) {
    // 数据本体留在单一 TU：两次调用须指向同一缓冲，否则说明头被改成了 inline 副本。
    const auto first = render::noto_sans_ttf();
    const auto second = render::noto_sans_ttf();
    AURORA_TEST_CHECK_EQ(first.size(), second.size());
    AURORA_TEST_CHECK_EQ(first.data(), second.data());
}

}  // namespace aurora::test_cases::utest_noto_font_data

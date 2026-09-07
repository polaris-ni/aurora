/// 测试类型: unit
/// 目标单元: include/aurora/widget/text_span.h
/// 测试说明: 富文本片段（默认黑色/默认字体、聚合初始化、i18n LocalizedString 片段、逐字段可覆盖）单元测试

#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/widget/text_span.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_text_span {

AURORA_TEST() {
    // ---- 1. 默认构造：黑色文本 + 默认字体 + 非本地化字面文本 ----
    {
        const TextSpan s;
        AURORA_TEST_CHECK(s.color == Color::black());
        AURORA_TEST_CHECK_EQ(std::string(s.font.family), std::string("sans-serif"));
        AURORA_TEST_CHECK_NEAR(s.font.size_pt, 14.0F, 1e-6F);
        AURORA_TEST_CHECK_EQ(s.font.weight, 400);
        AURORA_TEST_CHECK_FALSE(s.text.localize);
        AURORA_TEST_CHECK(s.text.text.empty());
    }

    // ---- 2. 聚合初始化（声明序 text / font / color） ----
    {
        Font f;
        f.size_pt = 20.0F;
        f.weight = 700;
        const TextSpan s{.text = LocalizedString{std::string("Hi")}, .font = f, .color = Color::red()};
        AURORA_TEST_CHECK_EQ(std::string(s.text.c_str()), std::string("Hi"));
        AURORA_TEST_CHECK_NEAR(s.font.size_pt, 20.0F, 1e-6F);
        AURORA_TEST_CHECK_EQ(s.font.weight, 700);
        AURORA_TEST_CHECK(s.color == Color::red());
    }

    // ---- 3. 指定初始化器按需覆盖单个字段，其余取默认 ----
    {
        const TextSpan s{.text = LocalizedString{"label"}, .font = Font{}, .color = Color::blue()};
        AURORA_TEST_CHECK(s.color == Color::blue());
        AURORA_TEST_CHECK(s.font == Font{});  // 未覆盖 → 默认字体
    }

    // ---- 4. 片段可承载待本地化文本（tr），供渲染期查表解析 ----
    {
        TextSpan s;
        s.text = LocalizedString::tr("greeting", {LocalizedString{"Aurora"}});
        AURORA_TEST_CHECK(s.text.localize);
        AURORA_TEST_CHECK_EQ(std::string(s.text.key), std::string("greeting"));
        AURORA_TEST_CHECK_EQ(s.text.args.size(), std::size_t{1});
    }
}

}  // namespace aurora::test_cases::utest_text_span

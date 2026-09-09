/// 测试类型: unit
/// 目标单元: include/aurora/core/string_util.h
/// 测试说明: 覆盖 aurora::internal::string_format 的整数/字符串/浮点格式化、%% 转义、按需扩容与空格式串/空指针退化路径

#include <numbers>
#include <string>

#include "aurora/core/string_util.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_string_util {

AURORA_TEST_CASE(formats_integers_and_text) {
    const auto text = aurora::internal::string_format("x=%d y=%d", 1, 42);
    AURORA_TEST_CHECK_STREQ(text, "x=1 y=42");

    const auto negative = aurora::internal::string_format("v=%d", -7);
    AURORA_TEST_CHECK_STREQ(negative, "v=-7");
}

AURORA_TEST_CASE(formats_strings_chars_and_floats) {
    const std::string name{"aurora"};
    const auto text = aurora::internal::string_format("%s|%c|%.2f", name.c_str(), 'A', std::numbers::pi);
    AURORA_TEST_CHECK_STREQ(text, "aurora|A|3.14");
}

AURORA_TEST_CASE(expands_percent_literal) {
    const auto text = aurora::internal::string_format("100%% %d%%", 30);
    AURORA_TEST_CHECK_STREQ(text, "100% 30%");
}

AURORA_TEST_CASE(grows_beyond_fixed_buffer_sizes) {
    // 结果超过常见栈缓冲（如 256/1024 字节）：验证「先量测再按需扩容」的实现能完整产出。
    const std::string half(600, 'a');
    const auto text = aurora::internal::string_format("%s-%s", half.c_str(), half.c_str());
    AURORA_TEST_REQUIRE_EQ(text.size(), 1201U);  // 600 + 1('-') + 600
    AURORA_TEST_CHECK_THAT(text, aurora::testing::matchers::starts_with("aaaa"));
    AURORA_TEST_CHECK_THAT(text, aurora::testing::matchers::ends_with("aaaa"));
    AURORA_TEST_CHECK(text.find('-') == 600U);
}

AURORA_TEST_CASE(empty_format_yields_empty_string) {
    // 空格式串：vsnprintf 量测结果为 0，实现按「失败/无产出」返回空串。
    const auto text = aurora::internal::string_format("");
    AURORA_TEST_CHECK(text.empty());
}

AURORA_TEST_CASE(null_format_yields_empty_string) {
    // 契约明确：fmt == nullptr 不解引用、返回空串（实现入口短路）。
    const auto text = aurora::internal::string_format(nullptr);
    AURORA_TEST_CHECK(text.empty());
}

}  // namespace aurora::test_cases::utest_string_util

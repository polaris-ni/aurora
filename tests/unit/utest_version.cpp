/// 测试类型: unit
/// 目标单元: include/aurora/core/version.h
/// 测试说明: 覆盖版本宏的两级字符串化、数字段三元组格式、完整 semver 串与后缀开关的组合契约

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "aurora/core/version.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_version {

namespace m = aurora::testing::matchers;  // 匹配器工厂别名（禁止 using-directive）

/// @brief 判定非空纯数字串（semver 数字分量）。
[[nodiscard]] static auto all_digits(std::string_view text) -> bool {
    return !text.empty() &&
           std::ranges::all_of(text, [](char c) -> bool { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

/// @brief 两级字符串化宏把数字分量展开为十进制串（CMake 注入值同样适用）。
AURORA_TEST_CASE(stringification_macros_expand_components) {
    AURORA_TEST_CHECK_STREQ(AURORA_VERSION_STR(AURORA_VERSION_MAJOR), std::to_string(AURORA_VERSION_MAJOR));
    AURORA_TEST_CHECK_STREQ(AURORA_VERSION_STR(AURORA_VERSION_MINOR), std::to_string(AURORA_VERSION_MINOR));
    AURORA_TEST_CHECK_STREQ(AURORA_VERSION_STR(AURORA_VERSION_PATCH), std::to_string(AURORA_VERSION_PATCH));
    AURORA_TEST_CHECK_STREQ(AURORA_VERSION_STR2(7), "7");
}

/// @brief 数字段为「MAJOR.MINOR.PATCH」三元组：两个分隔点、逐段纯数字、与分量宏组合一致。
AURORA_TEST_CASE(numeric_version_is_triplet_of_digits) {
    const std::string numeric = AURORA_VERSION_NUMERIC;
    const auto first_dot = numeric.find('.');
    const auto last_dot = numeric.rfind('.');
    AURORA_TEST_REQUIRE(first_dot != std::string::npos);
    AURORA_TEST_REQUIRE(last_dot != std::string::npos);
    AURORA_TEST_REQUIRE(first_dot != last_dot);  // 恰好两个点
    AURORA_TEST_CHECK(all_digits(numeric.substr(0, first_dot)));
    AURORA_TEST_CHECK(all_digits(numeric.substr(first_dot + 1, last_dot - first_dot - 1)));
    AURORA_TEST_CHECK(all_digits(numeric.substr(last_dot + 1)));

    const std::string composed = std::to_string(AURORA_VERSION_MAJOR) + "." + std::to_string(AURORA_VERSION_MINOR) +
                                 "." + std::to_string(AURORA_VERSION_PATCH);
    AURORA_TEST_CHECK_EQ(numeric, composed);
}

/// @brief 完整版本串 = 数字段 [- 后缀]，按 AURORA_HAS_VERSION_SUFFIX 组合。
AURORA_TEST_CASE(full_version_string_composes_with_suffix_flag) {
    static_assert(AURORA_HAS_VERSION_SUFFIX == 0 || AURORA_HAS_VERSION_SUFFIX == 1);
    const std::string numeric = AURORA_VERSION_NUMERIC;
#if AURORA_HAS_VERSION_SUFFIX
    const std::string expected = numeric + "-" + AURORA_VERSION_SUFFIX_STR;
#else
    const std::string expected = numeric;
#endif
    AURORA_TEST_CHECK_EQ(AURORA_VERSION_STRING, expected);
    AURORA_TEST_CHECK_THAT(std::string{AURORA_VERSION_STRING}, m::starts_with(numeric));
}

/// @brief 后缀串符合 semver 预发布段形态：非空、无前导 '-'、不含空白；稳定版回退为纯数字段。
AURORA_TEST_CASE(suffix_string_is_semver_prerelease_token) {
#if AURORA_HAS_VERSION_SUFFIX
    const std::string suffix = AURORA_VERSION_SUFFIX_STR;
    AURORA_TEST_REQUIRE_FALSE(suffix.empty());
    AURORA_TEST_CHECK_NE(suffix.front(), '-');
    AURORA_TEST_CHECK_MSG(
        std::ranges::none_of(suffix, [](char c) -> bool { return std::isspace(static_cast<unsigned char>(c)) != 0; }),
        "semver 预发布段不允许空白字符");
#else
    // 稳定版：完整串必须与数字段完全一致（后缀缺失路径）。
    AURORA_TEST_CHECK_EQ(AURORA_VERSION_STRING, AURORA_VERSION_NUMERIC);
#endif
}

}  // namespace aurora::test_cases::utest_version

#pragma once

// ============================================================
// 测试框架（tests/framework/）—— 匹配器（AURORA_TEST_CHECK_THAT）
// ------------------------------------------------------------
// 对标 GoogleTest 的 `EXPECT_THAT(value, Matcher)`。匹配器 = 「泛型谓词 + 人类可读描述」，
// 由工厂函数组装、可嵌套（`all_of(ge(2), le(5))`、`each(ge(0))`、`size_is(3)` ...）。
//
// 刻意不做 `Matcher<T>` 那种「按被匹配类型定型」的形态：谓词以 `const auto&` 接收，
// 在使用点才实例化，于是 `str_eq("abc")` 对 `std::string` / `std::string_view` / C 字符串
// 同样可用，容器类匹配器也不必在声明处推断容器类型（等价于 gtest 的 polymorphic matcher，
// 但不需要 SafeMatcherCast 那层类型体操）。
//
// 取值表等「跨使用点复用」的匹配器请用 `eq` 而非 `ref_eq`：前者按值捕获期望值副本，
// 后者仅在期望对象生命周期确实长于断言时才安全（如具名静态对象）。
// ============================================================

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "assertions.h"
#include "test_types.h"
#include "value_print.h"

namespace aurora::testing {

/// @brief 已绑定的匹配器：泛型谓词 + 失败时用于诊断的描述文本。
template <typename Pred>
class MatcherLike {
  public:
    MatcherLike(Pred pred, std::string description) : pred_(std::move(pred)), description_(std::move(description)) {}

    /// @brief 判定（在使用点按值的类型实例化谓词）。
    template <typename T>
    [[nodiscard]] auto matches(const T& value) const -> bool {
        return pred_(value);
    }

    /// @brief 期望描述（进入失败信息）。
    [[nodiscard]] auto describe() const -> const std::string& { return description_; }

  private:
    Pred pred_;
    std::string description_;
};

/// @brief 用分隔符串联若干描述文本（组合器的诊断）。
[[nodiscard]] inline auto join_descriptions([[maybe_unused]] std::string_view separator, std::string_view first)
    -> std::string {
    return std::string{first};
}

template <typename... Rest>
[[nodiscard]] auto join_descriptions(std::string_view separator, std::string_view first, Rest... rest) -> std::string {
    return std::string{first} + std::string{separator} + join_descriptions(separator, rest...);
}

/// @brief 匹配器工厂与组合器。
namespace matchers {

/// @brief 与期望值相等（按值捕获期望副本）。
template <typename Expected>
[[nodiscard]] auto eq(const Expected& expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return value == expected; },
                       "equals " + print_value(expected)};
}

/// @brief 与期望值不等。
template <typename Expected>
[[nodiscard]] auto ne(const Expected& expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return value != expected; },
                       "not equals " + print_value(expected)};
}

/// @brief 严格小于 / 小于等于。
template <typename Expected>
[[nodiscard]] auto lt(const Expected& expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return value < expected; },
                       "is less than " + print_value(expected)};
}

template <typename Expected>
[[nodiscard]] auto le(const Expected& expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return value <= expected; },
                       "is less than or equal to " + print_value(expected)};
}

/// @brief 严格大于 / 大于等于。
template <typename Expected>
[[nodiscard]] auto gt(const Expected& expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return value > expected; },
                       "is greater than " + print_value(expected)};
}

template <typename Expected>
[[nodiscard]] auto ge(const Expected& expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return value >= expected; },
                       "is greater than or equal to " + print_value(expected)};
}

/// @brief 字符串按内容相等（C 字符串 / std::string / string_view 皆可）。
[[nodiscard]] inline auto str_eq(std::string_view expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return std::string_view{value} == expected; },
                       "equals (string content) " + detail::quote(expected)};
}

/// @brief 字符串按内容不等。
[[nodiscard]] inline auto str_ne(std::string_view expected) {
    return MatcherLike{[expected](const auto& value) -> bool { return std::string_view{value} != expected; },
                       "not equals (string content) " + detail::quote(expected)};
}

/// @brief 字符串按内容相等，忽略 ASCII 大小写。
[[nodiscard]] inline auto str_case_eq(std::string_view expected) {
    return MatcherLike{[expected](const auto& value) -> bool {
                           return detail::strings_equal(std::string_view{value}, expected, false);
                       },
                       "equals (case-insensitive) " + detail::quote(expected)};
}

/// @brief 容器含指定元素。
template <typename Element>
[[nodiscard]] auto contains(const Element& element) {
    return MatcherLike{[element](const auto& container) -> bool {
                           using std::begin;
                           using std::end;
                           return std::find(begin(container), end(container), element) != end(container);
                       },
                       "contains " + print_value(element)};
}

/// @brief 字符串含指定子串。
[[nodiscard]] inline auto has_substr(std::string_view needle) {
    return MatcherLike{
        [needle](const auto& text) -> bool { return std::string_view{text}.find(needle) != std::string_view::npos; },
        "has substring " + detail::quote(needle)};
}

/// @brief 字符串以指定前缀开头。
[[nodiscard]] inline auto starts_with(std::string_view prefix) {
    return MatcherLike{[prefix](const auto& text) -> bool { return std::string_view{text}.starts_with(prefix); },
                       "starts with " + detail::quote(prefix)};
}

/// @brief 字符串以指定后缀结尾。
[[nodiscard]] inline auto ends_with(std::string_view suffix) {
    return MatcherLike{[suffix](const auto& text) -> bool { return std::string_view{text}.ends_with(suffix); },
                       "ends with " + detail::quote(suffix)};
}

/// @brief 容器大小等于 expected。
[[nodiscard]] inline auto size_is(std::size_t expected) {
    return MatcherLike{[expected](const auto& container) -> bool {
                           using std::begin;
                           using std::end;
                           return static_cast<std::size_t>(std::distance(begin(container), end(container))) == expected;
                       },
                       "has size " + std::to_string(expected)};
}

/// @brief 容器为空。
[[nodiscard]] inline auto is_empty() {
    return MatcherLike{[](const auto& container) -> bool {
                           using std::begin;
                           using std::end;
                           return begin(container) == end(container);
                       },
                       "is empty"};
}

/// @brief 容器每个元素都满足子匹配器。
template <typename Matcher>
[[nodiscard]] auto each(const Matcher& matcher) {
    return MatcherLike{[matcher](const auto& container) -> bool {
                           using std::begin;
                           using std::end;
                           for (auto it = begin(container); it != end(container); ++it) {
                               if (!matcher.matches(*it)) {
                                   return false;
                               }
                           }
                           return true;
                       },
                       "every element is " + matcher.describe()};
}

/// @brief 全部子匹配器都成立。
template <typename... Matchers>
[[nodiscard]] auto all_of(Matchers... matchers) {
    return MatcherLike{[matchers...](const auto& value) -> bool { return (matchers.matches(value) && ...); },
                       join_descriptions(" and ", matchers.describe()...)};
}

/// @brief 任一子匹配器成立。
template <typename... Matchers>
[[nodiscard]] auto any_of(Matchers... matchers) {
    return MatcherLike{[matchers...](const auto& value) -> bool { return (matchers.matches(value) || ...); },
                       join_descriptions(" or ", matchers.describe()...)};
}

/// @brief 子匹配器不成立。
template <typename Matcher>
[[nodiscard]] auto negated(const Matcher& matcher) {
    return MatcherLike{[matcher](const auto& value) -> bool { return !matcher.matches(value); },
                       "not " + matcher.describe()};
}

}  // namespace matchers

namespace detail {

/// @brief `CHECK_THAT` 内核：不匹配时给出 Actual / Expected 两侧信息。
template <typename T, typename Matcher>
[[nodiscard]] auto match_that_message(const T& value, const Matcher& matcher, std::string_view text) -> std::string {
    if (matcher.matches(value)) {
        return {};
    }
    return "Value of: " + std::string{text} + "\n    Actual: " + print_value(value) +
           "\n  Expected: " + matcher.describe();
}

}  // namespace detail
}  // namespace aurora::testing

// ---------------------------------------------------------------------------
// 宏层
// ---------------------------------------------------------------------------

/// @brief 非致命匹配器断言（对标 `EXPECT_THAT`）。
#define AURORA_TEST_CHECK_THAT(value, ...) \
    AURORA_TEST_CHECK_REPORT_(::aurora::testing::detail::match_that_message((value), (__VA_ARGS__), #value))

/// @brief 致命匹配器断言（对标 `ASSERT_THAT`）。
#define AURORA_TEST_REQUIRE_THAT(value, ...) \
    AURORA_TEST_REQUIRE_REPORT_(::aurora::testing::detail::match_that_message((value), (__VA_ARGS__), #value))

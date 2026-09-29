#pragma once

// ============================================================================
// 测试框架（tests/framework/）—— JSON 严格读取辅助
// ----------------------------------------------------------------------------
// `au::json::Value` 刻意取「指针 / Result」双出口而不抛异常（`at` 缺失返回 nullptr，
// `get<T>` 返回 Result），每个调用点都须自判结果。直接铺开关乎 `nullptr` 的三元表达式，
// 会让断言的本意被判空噪音淹没；本头把「缺失 / 越界 / 类型不符 ⇒ 本用例致命失败」
// 这条测试中通行的处置收成三个具名入口，从而在用例里恢复「读字段」这一件事的表达力。
//
// 与 `json_literals.h` 同宗：**不是**迁移期的兼容层——不引用任何第三方 JSON 库，
// 只在新 API 之上提供测试专用糖；迁移收尾后照常保留。
//
// 失败机制：`AURORA_TEST_REQUIRE` 在致命级别抛 `CaseAbort`（见 test_assert.cpp），
// 故下列函数在断言失败后不会真正返回，返回值可直接使用而无需再判空。
// ============================================================================

#include <cstddef>
#include <string_view>
#include <utility>

#include "assertions.h"
#include "aurora/core/json.h"

namespace aurora::testing {

/// @brief 断言对象含该键并返回其值；缺失或不是对象即本用例致命失败。
[[nodiscard]] inline auto require_child(const json::Value &parent, std::string_view key) -> const json::Value * {
    const auto *child = parent.at(key);
    AURORA_TEST_REQUIRE(child != nullptr);
    return child;
}

/// @brief 断言数组含该下标并返回元素；越界或不是数组即本用例致命失败。
[[nodiscard]] inline auto require_child_at(const json::Value &parent, std::size_t index) -> const json::Value * {
    const auto *child = parent.at(index);
    AURORA_TEST_REQUIRE(child != nullptr);
    return child;
}

/// @brief 断言字段存在且值可窄化为 T；缺失或类型不符即本用例致命失败。
template <json::json_readable T>
[[nodiscard]] inline auto require_field(const json::Value &object, std::string_view key) -> T {
    auto value = object.get<T>(key);
    AURORA_TEST_REQUIRE(value);
    return value.value();
}

}  // namespace aurora::testing

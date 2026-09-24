#pragma once

// ============================================================================
// literals.h — cli 模块的内部实现头（不安装、不进 include/、不进 aurora_api.json）
// ----------------------------------------------------------------------------
// `args.cpp`（扫描器）与 `command.cpp`（校验 / 帮助 / schema）共用同一套字面量转换，
// 避免「validate 认为合法而 parse 拒收」这类双实现漂移。
// ============================================================================

#include <string>
#include <string_view>
#include <utility>

#include "aurora/cli/args.h"
#include "aurora/core/result.h"

namespace aurora::cli::detail {

/// @brief 构造 `Value` 的唯一后门：仅暴露给本模块的两个实现 TU，公共 API 不因此放宽。
class LiteralFactory {
  public:
    [[nodiscard]] static auto make(ValueKind kind, Value::Raw raw, std::string literal) -> Value {
        return Value{kind, std::move(raw), std::move(literal)};
    }

    LiteralFactory() = delete;
};

/**
 * @brief 按声明类型把 token 转为 `Value`。
 * @param option_display 错误回显用的选项名（`--width` 或位置参数名）。
 * @return 失败返回 `cli-invalid-value`（携带 kind 与原文）。
 */
[[nodiscard]] auto convert_literal(ValueKind kind, std::string_view token, std::string_view option_display)
    -> Result<Value>;

/// @brief 构造 `cli-invalid-value`（供本模块各失败点复用，保证参数口径一致）。
[[nodiscard]] auto invalid_literal(std::string_view option_display, ValueKind kind, std::string_view token) -> Error;

/**
 * @brief 数值边界的可读文本：整值不带小数位（`1` 而非 `1.000000`），非整值去掉尾零。
 *
 * `command.cpp` 帮助里的 `[range: ...]` 与 `args.cpp` 的 `cli-range-violated` 错误文案共用它，
 * 避免出现「帮助写 `[1, 8192]`、报错写 `[1.000000, 8192.000000]`」的双口径漂移。
 */
[[nodiscard]] inline auto bound_text(double value) -> std::string {
    if (const auto whole = static_cast<long long>(value); static_cast<double>(whole) == value) {
        return std::to_string(whole);
    }
    std::string text = std::to_string(value);
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

/// @brief 该 kind 是否走数值分支（决定 min/max 取值域检查是否适用）。
[[nodiscard]] auto is_numeric_kind(ValueKind kind) noexcept -> bool;

}  // namespace aurora::cli::detail

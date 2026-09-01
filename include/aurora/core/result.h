#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

#include "aurora/core/error_codes.h"

namespace aurora {

/**
 * @brief 结构化错误（需求 #9：机器可解析错误）。
 *
 * 字段分两类受众：
 * - 进程外（JSON/日志/IDE 工具）：只认 `code`（冻结 slug，如 "nav-depth-exceeded"
 *   改名标识符也不变）与 `code_enum`（C++ 标识符，调试用）。
 * - 进程内：用 `code_enum` 做 `err.code_enum == ErrorCode::X` 类型安全分支，用 `severity`/
 *   `category`/`auto_fixable`/`fix_category`/`retryable` 元数据做策略判断。
 *
 * 所有元数据由 codespec/errors.toml 经生成器产出，经 make_error 自动填充，无需手填。
 * @note Thread: thread-safe
 * @note Side-effects: none
 */
struct Error {
    std::string code;       ///< 冻结对外 slug（如 "nav-depth-exceeded"），只增不删
    std::string message;    ///< 人类可读描述（由表模板 + params 渲染，或调用方覆盖）
    std::string suggestion; ///< 可选：修复建议
    std::string docs;       ///< 可选：文档链接/章节
    std::string where;      ///< 可选：发生位置（file:line）
    std::string hint;       ///< 修复提示（来自表，可被 make_error 覆盖）
    // 编译期枚举码 + 表驱动元数据
    ErrorCode code_enum{};                           ///< 编译期枚举码（默认 GeneralUnknown）
    ErrorSeverity severity = ErrorSeverity::Error;   ///< 来自 errors.toml
    ErrorCategory category = ErrorCategory::General; ///< 来自 errors.toml
    bool auto_fixable = false;                       ///< 来自 errors.toml
    bool retryable = false;                          ///< 来自 errors.toml
    std::string fix_category;                        ///< 修复策略分类（如 "type_error"|"missing_prop"）
    std::string fix_params;                          ///< 修复参数（JSON 字符串形式）

    [[nodiscard]] auto to_json() const -> std::string;
};

/// @brief 查表填充 Error 的 slug/severity/category/fix 元数据与一个 message。
/// @internal make_error 各重载共用此实现，确保元数据来源唯一。
[[nodiscard]] inline auto make_error_from_table(ErrorCode code, const std::string &message, std::string hint) -> Error {
    const auto &m = AURORA_ERROR_TABLE.at(static_cast<std::size_t>(code));
    return Error{
        .code = std::string(m.slug),
        .message = message,
        .hint = hint.empty() ? std::string(m.hint) : std::move(hint),
        .code_enum = code,
        .severity = m.severity,
        .category = m.category,
        .auto_fixable = m.auto_fixable,
        .retryable = m.retryable,
        .fix_category = std::string(m.fix_category),
        .fix_params = {},
    };
}

/// @brief 构造错误（主入口）：用 errors.toml 的 message 模板渲染 message。
/// @param code 编译期枚举码
/// @param params message 模板的 {placeholder} 键值表
/// @param hint 可选，覆盖表中的默认 hint
[[nodiscard]] inline auto make_error(ErrorCode code, const ErrorParams &params = {}, std::string hint = {}) -> Error {
    const auto &m = AURORA_ERROR_TABLE.at(static_cast<std::size_t>(code));
    return make_error_from_table(code, format_message(m.message_tpl, params), std::move(hint));
}

/// @brief 构造错误：调用方自定义 message（覆盖表模板），其余元数据仍来自表。
[[nodiscard]] inline auto make_error(ErrorCode code, const std::string &message, const ErrorParams &params = {},
                                     std::string hint = {}) -> Error {
    (void)params; // 调用方已提供最终 message，模板省略
    return make_error_from_table(code, message, std::move(hint));
}

/// @brief 构造错误（向后兼容）：枚举 + message + suggestion/docs/where。
///         slug/severity/category/fix 元数据仍来自表。
[[nodiscard]] inline auto make_error(ErrorCode code,
                                     const std::string &message, // NOLINT(bugprone-easily-swappable-parameters)
                                     std::string suggestion, std::string docs = {}, std::string where = {}) -> Error {
    auto e = make_error_from_table(code, message, {});
    e.suggestion = std::move(suggestion);
    e.docs = std::move(docs);
    e.where = std::move(where);
    return e;
}

/**
 * @brief 结果类型：成功持 T，失败持结构化 Error（需求 #9：统一失败路径）。
 * @tparam T 成功时的值类型。
 * @note Thread: thread-safe
 * @note Side-effects: none
 */
template<typename T> class Result {
  public:
    Result(T value) : m_data(std::move(value)) {} // NOLINT：成功值隐式构造
    Result(Error err) : m_data(std::move(err)) {} // NOLINT：错误隐式构造

    [[nodiscard]] auto ok() const -> bool { return std::holds_alternative<T>(m_data); }

    /// @brief 布尔语境：成功为 true（供 `if (result)` 使用）。
    explicit operator bool() const { return ok(); }

    [[nodiscard]] auto value() const -> const T & { return std::get<T>(m_data); }
    [[nodiscard]] auto value() -> T & { return std::get<T>(m_data); }
    [[nodiscard]] auto error() const -> const Error & { return std::get<Error>(m_data); }

    /// @brief 解包：成功返回值，失败抛 std::runtime_error（仅用于不可恢复场景）。
    [[nodiscard]] auto unwrap() const -> T {
        if (!ok()) {
            throw std::runtime_error(error().message);
        }
        return value();
    }

  private:
    std::variant<T, Error> m_data;
};

/**
 * @brief `Result<void>` 特化：仅表示成功/失败，无成功值（用于 `flush`/`reload` 等
 * 只关心“是否出错”的接口，统一失败路径，对齐需求 #9）。
 *
 * 注：`std::variant<void, ...>` 非法（void 非对象类型），故用 `bool` 标记成功态。
 * @note Thread: thread-safe
 * @note Side-effects: none
 */
template<> class Result<void> {
  public:
    Result() : m_ok(true) {}                     // 成功
    Result(Error err) : m_err(std::move(err)) {} // 失败

    [[nodiscard]] auto ok() const -> bool { return m_ok; }
    explicit operator bool() const { return m_ok; }
    [[nodiscard]] auto error() const -> const Error & { return m_err; }

  private:
    bool m_ok = false;
    Error m_err;
};

} // namespace aurora

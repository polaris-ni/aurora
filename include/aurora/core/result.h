#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

#include "aurora/core/error_codes.h"

namespace aurora {

/// @brief 结构化错误（需求 #9：机器可解析错误）。
///
/// 字段分两类受众：
/// - 进程外（JSON/日志/IDE 工具）：只认 `code`（冻结 slug，如 "nav-depth-exceeded"
/// 改名标识符也不变）与 `code_enum`（C++ 标识符，调试用）。
/// - 进程内：用 `code_enum` 做 `err.code_enum == ErrorCode::X` 类型安全分支，用 `severity`/
/// `category`/`auto_fixable`/`fix_category`/`retryable` 元数据做策略判断。
///
/// 所有元数据由 codespec/errors.toml 经生成器产出，经 make_error 自动填充，无需手填。
/// @note Thread: thread-safe
/// @note Side-effects: none
struct Error {
    std::string code;  ///< 冻结对外 slug（如 "nav-depth-exceeded"），只增不删
    std::string message;  ///< 人类可读描述（由表模板 + params 渲染，或调用方覆盖）
    std::string suggestion;  ///< 可选：修复建议
    std::string docs;  ///< 可选：文档链接/章节
    std::string where;  ///< 可选：发生位置（file:line）
    std::string hint;  ///< 修复提示（来自表，可被 make_error 覆盖）
    /// @brief 编译期枚举码（errors.toml 生成表的索引；表驱动元数据的分界标记）。
    ErrorCode code_enum{};  ///< 编译期枚举码（默认 GeneralUnknown）
    ErrorSeverity severity = ErrorSeverity::Error;  ///< 来自 errors.toml
    ErrorCategory category = ErrorCategory::General;  ///< 来自 errors.toml
    bool auto_fixable = false;  ///< 来自 errors.toml
    bool retryable = false;  ///< 来自 errors.toml
    std::string fix_category;  ///< 修复策略分类（如 "type_error"|"missing_prop"）
    std::string fix_params;  ///< 修复参数（JSON 字符串形式）

    /// @brief 序列化为单行 JSON：code/message/code_enum/severity/category/auto_fixable/retryable 恒输出，
    /// suggestion/docs/where/hint/fix_category 非空才输出，fix_params 原样内嵌（其本身已是 JSON）。
    /// @return JSON 对象一行文本（字符串字段做双引号、反斜杠与换行转义；不经三方 JSON 库）。
    [[nodiscard]] auto to_json() const -> std::string;
};

/// @brief 查表填充 Error 的 slug/severity/category/fix 元数据与一个 message。
/// @param code 编译期枚举码（AURORA_ERROR_TABLE 索引；未知码由 at() 抛异常拦截）。
/// @param message 人类可读描述（调用方给定，不经模板渲染）。
/// @param hint 修复提示；空时回退表中的默认 hint。
/// @return 表驱动填充完成的 Error 对象。
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
/// @return 表驱动填充的 Error（message 为模板渲染结果）。
[[nodiscard]] inline auto make_error(ErrorCode code, const ErrorParams &params = {}, std::string hint = {}) -> Error {
    const auto &m = AURORA_ERROR_TABLE.at(static_cast<std::size_t>(code));
    return make_error_from_table(code, format_message(m.message_tpl, params), std::move(hint));
}

/// @brief 构造错误：调用方自定义 message（覆盖表模板），其余元数据仍来自表。
/// @param code 编译期枚举码（表索引）。
/// @param message 最终人类可读描述（不走模板渲染）。
/// @param params 保留以兼容既有调用；已提供 message 时被忽略。
/// @param hint 可选，覆盖表中的默认 hint。
/// @return 表驱动填充、message 为调用方覆写值的 Error。
[[nodiscard]] inline auto make_error(ErrorCode code, const std::string &message, const ErrorParams &params = {},
                                     std::string hint = {}) -> Error {
    (void)params;  // 调用方已提供最终 message，模板省略
    return make_error_from_table(code, message, std::move(hint));
}

/// @brief 构造错误（向后兼容）：枚举 + message + suggestion/docs/where。
///         slug/severity/category/fix 元数据仍来自表。
/// @param code 编译期枚举码（表索引）。
/// @param message 最终人类可读描述。
/// @param suggestion 修复建议（覆盖表值）。
/// @param docs 文档链接/章节（非空才进 JSON）。
/// @param where 发生位置（file:line；非空才进 JSON）。
/// @return 表驱动填充、附加字段按实参覆写的 Error。
[[nodiscard]] inline auto make_error(ErrorCode code, const std::string &message, std::string suggestion,
                                     std::string docs = {}, std::string where = {}) -> Error {
    auto e = make_error_from_table(code, message, {});  // 先取表驱动基座，再逐字段覆盖 suggestion/docs/where。
    e.suggestion = std::move(suggestion);
    e.docs = std::move(docs);
    e.where = std::move(where);
    return e;
}

/// @brief 结果类型：成功持 T，失败持结构化 Error（需求 #9：统一失败路径）。
/// @tparam T 成功时的值类型。
/// @note Thread: thread-safe
/// @note Side-effects: none
template <typename T>
class Result {
  public:
    /// @brief 成功态构造：variant 持有成功值（隐式转换，行内豁免有意为之）。
    /// @param value 成功时的值（move 进 variant）。
    Result(T value) : data_(std::move(value)) {}  // NOLINT：成功值隐式构造
    /// @brief 失败态构造：variant 持有结构化 Error（隐式转换，行内豁免有意为之）。
    /// @param err 错误对象（move 进 variant）。
    Result(Error err) : data_(std::move(err)) {}  // NOLINT：错误隐式构造

    /// @brief 是否处于成功态。
    /// @return variant 当前持有 T（成功）时为 true。
    [[nodiscard]] auto ok() const -> bool { return std::holds_alternative<T>(data_); }

    /// @brief 布尔语境：成功为 true（供 `if (result)` 使用）。
    /// @return 同 ok()：variant 当前持有 T 时为 true。
    explicit operator bool() const { return ok(); }

    /// @brief 解包成功值（常量视图）。
    /// @return 成功值的 const 引用；失败态调用抛 std::bad_variant_access。
    [[nodiscard]] auto value() const -> const T & { return std::get<T>(data_); }
    /// @brief 解包成功值（可变视图）。
    /// @return 成功值的可变引用；失败态调用抛 std::bad_variant_access。
    [[nodiscard]] auto value() -> T & { return std::get<T>(data_); }
    /// @brief 取出错误载荷。
    /// @return 结构化 Error 的 const 引用；成功态调用抛 std::bad_variant_access。
    [[nodiscard]] auto error() const -> const Error & { return std::get<Error>(data_); }

    /// @brief 解包：成功返回值，失败抛 std::runtime_error（仅用于不可恢复场景）。
    /// @return 成功值的拷贝（T 按值返回）；失败时异常携带 error().message。
    [[nodiscard]] auto unwrap() const -> T {
        if (!ok()) {
            throw std::runtime_error(error().message);
        }
        return value();
    }

  private:
    std::variant<T, Error> data_;
};

/// @brief `Result<void>` 特化：仅表示成功/失败，无成功值（用于 `flush`/`reload` 等
/// 只关心“是否出错”的接口，统一失败路径，对齐需求 #9）。
/// @note `std::variant<void, ...>` 非法（void 非对象类型），故用 `bool` 标记成功态。
/// @note Thread: thread-safe
/// @note Side-effects: none
template <>
class Result<void> {
  public:
    /// @brief 默认构造即成功态（ok_ 置真，err_ 保持默认值）。
    Result() : ok_(true) {}  // 成功
    /// @brief 失败态构造：持有结构化 Error（隐式转换，行内豁免有意为之；ok_ 维持默认 false）。
    /// @param err 错误对象（move 保存）。
    Result(Error err) : err_(std::move(err)) {}  // NOLINT：错误隐式构造

    /// @brief 是否处于成功态。
    /// @return 成功标记 ok_ 的布尔值。
    [[nodiscard]] auto ok() const -> bool { return ok_; }
    /// @brief 布尔语境：成功为 true（供 `if (result)` 使用）。
    /// @return 同 ok()。
    explicit operator bool() const { return ok_; }
    /// @brief 取出错误载荷。
    /// @return 保存的 Error const 引用（成功态调用返回的是默认构造的错误对象）。
    [[nodiscard]] auto error() const -> const Error & { return err_; }

  private:
    bool ok_ = false;
    Error err_;
};

}  // namespace aurora

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"

namespace aurora {

/**
 * @brief 结构化修复建议（规格 §21：错误恢复增强）。
 *
 * 携带机器可读 `code`、人类可读 `description` 与可选的 `auto_fix` 回调。
 * 工具 / UI 可经 `Diagnostics::collect_fixes()` 取出，调用 `apply_fix(code)` 一键修复。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
struct FixSuggestion {
    std::string code;               ///< 机器可读标识（与 Diagnostic::code 对应）
    std::string description;        ///< 人类可读修复说明
    std::function<void()> auto_fix; ///< 可选：自动修复回调（无则仅提示）

    [[nodiscard]] auto has_auto_fix() const -> bool { return static_cast<bool>(auto_fix); }
};

/**
 * @brief 诊断记录（规格 §21：错误恢复与降级渲染）。
 *
 * 库在“输入非法 / 部分代码缺失”时**不崩溃、不中止**，而是降级到安全默认值，
 * 并产出一条结构化诊断，供运行时日志与工具消费（JSON 行）。
 *
 * `code` 为机器可读的稳定标识（如 `nav-depth-exceeded`），供 `explain_diagnostic`
 * 与 `--explain` 类工具输出人类可读的解释（CI / 开发者自助排查）。
 * `fix` 为可选的结构化修复建议（§8.3）。
 * @note Thread: main-thread only
 * @note Side-effects: none
 */
struct Diagnostic {
    ErrorSeverity severity = ErrorSeverity::Warning; ///< 与 Error 的 ErrorSeverity 对齐
    ErrorCategory category = ErrorCategory::General; ///< 与 Error 的 ErrorCategory 对齐
    std::string message;                             ///< 人类可读描述
    std::string where;                               ///< 可选：位置（file:line 或 widget 类型）
    std::string code;                                ///< 机器可读标识（冻结 slug，可为空）
    ErrorCode code_enum = ErrorCode::GeneralUnknown; ///< 与 code 对应的枚举（无码时为 GeneralUnknown）
    std::optional<FixSuggestion> fix;                ///< 可选：结构化修复建议

    /// @brief 便于遗留 / 测试按字符串比较 severity（如 `dg.severity == "warn"`）。
    /// @note 新代码应直接用 `ErrorSeverity` 枚举比较，避免字符串字面量。
    [[nodiscard]] auto severity_str() const -> std::string_view { return to_string(severity); }
    /// @brief 便于遗留 / 测试按字符串比较 category。
    [[nodiscard]] auto category_str() const -> std::string_view { return to_string(category); }

    [[nodiscard]] auto to_json_line() const -> std::string;
};

/// @brief 便于按语义比较的枚举别名（与 Error / ErrorSeverity 对齐）。
using DiagnosticSeverity [[deprecated("Compare Diagnostic::severity with the ErrorSeverity enum instead")]] =
    ErrorSeverity;
using DiagnosticCategory [[deprecated("Compare Diagnostic::category with the ErrorCategory enum instead")]] =
    ErrorCategory;

/// @brief 全局诊断收集器（单线程 UI，无需加锁）。
/// @note Thread: main-thread only
/// @note Side-effects: none
class Diagnostics {
  public:
    /// @brief 上报一条诊断（桥接到全局 Logger，便于统一日志格式与重定向）。
    /// 元数据（severity/category）由 `code` 对应的 ErrorCode 表驱动注入；`code` 为空时退化为
    /// Warning / General，并由 `is_degraded` 决定日志级别（degraded→Error，否则 Warn）。
    /// @note 同时保留到内存收集器与最近诊断环形缓冲，供测试 / 工具消费。
    static auto report(std::string_view message, std::string_view where = {}, std::string_view code = {},
                       bool is_degraded = false, FixSuggestion fix = {}) -> void;

    /// @brief 便捷：普通警告（桥接 Logger::Warn）。`code` 为可选冻结 slug / ErrorCode 标识。
    static void warn(std::string_view message, std::string_view where = {}, std::string_view code = {});

    /// @brief 便捷：降级渲染（桥接 Logger::Error，非法输入被替换为安全默认值）。
    /// 严格模式下（`strict_mode() == On`）升级为硬失败（debug 下 AURORA_ASSERT 触发）。
    /// `code` 为可选冻结 slug / ErrorCode 标识。
    static void degraded(std::string_view message, std::string_view where = {}, std::string_view code = {});

    /// @brief 取出并清空累计诊断（测试 / 工具用）。
    static auto take() -> std::vector<Diagnostic>;

    /// @brief 当前累计诊断数（断言/测试用）。
    [[nodiscard]] static auto count() -> std::size_t;

    /// @brief 最近诊断环形缓冲（最多保留 `AURORA_RECENT_CAP` 条），供 `explain` / 工具复查。
    [[nodiscard]] static auto get_last_diagnostics() -> std::vector<Diagnostic>;

    /// @brief 机器码(slug) → 人类可读解释。未知码返回兜底说明。
    [[nodiscard]] static auto explain_diagnostic(std::string_view code) -> std::string;

    /// @brief 错误码(枚举) → 人类可读解释（等价于 slug 重载）。
    [[nodiscard]] static auto explain_diagnostic(ErrorCode code) -> std::string;

    /// @brief 收集最近诊断中携带的修复建议（仅含 `code` 非空者），供工具 / UI 展示。
    /// @note 不消费；同一 code 可能出现多次。
    [[nodiscard]] static auto collect_fixes() -> std::vector<FixSuggestion>;

    /// @brief 按 `code` 应用一次自动修复（执行 `FixSuggestion::auto_fix`）。
    /// @return 命中并执行返回 true；未命中或无 auto_fix 返回 false。
    static auto apply_fix(std::string_view code) -> bool;

    /// @brief 注册错误码 → 修复策略映射。
    /// @note 全局注册表，线程安全性同 Diagnostics 本身（main-thread only）。
    static auto register_fix(ErrorCode code, FixSuggestion fix) -> void;

    /// @brief 应用所有已注册的自动修复，返回成功修复数。
    /// @note 按注册顺序尝试每个有 auto_fix 的条目。
    static auto auto_fix_all() -> std::size_t;

    /// @brief 按错误码应用自动修复。
    /// @return 命中并执行返回 true；未命中或无 auto_fix 返回 false。
    static auto apply_fix(ErrorCode code) -> bool;

    static constexpr std::size_t AURORA_RECENT_CAP = 64;

  private:
    static auto instance() -> Diagnostics &;

    std::vector<Diagnostic> m_log;    ///< 累计（take 时清空）
    std::vector<Diagnostic> m_recent; ///< 环形缓冲（保留最近 AURORA_RECENT_CAP 条）
};

} // namespace aurora

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/error_codes.h"

namespace aurora {

/// @brief 诊断记录（需求 SPEC.QUALITY.CORE.GRACEFUL-DEGRADATION.001：错误恢复与降级渲染）。
///
/// 库在“输入非法 / 部分代码缺失”时**不崩溃、不中止**，而是降级到安全默认值，
/// 并产出一条结构化诊断，供运行时日志与工具消费（JSON 行）。
///
/// `code` 为机器可读的稳定标识（如 `nav-depth-exceeded`），供 `explain_diagnostic`
/// 与 `--explain` 类工具输出人类可读的解释（CI / 开发者自助排查）。
/// 修复指引只有一个来源：`ErrorMeta::hint`（经 `explain_diagnostic()` 表驱动出码），本结构不再
/// 携带独立的修复建议载荷——库只上报错误信息，不代调用方决定怎么修。
/// @note Thread: main-thread only
/// @note Side-effects: none
struct Diagnostic {
    ErrorSeverity severity = ErrorSeverity::Warning;  ///< 与 Error 的 ErrorSeverity 对齐
    /// @brief 子系统归属，与 Error 的 ErrorDomain 对齐；缺省 `General`、`kind` 缺省 `Internal`，
    /// 与零值哨兵 `ErrorCode::GeneralUnknown` 那一行的表元数据一致。
    ErrorDomain domain = ErrorDomain::General;
    ErrorKind kind = ErrorKind::Internal;  ///< 故障性质，与 Error 的 ErrorKind 对齐
    std::string message;  ///< 人类可读描述
    std::string where;  ///< 可选：位置（file:line 或 widget 类型）
    std::string code;  ///< 机器可读标识（冻结 slug，可为空）
    ErrorCode code_enum = ErrorCode::GeneralUnknown;  ///< 与 code 对应的枚举（无码时为 GeneralUnknown）

    /// @brief 便于遗留 / 测试按字符串比较 severity（如 `dg.severity == "warn"`）。
    /// @note 新代码应直接用 `ErrorSeverity` 枚举比较，避免字符串字面量。
    /// @return `to_string(severity)` 的字符串视图。
    [[nodiscard]] auto severity_str() const -> std::string_view { return to_string(severity); }
    /// @brief 便于遗留 / 测试按字符串比较子系统归属。
    /// @return `to_string(domain)` 的字符串视图。
    [[nodiscard]] auto domain_str() const -> std::string_view { return to_string(domain); }
    /// @brief 便于遗留 / 测试按字符串比较故障性质。
    /// @return `to_string(kind)` 的字符串视图。
    [[nodiscard]] auto kind_str() const -> std::string_view { return to_string(kind); }

    /// @brief 序列化为单行 JSON（字段：severity/domain/kind/message，where/code 非空才输出）。
    /// @return JSON 一行文本。
    [[nodiscard]] auto to_json_line() const -> std::string;
};

using DiagnosticSeverity [[deprecated("Compare Diagnostic::severity with the ErrorSeverity enum instead")]] =
    ErrorSeverity;  ///< 已废弃的严重级别名：与 ErrorSeverity 同一类型（新代码直接用 ErrorSeverity）。

/// @brief 全局诊断收集器（单线程 UI，无需加锁）。
/// @note Thread: main-thread only
/// @note Side-effects: none
class Diagnostics {
  public:
    /// @brief 上报一条诊断（桥接到全局 Logger，便于统一日志格式与重定向）。
    /// 元数据（severity/domain/kind）由 `code` 对应的 ErrorCode 表驱动注入；`code` 为空时退化为
    /// Warning / General / Internal，并由 `is_degraded` 决定日志级别（degraded→Error，否则 Warn）。
    /// @param message 人类可读描述。
    /// @param where 位置（file:line 或 widget 类型；空为未指定）。
    /// @param code 冻结 slug（驱动 severity/domain/kind 表注入；空时退化为 Warning/General/Internal）。
    /// @param is_degraded 是否为降级路径（true→日志级别 Error，否则→Warn）。
    /// @note 同时保留到内存收集器与最近诊断环形缓冲，供测试 / 工具消费。
    static auto report(std::string_view message, std::string_view where = {}, std::string_view code = {},
                       bool is_degraded = false) -> void;

    /// @brief 便捷：普通警告（桥接 Logger::Warn）。`code` 为可选冻结 slug / ErrorCode 标识。
    /// @param message 人类可读描述。
    /// @param where 位置（file:line 或 widget 类型；空为未指定）。
    /// @param code 可选冻结 slug（表驱动注入 severity/domain/kind）。
    static void warn(std::string_view message, std::string_view where = {}, std::string_view code = {});

    /// @brief 便捷：降级渲染（桥接 Logger::Error，非法输入被替换为安全默认值）。
    /// 严格模式下（`strict_mode() == On`）升级为硬失败（debug 下 AURORA_ASSERT 触发）。
    /// `code` 为可选冻结 slug / ErrorCode 标识。
    /// @param message 人类可读描述。
    /// @param where 位置（file:line 或 widget 类型；空为未指定）。
    /// @param code 可选冻结 slug（严格模式判定与解释查询用）。
    static void degraded(std::string_view message, std::string_view where = {}, std::string_view code = {});

    /// @brief 取出并清空累计诊断（测试 / 工具用）。
    /// @return 本次累计的诊断列表（内存收集器随即清空）。
    static auto take() -> std::vector<Diagnostic>;

    /// @brief 当前累计诊断数（断言/测试用）。
    /// @return 内存收集器中尚未取走的诊断条数。
    [[nodiscard]] static auto count() -> std::size_t;

    /// @brief 最近诊断环形缓冲（最多保留 `AURORA_RECENT_CAP` 条），供 `explain` / 工具复查。
    /// @return 最近诊断的副本（不消费、不清空环形缓冲）。
    [[nodiscard]] static auto get_last_diagnostics() -> std::vector<Diagnostic>;

    /// @brief 机器码(slug) → 人类可读解释。未知码返回兜底说明。
    /// @param code 冻结 slug（查 ErrorMeta 表）。
    /// @return 该码对应的 hint 解释；未知码返回指向 codespec/ERROR_CATALOG.md 的兜底说明。
    [[nodiscard]] static auto explain_diagnostic(std::string_view code) -> std::string;

    /// @brief 错误码(枚举) → 人类可读解释（等价于 slug 重载）。
    /// @param code ErrorCode 枚举（内部转 slug 后走同一查询路径）。
    /// @return 同 slug 重载：命中返回 hint 解释，未知返回兜底说明。
    [[nodiscard]] static auto explain_diagnostic(ErrorCode code) -> std::string;

    static constexpr std::size_t AURORA_RECENT_CAP = 64;  ///< 最近诊断环形缓冲的容量（保留条数）。

  private:
    static auto instance() -> Diagnostics &;

    std::vector<Diagnostic> log_;  ///< 累计（take 时清空）
    std::vector<Diagnostic> recent_;  ///< 环形缓冲（保留最近 AURORA_RECENT_CAP 条）
};

}  // namespace aurora

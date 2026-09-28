#pragma once

// ============================================================================
// spec_lookup.h — cli 模块的内部查表与内建注入头（不安装、不进 include/、不进 aurora_api.json）
// ----------------------------------------------------------------------------
// `args.cpp`（扫描器）与 `command.cpp`（校验 / 帮助 / schema）必须对「这一层到底有没有内建
// `--help` / `--version`、它以什么形态出现」给出**同一个答案**：解析若认 `-h` 而帮助文本不列，
// 或反过来，就是口径漂移。故判定收敛为唯一的 `builtin_plan`，两侧都只读它。
//
// 三个 `find_*` 也在此声明：它们是库内的查表原语，公共 API 不再暴露（`CommandSpec` 是纯声明值，
// 对外发现能力由 `schema_json` 承担）。
// ============================================================================

#include <string_view>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"

namespace aurora::cli::detail {

/// @brief 按长名查本命令声明的选项（内建 help/version 不在其列）；未找到返回 nullptr。
[[nodiscard]] auto find_option(const CommandSpec &spec, std::string_view long_name) -> const OptionSchema *;

/// @brief 按短名查本命令声明的选项；未找到返回 nullptr。
[[nodiscard]] auto find_short(const CommandSpec &spec, char short_name) -> const OptionSchema *;

/// @brief 按名查直接子命令；未找到返回 nullptr。
[[nodiscard]] auto find_subcommand(const CommandSpec &spec, std::string_view sub_name) -> const CommandSpec *;

/**
 * @brief 一层命令的内建 help / version 实际注入形态。
 *
 * 规则（惰性注入，用户声明优先）：长名已被声明 → 该内建整体不注入；短名已被本层任一选项占用、
 * 或 `builtins.take_shorts == false` → 内建降级为仅长名。`version` 另需该层 `version` 非空。
 */
struct BuiltinPlan {
    bool help = false;  ///< 是否提供 `--help`
    bool help_short = false;  ///< `--help` 是否同时可用 `-h`
    bool version = false;  ///< 是否提供 `--version`
    bool version_short = false;  ///< `--version` 是否同时可用 `-V`
};

/// @brief 计算该层的内建注入形态；解析、`help_text`、`schema_json` 三方共用。
[[nodiscard]] auto builtin_plan(const CommandSpec &spec) -> BuiltinPlan;

/// @brief 内建 `--help` 的声明行（渲染与 schema 用；短名按 `builtin_plan` 决定是否带上）。
[[nodiscard]] auto builtin_help_option(const BuiltinPlan &plan) -> OptionSchema;

/// @brief 内建 `--version` 的声明行（渲染与 schema 用）。
[[nodiscard]] auto builtin_version_option(const BuiltinPlan &plan) -> OptionSchema;

}  // namespace aurora::cli::detail

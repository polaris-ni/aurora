#pragma once

// tools/verify/ 真机验收探针共用的 argv 入口：旗标解析交给 `aurora::cli` 声明表，探针体只读值。
//
// 为什么收敛到一处：14 份探针此前各写一份 argv 循环（`--interactive` 有四种等价写法、未知旗标
// 一律静默忽略、完全没有 `--help`），是命令行口径漂移的直接来源。改为同一份声明后，
// 帮助文本与 `Did you mean` 建议的口径与 `aurora_cli` / 测试 runner 一致
// （规格见 codespec/specification/09-cli.md §10.1）。
//
// ⚠️ 用法错误的退出码**不能沿用工具链惯用的 2**：探针侧的 2 早被 14 份头注释的退出码表占用为
// 「环境不可用」（无 DISPLAY / 无合成器 / 建窗失败），复用会让脚本无法区分「旗标写错」与
// 「本机没环境」——后者是 SKIP，前者是人的失误。故这里用 sysexits 的 `EX_USAGE = 64`，
// 与探针既有的 0/1/3/4/5/6/7 全部不重叠。
//
// 范围：仅服务 tools/verify/ 下的真机验收探针；不进 aurora 库、不进 CTest、不进 aurora_api.json。

#include <optional>
#include <string>
#include <utility>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/core/log.h"

namespace aurora_verify {

/// @brief argv 解析结论：`arguments` 有值即进入探针主体；否则调用方**立即**按 `exit_code` 返回。
///
/// 为什么不是一个 `std::optional`：`--help` 要退 0、用法错误要退 `AURORA_EXIT_USAGE`，二者都得让探针把码
/// 传回 `main` 的调用者（脚本按退出码判验收结论），但都不该让探针走到自己的判据段。
struct CommandLine {
    static constexpr int AURORA_EXIT_USAGE = 64;  ///< sysexits `EX_USAGE`；勿改成 2，理由见文件头 ⚠️ 段。

    std::optional<aurora::cli::Arguments> arguments;  ///< 无值 = 不进入探针主体
    int exit_code = 0;  ///< 仅在 `arguments` 无值时有意义：0 = 帮助/版本已打印，AURORA_EXIT_USAGE = 用法错误
};

/// @brief 解析探针 argv；`--help` 文本与用法诊断都在此打印。
[[nodiscard]] inline auto parse_command_line(const aurora::cli::CommandSpec &spec, int argc, char **argv)
    -> CommandLine {
    const auto parsed = aurora::cli::parse(spec, argc, argv);
    if (!parsed) {
        std::string detail = parsed.error().message;
        if (!parsed.error().suggestion.empty()) {
            detail += " — " + parsed.error().suggestion;  // 如 "Did you mean --interactive?"
        }
        AURORA_LOG_ERROR("verify", detail);
        AURORA_LOG_ERROR("verify", "Run with --help for the accepted flags.");
        return {.arguments = std::nullopt, .exit_code = CommandLine::AURORA_EXIT_USAGE};
    }
    const auto &invocation = parsed.value();
    if (invocation.outcome != aurora::cli::ParseOutcome::Ok) {
        AURORA_LOG_RAW("verify", invocation.display_text);  // --help / --version 是一等结局
        return {.arguments = std::nullopt, .exit_code = 0};
    }
    return {.arguments = invocation.arguments, .exit_code = 0};
}

/// @brief 只有 `--interactive` 旗标的探针声明表（多数探针的全部命令行面）。
[[nodiscard]] inline auto interactive_spec(std::string about) -> aurora::cli::CommandSpec {
    return aurora::cli::CommandSpec{
        .name = "aurora_verify",
        .about = std::move(about),
        .options = {aurora::cli::OptionSchema{
            .long_name = "interactive",
            .kind = aurora::cli::ValueKind::Bool,
            .arity = aurora::cli::Arity::flag(),
            .help = "Run the manual stage that only a human can judge (on-screen / audible)"}}};
}

/// @brief 最常见形态：探针只认 `--interactive`，返回其是否出现。
///
/// 用法错误 / `--help` 时 `arguments` 为空，调用方按 `exit_code` 直接返回：
/// @code
/// const auto cli = aurora_verify::parse_interactive("X11 cursor shapes", argc, argv);
/// if (!cli.arguments) { return cli.exit_code; }
/// const bool interactive = cli.arguments->flag("interactive");
/// @endcode
[[nodiscard]] inline auto parse_interactive(const char *about, int argc, char **argv) -> CommandLine {
    return parse_command_line(interactive_spec(about), argc, argv);
}

}  // namespace aurora_verify

// =============================================================================
// cli 模块人工验收载体（examples/demos/demo_cli.cpp）
// -----------------------------------------------------------------------------
// `aurora::cli` 是 schema-first 的参数解析库：调用方声明一张不可变的命令表
// （`CommandSpec` + `OptionSchema` + `PositionalSchema`），库负责 GNU/POSIX 语法扫描、
// 字面量类型转换、派生文本（usage / --help / --version）与结构化错误。它的可观测行为
// 全在**进程退出码 + stdout/stderr 文本**，与像素无关，因此本载体是纯控制台程序：
// 不 include demo_common.h、不创建窗口、不依赖任何 Surface 后端。
//
// 演示分组：
//   语法矩阵      --k=v / --k v / -k v / -kv / -k=v / -abc 聚组 / `--` 终止 / 负数取值
//   强类型打通    Length(25%|12px|fill) / Color(#rgb|#rrggbbaa|rgb()) / LogLevel / Duration
//   声明校验      validate() 拒绝自相矛盾的声明表（cli-spec-invalid）
//   派生文本      usage_line / help_text / version_text 全由声明表推导
//   一等结局      --help / --version 是成功的 Invocation（outcome 非 Ok），不是错误
//   子命令树      render 叶命令遮蔽父级同名选项；未知子命令给候选建议
//   自描述        schema_json() 输出整棵声明表，供 Inspector / MCP 消费
//
// 试跑（退出码约定：0=成功/help/version，2=用法错误，1=业务失败）：
//   demo_cli --help
//   demo_cli --width 1024 -vv --tint "#f008" --margin 25% --timeout 2s --tag a --tag b -- scene.one
//   demo_cli render --fps 30 scene.yaml -- --raw-flag -x
//   demo_cli --widht 1024          # 拼写接近 → Did you mean
//   demo_cli --width 99999         # 区间越界 → cli-range-violated
//   demo_cli --mode turbo          # 取值域外 → cli-choice-invalid + 允许值清单
//   demo_cli --dump-schema         # 打印 schema_json
//
// 注意上面 `--` 的位置：变长选项（`--tag`，arity = at_least_one）按 GNU 之外的「贪婪跨度」
// 规则吞掉其后所有裸 token（与 argparse 的 nargs='+' 同语义），所以位置参数要么写在选项之前，
// 要么用 `--` 隔开。这是本载体刻意演示的一条语法边界。
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/core/color.h"
#include "aurora/core/log.h"
#include "aurora/core/types.h"

namespace au = aurora;
namespace cli = aurora::cli;

namespace {

/// @brief stdout 功能输出（无前缀）：本载体的产品文本一律走 RAW 通道。
template <typename... Args>
auto emit(Args &&...args) -> void {
    AURORA_LOG_RAW("cli-demo", std::forward<Args>(args)..., '\n');
}

/// @brief 被演示的命令树：刻意覆盖全部 ValueKind、取值域、默认值、互斥与子命令。
[[nodiscard]] auto build_spec() -> const cli::CommandSpec & {
    static const cli::CommandSpec ROOT_SPEC = [] {
        cli::CommandSpec root;
        root.name = "demo_cli";
        root.about = "Aurora CLI demo carrier";
        root.description = "Declares every option kind so the derived help text is fully exercised.";
        root.version = "1.0.0";
        root.epilog = "Exit codes: 0 = ok/help/version, 2 = usage error, 1 = business failure.";
        root.options = {
            cli::OptionSchema{
                .long_name = "output", .short_name = 'o', .kind = cli::ValueKind::String, .help = "Output image path",
                .value_hint = "FILE", .default_text = "out.png",
            },
            cli::OptionSchema{
                .long_name = "width", .short_name = 'w', .kind = cli::ValueKind::Int, .help = "Canvas width in px",
                .default_text = "800", .minimum = 1, .maximum = 8192,
            },
            cli::OptionSchema{
                .long_name = "verbose", .short_name = 'v', .kind = cli::ValueKind::Bool, .arity = cli::Arity::flag(),
                .help = "Repeat for more detail",
            },
            cli::OptionSchema{
                .long_name = "level", .kind = cli::ValueKind::LogLevel, .help = "Log verbosity",
                .default_text = "info",
            },
            cli::OptionSchema{
                .long_name = "margin", .kind = cli::ValueKind::Length, .help = "Outer margin", .default_text = "0",
            },
            cli::OptionSchema{
                .long_name = "tint", .kind = cli::ValueKind::Color, .help = "Overlay tint", .default_text = "#000",
            },
            cli::OptionSchema{
                .long_name = "timeout", .kind = cli::ValueKind::Duration, .help = "Give up after",
                .default_text = "30s",
            },
            cli::OptionSchema{
                .long_name = "mode", .kind = cli::ValueKind::Enum, .help = "Quality/speed trade-off",
                .default_text = "balanced", .choices = {"fast", "balanced", "quality"},
            },
            cli::OptionSchema{
                .long_name = "tag", .kind = cli::ValueKind::String, .arity = cli::Arity::at_least_one(),
                .help = "Attach a tag (repeatable)",
            },
            cli::OptionSchema{
                .long_name = "force", .kind = cli::ValueKind::Bool, .arity = cli::Arity::flag(),
                .help = "Overwrite output", .conflicts_with = {"dry-run"},
            },
            cli::OptionSchema{
                .long_name = "dry-run", .kind = cli::ValueKind::Bool, .arity = cli::Arity::flag(),
                .help = "Print only", .conflicts_with = {"force"},
            },
            cli::OptionSchema{
                .long_name = "dump-schema", .kind = cli::ValueKind::Bool, .arity = cli::Arity::flag(),
                .help = "Print schema_json() before the summary",
            },
        };
        root.positionals = {
            cli::PositionalSchema{
                .name = "SCENE", .kind = cli::ValueKind::String, .arity = cli::Arity::zero_or_more(),
                .help = "Scenes to render",
            },
        };

        cli::CommandSpec render;
        render.name = "render";
        render.about = "Render one scene interactively";
        render.options = {
            cli::OptionSchema{
                .long_name = "fps", .kind = cli::ValueKind::Int, .help = "Target frame rate", .default_text = "60",
                .minimum = 1, .maximum = 240,
            },
            cli::OptionSchema{
                .long_name = "mode", .kind = cli::ValueKind::Enum, .help = "Leaf mode (shadows root)",
                .default_text = "png", .choices = {"png", "webp"},
            },
        };
        render.positionals = {
            cli::PositionalSchema{.name = "SRC", .kind = cli::ValueKind::String, .help = "Source scene"},
        };
        root.subcommands = {render};
        return root;
    }();
    return ROOT_SPEC;
}

/// @brief 声明表的可读一行：出现次数 + 各次原文 + 是否用户显式给出（默认值物化后仍可区分）。
///
/// 这里刻意走 `values()` 而非 `value()`：后者是「单值快捷入口」，对重复项
/// （`-vv`、`--tag a --tag b`）按契约返回 `cli-arity-violated`，多值须由调用方遍历。
auto report_option(const cli::Arguments &args, const cli::OptionSchema &option) -> void {
    const auto many = args.values(option.long_name);
    if (many.empty()) {
        emit("  --", option.long_name, " [", cli::to_string(option.kind), "] = <not given>");
        return;
    }
    std::string joined;
    for (const auto &value : many) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += '"' + value.raw_text() + '"';
    }
    emit("  --", option.long_name, " [", cli::to_string(option.kind), "] = ", joined, " (",
         std::to_string(many.size()), " 次", args.explicitly_given(option.long_name) ? "" : ", 默认值", ')');
}

/// @brief 强类型出口：证明字面量真的变成了 Length / Color / LogLevel / Duration，而非字符串。
auto report_strong_types(const cli::Arguments &args) -> void {
    const auto margin = args.get<au::Length>("margin");
    const auto tint = args.get<au::Color>("tint");
    const auto level = args.get<au::LogLevel>("level");
    const auto timeout = args.get<std::int64_t>("timeout");  // Duration 存毫秒

    const auto kind_of = [](au::LengthKind k) -> std::string_view {
        switch (k) {
            case au::LengthKind::WrapContent:
                return "WrapContent";
            case au::LengthKind::Expand:
                return "Expand";
            case au::LengthKind::Fixed:
                return "Fixed";
            case au::LengthKind::Fraction:
                return "Fraction";
        }
        return "?";
    };

    emit("  margin : ", margin ? std::string{kind_of(margin.value().kind)} + "(" + std::to_string(margin.value().value) + ")"
                               : std::string{"<unread-as-Length>"});
    emit("  tint   : ",
         tint ? "rgba(" + std::to_string(tint.value().r) + "," + std::to_string(tint.value().g) + "," +
                    std::to_string(tint.value().b) + "," + std::to_string(tint.value().a) + ")"
              : std::string{"<unread-as-Color>"});
    emit("  level  : ", level ? std::string{au::log_level_label(level.value())} : std::string{"<unread-as-LogLevel>"});
    emit("  timeout: ", timeout ? std::to_string(timeout.value()) + "ms" : std::string{"<unread-as-Duration>"});
}

/// @brief 打印一次成功调用的全部可观测结果。
auto report_ok(const cli::Invocation &invocation, const cli::CommandSpec &root) -> void {
    const auto &args = invocation.arguments;
    emit("outcome : ", cli::outcome_to_string(invocation.outcome));
    emit("command : ", args.command_display());
    emit("chain   : ", std::to_string(args.command_chain().size()), " level(s)");
    if (args.matched_command() != nullptr && args.matched_command() != &root) {
        emit("leaf    : ", args.matched_command()->name, " (其声明遮蔽父级同名选项)");
    }
    emit("options :");
    for (const auto &option : args.matched_command()->options) {
        report_option(args, option);
    }
    if (args.matched_command() == &root) {
        report_strong_types(args);
    }
    emit("positionals: ", std::to_string(args.positionals().size()), " 个");
    for (const auto &value : args.positionals()) {
        emit("  ", value.raw_text());
    }
    emit("rest (`--` 之后): ", std::to_string(args.rest().size()), " 个");
    for (const auto &token : args.rest()) {
        emit("  ", token);
    }
    if (args.flag("verbose")) {
        emit("verbosity: ", std::to_string(args.count("verbose")), " 次 -v");
    }
}

/// @brief 结构化错误的回显：slug + message + suggestion（人读），附带 JSON（机读）。
auto report_error(const au::Error &error) -> void {
    AURORA_LOG_ERROR("cli-demo", error.code, ": ", error.message);
    if (!error.suggestion.empty()) {
        AURORA_LOG_WARN("cli-demo", "hint: ", error.suggestion);
    }
    AURORA_LOG_INFO("cli-demo", "payload: ", error.to_json());
}

}  // namespace

// 演示载体入口：异常直达进程出口，终止即正确答案，故不额外包 try/catch（与 examples/ 其余 demo 同口径）。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const cli::CommandSpec &root = build_spec();

    // 声明表自身的静态门禁：错位的默认值 / 悬空的冲突目标在这里就红灯，而不是等到运行时。
    const auto checked = cli::validate(root);
    if (!checked) {
        report_error(checked.error());
        return 1;
    }
    emit("validate: 声明表通过，共 ", std::to_string(checked.value()), " 个命令");

    const auto parsed = cli::parse(root, argc, argv);
    if (!parsed) {
        report_error(parsed.error());
        return 2;
    }
    const cli::Invocation &invocation = parsed.value();

    // --help / --version 是「成功结局 + 已渲染文本」，不是错误：调用方原样打印即可。
    if (invocation.outcome != cli::ParseOutcome::Ok) {
        AURORA_LOG_RAW("cli-demo", invocation.display_text);  // 文本自带行尾换行
        return 0;
    }

    if (invocation.arguments.flag("dump-schema")) {
        emit(cli::schema_json(root).dump(2));
    }
    report_ok(invocation, root);
    return 0;
}

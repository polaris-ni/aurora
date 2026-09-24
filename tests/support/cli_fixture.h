#pragma once

// ============================================================
// 测试公共设施（tests/support/cli_fixture.h）—— aurora::cli 声明表样例
// ------------------------------------------------------------
// `aurora::cli` 的解析与派生视图都需要一棵「有代表性的命令树」作输入：既要覆盖全部
// ValueKind、又要覆盖 arity / 取值域 / 默认值 / 互斥 / 必填 / 子命令 / 隐藏项。
// utest_cli（语法与取值）与 utest_cli_format（声明校验与派生文本）共用同一棵树，
// 保证两个文件的断言口径一致；utest_cli_format 的 golden 快照也以这棵树为唯一输入。
//
// 生命周期契约：`aurora::cli::parse` 返回的 Invocation 以指针借用声明表，故这里以
// 函数内 static 单例暴露（进程期存活），测试只读不改。
// ============================================================

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"

namespace aurora::testing::cli_fixture {

using aurora::cli::Arity;
using aurora::cli::CommandSpec;
using aurora::cli::OptionSchema;
using aurora::cli::PositionalSchema;
using aurora::cli::ValueKind;

/// @brief token 列表（`parse` 的 vector 重载入参，不含程序名）；用 `Tokens{"--w","1"}` 书写。
struct Tokens : std::vector<std::string> {
    Tokens() = default;
    Tokens(std::initializer_list<std::string_view> items) : std::vector<std::string>(items.begin(), items.end()) {}
};

/// @brief 宽松的主样例树：裸跑根命令即可成功，便于单点测试各语法分支。
[[nodiscard]] inline auto spec() -> const CommandSpec & {
    static const CommandSpec SPEC = [] {
        CommandSpec root;
        root.name = "aurora-render";
        root.about = "Render Aurora scenes into image files";
        root.description = "Long description line for the root command.";
        root.version = "1.2.3";
        root.epilog = "Report issues at the project tracker.";
        root.options = {
            OptionSchema{
                .long_name = "output", .short_name = 'o', .kind = ValueKind::String, .help = "Output image path",
                .value_hint = "FILE", .default_text = "out.png",
            },
            OptionSchema{
                .long_name = "width", .short_name = 'w', .kind = ValueKind::Int, .help = "Canvas width in px",
                .default_text = "800", .minimum = 1, .maximum = 8192,
            },
            OptionSchema{
                .long_name = "scale", .short_name = 's', .kind = ValueKind::Double, .help = "Scale factor",
                .default_text = "1",
            },
            OptionSchema{
                .long_name = "verbose", .short_name = 'v', .kind = ValueKind::Bool, .arity = Arity::flag(),
                .help = "Repeat for more detail",
            },
            OptionSchema{
                .long_name = "force", .short_name = 'f', .kind = ValueKind::Bool, .arity = Arity::flag(),
                .help = "Overwrite output", .conflicts_with = {"dry-run"},
            },
            OptionSchema{
                .long_name = "dry-run", .kind = ValueKind::Bool, .arity = Arity::flag(), .help = "Print only",
                .conflicts_with = {"force"},
            },
            OptionSchema{
                .long_name = "level", .kind = ValueKind::LogLevel, .help = "Log verbosity",
                .default_text = "info", .group = "Diagnostics",
            },
            OptionSchema{
                .long_name = "mode", .kind = ValueKind::Enum, .help = "Quality/speed trade-off",
                .default_text = "balanced", .choices = {"fast", "balanced", "quality"}, .group = "Diagnostics",
            },
            OptionSchema{
                .long_name = "margin", .kind = ValueKind::Length, .help = "Outer margin", .default_text = "0",
            },
            OptionSchema{
                .long_name = "tint", .kind = ValueKind::Color, .help = "Overlay tint", .default_text = "#fff",
            },
            OptionSchema{
                .long_name = "timeout", .kind = ValueKind::Duration, .help = "Give up after",
                .default_text = "30s",
            },
            OptionSchema{
                .long_name = "tag", .kind = ValueKind::String, .arity = Arity::at_least_one(),
                .help = "Attach a tag (repeatable)",
            },
            OptionSchema{
                .long_name = "format", .kind = ValueKind::Enum, .help = "Root output format",
                .default_text = "text", .choices = {"text", "json"},
            },
            OptionSchema{
                .long_name = "trace-file", .kind = ValueKind::String, .help = "Hidden diagnostics path",
                .hidden = true,
            },
        };
        root.positionals = {
            PositionalSchema{
                .name = "SCENE", .kind = ValueKind::String, .arity = Arity::zero_or_more(),
                .help = "Scenes to render",
            },
        };

        CommandSpec render;
        render.name = "render";
        render.about = "Render one scene interactively";
        render.options = {
            OptionSchema{
                .long_name = "watch", .short_name = 'w', .kind = ValueKind::Bool, .arity = Arity::flag(),
                .help = "Re-render on change",
            },
            OptionSchema{
                .long_name = "fps", .kind = ValueKind::Int, .help = "Target frame rate", .default_text = "60",
                .minimum = 1, .maximum = 240,
            },
            OptionSchema{
                .long_name = "format", .kind = ValueKind::Enum, .help = "Leaf output format",
                .default_text = "png", .choices = {"png", "webp"},
            },
        };
        render.positionals = {
            PositionalSchema{.name = "SRC", .kind = ValueKind::String, .help = "Source scene"},
            PositionalSchema{
                .name = "DST", .kind = ValueKind::String, .arity = Arity::optional_one(), .help = "Target image",
            },
        };

        CommandSpec serve;
        serve.name = "serve";
        serve.about = "Serve the gallery over HTTP";
        serve.options = {
            OptionSchema{
                .long_name = "port", .short_name = 'p', .kind = ValueKind::Int, .help = "Listen port",
                .default_text = "8080", .minimum = 1, .maximum = 65535,
            },
        };

        root.subcommands = {render, serve};
        return root;
    }();
    return SPEC;
}

/// @brief 严格树：必填项 + 强制子命令，用于验证「缺什么」类错误码。
[[nodiscard]] inline auto strict_spec() -> const CommandSpec & {
    static const CommandSpec STRICT_SPEC = [] {
        CommandSpec root;
        root.name = "strict";
        root.about = "A command that refuses to run bare";
        root.subcommand_required = true;
        root.options = {
            OptionSchema{
                .long_name = "token", .kind = ValueKind::String, .help = "Auth token", .required = true,
            },
        };

        CommandSpec deploy;
        deploy.name = "deploy";
        deploy.about = "Deploy the build";
        deploy.positionals = {
            PositionalSchema{
                .name = "ENV", .kind = ValueKind::Enum, .help = "Target environment", .choices = {"dev", "prod"},
            },
        };
        root.subcommands = {deploy};
        return root;
    }();
    return STRICT_SPEC;
}

/// @brief 按长名取解析结果里的单值文本（失败即空串），便于紧凑断言。
[[nodiscard]] inline auto text_of(const aurora::cli::Arguments &args, std::string_view long_name) -> std::string {
    auto value = args.value(long_name);
    if (!value) {
        return {};
    }
    const auto text = value.value().as_string();
    return text ? text.value() : value.value().raw_text();
}

}  // namespace aurora::testing::cli_fixture

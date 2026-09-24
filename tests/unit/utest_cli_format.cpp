/// 测试类型: unit
/// 目标单元: include/aurora/cli/command.h
/// 测试说明: 覆盖 ValueKind 词表与 Arity 工厂的可枚举性、validate 对声明表的 20 余条静态门禁、
/// usage/help/version 派生文本形态、schema_json 自描述结构，并以 cli_snapshots.json
/// 做文本 golden 基线（AURORA_UPDATE_GOLDEN=1 再生成）

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/core/error_codes.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "support/cli_fixture.h"

namespace aurora::test_cases::utest_cli_format {

namespace au = aurora;
namespace cli = aurora::cli;
namespace m = aurora::testing::matchers;
namespace golden = aurora::testing::golden;
using aurora::testing::cli_fixture::spec;
using aurora::testing::cli_fixture::strict_spec;
using cli::Arity;
using cli::CommandSpec;
using cli::OptionSchema;
using cli::PositionalSchema;
using cli::ValueKind;

namespace {

/// @brief 构造只含给定声明的最小根命令，便于逐条触发 validate 的门禁。
[[nodiscard]] auto minimal(std::vector<OptionSchema> options, std::vector<PositionalSchema> positionals = {})
    -> CommandSpec {
    CommandSpec root;
    root.name = "tool";
    root.options = std::move(options);
    root.positionals = std::move(positionals);
    return root;
}

/// @brief 断言声明表被拒，且拒绝理由是 cli-spec-invalid（而非误伤其他码）。
auto expect_invalid(const CommandSpec &candidate) -> void {
    const auto result = cli::validate(candidate);
    AURORA_TEST_REQUIRE_FALSE(result);
    AURORA_TEST_CHECK(result.error().code_enum == au::ErrorCode::CliSpecInvalid);
    AURORA_TEST_CHECK(result.error().code == "cli-spec-invalid");
}

/// @brief 断言声明表通过校验。
auto expect_valid(const CommandSpec &candidate) -> void {
    const auto result = cli::validate(candidate);
    AURORA_TEST_REQUIRE_MSG(result, result ? std::string{} : result.error().message);
}

/// @brief 取 schema 里某个长名的选项节点；未找到返回 null。
[[nodiscard]] auto option_node(const nlohmann::json &command, std::string_view long_name) -> nlohmann::json {
    for (const auto &entry : command["options"]) {
        if (entry["long"].get<std::string>() == long_name) {
            return entry;
        }
    }
    return nlohmann::json{};
}

}  // namespace

// ------------------------------------------------------------ 可枚举性

AURORA_TEST_CASE(value_kind_vocabulary_is_closed_and_round_trips) {
    const auto kinds = cli::all_value_kinds();
    AURORA_TEST_REQUIRE_EQ(kinds.size(), 9U);
    std::vector<std::string> names;
    for (const auto kind : kinds) {
        const auto name = cli::to_string(kind);
        AURORA_TEST_CHECK_FALSE(name.empty());
        AURORA_TEST_CHECK(std::find(names.begin(), names.end(), std::string{name}) == names.end());
        names.emplace_back(name);
        const auto back = cli::value_kind_from_name(name);
        AURORA_TEST_REQUIRE(back);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): 上一行 REQUIRE 已断言持有值，其宏展开对路径分析不透明
        AURORA_TEST_CHECK(back.value() == kind);
    }
    AURORA_TEST_CHECK_FALSE(cli::value_kind_from_name("widget").has_value());
    AURORA_TEST_CHECK_FALSE(cli::value_kind_from_name("").has_value());
}

AURORA_TEST_CASE(arity_factories_describe_consumed_value_tokens) {
    AURORA_TEST_CHECK(Arity::flag().allows_no_value());
    AURORA_TEST_CHECK_FALSE(Arity::exactly_one().allows_no_value());
    AURORA_TEST_CHECK_EQ(Arity::exactly_one().min, 1);
    AURORA_TEST_CHECK_EQ(Arity::exactly(3).max, 3);
    AURORA_TEST_CHECK_EQ(Arity::at_most(2).min, 0);

    // 定长 span 决定「一条选项吃几个后续 token」
    AURORA_TEST_CHECK_EQ(Arity::exactly_one().fixed_span().value_or(-7), 1);
    AURORA_TEST_CHECK_EQ(Arity::exactly(2).fixed_span().value_or(-7), 2);
    AURORA_TEST_CHECK_FALSE(Arity::at_least_one().fixed_span().has_value());
    AURORA_TEST_CHECK_FALSE(Arity::zero_or_more().fixed_span().has_value());

    AURORA_TEST_CHECK_EQ(Arity::zero_or_more().max_text(), "∞");
    AURORA_TEST_CHECK_EQ(Arity::exactly(4).max_text(), "4");
}

AURORA_TEST_CASE(arity_help_placeholders_follow_optionality) {
    AURORA_TEST_CHECK_EQ(Arity::flag().help_placeholder("X"), "");
    AURORA_TEST_CHECK_EQ(Arity::exactly_one().help_placeholder("WIDTH"), "<WIDTH>");
    AURORA_TEST_CHECK_EQ(Arity::optional_one().help_placeholder("X"), "<X>");
    AURORA_TEST_CHECK_EQ(Arity::at_least_one().help_placeholder("TAG"), "<TAG> <TAG>...");
    AURORA_TEST_CHECK_EQ(Arity::zero_or_more().help_placeholder("TAG"), "<TAG>...");
    AURORA_TEST_CHECK_EQ(Arity::exactly(2).help_placeholder("XY"), "<XY>");
}

// ------------------------------------------------------------ validate 门禁

AURORA_TEST_CASE(validate_accepts_the_shared_fixtures) {
    const auto ok = cli::validate(spec());
    AURORA_TEST_REQUIRE(ok);
    AURORA_TEST_CHECK_EQ(ok.value(), 3);  // 根 + render + serve
    AURORA_TEST_CHECK_EQ(cli::validate(strict_spec()).unwrap(), 2);
}

AURORA_TEST_CASE(validate_rejects_malformed_long_names) {
    expect_invalid(minimal({OptionSchema{.kind = ValueKind::String}}));  // 无长名
    expect_invalid(minimal({OptionSchema{.long_name = "-x", .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "a b", .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "help", .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "version", .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "out", .kind = ValueKind::String},
                            OptionSchema{.long_name = "out", .short_name = 'z', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "out", .kind = ValueKind::String}}));
}

AURORA_TEST_CASE(validate_reserves_short_h_and_duplicate_shorts) {
    expect_invalid(minimal({OptionSchema{.long_name = "helpless", .short_name = 'h', .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "verbose", .short_name = 'V', .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "one", .short_name = 'o', .kind = ValueKind::String},
                            OptionSchema{.long_name = "two", .short_name = 'o', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "one", .short_name = 'o', .kind = ValueKind::String},
                          OptionSchema{.long_name = "two", .short_name = 't', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "verbose", .short_name = 'v', .kind = ValueKind::String}}));
}

AURORA_TEST_CASE(validate_requires_consistent_arity_per_kind) {
    // Bool 必须是零值 arity（flag），非 Bool 必须至少吞一个值
    expect_invalid(minimal({OptionSchema{.long_name = "flag", .kind = ValueKind::Bool}}));
    expect_invalid(
        minimal({OptionSchema{.long_name = "list", .kind = ValueKind::String, .arity = Arity::zero_or_more()}}));
    expect_invalid(
        minimal({OptionSchema{.long_name = "wide", .kind = ValueKind::Int, .arity = Arity{.min = 5, .max = 2}}}));
    expect_valid(minimal({OptionSchema{.long_name = "flag", .kind = ValueKind::Bool, .arity = Arity::flag()}}));
    expect_valid(
        minimal({OptionSchema{.long_name = "list", .kind = ValueKind::String, .arity = Arity::at_least_one()}}));
}

AURORA_TEST_CASE(validate_requires_enum_choices_and_consistent_defaults) {
    expect_invalid(minimal({OptionSchema{.long_name = "mode", .kind = ValueKind::Enum}}));  // Enum 无取值域

    // 默认值不在取值域内 → 声明自相矛盾
    expect_invalid(
        minimal({OptionSchema{.long_name = "mode", .kind = ValueKind::Enum, .default_text = "b", .choices = {"a"}}}));
    expect_valid(minimal(
        {OptionSchema{.long_name = "mode", .kind = ValueKind::Enum, .default_text = "a", .choices = {"a", "b"}}}));
    expect_invalid(minimal({OptionSchema{.long_name = "width", .kind = ValueKind::Int, .default_text = "wide"}}));
    expect_invalid(
        minimal({OptionSchema{.long_name = "width", .kind = ValueKind::Int, .default_text = "99", .maximum = 10}}));
    expect_valid(minimal({OptionSchema{
        .long_name = "width", .kind = ValueKind::Int, .default_text = "9", .minimum = 1, .maximum = 10}}));
}

AURORA_TEST_CASE(validate_requires_conflicts_to_point_at_real_options) {
    expect_invalid(minimal({OptionSchema{.long_name = "a", .kind = ValueKind::String, .conflicts_with = {"ghost"}}}));
    expect_invalid(minimal({OptionSchema{.long_name = "a", .kind = ValueKind::String, .conflicts_with = {"a"}}}));
    expect_valid(minimal({OptionSchema{.long_name = "a", .kind = ValueKind::String, .conflicts_with = {"b"}},
                          OptionSchema{.long_name = "b", .kind = ValueKind::String, .conflicts_with = {"a"}}}));
}

AURORA_TEST_CASE(validate_constrains_positional_declarations) {
    expect_invalid(minimal({}, {PositionalSchema{.kind = ValueKind::String}}));  // 无名
    expect_invalid(minimal({}, {PositionalSchema{.name = "A", .kind = ValueKind::Bool}}));  // 位置参数不能是 flag
    expect_invalid(minimal({}, {PositionalSchema{.name = "A", .kind = ValueKind::String},
                                PositionalSchema{.name = "A", .kind = ValueKind::String}}));  // 重名
    expect_invalid(minimal({OptionSchema{.long_name = "A", .kind = ValueKind::String}},
                           {PositionalSchema{.name = "A", .kind = ValueKind::String}}));  // 与选项长名冲突
    expect_invalid(
        minimal({}, {PositionalSchema{.name = "TAIL", .kind = ValueKind::String, .arity = Arity::zero_or_more()},
                     PositionalSchema{.name = "AFTER", .kind = ValueKind::String}}));  // 变长非末位
    expect_invalid(minimal(
        {}, {PositionalSchema{.name = "N", .kind = ValueKind::Int, .default_text = "x"}}));  // 默认值字面量不合法
    expect_valid(
        minimal({}, {PositionalSchema{.name = "TAIL", .kind = ValueKind::String, .arity = Arity::zero_or_more()}}));
}

AURORA_TEST_CASE(validate_checks_subcommand_declarations_recursively) {
    CommandSpec root;
    root.name = "root";
    root.subcommand_required = true;  // 要求子命令却没有子命令 → 自相矛盾
    expect_invalid(root);

    CommandSpec duplicated;
    duplicated.name = "parent";
    duplicated.subcommands = {CommandSpec{.name = "child"}, CommandSpec{.name = "child"}};
    expect_invalid(duplicated);

    CommandSpec blank;
    blank.name = "parent";
    blank.subcommands = {CommandSpec{}};  // 子命令无名
    expect_invalid(blank);

    // 孙层的错误也要被发现（递归覆盖全树）
    CommandSpec deep;
    deep.name = "deep";
    deep.subcommands = {CommandSpec{
        .name = "mid",
        .options = {OptionSchema{.long_name = "help", .kind = ValueKind::String}},
    }};
    const auto result = cli::validate(deep);
    AURORA_TEST_REQUIRE_FALSE(result);
    AURORA_TEST_CHECK(result.error().code == "cli-spec-invalid");
    AURORA_TEST_CHECK_THAT(result.error().message, m::has_substr("help"));
}

AURORA_TEST_CASE(spec_invalid_error_carries_the_reason) {
    const auto result = cli::validate(minimal({OptionSchema{.long_name = "help", .kind = ValueKind::String}}));
    AURORA_TEST_REQUIRE_FALSE(result);
    AURORA_TEST_CHECK(result.error().code_enum == au::ErrorCode::CliSpecInvalid);
    AURORA_TEST_CHECK_THAT(result.error().message, m::has_substr("built in"));
    AURORA_TEST_CHECK(result.error().category == au::ErrorCategory::Validation);
}

// ------------------------------------------------------------ 派生文本

AURORA_TEST_CASE(usage_line_renders_chain_options_positionals_and_commands) {
    AURORA_TEST_CHECK_EQ(cli::usage_line(spec()), "usage: aurora-render [OPTIONS] <SCENE>... [COMMAND] [-- ARGS...]");
    const auto *const render = spec().find_subcommand("render");
    AURORA_TEST_REQUIRE(render != nullptr);
    AURORA_TEST_CHECK_EQ(cli::usage_line(*render, {"aurora-render", "render"}),
                         "usage: aurora-render render [OPTIONS] <SRC> [<DST>] [-- ARGS...]");
    AURORA_TEST_CHECK_EQ(cli::usage_line(strict_spec()), "usage: strict [OPTIONS] <COMMAND> [-- ARGS...]");
}

AURORA_TEST_CASE(help_text_has_grouped_two_column_sections) {
    const auto text = cli::help_text(spec(), {"aurora-render"});
    AURORA_TEST_CHECK_THAT(text, m::starts_with("Render Aurora scenes into image files\n"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("usage: aurora-render [OPTIONS]"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Long description line for the root command."));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Options:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Diagnostics:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Help:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Arguments:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Commands:"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("  render"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Run 'aurora-render <COMMAND> --help'"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Report issues at the project tracker."));

    // 选项左列：短名 + 长名 + 占位符；右列带取值域 / 区间 / 默认值
    AURORA_TEST_CHECK_THAT(text, m::has_substr("-w, --width <WIDTH>"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[range: 1, 8192] [default: 800]"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[possible values: fast, balanced, quality]"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("-o, --output <FILE>"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("--tag <TAG> <TAG>..."));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("-f, --force"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("      --dry-run"));  // 无短名者以 4 空格占位对齐
    AURORA_TEST_CHECK_THAT(text, m::has_substr("-V, --version"));

    // hidden 项不进帮助
    AURORA_TEST_CHECK_FALSE(text.find("trace-file") != std::string::npos);
}

AURORA_TEST_CASE(help_text_marks_required_and_keeps_declaration_order) {
    const auto text = cli::help_text(strict_spec(), {"strict"});
    AURORA_TEST_CHECK_THAT(text, m::has_substr("--token <TOKEN>"));
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[required]"));
    const auto options_at = text.find("Options:");
    const auto help_at = text.find("Help:");
    AURORA_TEST_REQUIRE_TRUE(options_at != std::string::npos);
    AURORA_TEST_REQUIRE_TRUE(help_at != std::string::npos);
    AURORA_TEST_CHECK_TRUE(options_at < help_at);  // 内建 Help 组恒在最后
}

AURORA_TEST_CASE(help_text_renders_bounds_without_decimal_noise) {
    const auto root = minimal({OptionSchema{
        .long_name = "ratio",
        .kind = ValueKind::Double,
        .help = "Fractional and integral bounds",
        .minimum = 0.25,
        .maximum = 4,
    }});
    const auto text = cli::help_text(root);
    // 整值边界不带小数位、非整值去掉尾零：`[range: 0.25, 4]`，而非 std::to_string 的 6 位定长。
    AURORA_TEST_CHECK_THAT(text, m::has_substr("[range: 0.25, 4]"));
}

AURORA_TEST_CASE(version_text_is_program_scoped_and_empty_when_undeclared) {
    AURORA_TEST_CHECK_EQ(cli::version_text(spec()), "aurora-render 1.2.3\n");
    AURORA_TEST_CHECK_EQ(cli::version_text(spec(), "renamed"), "renamed 1.2.3\n");
    const auto *const render = spec().find_subcommand("render");
    AURORA_TEST_REQUIRE(render != nullptr);
    AURORA_TEST_CHECK_EQ(cli::version_text(*render), "");
    CommandSpec nameless;  // 无名根命令回落 "program"
    nameless.version = "0.1";
    AURORA_TEST_CHECK_EQ(cli::version_text(nameless), "program 0.1\n");
}

// ------------------------------------------------------------ schema 自描述

AURORA_TEST_CASE(schema_json_lists_declarations_builtins_and_subcommands) {
    const auto schema = cli::schema_json(spec());
    AURORA_TEST_CHECK_EQ(schema["name"].get<std::string>(), "aurora-render");
    AURORA_TEST_CHECK_EQ(schema["about"].get<std::string>(), "Render Aurora scenes into image files");
    AURORA_TEST_CHECK_EQ(schema["version"].get<std::string>(), "1.2.3");
    AURORA_TEST_CHECK_EQ(schema["usage"].get<std::string>(),
                         "usage: aurora-render [OPTIONS] <SCENE>... [COMMAND] [-- ARGS...]");
    AURORA_TEST_CHECK_EQ(schema["options"][0]["long"].get<std::string>(), "output");
    AURORA_TEST_CHECK_EQ(schema["subcommands"].size(), 2U);
    AURORA_TEST_CHECK_EQ(schema["subcommands"][0]["name"].get<std::string>(), "render");

    // 内建 help/version 一并列出，供 AI 枚举全量选项
    const auto &options = schema["options"];
    AURORA_TEST_CHECK_EQ(options[options.size() - 2]["long"].get<std::string>(), "help");
    AURORA_TEST_CHECK_EQ(options.back()["long"].get<std::string>(), "version");

    // hidden 项与 schema 的约定：不出现
    AURORA_TEST_CHECK_TRUE(option_node(schema, "trace-file").is_null());
}

AURORA_TEST_CASE(schema_json_encodes_arity_choices_and_bounds) {
    const auto schema = cli::schema_json(spec());
    const auto width = option_node(schema, "width");
    AURORA_TEST_REQUIRE_TRUE(width.is_object());
    AURORA_TEST_CHECK_EQ(width["min"].get<int>(), 1);
    AURORA_TEST_CHECK_EQ(width["max"].get<int>(), 1);
    AURORA_TEST_CHECK_EQ(width["type"].get<std::string>(), "int");
    AURORA_TEST_CHECK_EQ(width["short"].get<std::string>(), "w");
    AURORA_TEST_CHECK_EQ(width["maximum"].get<double>(), 8192.0);

    const auto tag = option_node(schema, "tag");
    AURORA_TEST_CHECK_EQ(tag["max"].get<int>(), Arity::AURORA_UNBOUNDED);
    AURORA_TEST_CHECK_EQ(tag["type"].get<std::string>(), "string");

    const auto mode = option_node(schema, "mode");
    AURORA_TEST_REQUIRE_EQ(mode["choices"].size(), 3U);
    AURORA_TEST_CHECK_EQ(mode["default"].get<std::string>(), "balanced");

    const auto verbose = option_node(schema, "verbose");
    AURORA_TEST_CHECK_EQ(verbose["type"].get<std::string>(), "bool");
    AURORA_TEST_CHECK_EQ(verbose["max"].get<int>(), 0);

    const auto &positional = schema["positionals"][0];
    AURORA_TEST_CHECK_EQ(positional["name"].get<std::string>(), "SCENE");
    AURORA_TEST_CHECK_EQ(positional["type"].get<std::string>(), "string");
}

AURORA_TEST_CASE(schema_json_reflects_positionals_and_required_subcommands) {
    const auto schema = cli::schema_json(strict_spec());
    AURORA_TEST_CHECK_EQ(schema["subcommand_required"].get<bool>(), true);
    AURORA_TEST_CHECK_FALSE(schema.contains("version"));
    const auto &deploy = schema["subcommands"][0];
    AURORA_TEST_REQUIRE_EQ(deploy["positionals"].size(), 1U);
    AURORA_TEST_CHECK_EQ(deploy["positionals"][0]["choices"].size(), 2U);
    AURORA_TEST_CHECK_EQ(deploy["usage"].get<std::string>(), "usage: deploy [OPTIONS] <ENV> [-- ARGS...]");
    AURORA_TEST_CHECK_EQ(option_node(schema, "token")["required"].get<bool>(), true);
}

// ------------------------------------------------------------ 文本 golden

AURORA_TEST_CASE(cli_texts_match_golden_baseline) {
    const std::filesystem::path path = golden::dir() / "cli_snapshots.json";
    const bool regen = golden::env_flag("AURORA_UPDATE_GOLDEN");

    nlohmann::json baseline = nlohmann::json::object();
    if (!regen) {
        std::ifstream in(path);
        AURORA_TEST_REQUIRE_MSG(
            in.good(), "golden baseline cli_snapshots.json must exist (run with AURORA_UPDATE_GOLDEN=1 to create)");
        in >> baseline;
        AURORA_TEST_REQUIRE_TRUE(baseline.contains("texts"));
    }

    const auto &root = spec();
    const auto *render = root.find_subcommand("render");
    AURORA_TEST_REQUIRE(render != nullptr);

    const nlohmann::json current = {
        {"texts",
         {
             {"root/usage", cli::usage_line(root)},
             {"root/help", cli::help_text(root, {"aurora-render"})},
             {"root/version", cli::version_text(root)},
             {"render/usage", cli::usage_line(*render, {"aurora-render", "render"})},
             {"render/help", cli::help_text(*render, {"aurora-render", "render"})},
             {"strict/usage", cli::usage_line(strict_spec())},
             {"strict/help", cli::help_text(strict_spec(), {"strict"})},
         }},
        {"schema", cli::schema_json(root)},
    };

    if (regen) {
        nlohmann::json doc;
        doc["_about"] =
            "Aurora aurora::cli text golden baseline (specification/09-cli.md). Do not hand-edit; regenerate with "
            "AURORA_UPDATE_GOLDEN=1 via aurora_test_runner --run=utest_cli_format.";
        doc["texts"] = current["texts"];
        doc["schema"] = current["schema"];
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        out << doc.dump(2) << "\n";
        AURORA_TEST_CHECK_TRUE(out.good());
        return;
    }

    for (const auto &[key, value] : current["texts"].items()) {
        AURORA_TEST_REQUIRE_TRUE(baseline["texts"].contains(key));
        AURORA_TEST_CHECK_EQ(value.get<std::string>(), baseline["texts"][key].get<std::string>());
    }
    AURORA_TEST_CHECK(current["schema"] == baseline["schema"]);
}

}  // namespace aurora::test_cases::utest_cli_format

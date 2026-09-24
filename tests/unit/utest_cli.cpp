/// 测试类型: unit
/// 目标单元: include/aurora/cli/args.h
/// 测试说明: 覆盖 argv 语法矩阵（长/短选项、四种带值写法、聚组、`--` 终止、负数消歧、重复项）、
/// arity/必填/取值域/互斥的失败路径与 cli-* 错误码、9 种 ValueKind 字面量转换与跨类型读出、
/// 子命令链下钻与叶遮蔽、默认值物化、--help/--version 一等结局及 argc/argv 入口

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/error_codes.h"
#include "framework/aurora_test.h"
#include "support/cli_fixture.h"

namespace aurora::test_cases::utest_cli {

namespace au = aurora;
namespace cli = aurora::cli;
using aurora::testing::cli_fixture::Tokens;
using aurora::testing::cli_fixture::spec;
using aurora::testing::cli_fixture::strict_spec;
using aurora::testing::cli_fixture::text_of;

/// @brief 用主样例树解析（program_name 留空 → 回落 root.name）。
[[nodiscard]] auto run(const std::vector<std::string> &tokens) -> au::Result<cli::Invocation> {
    return cli::parse(spec(), tokens);
}

/// @brief 取解析失败的错误码；成功返回 GeneralUnknown 以便断言失败时能打印出值。
[[nodiscard]] auto code_of(const au::Error &error) -> au::ErrorCode { return error.code_enum; }

AURORA_TEST_CASE(long_option_accepts_space_and_equals_forms) {
    for (const std::vector<std::string> &tokens :
         {Tokens{"--width", "1024"}, Tokens{"--width=1024"}}) {
        const auto parsed = run(tokens);
        AURORA_TEST_REQUIRE(parsed);
        AURORA_TEST_CHECK_EQ(parsed.value().arguments.get<int>("width").unwrap(), 1024);
    }
}

AURORA_TEST_CASE(short_option_accepts_space_attached_and_equals_value) {
    for (const std::vector<std::string> &tokens :
         {Tokens{"-w", "1024"}, Tokens{"-w1024"}, Tokens{"-w=1024"}}) {
        const auto parsed = run(tokens);
        AURORA_TEST_REQUIRE(parsed);
        AURORA_TEST_CHECK_EQ(parsed.value().arguments.get<int>("width").unwrap(), 1024);
    }
}

AURORA_TEST_CASE(short_flags_cluster_and_mixed_cluster_with_value) {
    const auto parsed = run(Tokens{"-vf"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_TRUE(parsed.value().arguments.flag("verbose"));
    AURORA_TEST_CHECK_TRUE(parsed.value().arguments.flag("force"));

    // 簇尾带值选项：`-vw80` = `-v -w 80`
    const auto mixed = run(Tokens{"-vw80"});
    AURORA_TEST_REQUIRE(mixed);
    AURORA_TEST_CHECK_TRUE(mixed.value().arguments.flag("verbose"));
    AURORA_TEST_CHECK_EQ(mixed.value().arguments.get<int>("width").unwrap(), 80);

    // 短名带值：`-o shot.png` 与 `-oshot.png` 同义
    const auto attached = run(Tokens{"-oshot.png"});
    AURORA_TEST_REQUIRE(attached);
    AURORA_TEST_CHECK_EQ(text_of(attached.value().arguments, "output"), "shot.png");
}

AURORA_TEST_CASE(flag_without_value_consumes_nothing) {
    const auto parsed = run(Tokens{"--verbose"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_TRUE(parsed.value().arguments.flag("verbose"));
    // flag 不吞后续裸 token：其后的 SCENE 仍是位置参数
    const auto with_positional = run(Tokens{"--verbose", "a.scene"});
    AURORA_TEST_REQUIRE(with_positional);
    AURORA_TEST_CHECK_TRUE(with_positional.value().arguments.flag("verbose"));
    AURORA_TEST_CHECK_EQ(with_positional.value().arguments.positionals().size(), 1U);
}

AURORA_TEST_CASE(explicit_boolean_value_is_accepted) {
    const auto parsed = run(Tokens{"--verbose=false"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_FALSE(parsed.value().arguments.flag("verbose"));
    AURORA_TEST_CHECK_TRUE(parsed.value().arguments.explicitly_given("verbose"));

    const auto truthy = run(Tokens{"--force=yes"});
    AURORA_TEST_REQUIRE(truthy);
    AURORA_TEST_CHECK_TRUE(truthy.value().arguments.flag("force"));
}

AURORA_TEST_CASE(defaults_are_materialized_but_not_marked_given) {
    const auto parsed = run({});
    AURORA_TEST_REQUIRE(parsed);
    const auto &args = parsed.value().arguments;
    AURORA_TEST_CHECK_EQ(args.get<int>("width").unwrap(), 800);
    AURORA_TEST_CHECK_EQ(text_of(args, "output"), "out.png");
    AURORA_TEST_CHECK_FALSE(args.explicitly_given("width"));
    AURORA_TEST_CHECK_FALSE(args.flag("verbose"));
    AURORA_TEST_CHECK_EQ(args.count("verbose"), 0);
    AURORA_TEST_CHECK_TRUE(args.values("tag").empty());
}

AURORA_TEST_CASE(explicit_value_overrides_default_and_marks_given) {
    const auto parsed = run(Tokens{"--width", "640"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_TRUE(parsed.value().arguments.explicitly_given("width"));
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.get<int>("width").unwrap(), 640);
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.count("width"), 1);
}

AURORA_TEST_CASE(repeatable_option_accumulates_in_order) {
    const auto parsed = run(Tokens{"--tag", "a", "--tag", "b"});
    AURORA_TEST_REQUIRE(parsed);
    const auto values = parsed.value().arguments.values("tag");
    AURORA_TEST_REQUIRE_EQ(values.size(), 2U);
    AURORA_TEST_CHECK_EQ(values[0].as_string().unwrap(), "a");
    AURORA_TEST_CHECK_EQ(values[1].as_string().unwrap(), "b");
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.count("tag"), 2);
}

AURORA_TEST_CASE(variable_arity_option_greedily_consumes_until_next_option) {
    const auto parsed = run(Tokens{"--tag", "a", "b", "c", "--width", "100"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.values("tag").size(), 3U);
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.get<int>("width").unwrap(), 100);
}

AURORA_TEST_CASE(single_value_option_rejects_multi_occurrence) {
    const auto parsed = run(Tokens{"--width", "10", "--width", "20"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliArityViolated);
    AURORA_TEST_CHECK_THAT(parsed.error().message, au::testing::matchers::has_substr("--width"));
}

AURORA_TEST_CASE(double_dash_terminates_option_parsing) {
    const auto parsed = run(Tokens{"--", "--width", "42"});
    AURORA_TEST_REQUIRE(parsed);
    // 根命令的 SCENE 是变长位置参数，故 `--` 之后仍按位置参数吸收，且不做类型转换以外解释
    const auto &args = parsed.value().arguments;
    AURORA_TEST_REQUIRE_EQ(args.positionals().size(), 2U);
    AURORA_TEST_CHECK_EQ(args.positionals()[0].as_string().unwrap(), "--width");
    AURORA_TEST_CHECK_EQ(args.get<int>("width").unwrap(), 800);  // 未被覆盖，仍是默认值
}

AURORA_TEST_CASE(overflow_after_double_dash_goes_to_rest) {
    const auto parsed = run(Tokens{"render", "src.scene", "dst.png", "--", "--flag", "-x"});
    AURORA_TEST_REQUIRE(parsed);
    const auto &args = parsed.value().arguments;
    AURORA_TEST_REQUIRE_EQ(args.positionals().size(), 2U);
    AURORA_TEST_CHECK_EQ(args.positional(0).unwrap().as_string().unwrap(), "src.scene");
    // `--` 之后的 token 原样落在 rest()，不做类型转换也不报错
    AURORA_TEST_REQUIRE_EQ(args.rest().size(), 2U);
    AURORA_TEST_CHECK_EQ(args.rest()[0], "--flag");
    AURORA_TEST_CHECK_EQ(args.rest()[1], "-x");
}

AURORA_TEST_CASE(negative_number_token_is_a_value_not_an_option) {
    const auto parsed = run(Tokens{"--scale", "-1.5"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_NEAR(parsed.value().arguments.get<double>("scale").unwrap(), -1.5, 0.0001);
}

AURORA_TEST_CASE(unknown_long_option_reports_did_you_mean) {
    const auto parsed = run(Tokens{"--widht", "10"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliUnknownOption);
    AURORA_TEST_CHECK_THAT(parsed.error().message, au::testing::matchers::has_substr("--widht"));
    AURORA_TEST_CHECK_THAT(parsed.error().suggestion, au::testing::matchers::has_substr("--width"));
}

AURORA_TEST_CASE(unknown_short_option_is_rejected) {
    const auto parsed = run(Tokens{"-Z"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliUnknownOption);
}

AURORA_TEST_CASE(option_without_value_is_missing_value_error) {
    for (const std::vector<std::string> &tokens : {Tokens{"--width"}, Tokens{"--width", "--force"}}) {
        const auto parsed = run(tokens);
        AURORA_TEST_REQUIRE_FALSE(parsed);
        AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliMissingValue);
    }
}

AURORA_TEST_CASE(mismatched_literal_is_invalid_value_error) {
    const auto parsed = run(Tokens{"--width", "abc"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliInvalidValue);
    AURORA_TEST_CHECK_THAT(parsed.error().message, au::testing::matchers::has_substr("abc"));
}

AURORA_TEST_CASE(choice_outside_vocabulary_is_rejected) {
    const auto parsed = run(Tokens{"--mode", "turbo"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliChoiceInvalid);
    AURORA_TEST_CHECK_THAT(parsed.error().suggestion, au::testing::matchers::has_substr("fast"));
}

AURORA_TEST_CASE(numeric_value_outside_declared_range_is_rejected) {
    const auto parsed = run(Tokens{"--width", "99999"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliRangeViolated);
    // 边界文案与 --help 的 `[range: ...]` 同口径：整值不带小数位。
    AURORA_TEST_CHECK_THAT(parsed.error().message, au::testing::matchers::has_substr("[1, 8192]"));
    AURORA_TEST_CHECK_THAT(parsed.error().message, au::testing::matchers::has_substr("8192"));
}

AURORA_TEST_CASE(conflicting_flags_cannot_coexist) {
    const auto parsed = run(Tokens{"--force", "--dry-run"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliConflictViolated);
    // 单侧出现则放行
    AURORA_TEST_CHECK_TRUE(run(Tokens{"--force"}));
}

AURORA_TEST_CASE(required_option_and_mandatory_subcommand_are_enforced) {
    const std::vector<std::string> none;
    const auto bare = cli::parse(strict_spec(), none);
    AURORA_TEST_REQUIRE_FALSE(bare);
    AURORA_TEST_CHECK(code_of(bare.error()) == au::ErrorCode::CliMissingRequired);

    const auto without_sub = cli::parse(strict_spec(), Tokens{"--token", "secret"});
    AURORA_TEST_REQUIRE_FALSE(without_sub);
    AURORA_TEST_CHECK(code_of(without_sub.error()) == au::ErrorCode::CliMissingSubcommand);

    const auto without_positional = cli::parse(strict_spec(), Tokens{"--token", "secret", "deploy"});
    AURORA_TEST_REQUIRE_FALSE(without_positional);
    AURORA_TEST_CHECK(code_of(without_positional.error()) == au::ErrorCode::CliMissingRequired);

    const auto ok = cli::parse(strict_spec(), Tokens{"--token", "secret", "deploy", "prod"});
    AURORA_TEST_REQUIRE(ok);
    AURORA_TEST_CHECK_EQ(text_of(ok.value().arguments, "token"), "secret");
    AURORA_TEST_CHECK_EQ(ok.value().arguments.positional(0).unwrap().as_string().unwrap(), "prod");
}

AURORA_TEST_CASE(help_beats_pending_required_errors) {
    // 严格树缺 --token 会报必填，但 --help 的优先级更高：用户要的是说明书
    const auto parsed = cli::parse(strict_spec(), Tokens{"--help"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK(parsed.value().outcome == cli::ParseOutcome::Help);
    AURORA_TEST_CHECK_THAT(parsed.value().display_text, au::testing::matchers::has_substr("usage: strict"));
}

AURORA_TEST_CASE(positionals_are_bound_in_declaration_order) {
    const auto parsed = run(Tokens{"render", "in.scene", "out.webp"});
    AURORA_TEST_REQUIRE(parsed);
    const auto &args = parsed.value().arguments;
    AURORA_TEST_CHECK_EQ(args.positional(0).unwrap().as_string().unwrap(), "in.scene");
    AURORA_TEST_CHECK_EQ(args.positional(1).unwrap().as_string().unwrap(), "out.webp");
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(args.positional(2)));
}

AURORA_TEST_CASE(too_many_positionals_are_rejected) {
    const auto parsed = run(Tokens{"render", "a", "b", "c"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliTooManyPositionals);
}

AURORA_TEST_CASE(subcommand_chain_is_recorded_and_leaf_shadows_parent) {
    const auto leaf = run(Tokens{"render", "--format", "webp", "x"});
    AURORA_TEST_REQUIRE(leaf);
    const auto &chain = leaf.value().arguments.command_chain();
    AURORA_TEST_REQUIRE_EQ(chain.size(), 2U);
    AURORA_TEST_CHECK_EQ(chain[0], "aurora-render");
    AURORA_TEST_CHECK_EQ(chain[1], "render");
    AURORA_TEST_CHECK_EQ(leaf.value().arguments.command_display(), "aurora-render render");
    AURORA_TEST_CHECK_EQ(text_of(leaf.value().arguments, "format"), "webp");
    AURORA_TEST_CHECK(leaf.value().arguments.matched_command() != nullptr);
    AURORA_TEST_CHECK(leaf.value().arguments.matched_command()->name == "render");

    // 父级选项必须写在子命令名之前；合并取「最深显式给出者」，叶级默认值不抹掉父级显式值
    const auto parent_given = run(Tokens{"--format", "json", "render", "x"});
    AURORA_TEST_REQUIRE(parent_given);
    AURORA_TEST_CHECK_EQ(text_of(parent_given.value().arguments, "format"), "json");

    // 叶级一旦显式写出，就遮蔽父级（哪怕父级也写过）
    const auto leaf_given = run(Tokens{"--format", "json", "render", "--format", "webp", "x"});
    AURORA_TEST_REQUIRE(leaf_given);
    AURORA_TEST_CHECK_EQ(text_of(leaf_given.value().arguments, "format"), "webp");

    // 两层都没写：叶级的默认值生效
    const auto neither = run(Tokens{"render", "x"});
    AURORA_TEST_REQUIRE(neither);
    AURORA_TEST_CHECK_EQ(text_of(neither.value().arguments, "format"), "png");
    AURORA_TEST_CHECK_FALSE(neither.value().arguments.explicitly_given("format"));
}

AURORA_TEST_CASE(parent_option_after_subcommand_is_unknown) {
    // `render` 自己没有 --width，故子命令之后的 --width 属未知选项（GNU 层级语义）
    const auto parsed = run(Tokens{"render", "--width", "100", "x"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliUnknownOption);
}

AURORA_TEST_CASE(unknown_subcommand_lists_candidates) {
    // 严格树没有可吸收裸 token 的位置参数槽，故未命中子命令名即为未知子命令
    const auto parsed = cli::parse(strict_spec(), Tokens{"rnder"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    AURORA_TEST_CHECK(code_of(parsed.error()) == au::ErrorCode::CliUnknownSubcommand);
    AURORA_TEST_CHECK_THAT(parsed.error().suggestion, au::testing::matchers::has_substr("deploy"));
}

AURORA_TEST_CASE(variadic_positional_absorbs_tokens_that_are_not_subcommand_names) {
    // 精确命中子命令名才下钻；近似拼写被根命令的变长位置参数吸收（git / cobra 同规则）
    const auto parsed = run(Tokens{"rnder"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.positionals()[0].as_string().unwrap(), "rnder");
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.command_chain().size(), 1U);
}

AURORA_TEST_CASE(strong_type_literals_convert_to_project_types) {
    const auto parsed = run(Tokens{"--margin", "25%", "--tint", "#ff0000", "--level", "debug", "--timeout", "2s"});
    AURORA_TEST_REQUIRE(parsed);
    const auto &args = parsed.value().arguments;

    const auto margin = args.get<au::Length>("margin");
    AURORA_TEST_REQUIRE(margin);
    AURORA_TEST_CHECK(margin.value().kind == au::LengthKind::Fraction);
    AURORA_TEST_CHECK_NEAR(margin.value().value, 0.25F, 0.0001);

    const auto tint = args.get<au::Color>("tint");
    AURORA_TEST_REQUIRE(tint);
    AURORA_TEST_CHECK(tint.value() == au::Color{255, 0, 0});

    const auto level = args.get<au::LogLevel>("level");
    AURORA_TEST_REQUIRE(level);
    AURORA_TEST_CHECK(level.value() == au::LogLevel::Debug);

    const auto timeout = args.value("timeout");
    AURORA_TEST_REQUIRE(timeout);
    AURORA_TEST_CHECK_EQ(timeout.value().as_duration_ms().unwrap(), 2000);
}

AURORA_TEST_CASE(length_and_color_accept_alternate_spellings) {
    const auto fixed = run(Tokens{"--margin", "12px"});
    AURORA_TEST_REQUIRE(fixed);
    AURORA_TEST_CHECK(fixed.value().arguments.get<au::Length>("margin").unwrap().kind == au::LengthKind::Fixed);

    const auto keyword = run(Tokens{"--margin", "fill"});
    AURORA_TEST_REQUIRE(keyword);
    AURORA_TEST_CHECK(keyword.value().arguments.get<au::Length>("margin").unwrap().kind == au::LengthKind::Expand);

    const auto short_hex = run(Tokens{"--tint", "#f008"});
    AURORA_TEST_REQUIRE(short_hex);
    AURORA_TEST_CHECK(short_hex.value().arguments.get<au::Color>("tint").unwrap() == au::Color{255, 0, 0, 136});

    const auto functional = run(Tokens{"--tint", "rgb(1,2,3)"});
    AURORA_TEST_REQUIRE(functional);
    AURORA_TEST_CHECK(functional.value().arguments.get<au::Color>("tint").unwrap() == au::Color{1, 2, 3, 255});

    const auto rgba = run(Tokens{"--tint", "rgba(1,2,3,4)"});
    AURORA_TEST_REQUIRE(rgba);
    AURORA_TEST_CHECK(rgba.value().arguments.get<au::Color>("tint").unwrap() == au::Color{1, 2, 3, 4});

    // 通道号越界/写法残缺一律按非法字面量拒绝
    AURORA_TEST_CHECK_FALSE(run(Tokens{"--tint", "#12345"}));
    AURORA_TEST_CHECK_FALSE(run(Tokens{"--tint", "rgb(300,0,0)"}));
    AURORA_TEST_CHECK_FALSE(run(Tokens{"--margin", "-4"}));
}

AURORA_TEST_CASE(log_level_accepts_full_and_short_words_case_insensitively) {
    struct Case {
        std::string_view token;
        au::LogLevel level;
    };
    for (const Case &c : {Case{.token = "TRACE", .level = au::LogLevel::Trace}, Case{.token = "trc", .level = au::LogLevel::Trace},
                          Case{.token = "Warning", .level = au::LogLevel::Warn}, Case{.token = "wrn", .level = au::LogLevel::Warn},
                          Case{.token = "fatal", .level = au::LogLevel::Fatal}}) {
        const auto parsed = run(Tokens{"--level", c.token});
        AURORA_TEST_REQUIRE(parsed);
        const auto level = parsed.value().arguments.get<au::LogLevel>("level");
        AURORA_TEST_REQUIRE(level);
        AURORA_TEST_CHECK(level.value() == c.level);
    }
    const auto bogus = run(Tokens{"--level", "loud"});
    AURORA_TEST_REQUIRE_FALSE(bogus);
    AURORA_TEST_CHECK(code_of(bogus.error()) == au::ErrorCode::CliInvalidValue);
}

AURORA_TEST_CASE(duration_units_scale_to_milliseconds) {
    struct Case {
        std::string_view token;
        std::int64_t ms;
    };
    for (const Case &c : {Case{.token = "500", .ms = 500}, Case{.token = "250ms", .ms = 250}, Case{.token = "5s", .ms = 5000},
                          Case{.token = "2m", .ms = 120000},
                          Case{.token = "1h", .ms = 3600000}, Case{.token = "1d", .ms = 86400000}}) {
        const auto parsed = run(Tokens{"--timeout", c.token});
        AURORA_TEST_REQUIRE(parsed);
        AURORA_TEST_CHECK_EQ(parsed.value().arguments.value("timeout").unwrap().as_duration_ms().unwrap(), c.ms);
    }
}

AURORA_TEST_CASE(numeric_kinds_widen_losslessly_and_reject_lossy_reads) {
    // Int → double 加宽
    const auto integer = run(Tokens{"--width", "800"});
    AURORA_TEST_REQUIRE(integer);
    AURORA_TEST_CHECK_NEAR(integer.value().arguments.value("width").unwrap().as_double().unwrap(), 800.0, 0.0001);

    // double 整值 → int 放行；非整值 → cli-invalid-value
    const auto whole = run(Tokens{"--scale", "3"});
    AURORA_TEST_REQUIRE(whole);
    AURORA_TEST_CHECK_EQ(whole.value().arguments.value("scale").unwrap().as_int().unwrap(), 3);

    const auto fractional = run(Tokens{"--scale", "1.5"});
    AURORA_TEST_REQUIRE(fractional);
    const auto narrowed = fractional.value().arguments.value("scale").unwrap().as_int();
    AURORA_TEST_REQUIRE_FALSE(narrowed);
    AURORA_TEST_CHECK(code_of(narrowed.error()) == au::ErrorCode::CliInvalidValue);
}

AURORA_TEST_CASE(type_mismatch_read_is_invalid_value_error) {
    const auto parsed = run(Tokens{"--width", "100"});
    AURORA_TEST_REQUIRE(parsed);
    const auto as_text = parsed.value().arguments.get<std::string>("width");
    AURORA_TEST_REQUIRE_FALSE(as_text);
    AURORA_TEST_CHECK(code_of(as_text.error()) == au::ErrorCode::CliInvalidValue);

    // 未声明的长名：value 失败、flag 保守返回 false
    AURORA_TEST_CHECK(code_of(parsed.value().arguments.value("nope").error()) == au::ErrorCode::CliMissingRequired);
    AURORA_TEST_CHECK_FALSE(parsed.value().arguments.flag("nope"));
}

AURORA_TEST_CASE(raw_text_is_always_available_regardless_of_kind) {
    const auto parsed = run(Tokens{"--margin", "25%", "--timeout", "2s"});
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.value("margin").unwrap().raw_text(), "25%");
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.value("timeout").unwrap().raw_text(), "2s");
    AURORA_TEST_CHECK(parsed.value().arguments.value("margin").unwrap().kind() == cli::ValueKind::Length);
}

AURORA_TEST_CASE(help_option_is_builtin_at_every_level) {
    for (const std::vector<std::string> &tokens :
         {Tokens{"--help"}, Tokens{"-h"}, Tokens{"render", "--help"}, Tokens{"serve", "-h"}}) {
        const auto parsed = run(tokens);
        AURORA_TEST_REQUIRE(parsed);
        AURORA_TEST_CHECK(parsed.value().outcome == cli::ParseOutcome::Help);
        AURORA_TEST_CHECK_FALSE(parsed.value().display_text.empty());
    }
    const auto nested = run(Tokens{"render", "--help"});
    AURORA_TEST_REQUIRE(nested);
    AURORA_TEST_CHECK_THAT(nested.value().display_text, au::testing::matchers::has_substr("aurora-render render"));
}

AURORA_TEST_CASE(version_option_only_exists_where_declared) {
    for (const std::vector<std::string> &tokens : {Tokens{"--version"}, Tokens{"-V"}}) {
        const auto parsed = run(tokens);
        AURORA_TEST_REQUIRE(parsed);
        AURORA_TEST_CHECK(parsed.value().outcome == cli::ParseOutcome::Version);
        AURORA_TEST_CHECK_EQ(parsed.value().display_text, "aurora-render 1.2.3\n");
    }

    // render 子命令未声明 version → --version / -V 不成立，按未知选项处理
    const auto sub = run(Tokens{"render", "--version"});
    AURORA_TEST_REQUIRE_FALSE(sub);
    AURORA_TEST_CHECK(code_of(sub.error()) == au::ErrorCode::CliUnknownOption);
    const auto sub_short = run(Tokens{"render", "-V"});
    AURORA_TEST_REQUIRE_FALSE(sub_short);
    AURORA_TEST_CHECK(code_of(sub_short.error()) == au::ErrorCode::CliUnknownOption);
}

AURORA_TEST_CASE(outcome_to_string_is_enumerable_wire_vocabulary) {
    AURORA_TEST_CHECK_EQ(cli::outcome_to_string(cli::ParseOutcome::Ok), "ok");
    AURORA_TEST_CHECK_EQ(cli::outcome_to_string(cli::ParseOutcome::Help), "help");
    AURORA_TEST_CHECK_EQ(cli::outcome_to_string(cli::ParseOutcome::Version), "version");
}

AURORA_TEST_CASE(argc_argv_entry_strips_program_name_and_uses_basename) {
    const char *windows_argv[] = {"C:\\tools\\aurora-render.exe", "--width", "320"};
    const auto windows = cli::parse(spec(), 3, windows_argv);
    AURORA_TEST_REQUIRE(windows);
    AURORA_TEST_CHECK_EQ(std::string{windows.value().arguments.program_name()}, "aurora-render.exe");
    AURORA_TEST_CHECK_EQ(windows.value().arguments.get<int>("width").unwrap(), 320);

    const char *posix_argv[] = {"/usr/local/bin/aurora-render", "--width", "320"};
    const auto posix = cli::parse(spec(), 3, posix_argv);
    AURORA_TEST_REQUIRE(posix);
    AURORA_TEST_CHECK_EQ(std::string{posix.value().arguments.program_name()}, "aurora-render");

    // 无 argv 可用时回落根命令名，且 argc<=0 不算错误
    const auto none = cli::parse(spec(), 0, nullptr);
    AURORA_TEST_REQUIRE(none);
    AURORA_TEST_CHECK_EQ(std::string{none.value().arguments.program_name()}, "aurora-render");
}

AURORA_TEST_CASE(program_name_override_wins_over_root_name) {
    const auto parsed = cli::parse(spec(), Tokens{"--width", "1"}, "custom-name");
    AURORA_TEST_REQUIRE(parsed);
    AURORA_TEST_CHECK_EQ(std::string{parsed.value().arguments.program_name()}, "custom-name");
    AURORA_TEST_CHECK_EQ(parsed.value().arguments.command_display(), "custom-name");
}

AURORA_TEST_CASE(error_payload_carries_slug_and_machine_fields) {
    const auto parsed = run(Tokens{"--mode", "turbo"});
    AURORA_TEST_REQUIRE_FALSE(parsed);
    const auto &error = parsed.error();
    AURORA_TEST_CHECK_EQ(error.code, "cli-choice-invalid");
    AURORA_TEST_CHECK(error.category == au::ErrorCategory::Validation);
    AURORA_TEST_CHECK_THAT(error.to_json(), au::testing::matchers::has_substr("cli-choice-invalid"));
}

}  // namespace aurora::test_cases::utest_cli

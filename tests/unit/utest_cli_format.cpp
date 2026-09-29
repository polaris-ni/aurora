/// 测试类型: unit
/// 目标单元: include/aurora/cli/command.h
/// 测试说明: 覆盖 ValueKind 词表与 Arity 工厂的可枚举性、validate 对声明表的 20 余条静态门禁
/// （含 `early_view` 必须是零值 arity 的 Bool）、内建 help/version 的惰性注入与让位、
/// usage/help/version 派生文本形态、schema_json 自描述结构，并以 cli_snapshots.json
/// 做文本 golden 基线（AURORA_UPDATE_GOLDEN=1 再生成）

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/core/error_codes.h"
#include "framework/aurora_test.h"
#include "framework/golden.h"
#include "framework/json_access.h"
#include "support/cli_fixture.h"

namespace aurora::test_cases::utest_cli_format {

namespace au = aurora;
namespace cli = aurora::cli;
namespace m = aurora::testing::matchers;
namespace golden = aurora::testing::golden;
using aurora::testing::require_child;
using aurora::testing::require_child_at;
using aurora::testing::require_field;
using aurora::testing::require_value;
using aurora::testing::cli_fixture::displacement_spec;
using aurora::testing::cli_fixture::spec;
using aurora::testing::cli_fixture::strict_spec;
using aurora::testing::cli_fixture::Tokens;
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

/// @brief 按名找直接子命令。`CommandSpec` 不再暴露 `find_*`（对外发现能力由 `schema_json` 承担），
///        测试侧用同语义的本地查找代替，避免为了测试把库内原语重新开放成公共 API。
[[nodiscard]] auto sub_of(const CommandSpec &root, std::string_view name) -> const CommandSpec * {
    for (const auto &sub : root.subcommands) {
        if (sub.name == name) {
            return &sub;
        }
    }
    return nullptr;
}

/// @brief 取 schema 里某个长名的选项节点；未找到返回 nullptr。
[[nodiscard]] auto option_node(const json::Value &command, std::string_view long_name) -> const json::Value * {
    const auto *options = command.at("options");
    if (options == nullptr) {
        return nullptr;
    }
    for (const auto &option : *options) {
        if (option.as_or<std::string_view>("long", std::string_view{}) == long_name) {
            return &option;
        }
    }
    return nullptr;
}

/// @brief 取 schema 里某个长名的选项节点并断言其存在；未找到即本用例致命失败。
[[nodiscard]] auto option_of(const json::Value &command, std::string_view long_name) -> const json::Value & {
    const auto *node = option_node(command, long_name);
    AURORA_TEST_REQUIRE(node != nullptr);
    return *node;
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

/// @brief 两张封闭表的桥：`ValueKind`（9 项，schema 的 `type` 词表）↔ `Value::as<T>()` 的 C++ 支持集。
struct KindRead {
    ValueKind kind;
    std::string_view literal;  ///< 该 kind 的一个合法字面量
    std::string_view cpp_type;  ///< 规范读出类型（`as<T>` 的 T 名）
};

/// @brief 按登记的类型名直调 `as<T>()`，返回是否读成功（表驱动穷尽性检查用）。
[[nodiscard]] auto read_back(const cli::Value &value, std::string_view cpp_type) -> bool {
    if (cpp_type == "bool") {
        return static_cast<bool>(value.as<bool>());
    }
    if (cpp_type == "int64_t") {
        return static_cast<bool>(value.as<std::int64_t>());
    }
    if (cpp_type == "double") {
        return static_cast<bool>(value.as<double>());
    }
    if (cpp_type == "std::string") {
        return static_cast<bool>(value.as<std::string>());
    }
    if (cpp_type == "Length") {
        return static_cast<bool>(value.as<au::Length>());
    }
    if (cpp_type == "Color") {
        return static_cast<bool>(value.as<au::Color>());
    }
    if (cpp_type == "LogLevel") {
        return static_cast<bool>(value.as<au::LogLevel>());
    }
    return false;
}

AURORA_TEST_CASE(every_value_kind_has_a_documented_read_type) {
    // 词表封闭是需求 SPEC.QUALITY.CORE.STRUCTURED-ERROR.001 的承诺：9 个 ValueKind 各自必须至少有一个 `as<T>`
    // 类型能无损读出， 且登记的 C++ 类型集不得超出 args.h static_assert 文案那份（8 个：Enum 复用 string、 Duration
    // 复用 int64_t）。任何一侧新增/删除项而未同步另一侧，这里先红。
    const std::vector<KindRead> reads = {
        KindRead{.kind = ValueKind::Bool, .literal = "true", .cpp_type = "bool"},
        KindRead{.kind = ValueKind::Int, .literal = "12", .cpp_type = "int64_t"},
        KindRead{.kind = ValueKind::Double, .literal = "1.5", .cpp_type = "double"},
        KindRead{.kind = ValueKind::String, .literal = "s", .cpp_type = "std::string"},
        KindRead{.kind = ValueKind::Enum, .literal = "a", .cpp_type = "std::string"},
        KindRead{.kind = ValueKind::Length, .literal = "25%", .cpp_type = "Length"},
        KindRead{.kind = ValueKind::Color, .literal = "#fff", .cpp_type = "Color"},
        KindRead{.kind = ValueKind::LogLevel, .literal = "debug", .cpp_type = "LogLevel"},
        KindRead{.kind = ValueKind::Duration, .literal = "2s", .cpp_type = "int64_t"},
    };
    AURORA_TEST_REQUIRE_EQ(reads.size(), cli::all_value_kinds().size());
    for (const KindRead &read : reads) {
        OptionSchema option;
        option.long_name = "v";
        option.kind = read.kind;
        option.arity = (read.kind == ValueKind::Bool) ? Arity::flag() : Arity::exactly_one();
        if (read.kind == ValueKind::Enum) {
            option.choices = {std::string{read.literal}};  // Enum 的取值域唯一合法来源
        }
        // 声明表按指针借用，故 root 必须比 parsed 活得久：两者都留在本层作用域
        const auto root = minimal({option});
        const auto parsed = cli::parse(root, Tokens{"--v=" + std::string{read.literal}});
        AURORA_TEST_REQUIRE_MSG(
            parsed,
            parsed ? std::string{} : "kind " + std::string{cli::to_string(read.kind)} + ": " + parsed.error().message);
        const auto value = parsed.unwrap().arguments.values("v").front();
        AURORA_TEST_CHECK(value.kind() == read.kind);
        AURORA_TEST_CHECK_EQ(value.raw_text(), std::string{read.literal});
        AURORA_TEST_CHECK_MSG(read_back(value, read.cpp_type), "kind " + std::string{cli::to_string(read.kind)} +
                                                                   " unreadable as " + std::string{read.cpp_type});
    }
    // 登记到的 C++ 类型去重后 7 个；`as<T>` 另收 `int`（int64_t 的窄化别名），合计 args.h
    // static_assert 文案里那 8 个。表里若多出未登记的类型名，read_back 会返回 false 而在上面红灯。
    std::vector<std::string_view> distinct;
    for (const KindRead &read : reads) {
        if (std::find(distinct.begin(), distinct.end(), read.cpp_type) == distinct.end()) {
            distinct.push_back(read.cpp_type);
        }
    }
    AURORA_TEST_CHECK_EQ(distinct.size(), 7U);
}

AURORA_TEST_CASE(arity_factories_describe_consumed_value_tokens) {
    AURORA_TEST_CHECK(Arity::flag().allows_no_value());
    AURORA_TEST_CHECK_FALSE(Arity::exactly_one().allows_no_value());
    AURORA_TEST_CHECK_EQ(Arity::exactly_one().min, 1);
    // 非标准区间（如定长 3、至多 2）不再有命名工厂：`Arity` 是公开聚合，直接写区间即可。
    AURORA_TEST_CHECK_EQ((Arity{.min = 3, .max = 3}).max, 3);
    AURORA_TEST_CHECK_EQ((Arity{.min = 0, .max = 2}).min, 0);

    // 定长 span 决定「一条选项吃几个后续 token」
    AURORA_TEST_CHECK_EQ(Arity::exactly_one().fixed_span().value_or(-7), 1);
    AURORA_TEST_CHECK_EQ((Arity{.min = 2, .max = 2}).fixed_span().value_or(-7), 2);
    AURORA_TEST_CHECK_FALSE(Arity::at_least_one().fixed_span().has_value());
    AURORA_TEST_CHECK_FALSE(Arity::zero_or_more().fixed_span().has_value());

    AURORA_TEST_CHECK_EQ(Arity::zero_or_more().max_text(), "∞");
    AURORA_TEST_CHECK_EQ((Arity{.min = 4, .max = 4}).max_text(), "4");
}

AURORA_TEST_CASE(arity_help_placeholders_follow_optionality) {
    AURORA_TEST_CHECK_EQ(Arity::flag().help_placeholder("X"), "");
    AURORA_TEST_CHECK_EQ(Arity::exactly_one().help_placeholder("WIDTH"), "<WIDTH>");
    AURORA_TEST_CHECK_EQ(Arity::optional_one().help_placeholder("X"), "<X>");
    AURORA_TEST_CHECK_EQ(Arity::at_least_one().help_placeholder("TAG"), "<TAG> <TAG>...");
    AURORA_TEST_CHECK_EQ(Arity::zero_or_more().help_placeholder("TAG"), "<TAG>...");
    AURORA_TEST_CHECK_EQ((Arity{.min = 2, .max = 2}).help_placeholder("XY"), "<XY>");
}

// ------------------------------------------------------------ validate 门禁

AURORA_TEST_CASE(validate_accepts_the_shared_fixtures) {
    const auto ok = cli::validate(spec());
    AURORA_TEST_REQUIRE(ok);
    AURORA_TEST_CHECK_EQ(ok.value(), 3);  // 根 + render + serve
    AURORA_TEST_CHECK_EQ(cli::validate(strict_spec()).unwrap(), 2);
    // 让位树（占用 -h/-V、自标 early_view、子命令关内建）同样是合法声明
    AURORA_TEST_CHECK_EQ(cli::validate(displacement_spec()).unwrap(), 2);
}

AURORA_TEST_CASE(validate_rejects_malformed_long_names) {
    expect_invalid(minimal({OptionSchema{.kind = ValueKind::String}}));  // 无长名
    expect_invalid(minimal({OptionSchema{.long_name = "-x", .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "a b", .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "out", .kind = ValueKind::String},
                            OptionSchema{.long_name = "out", .short_name = 'z', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "out", .kind = ValueKind::String}}));
    // help / version 不再是保留名：声明它们 = 内建让位（口径见 09-cli.md §4.6）
    expect_valid(minimal({OptionSchema{
        .long_name = "help", .kind = ValueKind::Bool, .arity = Arity::flag(), .early_view = cli::EarlyView::Help}}));
    expect_valid(minimal({OptionSchema{.long_name = "version",
                                       .kind = ValueKind::Bool,
                                       .arity = Arity::flag(),
                                       .early_view = cli::EarlyView::Version}}));
}

AURORA_TEST_CASE(validate_allows_h_and_V_but_not_duplicate_shorts) {
    // 旧门禁「-h / -V 一律拒绝」已废除：占用即让位，内建降级为仅长名，不再有「必须改名」的强制。
    expect_valid(minimal({OptionSchema{.long_name = "helpless", .short_name = 'h', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "verbose", .short_name = 'V', .kind = ValueKind::String}}));
    expect_invalid(minimal({OptionSchema{.long_name = "one", .short_name = 'o', .kind = ValueKind::String},
                            OptionSchema{.long_name = "two", .short_name = 'o', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "one", .short_name = 'o', .kind = ValueKind::String},
                          OptionSchema{.long_name = "two", .short_name = 't', .kind = ValueKind::String}}));
    expect_valid(minimal({OptionSchema{.long_name = "verbose", .short_name = 'v', .kind = ValueKind::String}}));
}

AURORA_TEST_CASE(validate_requires_early_view_to_be_a_value_less_bool) {
    // 提前展示是「出现即短路」，消耗值的旗标会让 `--dump-schema=…` 这类写法语义不明
    expect_invalid(
        minimal({OptionSchema{.long_name = "dump", .kind = ValueKind::String, .early_view = cli::EarlyView::Schema}}));
    expect_invalid(minimal({OptionSchema{.long_name = "dump",
                                         .kind = ValueKind::Bool,
                                         .arity = Arity::optional_one(),
                                         .early_view = cli::EarlyView::Schema}}));
    expect_valid(minimal({OptionSchema{
        .long_name = "dump", .kind = ValueKind::Bool, .arity = Arity::flag(), .early_view = cli::EarlyView::Schema}}));
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
        .options = {OptionSchema{.long_name = "wide", .kind = ValueKind::Int, .default_text = "x"}},
    }};
    const auto result = cli::validate(deep);
    AURORA_TEST_REQUIRE_FALSE(result);
    AURORA_TEST_CHECK(result.error().code == "cli-spec-invalid");
    // 错误带完整命令路径：既证明递归扫到了孙层，也让十来个子命令的树一眼定位到该改哪。
    AURORA_TEST_CHECK_THAT(result.error().message, m::has_substr("mid --wide"));

    // 内建让位是合法的，递归层也一样：子命令自己声明 help 即顶掉该层内建
    CommandSpec displaced;
    displaced.name = "displaced";
    displaced.subcommands = {CommandSpec{
        .name = "child",
        .options = {OptionSchema{
            .long_name = "help", .kind = ValueKind::Bool, .arity = Arity::flag(), .early_view = cli::EarlyView::Help}},
    }};
    expect_valid(displaced);
}

AURORA_TEST_CASE(spec_invalid_error_carries_the_reason) {
    const auto result = cli::validate(minimal({OptionSchema{.long_name = "a b", .kind = ValueKind::String}}));
    AURORA_TEST_REQUIRE_FALSE(result);
    AURORA_TEST_CHECK(result.error().code_enum == au::ErrorCode::CliSpecInvalid);
    AURORA_TEST_CHECK_THAT(result.error().message, m::has_substr("long name"));
    AURORA_TEST_CHECK(result.error().category == au::ErrorCategory::Validation);
}

// ------------------------------------------------------------ 派生文本

AURORA_TEST_CASE(usage_line_renders_chain_options_positionals_and_commands) {
    AURORA_TEST_CHECK_EQ(cli::usage_line(spec()), "usage: aurora-render [OPTIONS] <SCENE>... [COMMAND] [-- ARGS...]");
    const auto *const render = sub_of(spec(), "render");
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

AURORA_TEST_CASE(displaced_builtins_degrade_to_long_names_only) {
    const auto &root = displacement_spec();

    const auto text = cli::help_text(root, {"displace"});
    // `-h` 归 height、`-V` 归 verify → 内建 --version 只以长名出现（帮助不再谎报短名）
    AURORA_TEST_CHECK_THAT(text, m::has_substr("      --version"));
    AURORA_TEST_CHECK_FALSE(text.find("-V, --version") != std::string::npos);
    AURORA_TEST_CHECK_THAT(text, m::has_substr("-h, --height <HEIGHT>"));
    // help 已由调用方自己声明：内建那条不该再出现，否则「文档有、代码无」
    AURORA_TEST_CHECK_FALSE(text.find("Show this help and exit") != std::string::npos);
    AURORA_TEST_CHECK_THAT(text, m::has_substr("Hand-rolled help flag"));

    // schema 与帮助同源：让位形态在两处一致，且 early_view 只标在真有短路语义的项上
    const auto schema = cli::schema_json(root);
    const auto *const version = option_node(schema, "version");
    AURORA_TEST_REQUIRE(version != nullptr);
    AURORA_TEST_CHECK_FALSE(version->contains("short"));
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*version, "early_view"), "version");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(option_of(schema, "help"), "early_view"), "help");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(option_of(schema, "dump-schema"), "early_view"), "schema");
    const auto *const height = option_node(schema, "height");
    AURORA_TEST_REQUIRE(height != nullptr);
    AURORA_TEST_CHECK_FALSE(height->contains("early_view"));

    // 关掉全部内建且无自有选项的层：连 [OPTIONS] 都不该出现在 usage 里
    const auto *const bare = sub_of(root, "bare");
    AURORA_TEST_REQUIRE(bare != nullptr);
    AURORA_TEST_CHECK_EQ(cli::usage_line(*bare, {"displace", "bare"}), "usage: displace bare [-- ARGS...]");
}

AURORA_TEST_CASE(version_text_is_program_scoped_and_empty_when_undeclared) {
    AURORA_TEST_CHECK_EQ(cli::version_text(spec()), "aurora-render 1.2.3\n");
    AURORA_TEST_CHECK_EQ(cli::version_text(spec(), "renamed"), "renamed 1.2.3\n");
    const auto *const render = sub_of(spec(), "render");
    AURORA_TEST_REQUIRE(render != nullptr);
    AURORA_TEST_CHECK_EQ(cli::version_text(*render), "");
    CommandSpec nameless;  // 无名根命令回落 "program"
    nameless.version = "0.1";
    AURORA_TEST_CHECK_EQ(cli::version_text(nameless), "program 0.1\n");
}

// ------------------------------------------------------------ schema 自描述

AURORA_TEST_CASE(schema_json_lists_declarations_builtins_and_subcommands) {
    const auto schema = cli::schema_json(spec());
    AURORA_TEST_CHECK_EQ(require_field<std::string>(schema, "name"), "aurora-render");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(schema, "about"), "Render Aurora scenes into image files");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(schema, "version"), "1.2.3");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(schema, "usage"),
                         "usage: aurora-render [OPTIONS] <SCENE>... [COMMAND] [-- ARGS...]");
    const auto *const first_option = require_child_at(*require_child(schema, "options"), 0);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first_option, "long"), "output");
    AURORA_TEST_CHECK_EQ(require_child(schema, "subcommands")->size(), 2U);
    const auto *const first_subcommand = require_child_at(*require_child(schema, "subcommands"), 0);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*first_subcommand, "name"), "render");

    // 内建 help/version 一并列出，供 AI 枚举全量选项；它们与用户自标 early_view 的旗标同一条通道
    const auto *const options = require_child(schema, "options");
    const auto *const help = require_child_at(*options, options->size() - 2);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*help, "long"), "help");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*require_child_at(*options, options->size() - 1), "long"),
                         "version");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(option_of(schema, "help"), "early_view"), "help");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(option_of(schema, "version"), "early_view"), "version");
    AURORA_TEST_CHECK_EQ(option_of(schema, "help").as_or<std::string>("short", std::string{}), "h");

    // hidden 项与 schema 的约定：不出现
    AURORA_TEST_CHECK_TRUE(option_node(schema, "trace-file") == nullptr);
}

AURORA_TEST_CASE(schema_json_encodes_arity_choices_and_bounds) {
    const auto schema = cli::schema_json(spec());
    const auto &width = option_of(schema, "width");
    AURORA_TEST_REQUIRE_TRUE(width.is_object());
    AURORA_TEST_CHECK_EQ(require_field<int>(width, "min"), 1);
    AURORA_TEST_CHECK_EQ(require_field<int>(width, "max"), 1);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(width, "type"), "int");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(width, "short"), "w");
    AURORA_TEST_CHECK_EQ(require_field<double>(width, "maximum"), 8192.0);

    const auto &tag = option_of(schema, "tag");
    AURORA_TEST_CHECK_EQ(require_field<int>(tag, "max"), Arity::AURORA_UNBOUNDED);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(tag, "type"), "string");

    const auto &mode = option_of(schema, "mode");
    AURORA_TEST_REQUIRE_EQ(require_child(mode, "choices")->size(), 3U);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(mode, "default"), "balanced");

    const auto &verbose = option_of(schema, "verbose");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(verbose, "type"), "bool");
    AURORA_TEST_CHECK_EQ(require_field<int>(verbose, "max"), 0);

    const auto *const positional = require_child_at(*require_child(schema, "positionals"), 0);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*positional, "name"), "SCENE");
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*positional, "type"), "string");
}

AURORA_TEST_CASE(schema_json_reflects_positionals_and_required_subcommands) {
    const auto schema = cli::schema_json(strict_spec());
    AURORA_TEST_CHECK_EQ(require_field<bool>(schema, "subcommand_required"), true);
    AURORA_TEST_CHECK_FALSE(schema.contains("version"));
    const auto *const deploy = require_child_at(*require_child(schema, "subcommands"), 0);
    const auto *const slots = require_child(*deploy, "positionals");
    AURORA_TEST_REQUIRE_EQ(slots->size(), 1U);
    AURORA_TEST_CHECK_EQ(require_child(*require_child_at(*slots, 0), "choices")->size(), 2U);
    AURORA_TEST_CHECK_EQ(require_field<std::string>(*deploy, "usage"), "usage: deploy [OPTIONS] <ENV> [-- ARGS...]");
    AURORA_TEST_CHECK_EQ(require_field<bool>(option_of(schema, "token"), "required"), true);
}

// ------------------------------------------------------------ 文本 golden

AURORA_TEST_CASE(cli_texts_match_golden_baseline) {
    const std::filesystem::path path = golden::dir() / "cli_snapshots.json";
    const bool regen = golden::env_flag("AURORA_UPDATE_GOLDEN");

    auto baseline = json::Value::object();
    if (!regen) {
        std::ifstream in(path);
        AURORA_TEST_REQUIRE_MSG(
            in.good(), "golden baseline cli_snapshots.json must exist (run with AURORA_UPDATE_GOLDEN=1 to create)");
        const auto parsed =
            json::parse(std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}});
        AURORA_TEST_REQUIRE_MSG(parsed, parsed ? std::string{} : parsed.error().message);
        baseline = parsed.value();
        AURORA_TEST_REQUIRE_TRUE(baseline.contains("texts"));
    }

    const auto &root = spec();
    const auto *render = sub_of(root, "render");
    AURORA_TEST_REQUIRE(render != nullptr);

    auto texts = json::Value::object();
    texts.set("root/usage", cli::usage_line(root));
    texts.set("root/help", cli::help_text(root, {"aurora-render"}));
    texts.set("root/version", cli::version_text(root));
    texts.set("render/usage", cli::usage_line(*render, {"aurora-render", "render"}));
    texts.set("render/help", cli::help_text(*render, {"aurora-render", "render"}));
    texts.set("strict/usage", cli::usage_line(strict_spec()));
    texts.set("strict/help", cli::help_text(strict_spec(), {"strict"}));

    auto current = json::Value::object();
    current.set("texts", std::move(texts));
    current.set("schema", cli::schema_json(root));

    if (regen) {
        auto doc = json::Value::object();
        doc.set("_about",
                "Aurora aurora::cli text golden baseline (specification/09-cli.md). Do not hand-edit; regenerate with "
                "AURORA_UPDATE_GOLDEN=1 via aurora_test_runner --run=utest_cli_format.");
        doc.set("texts", *require_child(current, "texts"));
        doc.set("schema", *require_child(current, "schema"));
        const auto text = json::dump(doc, {.indent = 2});
        AURORA_TEST_REQUIRE_MSG(text, text ? std::string{} : text.error().message);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        out << text.value() << "\n";
        AURORA_TEST_CHECK_TRUE(out.good());
        return;
    }

    const auto *const expected_texts = require_child(baseline, "texts");
    for (const auto &entry : require_child(current, "texts")->entries()) {
        AURORA_TEST_CHECK_EQ(require_value(entry.value.as_string()),
                             require_field<std::string>(*expected_texts, entry.key));
    }
    // 新容器按插入序保持对象键，故此处是全序相等：键顺序漂移也会被判为回归
    AURORA_TEST_CHECK(*require_child(current, "schema") == *require_child(baseline, "schema"));
}

}  // namespace aurora::test_cases::utest_cli_format

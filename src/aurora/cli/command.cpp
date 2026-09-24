// command.h 的实现：命令声明表的查找、静态校验与派生视图（usage / help / schema）。
// 规格：codespec/specification/09-cli.md §3（声明表）、§7（派生视图）。
// 纪律：全部纯函数，不写任何流；文本由调用方经 AURORA_LOG_RAW 输出。

#include "aurora/cli/command.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/cli/literals.h"

namespace aurora::cli {
namespace {

// ------------------------------------------------------------ 名称与分组

[[nodiscard]] auto is_valid_long_name(std::string_view name) -> bool {
    if (name.empty() || name.front() == '-') {
        return false;
    }
    return std::none_of(name.begin(), name.end(),
                        [](char c) { return c == ' ' || c == '=' || c == '\t' || c == '\n'; });
}

/// @brief 内建长名：声明表里出现即视为冲突（`--help` / `--version` 由库注入）。
constexpr std::array<std::string_view, 2> AURORA_RESERVED_LONG_NAMES{{"help", "version"}};

[[nodiscard]] auto default_value_hint(const OptionSchema &option) -> std::string {
    if (!option.value_hint.empty()) {
        return option.value_hint;
    }
    std::string hint = option.long_name;
    std::transform(hint.begin(), hint.end(), hint.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return hint;
}

[[nodiscard]] auto group_of(const OptionSchema &option) -> std::string {
    return option.group.empty() ? std::string{"Options"} : option.group;
}

/// @brief 一条帮助条目的渲染素材。
struct HelpEntry {
    std::string group;
    std::string names;  ///< "-w, --width <WIDTH>" 形态的左列
    std::string detail;  ///< 说明 + 取值域 + 默认值 的右列
};

[[nodiscard]] auto names_column(const OptionSchema &option) -> std::string {
    std::string out;
    if (option.short_name != '\0') {
        out += '-';
        out += option.short_name;
        out += ", ";
    } else {
        out += "    ";
    }
    out += "--" + option.long_name;
    const auto placeholder = option.arity.help_placeholder(default_value_hint(option));
    if (!placeholder.empty()) {
        out += ' ';
        out += placeholder;
    }
    return out;
}

[[nodiscard]] auto detail_column(const OptionSchema &option) -> std::string {
    std::string out = option.help;
    if (!option.choices.empty()) {
        std::string allowed;
        for (const auto &choice : option.choices) {
            if (!allowed.empty()) {
                allowed += ", ";
            }
            allowed += choice;
        }
        if (!out.empty()) {
            out += " ";
        }
        out += "[possible values: " + allowed + "]";
    }
    if (option.minimum || option.maximum) {
        out += " [range: " + (option.minimum ? detail::bound_text(*option.minimum) : std::string{"-inf"}) + ", " +
               (option.maximum ? detail::bound_text(*option.maximum) : std::string{"inf"}) + "]";
    }
    if (!option.default_text.empty()) {
        if (!out.empty()) {
            out += " ";
        }
        out += "[default: " + option.default_text + "]";
    }
    if (option.required) {
        if (!out.empty()) {
            out += " ";
        }
        out += "[required]";
    }
    return out;
}

/// @brief 两列对齐渲染：左列宽 = max(左列长度)，右列空时只出左列。
[[nodiscard]] auto render_entries(const std::vector<HelpEntry> &entries) -> std::string {
    std::size_t width = 0;
    for (const auto &entry : entries) {
        width = std::max(width, entry.names.size());
    }
    std::string out;
    for (const auto &entry : entries) {
        out += "  ";
        out += entry.names;
        if (entry.detail.empty()) {
            out += "\n";
            continue;
        }
        out += std::string(width - entry.names.size() + 2U, ' ');
        out += entry.detail;
        out += "\n";
    }
    return out;
}

/// @brief 按声明顺序归组（组名首次出现定序），内建 Help 组固定排在最后。
[[nodiscard]] auto group_entries(std::vector<HelpEntry> entries)
    -> std::vector<std::pair<std::string, std::vector<HelpEntry>>> {
    std::vector<std::pair<std::string, std::vector<HelpEntry>>> groups;
    for (auto &entry : entries) {
        const auto hit =
            std::find_if(groups.begin(), groups.end(), [&](const auto &pair) { return pair.first == entry.group; });
        if (hit == groups.end()) {
            groups.emplace_back(entry.group, std::vector<HelpEntry>{});
            groups.back().second.push_back(std::move(entry));
        } else {
            hit->second.push_back(std::move(entry));
        }
    }
    std::stable_partition(groups.begin(), groups.end(), [](const auto &pair) { return pair.first != "Help"; });
    return groups;
}

[[nodiscard]] auto chain_display(const CommandSpec &spec, const std::vector<std::string> &path) -> std::string {
    std::string out;
    if (!path.empty()) {
        out = path.front();
        for (std::size_t i = 1; i < path.size(); ++i) {
            out += ' ';
            out += path[i];
        }
        return out;
    }
    out = spec.name.empty() ? std::string{"program"} : spec.name;
    return out;
}

// ------------------------------------------------------------ validate

[[nodiscard]] auto spec_invalid(std::string reason) -> Error {
    return make_error(ErrorCode::CliSpecInvalid, ErrorParams{{"reason", std::move(reason)}});
}

/// @brief 默认值字面量必须可按 kind 转换、落在取值域内，否则声明即自相矛盾。
[[nodiscard]] auto check_default(const std::string &owner, const ValueKind kind, const std::string &literal,
                                 const std::vector<std::string> &choices, const std::optional<double> &minimum,
                                 const std::optional<double> &maximum) -> std::optional<Error> {
    if (literal.empty()) {
        return std::nullopt;
    }
    auto converted = detail::convert_literal(kind, literal, owner);
    if (!converted) {
        return spec_invalid(owner + ": default value '" + literal + "' is not a valid " + std::string{to_string(kind)});
    }
    if (!choices.empty() && std::find(choices.begin(), choices.end(), literal) == choices.end()) {
        return spec_invalid(owner + ": default value '" + literal + "' is not in the choices list");
    }
    if (const auto number = converted.value().as_double(); number && (minimum || maximum)) {
        if (minimum && number.value() < *minimum) {
            return spec_invalid(owner + ": default value is below minimum");
        }
        if (maximum && number.value() > *maximum) {
            return spec_invalid(owner + ": default value is above maximum");
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto validate_command(const CommandSpec &spec) -> std::optional<Error> {
    if (spec.name.empty() && spec.subcommands.empty() && spec.options.empty() && spec.positionals.empty()) {
        return spec_invalid("command declares nothing (no name, options, positionals or subcommands)");
    }
    std::vector<std::string> long_names;
    std::vector<std::string> short_names;
    for (const auto &option : spec.options) {
        const std::string owner = "--" + (option.long_name.empty() ? std::string{"<unnamed>"} : option.long_name);
        if (!is_valid_long_name(option.long_name)) {
            return spec_invalid(owner + ": long name must be non-empty, must not start with '-' or contain blanks/'='");
        }
        if (std::find(AURORA_RESERVED_LONG_NAMES.begin(), AURORA_RESERVED_LONG_NAMES.end(), option.long_name) !=
            AURORA_RESERVED_LONG_NAMES.end()) {
            return spec_invalid(owner + ": this long name is built in and must not be declared");
        }
        if (std::find(long_names.begin(), long_names.end(), option.long_name) != long_names.end()) {
            return spec_invalid(owner + ": duplicate long name");
        }
        long_names.push_back(option.long_name);
        if (option.short_name != '\0') {
            const std::string short_name(1, option.short_name);
            if (short_name == "h") {
                return spec_invalid(owner + ": -h is reserved for the built-in help flag");
            }
            if (short_name == "V") {
                return spec_invalid(owner + ": -V is reserved for the built-in version flag");
            }
            if (std::find(short_names.begin(), short_names.end(), short_name) != short_names.end()) {
                std::string message{owner};
                message += ": duplicate short name -";
                message += short_name;
                return spec_invalid(std::move(message));
            }
            short_names.push_back(short_name);
        }
        if (option.arity.min < 0 ||
            (option.arity.max != Arity::AURORA_UNBOUNDED && option.arity.max < option.arity.min)) {
            return spec_invalid(owner + ": arity must satisfy 0 <= min <= max");
        }
        if (option.kind == ValueKind::Bool) {
            if (option.arity.max != 0) {
                return spec_invalid(owner + ": a Bool option must take no value (use Arity::flag())");
            }
        } else if (option.arity.min < 1) {
            return spec_invalid(owner + ": a value-taking option must require at least one value");
        }
        if (option.kind == ValueKind::Enum && option.choices.empty()) {
            return spec_invalid(owner + ": an Enum option must list its choices");
        }
        if (auto error =
                check_default(owner, option.kind, option.default_text, option.choices, option.minimum, option.maximum);
            error) {
            return error;
        }
        for (const auto &conflict : option.conflicts_with) {
            if (conflict == option.long_name || spec.find_option(conflict) == nullptr) {
                std::string message{owner};
                message += ": conflicts_with names an unknown option '--";
                message += conflict;
                message += "'";
                return spec_invalid(std::move(message));
            }
        }
    }
    std::vector<std::string> positional_names;
    for (std::size_t i = 0; i < spec.positionals.size(); ++i) {
        const auto &slot = spec.positionals[i];
        const std::string owner =
            slot.name.empty() ? ("positional[" + std::to_string(i) + "]") : ("<" + slot.name + ">");
        if (slot.name.empty() || slot.name.front() == '-') {
            return spec_invalid(owner + ": positional name must be non-empty and must not start with '-'");
        }
        if (std::find(positional_names.begin(), positional_names.end(), slot.name) != positional_names.end()) {
            return spec_invalid(owner + ": duplicate positional name");
        }
        if (std::find(long_names.begin(), long_names.end(), slot.name) != long_names.end()) {
            return spec_invalid(owner + ": positional name collides with option '--" + slot.name + "'");
        }
        positional_names.push_back(slot.name);
        if (slot.arity.min < 0 || (slot.arity.max != Arity::AURORA_UNBOUNDED && slot.arity.max < slot.arity.min)) {
            return spec_invalid(owner + ": arity must satisfy 0 <= min <= max");
        }
        if (slot.kind == ValueKind::Bool) {
            return spec_invalid(owner + ": a positional cannot be a Bool flag");
        }
        if (slot.arity.max == Arity::AURORA_UNBOUNDED && i + 1U != spec.positionals.size()) {
            return spec_invalid(owner + ": a variadic positional must be the last one");
        }
        if (auto error = check_default(owner, slot.kind, slot.default_text, slot.choices, std::nullopt, std::nullopt);
            error) {
            return error;
        }
    }
    std::vector<std::string> sub_names;
    for (const auto &sub : spec.subcommands) {
        if (sub.name.empty() || !is_valid_long_name(sub.name)) {
            return spec_invalid("subcommand name must be non-empty and free of blanks");
        }
        if (std::find(sub_names.begin(), sub_names.end(), sub.name) != sub_names.end()) {
            return spec_invalid("duplicate subcommand '" + sub.name + "'");
        }
        sub_names.push_back(sub.name);
    }
    if (spec.subcommand_required && spec.subcommands.empty()) {
        return spec_invalid("'" + spec.name + "': subcommand_required without any subcommand");
    }
    return std::nullopt;
}

auto count_commands(const CommandSpec &spec) -> int {
    int total = 1;
    for (const auto &sub : spec.subcommands) {
        total += count_commands(sub);
    }
    return total;
}

/// @brief 递归校验；返回首个违规错误。
[[nodiscard]] auto validate_tree(const CommandSpec &spec) -> std::optional<Error> {
    if (auto error = validate_command(spec)) {
        return error;
    }
    for (const auto &sub : spec.subcommands) {
        if (auto error = validate_tree(sub)) {
            return error;
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------ schema

[[nodiscard]] auto option_to_json(const OptionSchema &option) -> Json {
    Json entry = Json::object();
    entry["long"] = option.long_name;
    if (option.short_name != '\0') {
        entry["short"] = std::string(1, option.short_name);
    }
    entry["type"] = std::string{to_string(option.kind)};
    entry["min"] = option.arity.min;
    entry["max"] = option.arity.max;
    entry["required"] = option.required;
    entry["hidden"] = option.hidden;
    if (!option.value_hint.empty()) {
        entry["value_hint"] = option.value_hint;
    }
    if (!option.choices.empty()) {
        entry["choices"] = option.choices;
    }
    if (!option.default_text.empty()) {
        entry["default"] = option.default_text;
    }
    if (option.minimum) {
        entry["minimum"] = *option.minimum;
    }
    if (option.maximum) {
        entry["maximum"] = *option.maximum;
    }
    if (!option.conflicts_with.empty()) {
        entry["conflicts_with"] = option.conflicts_with;
    }
    if (!option.group.empty()) {
        entry["group"] = option.group;
    }
    entry["help"] = option.help;
    return entry;
}

[[nodiscard]] auto builtin_help_option() -> OptionSchema {
    return OptionSchema{
        .long_name = "help",
        .short_name = 'h',
        .kind = ValueKind::Bool,
        .arity = Arity::flag(),
        .help = "Show this help and exit",
    };
}

[[nodiscard]] auto builtin_version_option() -> OptionSchema {
    return OptionSchema{
        .long_name = "version",
        .short_name = 'V',
        .kind = ValueKind::Bool,
        .arity = Arity::flag(),
        .help = "Show the version and exit",
        .group = "Help",
    };
}

[[nodiscard]] auto positional_to_json(const PositionalSchema &slot) -> Json {
    Json entry = Json::object();
    entry["name"] = slot.name;
    entry["type"] = std::string{to_string(slot.kind)};
    entry["min"] = slot.arity.min;
    entry["max"] = slot.arity.max;
    if (!slot.choices.empty()) {
        entry["choices"] = slot.choices;
    }
    if (!slot.default_text.empty()) {
        entry["default"] = slot.default_text;
    }
    entry["help"] = slot.help;
    return entry;
}

[[nodiscard]] auto command_to_json(const CommandSpec &spec) -> Json {
    Json out = Json::object();
    out["name"] = spec.name;
    out["about"] = spec.about;
    out["usage"] =
        usage_line(spec, spec.name.empty() ? std::vector<std::string>{} : std::vector<std::string>{spec.name});
    if (!spec.version.empty()) {
        out["version"] = spec.version;
    }
    out["subcommand_required"] = spec.subcommand_required;
    Json options = Json::array();
    for (const auto &option : spec.options) {
        if (!option.hidden) {  // hidden 的约定：既不进 --help，也不进 schema，仍可正常解析
            options.push_back(option_to_json(option));
        }
    }
    options.push_back(option_to_json(builtin_help_option()));
    if (!spec.version.empty()) {
        options.push_back(option_to_json(builtin_version_option()));
    }
    out["options"] = std::move(options);
    Json positionals = Json::array();
    for (const auto &slot : spec.positionals) {
        positionals.push_back(positional_to_json(slot));
    }
    out["positionals"] = std::move(positionals);
    Json subcommands = Json::array();
    for (const auto &sub : spec.subcommands) {
        subcommands.push_back(command_to_json(sub));
    }
    out["subcommands"] = std::move(subcommands);
    return out;
}

}  // namespace

// ------------------------------------------------------------ CommandSpec

auto CommandSpec::find_option(std::string_view long_name) const -> const OptionSchema * {
    for (const auto &option : options) {
        if (option.long_name == long_name) {
            return &option;
        }
    }
    return nullptr;
}

auto CommandSpec::find_short(char short_name) const -> const OptionSchema * {
    for (const auto &option : options) {
        if (option.short_name != '\0' && option.short_name == short_name) {
            return &option;
        }
    }
    return nullptr;
}

auto CommandSpec::find_subcommand(std::string_view sub_name) const -> const CommandSpec * {
    for (const auto &sub : subcommands) {
        if (sub.name == sub_name) {
            return &sub;
        }
    }
    return nullptr;
}

auto Arity::help_placeholder(std::string_view value_hint) const -> std::string {
    if (max == 0) {
        return {};
    }
    const std::string one = "<" + std::string{value_hint} + ">";
    if (max == Arity::AURORA_UNBOUNDED) {
        return (min == 0) ? one + "..." : one + " " + one + "...";
    }
    // 省略号专属「可重复」：最多吞一个值的 arity（0..1 与 1..1）都只渲染单个占位符
    return (max == 1 || min == max) ? one : one + "...";
}

// ------------------------------------------------------------ validate

auto validate(const CommandSpec &root) -> Result<int> {
    if (auto error = validate_tree(root)) {
        return *error;
    }
    return count_commands(root);
}

// ------------------------------------------------------------ 派生视图

auto usage_line(const CommandSpec &spec, const std::vector<std::string> &path) -> std::string {
    std::string out = "usage: " + chain_display(spec, path);
    // [OPTIONS] 恒在：`--help` 是内建项，任何命令都至少有一个选项。
    out += " [OPTIONS]";
    for (const auto &slot : spec.positionals) {
        const std::string one = "<" + slot.name + ">";
        if (slot.arity.max == Arity::AURORA_UNBOUNDED) {
            out += " " + one + "...";
        } else if (slot.arity.min == 0) {
            out += " [" + one + "]";
        } else {
            out += " " + one;
        }
    }
    if (!spec.subcommands.empty()) {
        out += spec.subcommand_required ? " <COMMAND>" : " [COMMAND]";
    }
    out += " [-- ARGS...]";
    return out;
}

auto help_text(const CommandSpec &spec, const std::vector<std::string> &path) -> std::string {
    const std::string display = chain_display(spec, path);
    std::string out;
    if (!spec.about.empty()) {
        out += spec.about + "\n";
    }
    out += usage_line(spec, path.empty() ? std::vector<std::string>{} : std::vector<std::string>{display}) + "\n";
    if (!spec.description.empty()) {
        out += "\n" + spec.description + "\n";
    }

    std::vector<HelpEntry> entries;
    for (const auto &option : spec.options) {
        if (option.hidden) {
            continue;
        }
        entries.push_back(
            HelpEntry{.group = group_of(option), .names = names_column(option), .detail = detail_column(option)});
    }
    const auto help_builtin = builtin_help_option();
    entries.push_back(
        HelpEntry{.group = "Help", .names = names_column(help_builtin), .detail = detail_column(help_builtin)});
    if (!spec.version.empty()) {
        const auto version_builtin = builtin_version_option();
        entries.push_back(HelpEntry{
            .group = "Help", .names = names_column(version_builtin), .detail = detail_column(version_builtin)});
    }
    out += "\n";
    for (const auto &[group, items] : group_entries(std::move(entries))) {
        out += group + ":\n" + render_entries(items) + "\n";
    }

    if (!spec.positionals.empty()) {
        out += "Arguments:\n";
        std::vector<HelpEntry> positional_entries;
        for (const auto &slot : spec.positionals) {
            std::string names = "<" + slot.name + ">";
            if (slot.arity.max == Arity::AURORA_UNBOUNDED) {
                names += "...";
            }
            const OptionSchema view{
                .help = slot.help,
                .default_text = slot.default_text,
                .choices = slot.choices,
            };
            positional_entries.push_back(
                HelpEntry{.group = "Arguments", .names = std::move(names), .detail = detail_column(view)});
        }
        out += render_entries(positional_entries) + "\n";
    }

    if (!spec.subcommands.empty()) {
        out += "Commands:\n";
        std::vector<HelpEntry> sub_entries;
        for (const auto &sub : spec.subcommands) {
            if (!sub.name.empty()) {
                sub_entries.push_back(HelpEntry{.group = "Commands", .names = sub.name, .detail = sub.about});
            }
        }
        out += render_entries(sub_entries) + "\n";
        out += "Run '" + display + " <COMMAND> --help' for more information on a command.\n\n";
    }

    if (!spec.epilog.empty()) {
        out += spec.epilog + "\n";
    }
    return out;
}

auto version_text(const CommandSpec &spec, std::string_view program_name) -> std::string {
    if (spec.version.empty()) {
        return {};
    }
    std::string out =
        program_name.empty() ? (spec.name.empty() ? std::string{"program"} : spec.name) : std::string{program_name};
    out += " ";
    out += spec.version;
    out += "\n";
    return out;
}

auto schema_json(const CommandSpec &spec) -> Json { return command_to_json(spec); }

}  // namespace aurora::cli

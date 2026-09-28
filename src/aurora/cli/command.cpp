// command.h 的实现：静态校验、内建注入判定与派生视图（usage / help / version / schema）。
// 规格：codespec/specification/09-cli.md §3（声明表与 validate）、§4.6（提前展示通道）、§7（派生视图）。
// 纪律：全部纯函数，不写任何流；文本由调用方经 AURORA_LOG_RAW 输出。

#include "aurora/cli/command.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/cli/literals.h"
#include "aurora/cli/spec_lookup.h"

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
    if (const auto number = converted.value().as<double>(); number && (minimum || maximum)) {
        if (minimum && number.value() < *minimum) {
            return spec_invalid(owner + ": default value is below minimum");
        }
        if (maximum && number.value() > *maximum) {
            return spec_invalid(owner + ": default value is above maximum");
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto validate_command(const CommandSpec &spec, const std::string &command) -> std::optional<Error> {
    if (spec.name.empty() && spec.subcommands.empty() && spec.options.empty() && spec.positionals.empty()) {
        return spec_invalid(command + ": command declares nothing (no name, options, positionals or subcommands)");
    }
    std::vector<std::string> long_names;
    std::vector<std::string> short_names;
    for (const auto &option : spec.options) {
        // 归属带命令路径（`aurora_cli render --height`），与其余 `cli-*` 错误的 `command` 参数同口径；
        // 十来个子命令的树里，只说 `--height` 无从下手。
        const std::string owner =
            command + " --" + (option.long_name.empty() ? std::string{"<unnamed>"} : option.long_name);
        if (!is_valid_long_name(option.long_name)) {
            return spec_invalid(owner + ": long name must be non-empty, must not start with '-' or contain blanks/'='");
        }
        if (std::find(long_names.begin(), long_names.end(), option.long_name) != long_names.end()) {
            return spec_invalid(owner + ": duplicate long name");
        }
        long_names.push_back(option.long_name);
        if (option.short_name != '\0') {
            const std::string short_name(1, option.short_name);
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
        // 提前展示是「出现即短路」，它必须不消耗值，否则 --dump-schema=x 这类写法语义不明。
        if (option.early_view != EarlyView::None && (option.kind != ValueKind::Bool || option.arity.max != 0)) {
            return spec_invalid(owner + ": an early_view option must be a value-less Bool flag (Arity::flag())");
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
            if (conflict == option.long_name || detail::find_option(spec, conflict) == nullptr) {
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
            command + " " + (slot.name.empty() ? ("positional[" + std::to_string(i) + "]") : ("<" + slot.name + ">"));
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
            return spec_invalid(command + ": subcommand name must be non-empty and free of blanks");
        }
        if (std::find(sub_names.begin(), sub_names.end(), sub.name) != sub_names.end()) {
            return spec_invalid(command + ": duplicate subcommand '" + sub.name + "'");
        }
        sub_names.push_back(sub.name);
    }
    if (spec.subcommand_required && spec.subcommands.empty()) {
        return spec_invalid(command + ": subcommand_required without any subcommand");
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

/// @brief 递归校验；返回首个违规错误。`command` 是已走过的命令路径，用于给错误定位。
[[nodiscard]] auto validate_tree(const CommandSpec &spec, const std::string &command) -> std::optional<Error> {
    if (auto error = validate_command(spec, command)) {
        return error;
    }
    for (const auto &sub : spec.subcommands) {
        if (auto error = validate_tree(sub, command + " " + sub.name)) {
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
    if (option.early_view != EarlyView::None) {
        entry["early_view"] = std::string{early_view_to_string(option.early_view)};
    }
    entry["help"] = option.help;
    return entry;
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
    const auto plan = detail::builtin_plan(spec);
    Json options = Json::array();
    for (const auto &option : spec.options) {
        if (!option.hidden) {  // hidden 的约定：既不进 --help，也不进 schema，仍可正常解析
            options.push_back(option_to_json(option));
        }
    }
    // 内建行按 builtin_plan 的实际注入形态列出：被用户声明顶掉的不列，短名让位时只列长名。
    if (plan.help) {
        options.push_back(option_to_json(detail::builtin_help_option(plan)));
    }
    if (plan.version) {
        options.push_back(option_to_json(detail::builtin_version_option(plan)));
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

// ------------------------------------------------------------ 库内查表与内建注入

namespace detail {

auto find_option(const CommandSpec &spec, std::string_view long_name) -> const OptionSchema * {
    for (const auto &option : spec.options) {
        if (option.long_name == long_name) {
            return &option;
        }
    }
    return nullptr;
}

auto find_short(const CommandSpec &spec, char short_name) -> const OptionSchema * {
    for (const auto &option : spec.options) {
        if (option.short_name != '\0' && option.short_name == short_name) {
            return &option;
        }
    }
    return nullptr;
}

auto find_subcommand(const CommandSpec &spec, std::string_view sub_name) -> const CommandSpec * {
    for (const auto &sub : spec.subcommands) {
        if (sub.name == sub_name) {
            return &sub;
        }
    }
    return nullptr;
}

auto builtin_plan(const CommandSpec &spec) -> BuiltinPlan {
    BuiltinPlan plan;
    const bool shorts_free = spec.builtins.take_shorts;
    if (spec.builtins.help && find_option(spec, "help") == nullptr) {
        plan.help = true;
        plan.help_short = shorts_free && find_short(spec, 'h') == nullptr;
    }
    if (spec.builtins.version && !spec.version.empty() && find_option(spec, "version") == nullptr) {
        plan.version = true;
        plan.version_short = shorts_free && find_short(spec, 'V') == nullptr;
    }
    return plan;
}

auto builtin_help_option(const BuiltinPlan &plan) -> OptionSchema {
    return OptionSchema{
        .long_name = "help",
        .short_name = plan.help_short ? 'h' : '\0',
        .kind = ValueKind::Bool,
        .arity = Arity::flag(),
        .help = "Show this help and exit",
        .early_view = EarlyView::Help,
    };
}

auto builtin_version_option(const BuiltinPlan &plan) -> OptionSchema {
    return OptionSchema{
        .long_name = "version",
        .short_name = plan.version_short ? 'V' : '\0',
        .kind = ValueKind::Bool,
        .arity = Arity::flag(),
        .help = "Show the version and exit",
        .group = "Help",
        .early_view = EarlyView::Version,
    };
}

}  // namespace detail

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
    if (auto error = validate_tree(root, chain_display(root, {}))) {
        return *error;
    }
    return count_commands(root);
}

// ------------------------------------------------------------ 派生视图

auto usage_line(const CommandSpec &spec, const std::vector<std::string> &path) -> std::string {
    std::string out = "usage: " + chain_display(spec, path);
    // [OPTIONS] 只在本层真有需要时出现：内建 help/version 可被 builtins 关掉，关掉后可能一个选项都没有。
    const auto plan = detail::builtin_plan(spec);
    if (!spec.options.empty() || plan.help || plan.version) {
        out += " [OPTIONS]";
    }
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
    const auto plan = detail::builtin_plan(spec);
    if (plan.help) {
        const auto help_builtin = detail::builtin_help_option(plan);
        entries.push_back(
            HelpEntry{.group = "Help", .names = names_column(help_builtin), .detail = detail_column(help_builtin)});
    }
    if (plan.version) {
        const auto version_builtin = detail::builtin_version_option(plan);
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

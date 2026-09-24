// tools/servers/aurora_cli.cpp
//
// aurora — Aurora CLI toolchain (spec #17).
//
// Provides subcommands for component discovery, UI-tree validation, offscreen rendering, code generation, etc.
// All output defaults to JSON (machine-readable).
//
// The whole command surface lives in one `aurora::cli::CommandSpec` declaration table (`build_spec()`): the
// parser, the per-level `--help` text, the usage line and `schema_json` all derive from that single source.
//
// Usage:
//   aurora_cli <command> [options]        run `aurora_cli --help` (or `-h`) for the command list,
//                                         and `aurora_cli <command> --help` for one command's options.
//
// `schema` prints the API skeleton rebuilt at runtime from the live registry (library / language / include /
// alias + widgets + enums), which is not the committed aurora_api.json. The full file is only produced by the
// build-time generators: layout_rules / state_patterns come from gen_api_tools, error_codes from
// gen_error_codes and debug from gen_debug_api.
//
// Exit codes: 0 on success, 1 on validation failure, 2 on usage error.

#include <cstddef>
#include <string>
#include <vector>

#include "api_schema.h"
#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/yaml.h"
#include "code_style.h"
#include "json_file.h"

// ---------- Known enums (single source of truth: tools/include/known_enums.h) ----------

namespace {

// ---------- helpers ----------

// read_json_file / parse_code_style / build_api_skeleton are provided by shared headers under tools/include.

/// @brief 渲染尺寸 / 输出路径 / 代码风格等按子命令复用的选项声明。
struct CliOptions {
    int width = 800;
    int height = 600;
    std::string output = "aurora_render.png";
    std::string style = "fluent";
    std::string file;
};

namespace cli = aurora::cli;

[[nodiscard]] auto width_option() -> cli::OptionSchema {
    return cli::OptionSchema{
        .long_name = "width",
        .short_name = 'w',
        .kind = cli::ValueKind::Int,
        .help = "Viewport width in logical pixels",
        .value_hint = "PX",
        .default_text = "800",
        .minimum = 1,
        .maximum = 8192,
    };
}

/// @brief 高度用 `-H`：`-h` 是 `aurora::cli` 内建 help 短名，声明即 `cli-spec-invalid`。
[[nodiscard]] auto height_option() -> cli::OptionSchema {
    return cli::OptionSchema{
        .long_name = "height",
        .short_name = 'H',
        .kind = cli::ValueKind::Int,
        .help = "Viewport height in logical pixels",
        .value_hint = "PX",
        .default_text = "600",
        .minimum = 1,
        .maximum = 8192,
    };
}

[[nodiscard]] auto output_option() -> cli::OptionSchema {
    return cli::OptionSchema{
        .long_name = "output",
        .short_name = 'o',
        .kind = cli::ValueKind::String,
        .help = "Output PNG path",
        .value_hint = "FILE",
        .default_text = "aurora_render.png",
    };
}

[[nodiscard]] auto style_option() -> cli::OptionSchema {
    return cli::OptionSchema{
        .long_name = "style",
        .kind = cli::ValueKind::Enum,
        .help = "Generated code style",
        .value_hint = "STYLE",
        .default_text = "fluent",
        .choices = {"fluent", "step", "di"},
    };
}

[[nodiscard]] auto positional(std::string name, std::string help) -> cli::PositionalSchema {
    return cli::PositionalSchema{.name = std::move(name), .help = std::move(help)};
}

[[nodiscard]] auto leaf(std::string name, std::string about, std::vector<cli::OptionSchema> options = {},
                        std::vector<cli::PositionalSchema> positionals = {}) -> cli::CommandSpec {
    return cli::CommandSpec{
        .name = std::move(name),
        .about = std::move(about),
        .options = std::move(options),
        .positionals = std::move(positionals),
    };
}

/// @brief 唯一的命令声明源：10 个子命令 + 内建 `--help` / `--version`。
[[nodiscard]] auto build_spec() -> cli::CommandSpec {
    const std::string tree_help = "Path to the UI-tree JSON file";
    return cli::CommandSpec{
        .name = "aurora_cli",
        .about = "Aurora CLI toolchain -- AI-first GUI toolkit; all output is JSON unless stated otherwise",
        .description =
            "Subcommands cover component discovery, UI-tree validation, offscreen rendering and code "
            "generation. Options are per-command: `aurora_cli render --help` lists only what render accepts.",
        .subcommands =
            {
                leaf("components", "List all registered component types"),
                leaf("describe", "Print the full schema of a single component", {},
                     {positional("name", "Component type name, e.g. Button")}),
                leaf("search", "Search components by name substring", {},
                     {positional("keyword", "Case-sensitive substring matched against type names")}),
                leaf("validate", "Validate a UI-tree JSON and print diagnostics", {},
                     {positional("tree.json", tree_help)}),
                leaf("snapshot", "Print a logical snapshot JSON", {width_option(), height_option()},
                     {positional("tree.json", tree_help)}),
                leaf("render", "Render offscreen to PNG", {width_option(), height_option(), output_option()},
                     {positional("tree.json", tree_help)}),
                leaf("preview",
                     "Quick UI preview (opens a temporary window; falls back to a single headless frame then "
                     "exits when no display backend is available)",
                     {width_option(), height_option()}, {positional("tree.json", tree_help)}),
                leaf("to-code", "UI tree -> C++ code", {style_option()}, {positional("tree.json", tree_help)}),
                leaf("to-yaml", "UI tree -> YAML format", {}, {positional("tree.json", tree_help)}),
                leaf("schema", "Print the runtime API skeleton (widgets + enums)"),
            },
        .version = AURORA_VERSION_STRING,
        .epilog =
            "Exit codes: 0 on success, 1 on validation failure, 2 on usage error.\n"
            "`schema` reflects the live registry; the committed aurora_api.json is produced by gen_api_tools.",
        .subcommand_required = true,
    };
}

/// @brief 必填位置参数的原文：叶命令均声明为 `exactly_one()`，故成功解析即可读。
[[nodiscard]] auto positional_text(const cli::Arguments &args, std::size_t index) -> std::string {
    const auto value = args.positional(index);
    return value.ok() ? value.value().raw_text() : std::string{};
}

[[nodiscard]] auto int_option(const cli::Arguments &args, const char *name, int fallback) -> int {
    const auto value = args.get<int>(name);
    return value.ok() ? value.value() : fallback;
}

[[nodiscard]] auto text_option(const cli::Arguments &args, const char *name, const char *fallback) -> std::string {
    const auto value = args.get<std::string>(name);
    return value.ok() ? value.value() : std::string{fallback};
}

/// @brief 把叶命令的声明值收进 CliOptions（tree.json 恒为位置参数 0）。
[[nodiscard]] auto cli_options(const cli::Arguments &args) -> CliOptions {
    CliOptions opts;
    opts.file = positional_text(args, 0);
    opts.width = int_option(args, "width", opts.width);
    opts.height = int_option(args, "height", opts.height);
    opts.output = text_option(args, "output", opts.output.c_str());
    opts.style = text_option(args, "style", opts.style.c_str());
    return opts;
}

// ---------- subcommand implementations ----------

auto cmd_components() -> int {
    const auto types = au::list_all_components();
    au::Json arr = au::Json::array();
    for (const auto &t : types) {
        arr.push_back(t);
    }
    AURORA_LOG_RAW("cli", arr.dump(2), "\n");
    return 0;
}

auto cmd_describe(const std::string &name) -> int {
    const au::Json schema = au::describe_component(name);
    if (schema.empty()) {
        AURORA_LOG_ERROR("cli", "Error: unknown component '", name, "'");
        return 2;
    }
    AURORA_LOG_RAW("cli", schema.dump(2), "\n");
    return 0;
}

auto cmd_search(const std::string &query) -> int {
    const auto results = au::search_components(query);
    au::Json arr = au::Json::array();
    for (const auto &r : results) {
        arr.push_back(r);
    }
    AURORA_LOG_RAW("cli", arr.dump(2), "\n");
    return 0;
}

auto cmd_validate(const std::string &path) -> int {
    au::Json tree = au::tools::read_json_file(path);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", path, "'");
        return 2;
    }

    auto widget = au::serialization::from_json(tree);
    if (!widget) {
        const auto err = au::Json{{"ok", false}, {"error", widget.error().to_json()}};
        AURORA_LOG_RAW("cli", err.dump(2), "\n");
        return 1;
    }

    au::Node root(std::move(widget.value()));
    auto ok = validate(root);
    if (!ok) {
        auto err = au::Json{{"ok", false}, {"error", ok.error().to_json()}};
        AURORA_LOG_RAW("cli", err.dump(2), "\n");
        return 1;
    }

    AURORA_LOG_RAW("cli", au::Json{{"ok", true}}.dump(2), "\n");
    return 0;
}

auto cmd_snapshot(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    au::Json tree = au::tools::read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    auto widget = au::serialization::from_json(tree);
    if (!widget) {
        AURORA_LOG_ERROR("cli", "Error: ", widget.error().message);
        return 1;
    }

    au::Node root(std::move(widget.value()));
    au::Json snapshot = render_to_logical_snapshot(root, opts.width, opts.height);
    AURORA_LOG_RAW("cli", snapshot.dump(2), "\n");
    return 0;
}

auto cmd_render(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    au::Json tree = au::tools::read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    auto widget = au::serialization::from_json(tree);
    if (!widget) {
        AURORA_LOG_ERROR("cli", "Error: ", widget.error().message);
        return 1;
    }

    au::Node root(std::move(widget.value()));
    auto ok = au::render_to_png(root, opts.width, opts.height, opts.output.c_str());
    if (!ok) {
        AURORA_LOG_ERROR("cli", "Error: ", ok.error().message);
        return 1;
    }

    AURORA_LOG_RAW(
        "cli", au::Json{{"ok", true}, {"path", opts.output}, {"width", opts.width}, {"height", opts.height}}.dump(2),
        "\n");
    return 0;
}

auto cmd_preview(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: preview requires <tree.json>");
        return 2;
    }
    au::Json tree = au::tools::read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    auto widget = au::serialization::from_json(tree);
    if (!widget) {
        AURORA_LOG_ERROR("cli", "Error: ", widget.error().message);
        return 1;
    }

    au::Node root(std::move(widget.value()));
    au::WindowOptions win_opts;
    win_opts.size = au::Size{.width = static_cast<float>(opts.width), .height = static_cast<float>(opts.height)};
    win_opts.title = "Aurora Preview";
    // No real display backend (CI/SSH etc.): when falling back to Headless, render only one frame then exit,
    // avoiding a windowless idle frame loop.
    if (au::auto_detect_surface() == au::SurfaceKind::Headless) {
        win_opts.max_frames = 1;
    }

    auto win = create_native_window(win_opts);
    if (!win) {
        AURORA_LOG_ERROR("cli", "Error: ", win.error().message);
        return 1;
    }

    au::App().window(std::move(win.value())).view(std::move(root)).run();
    AURORA_LOG_RAW(
        "cli", au::Json{{"ok", true}, {"preview", opts.file}, {"width", opts.width}, {"height", opts.height}}.dump(2),
        "\n");
    return 0;
}

auto cmd_to_code(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    const au::Json tree = au::tools::read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    const au::serialization::CodeStyle style = au::tools::parse_code_style(opts.style);

    std::string code = to_code(tree, style);
    AURORA_LOG_RAW("cli", code, "\n");
    return 0;
}

auto cmd_to_yaml(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    const au::Json tree = au::tools::read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    std::string yaml = au::serialization::to_yaml(tree);
    AURORA_LOG_RAW("cli", yaml, "\n");
    return 0;
}

auto cmd_schema() -> int {
    const au::Json api = au::tools::build_api_skeleton();
    AURORA_LOG_RAW("cli", api.dump(2), "\n");
    return 0;
}

}  // namespace

// ---------- main ----------

auto main(int argc, char *argv[]) -> int {  // NOLINT(bugprone-exception-escape) 入口函数允许库异常逃逸到
                                            // main（terminate 即失败路径），CLI 不包装 try/catch
    au::serialization::register_core_widgets();

    const auto spec = build_spec();
    const auto parsed = cli::parse(spec, argc, argv);
    if (!parsed) {
        std::string detail = parsed.error().message;  // 用法错误：回显 token + 命令名
        if (!parsed.error().suggestion.empty()) {
            detail += " — " + parsed.error().suggestion;  // 如 "Did you mean --width?"
        }
        AURORA_LOG_ERROR("cli", detail);
        AURORA_LOG_ERROR("cli", "Run 'aurora_cli --help' for the command list.");
        return 2;  // 用法错误
    }

    const cli::Invocation &invocation = parsed.value();
    if (invocation.outcome != cli::ParseOutcome::Ok) {
        AURORA_LOG_RAW("cli", invocation.display_text);  // --help / --version 是一等结局
        return 0;
    }

    const cli::CommandSpec *const leaf_command = invocation.arguments.matched_command();
    const std::string cmd = (leaf_command == nullptr) ? std::string{} : leaf_command->name;
    const CliOptions opts = cli_options(invocation.arguments);

    if (cmd == "components") {
        return cmd_components();
    }
    if (cmd == "describe") {
        return cmd_describe(positional_text(invocation.arguments, 0));
    }
    if (cmd == "search") {
        return cmd_search(positional_text(invocation.arguments, 0));
    }
    if (cmd == "validate") {
        return cmd_validate(positional_text(invocation.arguments, 0));
    }
    if (cmd == "snapshot") {
        return cmd_snapshot(opts);
    }
    if (cmd == "render") {
        return cmd_render(opts);
    }
    if (cmd == "preview") {
        return cmd_preview(opts);
    }
    if (cmd == "to-code") {
        return cmd_to_code(opts);
    }
    if (cmd == "to-yaml") {
        return cmd_to_yaml(opts);
    }
    if (cmd == "schema") {
        return cmd_schema();
    }

    AURORA_LOG_ERROR("cli", "Error: unknown command '", cmd, "'");
    AURORA_LOG_RAW("cli", cli::help_text(spec, {spec.name}));
    return 2;
}

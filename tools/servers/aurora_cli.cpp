// tools/servers/aurora_cli.cpp
//
// aurora — Aurora CLI toolchain (spec #17).
//
// Provides subcommands for component discovery, UI-tree validation, offscreen rendering, code generation, etc.
// All output defaults to JSON (machine-readable); --format text gives human-readable output.
//
// Usage:
//   aurora components                                    list all registered component types
//   aurora describe <name>                               print the full schema of a single component
//   aurora search <keyword>                              search components by name
//   aurora validate <tree.json>                          validate a UI-tree JSON
//   aurora snapshot <tree.json> [-w W] [-h H]            print a logical snapshot JSON
//   aurora render <tree.json> [-w W] [-h H] [-o out.png] render offscreen to PNG
//   aurora preview <tree.json> [-w W] [-h H]             quick UI preview (opens a temporary window; falls back to a
//   single headless frame then exits when no display backend is available) aurora to-code <tree.json> [--style
//   fluent|step|di]  UI tree -> C++ code aurora to-yaml <tree.json>                           UI tree -> YAML format
//   aurora schema                                        print the full aurora_api.json
//   aurora --help                                        help information
//
// Exit codes: 0 on success, 1 on validation failure, 2 on usage error.

#include <cstring>
#include <string>
#include <vector>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/yaml.h"

#include "api_schema.h"
#include "code_style.h"
#include "json_file.h"

// ---------- Known enums (single source of truth: tools/include/known_enums.h) ----------

namespace {

// ---------- helpers ----------

// read_json_file / parse_code_style / build_api_skeleton are provided by shared headers under tools/include.

auto print_usage() -> void {
    AURORA_LOG_RAW(
        "cli", "Aurora CLI v" AURORA_VERSION_STRING " -- AI-first GUI toolchain\n\n"
               "Usage: aurora_cli <command> [options]\n\n"
               "Commands:\n"
               "  components                                     list all registered component types\n"
               "  describe <name>                                print the full schema of a single component (JSON)\n"
               "  search <keyword>                               search components by name\n"
               "  validate <tree.json>                           validate a UI-tree JSON and print diagnostics\n"
               "  snapshot <tree.json> [-w W] [-h H]             print a logical snapshot JSON\n"
               "  render <tree.json> [-w W] [-h H] [-o out.png]  render offscreen to PNG\n"
               "  preview <tree.json> [-w W] [-h H]              quick UI preview "
               "(opens a temporary window; falls back to a single headless frame then exits when no display backend is "
               "available)\n"
               "  to-code <tree.json> [--style fluent|step|di]   UI tree -> C++ code\n"
               "  to-yaml <tree.json>                            UI tree -> YAML format\n"
               "  schema                                         print the full aurora_api.json\n"
               "  --help, -h                                     show this help\n"
               "  --version, -V                                  show version\n\n"
               "Exit codes: 0 on success, 1 on validation failure, 2 on usage error\n");
}

/// Parse the -w / -h / -o / --style options.
struct CliOptions {
    int width = 800;
    int height = 600;
    std::string output = "aurora_render.png";
    std::string style = "fluent";
    std::string file;
};

// NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
[[nodiscard]] auto parse_options(int argc, char *argv[], int start) -> CliOptions { // NOLINT(*-avoid-c-arrays)
    CliOptions opts;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" && i + 1 < argc) {
            opts.width = std::strtol(argv[++i], nullptr, 10);
        } else if (arg == "-h" && i + 1 < argc) {
            opts.height = std::strtol(argv[++i], nullptr, 10);
        } else if (arg == "-o" && i + 1 < argc) {
            opts.output = argv[++i];
        } else if (arg == "--style" && i + 1 < argc) {
            opts.style = argv[++i];
        } else if (opts.file.empty() && arg[0] != '-') {
            opts.file = arg;
        }
    }
    return opts;
}
// NOLINTEND(*-pro-bounds-pointer-arithmetic)

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
        const auto err = au::Json{ { "ok", false }, { "error", widget.error().to_json() } };
        AURORA_LOG_RAW("cli", err.dump(2), "\n");
        return 1;
    }

    au::Node root(std::move(widget.value()));
    auto ok = validate(root);
    if (!ok) {
        auto err = au::Json{ { "ok", false }, { "error", ok.error().to_json() } };
        AURORA_LOG_RAW("cli", err.dump(2), "\n");
        return 1;
    }

    AURORA_LOG_RAW("cli", au::Json{ { "ok", true } }.dump(2), "\n");
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
        "cli",
        au::Json{ { "ok", true }, { "path", opts.output }, { "width", opts.width }, { "height", opts.height } }.dump(2),
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
    win_opts.size = au::Size{ .width = static_cast<float>(opts.width), .height = static_cast<float>(opts.height) };
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
        "cli",
        au::Json{ { "ok", true }, { "preview", opts.file }, { "width", opts.width }, { "height", opts.height } }.dump(
            2),
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

} // namespace

// ---------- main ----------

auto main(int argc, char *argv[]) -> int {
    au::serialization::register_core_widgets();

    // NOLINTBEGIN(*-pro-bounds-pointer-arithmetic)
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc < 2 ? 2 : 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0) {
        AURORA_LOG_RAW("cli", AURORA_VERSION_STRING, "\n");
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "components") {
        return cmd_components();
    }
    if (cmd == "describe") {
        if (argc < 3) {
            AURORA_LOG_ERROR("cli", "Error: describe requires <name>");
            return 2;
        }
        return cmd_describe(argv[2]);
    }
    if (cmd == "search") {
        if (argc < 3) {
            AURORA_LOG_ERROR("cli", "Error: search requires <keyword>");
            return 2;
        }
        return cmd_search(argv[2]);
    }
    if (cmd == "validate") {
        if (argc < 3) {
            AURORA_LOG_ERROR("cli", "Error: validate requires <tree.json>");
            return 2;
        }
        return cmd_validate(argv[2]);
    }
    if (cmd == "snapshot") {
        return cmd_snapshot(parse_options(argc, argv, 2));
    }
    if (cmd == "render") {
        return cmd_render(parse_options(argc, argv, 2));
    }
    if (cmd == "preview") {
        return cmd_preview(parse_options(argc, argv, 2));
    }
    if (cmd == "to-code") {
        return cmd_to_code(parse_options(argc, argv, 2));
    }
    if (cmd == "to-yaml") {
        return cmd_to_yaml(parse_options(argc, argv, 2));
    }
    if (cmd == "schema") {
        return cmd_schema();
    }

    // NOLINTEND(*-pro-bounds-pointer-arithmetic)

    AURORA_LOG_ERROR("cli", "Error: unknown command '", cmd, "'");
    print_usage();
    return 2;
}

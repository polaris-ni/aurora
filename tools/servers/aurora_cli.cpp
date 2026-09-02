// tools/servers/aurora_cli.cpp
//
// aurora — Aurora CLI 工具链（规格 #17）。
//
// 提供组件发现、UI 树校验、离屏渲染、代码生成等子命令，
// 所有输出默认 JSON（机器可读），可选 --format text 人类可读。
//
// 用法：
//   aurora components                         列出所有已注册组件类型
//   aurora describe <name>                    输出单个组件的完整 schema
//   aurora search <keyword>                   按名称搜索组件
//   aurora validate <tree.json>               校验 UI 树 JSON
//   aurora snapshot <tree.json> [-w W] [-h H] 输出逻辑快照 JSON
//   aurora render <tree.json> [-w W] [-h H] [-o out.png]  离屏渲染为 PNG
//   aurora preview <tree.json> [-w W] [-h H] 快速预览 UI（启动临时窗口；无显示后端时回退无头渲染一帧退出）
//   aurora to-code <tree.json> [--style fluent|step|di]   UI 树 → C++ 代码
//   aurora to-yaml <tree.json>                UI 树 → YAML 格式
//   aurora schema                             输出完整 aurora_api.json
//   aurora --help                             帮助信息
//
// 退出码：成功 0，校验失败 1，用法错误 2。

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/inspect.h"
#include "aurora/widget/yaml.h"
#include "known_enums.h"

#include "api_schema.h"
#include "code_style.h"
#include "json_file.h"

using namespace aurora;
using namespace aurora::serialization;
using namespace aurora::tools;

// ---------- 已知枚举（单一来源：tools/include/known_enums.h） ----------

namespace {

// ---------- 辅助 ----------

// read_json_file / parse_code_style / build_api_skeleton 由 tools/include 下的共享头提供。

auto print_usage() -> void {
    AURORA_LOG_RAW("cli", "Aurora CLI v" AURORA_VERSION_STRING " — AI-first GUI 工具链\n\n"
                          "用法：aurora_cli <command> [options]\n\n"
                          "命令：\n"
                          "  components                         列出所有已注册组件类型\n"
                          "  describe <name>                    输出单个组件的完整 schema（JSON）\n"
                          "  search <keyword>                   按名称搜索组件\n"
                          "  validate <tree.json>               校验 UI 树 JSON，输出诊断\n"
                          "  snapshot <tree.json> [-w W] [-h H] 输出逻辑快照 JSON\n"
                          "  render <tree.json> [-w W] [-h H] [-o out.png]  离屏渲染为 PNG\n"
                          "  preview <tree.json> [-w W] [-h H]  快速预览 UI（启动临时窗口；无显示后端时回退无头渲染一帧退出）\n"
                          "  to-code <tree.json> [--style fluent|step|di]   UI 树 → C++ 代码\n"
                          "  to-yaml <tree.json>                UI 树 → YAML 格式\n"
                          "  schema                             输出完整 aurora_api.json\n"
                          "  --help, -h                        显示本帮助\n"
                          "  --version, -V                     显示版本\n\n"
                          "退出码：成功 0，校验失败 1，用法错误 2\n");
}

/// 解析 -w / -h / -o / --style 选项。
struct CliOptions {
    int width = 800;
    int height = 600;
    std::string output = "aurora_render.png";
    std::string style = "fluent";
    std::string file;
};

[[nodiscard]] auto parse_options(int argc, char *argv[], int start) -> CliOptions {
    CliOptions opts;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-w" && i + 1 < argc) {
            opts.width = std::atoi(argv[++i]);
        } else if (arg == "-h" && i + 1 < argc) {
            opts.height = std::atoi(argv[++i]);
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

// ---------- 子命令实现 ----------

auto cmd_components() -> int {
    auto types = list_all_components();
    Json arr = Json::array();
    for (const auto &t : types)
        arr.push_back(t);
    AURORA_LOG_RAW("cli", arr.dump(2), "\n");
    return 0;
}

auto cmd_describe(const std::string &name) -> int {
    Json schema = describe_component(name);
    if (schema.empty()) {
        AURORA_LOG_ERROR("cli", "Error: unknown component '", name, "'");
        return 2;
    }
    AURORA_LOG_RAW("cli", schema.dump(2), "\n");
    return 0;
}

auto cmd_search(const std::string &query) -> int {
    auto results = search_components(query);
    Json arr = Json::array();
    for (const auto &r : results)
        arr.push_back(r);
    AURORA_LOG_RAW("cli", arr.dump(2), "\n");
    return 0;
}

auto cmd_validate(const std::string &path) -> int {
    Json tree = read_json_file(path);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", path, "'");
        return 2;
    }

    auto widget = from_json(tree);
    if (!widget) {
        Json err = Json{ { "ok", false }, { "error", widget.error().to_json() } };
        AURORA_LOG_RAW("cli", err.dump(2), "\n");
        return 1;
    }

    Node root(std::move(widget.value()));
    auto ok = validate(root);
    if (!ok) {
        Json err = Json{ { "ok", false }, { "error", ok.error().to_json() } };
        AURORA_LOG_RAW("cli", err.dump(2), "\n");
        return 1;
    }

    AURORA_LOG_RAW("cli", Json{ { "ok", true } }.dump(2), "\n");
    return 0;
}

auto cmd_snapshot(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    Json tree = read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    auto widget = from_json(tree);
    if (!widget) {
        AURORA_LOG_ERROR("cli", "Error: ", widget.error().message);
        return 1;
    }

    Node root(std::move(widget.value()));
    Json snapshot = render_to_logical_snapshot(root, opts.width, opts.height);
    AURORA_LOG_RAW("cli", snapshot.dump(2), "\n");
    return 0;
}

auto cmd_render(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    Json tree = read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    auto widget = from_json(tree);
    if (!widget) {
        AURORA_LOG_ERROR("cli", "Error: ", widget.error().message);
        return 1;
    }

    Node root(std::move(widget.value()));
    auto ok = render_to_png(root, opts.width, opts.height, opts.output.c_str());
    if (!ok) {
        AURORA_LOG_ERROR("cli", "Error: ", ok.error().message);
        return 1;
    }

    AURORA_LOG_RAW(
        "cli",
        Json{ { "ok", true }, { "path", opts.output }, { "width", opts.width }, { "height", opts.height } }.dump(2),
        "\n");
    return 0;
}

auto cmd_preview(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: preview requires <tree.json>");
        return 2;
    }
    Json tree = read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    auto widget = from_json(tree);
    if (!widget) {
        AURORA_LOG_ERROR("cli", "Error: ", widget.error().message);
        return 1;
    }

    Node root(std::move(widget.value()));
    WindowOptions wopts;
    wopts.size = Size{ .width = static_cast<float>(opts.width), .height = static_cast<float>(opts.height) };
    wopts.title = "Aurora Preview";
    // 无真实显示后端（CI/SSH 等）：回退 Headless 时只渲染一帧即退出，避免无窗口空转帧循环。
    if (auto_detect_surface() == SurfaceKind::Headless)
        wopts.max_frames = 1;

    auto win = create_native_window(wopts);
    if (!win) {
        AURORA_LOG_ERROR("cli", "Error: ", win.error().message);
        return 1;
    }

    App().window(std::move(win.value())).view(std::move(root)).run();
    AURORA_LOG_RAW("cli", Json{ { "ok", true }, { "preview", opts.file }, { "width", opts.width },
                                { "height", opts.height } }
                       .dump(2),
                   "\n");
    return 0;
}

auto cmd_to_code(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    Json tree = read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    CodeStyle style = aurora::tools::parse_code_style(opts.style);

    std::string code = to_code(tree, style);
    AURORA_LOG_RAW("cli", code, "\n");
    return 0;
}

auto cmd_to_yaml(const CliOptions &opts) -> int {
    if (opts.file.empty()) {
        AURORA_LOG_ERROR("cli", "Error: missing <tree.json> argument");
        return 2;
    }
    Json tree = read_json_file(opts.file);
    if (tree.is_discarded() || tree.is_null()) {
        AURORA_LOG_ERROR("cli", "Error: invalid JSON in '", opts.file, "'");
        return 2;
    }

    std::string yaml = aurora::serialization::to_yaml(tree);
    AURORA_LOG_RAW("cli", yaml, "\n");
    return 0;
}

auto cmd_schema() -> int {
    Json api = aurora::tools::build_api_skeleton();
    AURORA_LOG_RAW("cli", api.dump(2), "\n");
    return 0;
}

} // namespace

// ---------- main ----------

int main(int argc, char *argv[]) {
    register_core_widgets();

    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        print_usage();
        return argc < 2 ? 2 : 0;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0) {
        AURORA_LOG_RAW("cli", AURORA_VERSION_STRING, "\n");
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "components")
        return cmd_components();
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
    if (cmd == "schema")
        return cmd_schema();

    AURORA_LOG_ERROR("cli", "Error: unknown command '", cmd, "'");
    print_usage();
    return 2;
}

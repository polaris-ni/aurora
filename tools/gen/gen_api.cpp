// Requirement SPEC.FEAT.TOOLING.API-SCHEMA.001: generate aurora_api.json (Aurora public API description, for toolchain
// / LSP / docs).
//
// Extracts registered widget types and their property keys via runtime reflection
// (WidgetRegistry + serializeProps of each widget's default instance), supplements core enums
// (Color palette, LengthKind, Alignment, KeyCode), and outputs schema-friendly JSON.
//
// Usage:
//   gen_api_tools                 -> write JSON to stdout (can be redirected with `> aurora_api.json`)
//   gen_api_tools aurora_api.json -> write the file directly, cross-platform (used by the CMake target aurora_api_json)
//   gen_api_tools -               -> explicit stdout sentinel (used by tools/check/check_api_schema_sync.py)
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "api_schema.h"
#include "aurora/aurora.h"
#include "aurora/cli/args.h"
#include "aurora/cli/command.h"
#include "known_enums.h"

namespace {
// Known enums (compile-time constants, not obtainable by pure runtime reflection).
// Single source of truth: tools/include/known_enums.h — shared by gen_api / aurora_mcp / aurora_cli / aurora_lsp;
// values must match the real enum members in include/aurora/** verbatim (guarded by
// tests/integration/itest_known_enums.cpp).
auto known_enums() -> std::map<std::string, std::vector<std::string>> { return aurora::tools::known_enums(); }

/// @brief enum value list -> Json array (json::Value has no implicit construction from std::vector).
[[nodiscard]] auto enum_values_json(const std::string &name) -> au::Json {
    au::Json arr = au::Json::array();
    // 关键生命周期修正：known_enums() 返回的是临时 std::map，若直接 `known_enums()[name]` 取下标，
    // 临时 map 会在 range 表达式求值结束时被析构，循环拿到的是悬垂引用（ASan 报告的
    // heap-use-after-free）。先绑定到具名局部引用，把临时 map 的生命周期延长到本函数作用域。
    const auto &enums = known_enums();
    if (const auto it = enums.find(name); it != enums.end()) {
        for (const auto &v : it->second) {
            arr.push_back(v);
        }
    }
    return arr;
}

/// @brief 命令行声明表：唯一的输出路径既写又读（就地合并），`-` 与缺省均为「输出到 stdout」。
[[nodiscard]] auto build_spec() -> const aurora::cli::CommandSpec & {
    static const aurora::cli::CommandSpec ROOT_SPEC = [] {
        aurora::cli::CommandSpec root;
        root.name = "gen_api_tools";
        root.about = "Generate the aurora_api.json public-API description from the runtime registry.";
        root.positionals = {
            aurora::cli::PositionalSchema{
                .name = "OUT",
                .kind = aurora::cli::ValueKind::String,
                .arity = aurora::cli::Arity::optional_one(),
                .help = "Output path; `-` or omitted writes to stdout",
                .default_text = "-",
            },
        };
        root.epilog = "Exit codes: 0 = ok, 2 = usage error, 1 = business failure.";
        return root;
    }();
    return ROOT_SPEC;
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
// 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做
// try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto parsed = aurora::cli::parse(build_spec(), argc, argv);
    if (!parsed) {
        std::string detail = parsed.error().message;
        if (!parsed.error().suggestion.empty()) {
            detail += " — " + parsed.error().suggestion;
        }
        AURORA_LOG_ERROR("genapi", detail);
        return 2;
    }
    const aurora::cli::Invocation &invocation = parsed.value();
    if (invocation.shows_display()) {
        AURORA_LOG_RAW("genapi", invocation.display_text);
        return 0;
    }
    // 声明表给了 default_text，故该槽必然存在且必然可按 String 读出（不变量，无需再判错）。
    const std::string output_path = invocation.arguments.positional(0).value().as<std::string>().value();
    const bool to_stdout = (output_path == "-");

    aurora::serialization::register_core_widgets();

    au::Json api = aurora::tools::build_api_skeleton();

    // ---- layout_rules: simplified layout-protocol summary (for AI to generate JSON within constraints) ----
    au::Json layout_rules = au::Json::object();
    {
        au::Json flex = au::Json::object();
        flex.set("description",
                 "Flex layout (Column/Row): children are laid out along the main axis, aligned on the cross axis");
        flex.set("main_axis_alignment", enum_values_json("MainAxisAlignment"));
        flex.set("cross_axis_alignment", enum_values_json("CrossAxisAlignment"));
        flex.set("main_axis_size", enum_values_json("MainAxisSize"));
        flex.set("gap_constraint", "gap >= 0");
        layout_rules.set("flex", flex);

        au::Json stack = au::Json::object();
        stack.set("description", "Stack layout: children are layered, later-drawn ones on top");
        stack.set("fit", enum_values_json("StackFit"));
        layout_rules.set("stack", stack);

        au::Json grid = au::Json::object();
        grid.set("description", "Grid layout: a two-dimensional grid with a fixed column count");
        au::Json required_props = au::Json::array();
        required_props.push_back("columns");
        grid.set("required_props", std::move(required_props));
        grid.set("column_constraint", "columns >= 1");
        layout_rules.set("grid", grid);

        au::Json length = au::Json::object();
        length.set("description", "Length type: auto | fill | [px, v] | [percent, v]");
        length.set("auto", "WrapContent, sized by its content");
        length.set("fill", "Expand, absorb the remaining space");
        au::Json px_example = au::Json::array();
        px_example.push_back("px");
        px_example.push_back(100);
        length.set("px_example", std::move(px_example));
        au::Json percent_example = au::Json::array();
        percent_example.push_back("percent");
        percent_example.push_back(50);
        length.set("percent_example", std::move(percent_example));
        layout_rules.set("length", length);

        au::Json edge_insets = au::Json::object();
        edge_insets.set("description", "EdgeInsets object: {left, top, right, bottom}, unit dp");
        layout_rules.set("edge_insets", edge_insets);

        au::Json color = au::Json::object();
        color.set("description", "Color type: [r, g, b, a], each component 0-255");
        au::Json color_example = au::Json::array();
        color_example.push_back(255);
        color_example.push_back(128);
        color_example.push_back(0);
        color_example.push_back(255);
        color.set("example", std::move(color_example));
        layout_rules.set("color", color);
    }
    api.set("layout_rules", layout_rules);

    // ---- state_patterns: usage scenarios and JSON examples for the three state patterns ----
    au::Json state_patterns = au::Json::array();
    {
        au::Json p1 = au::Json::object();
        p1.set("name", "simple_value");
        p1.set("description",
               "Simple value state: the widget holds a single mutable value and notifies via the on_changed callback");
        au::Json p1_widgets = au::Json::array();
        for (const char *w :
             {"TextInput", "Slider", "Checkbox", "Switch", "Dropdown", "RadioGroup", "SegmentedControl", "TabBar"}) {
            p1_widgets.push_back(w);
        }
        p1.set("applicable_widgets", std::move(p1_widgets));
        p1.set("json_example",
               R"({"type": "TextInput", "props": {"value": "hello"}, "events": {"on_changed": "handler_name"}})");
        state_patterns.push_back(p1);

        au::Json p2 = au::Json::object();
        p2.set("name", "selection_index");
        p2.set("description",
               "Selection index state: one item is selected from an options list, managed via selected_index and "
               "on_change");
        au::Json p2_widgets = au::Json::array();
        for (const char *w : {"Dropdown", "RadioGroup", "SegmentedControl", "TabBar"}) {
            p2_widgets.push_back(w);
        }
        p2.set("applicable_widgets", std::move(p2_widgets));
        p2.set(
            "json_example",
            R"({"type": "Dropdown", "props": {"options": ["A","B","C"], "selected_index": 1}, "events": {"on_change": "handler"}})");
        state_patterns.push_back(p2);

        au::Json p3 = au::Json::object();
        p3.set("name", "toggle_state");
        p3.set("description", "Toggle state: a boolean toggle, managed via checked/value and on_changed/on_toggled");
        au::Json p3_widgets = au::Json::array();
        for (const char *w : {"Checkbox", "Switch", "ExpansionPanel"}) {
            p3_widgets.push_back(w);
        }
        p3.set("applicable_widgets", std::move(p3_widgets));
        p3.set("json_example",
               R"({"type": "Checkbox", "props": {"checked": false}, "events": {"on_changed": "handler"}})");
        state_patterns.push_back(p3);
    }
    api.set("state_patterns", state_patterns);

    // Preserve the error_codes section generated by gen_error_codes so a full rewrite does not overwrite it.
    if (!to_stdout) {
        std::ifstream ein(output_path, std::ios::binary);
        if (ein) {
            std::ostringstream ss;
            ss << ein.rdbuf();
            // 忽略损坏的既有文件：解析失败时保留新骨架（不继承 error_codes/debug 段）。
            if (auto existing_r = au::json::parse(ss.str()); existing_r) {
                const au::Json &existing = existing_r.value();
                if (existing.contains("error_codes")) {
                    api.set("error_codes", *existing.at("error_codes"));
                }
                // Preserve the debug section generated by gen_debug_api (same merge-only pattern) so a full
                // rewrite does not overwrite it.
                if (existing.contains("debug")) {
                    api.set("debug", *existing.at("debug"));
                }
            }
        }
    }

    const auto dumped = au::json::dump(api, {.indent = 2});
    const std::string text = dumped.ok() ? dumped.value() : std::string{};
    // When a file path argument is given, write the file directly (cross-platform, for the CMake target
    // aurora_api_json); otherwise write to stdout, preserving the `gen_api_tools > aurora_api.json` manual
    // redirection usage.
    if (to_stdout) {
        AURORA_LOG_RAW("genapi", text, "\n");
        return 0;
    }
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        AURORA_LOG_ERROR("genapi", "cannot open output file: ", output_path);
        return 1;
    }
    out << text << "\n";
    return 0;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

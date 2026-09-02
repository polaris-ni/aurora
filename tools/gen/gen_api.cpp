// Requirement #12: generate aurora_api.json (Aurora public API description, for toolchain / LSP / docs).
//
// Extracts registered widget types and their property keys via runtime reflection
// (WidgetRegistry + serializeProps of each widget's default instance), supplements core enums
// (Color palette, LengthKind, Alignment, KeyCode), and outputs schema-friendly JSON.
//
// Usage:
//   gen_api_tools                 -> write JSON to stdout (can be redirected with `> aurora_api.json`)
//   gen_api_tools aurora_api.json -> write the file directly, cross-platform (used by the CMake target aurora_api_json)
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#include "api_schema.h"
#include "known_enums.h"

namespace {
// Known enums (compile-time constants, not obtainable by pure runtime reflection).
// Single source of truth: tools/include/known_enums.h — shared by gen_api / aurora_mcp / aurora_cli / aurora_lsp;
// values must match the real enum members in include/aurora/** verbatim (guarded by tests/test_known_enums.cpp).
auto known_enums() -> std::map<std::string, std::vector<std::string>> { return aurora::tools::known_enums(); }

} // namespace

auto main(int argc, char **argv) -> int {
    aurora::serialization::register_core_widgets();

    aurora::storage::Json api = aurora::tools::build_api_skeleton();

    // ---- layout_rules: simplified layout-protocol summary (for AI to generate JSON within constraints) ----
    aurora::storage::Json layout_rules = aurora::storage::Json::object();
    {
        aurora::storage::Json flex = aurora::storage::Json::object();
        flex["description"] = "Flex layout (Column/Row): children are laid out along the main axis, aligned on the cross axis";
        flex["main_axis_alignment"] = known_enums()["MainAxisAlignment"];
        flex["cross_axis_alignment"] = known_enums()["CrossAxisAlignment"];
        flex["main_axis_size"] = known_enums()["MainAxisSize"];
        flex["gap_constraint"] = "gap >= 0";
        layout_rules["flex"] = flex;

        aurora::storage::Json stack = aurora::storage::Json::object();
        stack["description"] = "Stack layout: children are layered, later-drawn ones on top";
        stack["fit"] = known_enums()["StackFit"];
        layout_rules["stack"] = stack;

        aurora::storage::Json grid = aurora::storage::Json::object();
        grid["description"] = "Grid layout: a two-dimensional grid with a fixed column count";
        grid["required_props"] = aurora::storage::Json::array({ "columns" });
        grid["column_constraint"] = "columns >= 1";
        layout_rules["grid"] = grid;

        aurora::storage::Json length = aurora::storage::Json::object();
        length["description"] = "Length type: auto | fill | [px, v] | [percent, v]";
        length["auto"] = "WrapContent, sized by its content";
        length["fill"] = "Expand, absorb the remaining space";
        length["px_example"] = aurora::storage::Json::array({ "px", 100 });
        length["percent_example"] = aurora::storage::Json::array({ "percent", 50 });
        layout_rules["length"] = length;

        aurora::storage::Json edge_insets = aurora::storage::Json::object();
        edge_insets["description"] = "EdgeInsets object: {left, top, right, bottom}, unit dp";
        layout_rules["edge_insets"] = edge_insets;

        aurora::storage::Json color = aurora::storage::Json::object();
        color["description"] = "Color type: [r, g, b, a], each component 0-255";
        color["example"] = aurora::storage::Json::array({ 255, 128, 0, 255 });
        layout_rules["color"] = color;
    }
    api["layout_rules"] = layout_rules;

    // ---- state_patterns: usage scenarios and JSON examples for the three state patterns ----
    aurora::storage::Json state_patterns = aurora::storage::Json::array();
    {
        aurora::storage::Json p1 = aurora::storage::Json::object();
        p1["name"] = "simple_value";
        p1["description"] = "Simple value state: the widget holds a single mutable value and notifies via the on_changed callback";
        p1["applicable_widgets"] = aurora::storage::Json::array(
            { "TextInput", "Slider", "Checkbox", "Switch", "Dropdown", "RadioGroup", "SegmentedControl", "TabBar" });
        p1["json_example"] =
            R"({"type": "TextInput", "props": {"value": "hello"}, "events": {"on_changed": "handler_name"}})";
        state_patterns.push_back(p1);

        aurora::storage::Json p2 = aurora::storage::Json::object();
        p2["name"] = "selection_index";
        p2["description"] = "Selection index state: one item is selected from an options list, managed via selected_index and on_change";
        p2["applicable_widgets"] =
            aurora::storage::Json::array({ "Dropdown", "RadioGroup", "SegmentedControl", "TabBar" });
        p2["json_example"] =
            R"({"type": "Dropdown", "props": {"options": ["A","B","C"], "selected_index": 1}, "events": {"on_change": "handler"}})";
        state_patterns.push_back(p2);

        aurora::storage::Json p3 = aurora::storage::Json::object();
        p3["name"] = "toggle_state";
        p3["description"] = "Toggle state: a boolean toggle, managed via checked/value and on_changed/on_toggled";
        p3["applicable_widgets"] = aurora::storage::Json::array({ "Checkbox", "Switch", "ExpansionPanel" });
        p3["json_example"] =
            R"({"type": "Checkbox", "props": {"checked": false}, "events": {"on_changed": "handler"}})";
        state_patterns.push_back(p3);
    }
    api["state_patterns"] = state_patterns;

    // Preserve the error_codes section generated by gen_error_codes so a full rewrite does not overwrite it.
    if (argc > 1 && std::string(argv[1]) != "-") {    // NOLINT(*-pro-bounds-pointer-arithmetic)
        std::ifstream ein(argv[1], std::ios::binary); // NOLINT(*-pro-bounds-pointer-arithmetic)
        if (ein) {
            try {
                nlohmann::json existing = nlohmann::json::parse(ein);
                if (existing.contains("error_codes")) {
                    api["error_codes"] = existing["error_codes"];
                }
                // Preserve the debug section generated by gen_debug_api (same merge-only pattern) so a full
                // rewrite does not overwrite it.
                if (existing.contains("debug")) {
                    api["debug"] = existing["debug"];
                }
                // NOLINTNEXTLINE(*-empty-catch)
            } catch (...) { /* ignore a corrupt existing file */
            }
        }
    }

    const std::string text = api.dump(2);
    // When a file path argument is given, write the file directly (cross-platform, for the CMake target
    // aurora_api_json); otherwise write to stdout, preserving the `gen_api_tools > aurora_api.json` manual
    // redirection usage.
    char *argv1 = argv[1]; // NOLINT(*-pro-bounds-pointer-arithmetic)
    if (argc > 1 && std::string(argv1) != "-") {
        std::ofstream out(argv1, std::ios::binary);
        if (!out) {
            AURORA_LOG_RAW("genapi", "error: cannot open output file: ", argv1, "\n");
            return 1;
        }
        out << text << "\n";
        return 0;
    }
    AURORA_LOG_RAW("genapi", text, "\n");
    return 0;
}

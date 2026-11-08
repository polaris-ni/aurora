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
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
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

/// @brief 字符串列表 -> Json array（同 `enum_values_json` 的理由：`json::Value` 无 vector 隐式构造）。
[[nodiscard]] auto string_array(std::initializer_list<std::string_view> items) -> au::Json {
    au::Json arr = au::Json::array();
    for (const auto &item : items) {
        arr.push_back(item);
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

    // ---- theme_tokens: machine-readable token vocabulary (non-entry block, one row per registered group) ----
    // 数据源是各令牌组头里的「成员指针 -> 短名」登记表（aurora::theme_token_groups()），故本块与
    // schema 里各条目的 default_expr 不可能各说各话：两处读的是同一份表。
    au::Json theme_tokens = au::Json::object();
    {
        theme_tokens.set(
            "name_rule",
            "short name = theme.<Theme field>.<member> for metric groups (shapes / spacing / controls / "
            "motion), theme.<member> for color roles; the prefix column gives the group's prefix verbatim");
        theme_tokens.set(
            "usage",
            "a token name is a display path only; property fallbacks bind the member pointer, so renaming a "
            "token is a compile error rather than a stale string");
        au::Json groups = au::Json::array();
        for (const auto &group : au::theme_token_groups()) {
            au::Json row = au::Json::object();
            row.set("owner", std::string{group.owner});
            row.set("field", std::string{group.field});
            row.set("prefix", std::string{group.prefix});
            row.set("type", std::string{group.value_type});
            au::Json tokens = au::Json::array();
            for (const auto &name : group.tokens) {
                tokens.push_back(std::string{name});
            }
            row.set("count", au::Json{static_cast<int>(group.tokens.size())});
            row.set("tokens", std::move(tokens));
            groups.push_back(std::move(row));
        }
        theme_tokens.set("groups", std::move(groups));
    }
    api.set("theme_tokens", theme_tokens);

    // ---- accessor_rules: how a property key maps to public accessor names (non-entry block) ----
    // 一次性的规则块，不随控件数增长：公共访问器名从不在 schema 条目里（实测 widgets[i] 只有 type /
    // props / events / children_policy / examples），消费方按本块 + required + source 即可推全名。
    // 逐条判据取自**已迁到权威表的 18 型**实测签名（`button.h` / `text.h` / `text_input.h` 等），未迁控件
    // 不在本块的承诺范围内 —— 故 retired_scope 一行的措辞只收「表内行」，不把 `show.h` 一类残留具名件
    // 说成已废止（说满即假，本块是给模型的裁决面而不是愿景）。
    au::Json accessor_rules = au::Json::object();
    {
        accessor_rules.set("root", "the table key is the word root: table key, JSON key and accessor root agree");
        accessor_rules.set("value_writer",
                           "set_<key>(<bare value>) -> Props&   // exactly one per row, written on the XxxProps class");
        accessor_rules.set("value_writer_parameter",
                           "the parameter is the row's bare value type; passing form (by value or const reference) is "
                           "written per control, so read the header for that detail");
        accessor_rules.set("value_writer_string_conveniences",
                           "extra (const std::string &) and (const char *) overloads appear only where the bare value "
                           "is string-like, never for Color / float / enum rows");
        accessor_rules.set("source_writer",
                           "set_<key>(ReadOnlyState<T>) -> Props&   // exactly the rows whose entry carries "
                           "source=true; rebinds the row to that source");
        accessor_rules.set("writable_source_writer",
                           "set_<key>(State<T>) -> Props&   // only where the field is State<T>; there the value "
                           "writer above writes through the current source instead of replacing it");
        accessor_rules.set("reader", "<key>() const -> T   // the row's fallback is the constant tier");
        accessor_rules.set("reader_scoped",
                           "<key>(const BuildContext &) const -> T   // the row's fallback is a live token or derived");
        accessor_rules.set("reader_rule",
                           "exactly one reader per row; the two shapes are mutually exclusive and picking the wrong "
                           "one is a compile error, not a stale value");
        accessor_rules.set("widget_side",
                           "each widget publicly inherits its XxxProps class, so every writer and reader above is "
                           "callable on the widget and returns the Props base reference; a few controls add "
                           "widget-typed conveniences on top");
        accessor_rules.set("bit_required",
                           "required=true means the row's field is non-optional: clear(row) has no overload for it "
                           "and the row always carries a value");
        accessor_rules.set("bit_source",
                           "source=true means the row's field is a reactive handle (ReadOnlyState<T> or State<T>); "
                           "the bit is derived from the field shape and never written on the row");
        accessor_rules.set("common_pieces", string_array({"clear(row)", "rows()"}));
        accessor_rules.set("names_retired",
                           string_array({"reset_<key>", "has_<key>", "set_<key>_source", "<key>_source"}));
        accessor_rules.set("names_retired_scope",
                           "retired for rows declared on the authoritative table; controls still on hand-written "
                           "descriptors and the widget base's modifier plumbing keep their own *_source() names");
    }
    api.set("accessor_rules", accessor_rules);

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

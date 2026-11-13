// Emits the translation unit body that proves every registered widget type still produces compilable
// code in all three generation styles. The matrix is type x style: one function per cell, each built by
// the library's own aurora::serialization::to_code from a minimal node snapshot, so a style that drifts
// away from the migrated property surface (a renamed writer, a missing Props class, a wrong namespace
// prefix) fails at compile time instead of rotting in a string comparison.
//
// The node snapshots stay deliberately minimal -- type plus empty props and children. That is the shape
// the single-declaration surface has to keep working for: a widget must be constructible with no stated
// attributes, because every row falls back to its declared provider. Stating attributes here would test
// the emitters, which itest_to_code already pins value by value; what this matrix adds is coverage of the
// whole type roster, including the ones whose properties are omitted by design.
//
// Types whose product does not compile today live in codegen_matrix_exemptions.txt and are emitted into a
// separate table instead of the compile cells. That list is a measured baseline, not an allowance: an
// entry that becomes compilable must be deleted by hand.
//
// Usage:
//   gen_codegen_matrix <aurora_api.json> --out <dir> [--exempt <file>]
//       write <dir>/codegen_matrix.inc (one function per compilable cell + the entry tables)
//   gen_codegen_matrix <aurora_api.json> --dump <Type>
//       write that type's three products to stdout for triage
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/cli/args.h"
#include "aurora/cli/command.h"

namespace {

/// 该风格产物是表达式还是语句序列：StepByStep 走「先声明后赋值」，需要包成函数体。
auto is_expression_style(au::serialization::CodeStyle style) -> bool {
    return style != au::serialization::CodeStyle::StepByStep;
}

auto style_tag(au::serialization::CodeStyle style) -> const char * {
    switch (style) {
        case au::serialization::CodeStyle::Fluent:
            return "fluent";
        case au::serialization::CodeStyle::DesignatedInit:
            return "designated_init";
        case au::serialization::CodeStyle::StepByStep:
            return "step_by_step";
    }
    return "unknown";
}

/// 去掉产物末尾的空行与尾随空格，便于拼进函数体。
auto trim_trailing(std::string s) -> std::string {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

/// 豁免清单：`<Type> | <reason>`，`#` 之后的文本忽略。
auto read_exemptions(const std::string &path, std::vector<std::string> &out) -> bool {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        const auto bar = line.find('|');
        if (bar == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, bar);
        const auto first = name.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        const auto last = name.find_last_not_of(" \t\r\n");
        out.push_back(name.substr(first, last - first + 1));
    }
    return true;
}

/// @brief Command-line declaration table: parsing, derived `--help` and the mutual exclusion between
///        writing the matrix and dumping one type all come from this single schema (`aurora::cli`).
[[nodiscard]] auto build_spec() -> const aurora::cli::CommandSpec & {
    using aurora::cli::OptionSchema;
    using aurora::cli::PositionalSchema;
    using aurora::cli::ValueKind;

    static const aurora::cli::CommandSpec ROOT_SPEC = [] {
        aurora::cli::CommandSpec root;
        root.name = "gen_codegen_matrix";
        root.about =
            "Emit the compile matrix that holds every registered widget type to compilable code "
            "in all three generation styles";
        root.positionals = {
            PositionalSchema{
                .name = "API",
                .kind = ValueKind::String,
                .help = "Path to aurora_api.json; the type roster is read from its widgets array",
            },
        };
        root.options = {
            OptionSchema{
                .long_name = "out",
                .kind = ValueKind::String,
                .help = "Output directory for codegen_matrix.inc",
                .value_hint = "DIR",
                .conflicts_with = {"dump"},
            },
            OptionSchema{
                .long_name = "exempt",
                .kind = ValueKind::String,
                .help = "Exemption list of types whose product does not compile today",
                .value_hint = "FILE",
            },
            OptionSchema{
                .long_name = "dump",
                .kind = ValueKind::String,
                .help = "Print one type's three products instead of writing the matrix",
                .value_hint = "TYPE",
                .conflicts_with = {"out"},
            },
        };
        root.epilog = "Exit codes: 0 = ok, 2 = usage error, 1 = business failure.";
        return root;
    }();
    return ROOT_SPEC;
}

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），生成器不做 try/catch 包装
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto parsed = aurora::cli::parse(build_spec(), argc, argv);
    if (!parsed) {
        AURORA_LOG_ERROR("codegen", parsed.error().message);
        return 2;
    }
    const aurora::cli::Invocation &invocation = parsed.value();
    if (invocation.shows_display()) {
        AURORA_LOG_RAW("codegen", invocation.display_text);
        return 0;
    }

    const auto api_arg = invocation.arguments.positional(0);
    if (!api_arg) {
        AURORA_LOG_ERROR("codegen", api_arg.error().message);
        return 2;
    }
    const std::string api_path = api_arg.value().as<std::string>().value();

    const auto read_option = [&invocation](std::string_view name) -> std::string {
        if (!invocation.arguments.explicitly_given(name)) {
            return {};
        }
        const auto value = invocation.arguments.get<std::string>(name);
        return value ? value.value() : std::string{};
    };
    const std::string out_dir = read_option("out");
    const std::string exempt_path = read_option("exempt");
    const std::string dump_type = read_option("dump");

    if (out_dir.empty() && dump_type.empty()) {
        AURORA_LOG_ERROR("codegen", "either --out <dir> or --dump <Type> is required");
        return 2;
    }

    std::ifstream in(api_path, std::ios::binary);
    if (!in) {
        AURORA_LOG_ERROR("codegen", "cannot read '", api_path, "'");
        return 1;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    auto parsed_json = au::json::parse(text);
    if (!parsed_json) {
        AURORA_LOG_ERROR("codegen", "'", api_path, "' is not valid JSON: ", parsed_json.error().message);
        return 1;
    }
    const au::Json &root = parsed_json.value();

    constexpr au::serialization::CodeStyle styles[] = {
        au::serialization::CodeStyle::Fluent,
        au::serialization::CodeStyle::DesignatedInit,
        au::serialization::CodeStyle::StepByStep,
    };

    // 调试面：把一个类型的三种产物打到 stdout，供人工核对包装形态。
    if (!dump_type.empty()) {
        au::Json node = au::Json::object();
        node.set("type", dump_type);
        node.set("props", au::Json::object());
        node.set("children", au::Json::array());
        for (const auto style : styles) {
            AURORA_LOG_RAW("codegen", "--- ", dump_type, " / ", style_tag(style), " ---\n",
                           au::serialization::to_code(node, style), "\n");
        }
        return 0;
    }

    const auto *widgets = root.find("widgets");
    if (widgets == nullptr || !widgets->is_array()) {
        AURORA_LOG_ERROR("codegen", "'", api_path, "' has no widgets array");
        return 1;
    }

    std::vector<std::string> exempt_types;
    if (!exempt_path.empty() && !read_exemptions(exempt_path, exempt_types)) {
        AURORA_LOG_ERROR("codegen", "cannot read exemption list '", exempt_path, "'");
        return 1;
    }

    std::string body;
    std::string table;
    std::string exempt_table;
    std::size_t cell = 0;
    std::size_t exempt_count = 0;

    for (std::size_t w = 0; w < widgets->size(); ++w) {
        const auto *item = widgets->at(w);
        if (item == nullptr) {
            continue;
        }
        const auto *type = item->find("type");
        if (type == nullptr || !type->is_string()) {
            AURORA_LOG_ERROR("codegen", "widgets[", w, "] has no string 'type'");
            return 1;
        }
        const auto type_name = type->as_or<std::string>("");

        if (std::ranges::find(exempt_types, type_name) != exempt_types.end()) {
            // 仍走一次 to_code，确认豁免的只是「不可编译」而不是「不可生成」。
            au::Json probe = au::Json::object();
            probe.set("type", type_name);
            probe.set("props", au::Json::object());
            probe.set("children", au::Json::array());
            (void)au::serialization::to_code(probe, au::serialization::CodeStyle::Fluent);
            exempt_table += "    { \"";
            exempt_table += type_name;
            exempt_table += "\" },\n";
            ++exempt_count;
            continue;
        }

        au::Json node = au::Json::object();
        node.set("type", type_name);
        node.set("props", au::Json::object());
        node.set("children", au::Json::array());

        for (const auto style : styles) {
            const std::string product = trim_trailing(au::serialization::to_code(node, style));
            const std::string fn = "codegen_matrix_cell_" + std::to_string(cell);

            body += "// ";
            body += type_name;
            body += " / ";
            body += style_tag(style);
            body += "\ninline auto ";
            body += fn;
            body += "() -> au::Node {\n";
            if (is_expression_style(style)) {
                body += "    return ";
                body += product;
                body += ";\n";
            } else {
                // 语句形态：产物以根变量 `__w0` 收尾，直接交出去。
                body += product;
                body += "\n    return au::Node{std::move(__w0)};\n";
            }
            body += "}\n\n";

            table += "    { \"";
            table += type_name;
            table += "/";
            table += style_tag(style);
            table += "\", &";
            table += fn;
            table += " },\n";
            ++cell;
        }
    }

    const std::string path = out_dir + "/codegen_matrix.inc";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        AURORA_LOG_ERROR("codegen", "cannot write '", path, "'");
        return 1;
    }
    out << "// Generated by gen_codegen_matrix from aurora_api.json -- do not edit.\n";
    out << "// Cells: " << cell << " (compilable widget types x generation styles); exempt: " << exempt_count
        << ".\n\n";
    out << body;
    out << "struct CodegenMatrixEntry {\n";
    out << "    const char *label;\n";
    out << "    au::Node (*build)();\n";
    out << "};\n\n";
    out << "inline constexpr CodegenMatrixEntry k_codegen_matrix[] = {\n";
    out << table;
    out << "};\n";
    out << "\nstruct CodegenMatrixExempt {\n";
    out << "    const char *type;\n";
    out << "};\n\n";
    out << "// Types whose to_code product does not compile today (see tools/gen/codegen_matrix_exemptions.txt).\n";
    out << "inline constexpr CodegenMatrixExempt k_codegen_matrix_exempt[] = {\n";
    out << exempt_table;
    out << "};\n";
    if (!out) {
        AURORA_LOG_ERROR("codegen", "write to '", path, "' failed");
        return 1;
    }

    AURORA_LOG_INFO("codegen", cell, " compilable cells from ", widgets->size(), " types, ", exempt_count,
                    " exempted -> ", path);
    return 0;
}

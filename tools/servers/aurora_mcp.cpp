// tools/servers/aurora_mcp.cpp
//
// aurora-mcp — Aurora MCP Server (Model Context Protocol, stdio JSON-RPC 2.0).
//
// Provides AI Agents with runtime component discovery, UI-tree validation, offscreen rendering and code generation.
// Transport: stdin/stdout, message format Content-Length: <N>\r\n\r\n<JSON-RPC 2.0 body>.
//
// Usage: aurora_mcp (launched by an AI Agent in stdio mode; no human interaction required)
//
// Exposes 10 MCP Tools:
//   list_components / describe_component / search_components /
//   validate_tree / validate_ui / render_snapshot / render_png /
//   to_code / to_yaml / get_schema

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/yaml.h"

#include "api_schema.h"
#include "code_style.h"

// ---------- Known enums (single source of truth: tools/include/known_enums.h) ----------

namespace {

// ---------- MCP protocol I/O ----------

/// Read one MCP message from stdin (Content-Length header + JSON body). Returns an empty au::Json on EOF.
[[nodiscard]] auto read_message() -> au::Json {
    // read header line
    std::string line;
    int content_length = -1;
    while (std::getline(std::cin, line)) {
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break; // empty line ends the header
        }
        if (line.starts_with("Content-Length:")) {
            // Wire-protocol headers come from the peer and may be malformed ("Content-Length: abc").
            // The invalid_argument/out_of_range thrown by std::stoi are not caught here and would terminate the server.
            try {
                content_length = std::stoi(line.substr(15));
            } catch (const std::exception &) {
                return au::Json{}; // treat as protocol error, behave like EOF
            }
        }
    }
    if (content_length < 0) {
        return au::Json{};
    }
    // Upper-bound guard: a malformed/malicious peer may declare an enormous Content-Length; an unbounded
    // resize would terminate the server via bad_alloc / length_error. Legitimate MCP messages are far below 64MiB.
    constexpr int max_message_bytes = 64 * 1024 * 1024;
    if (content_length > max_message_bytes) {
        return au::Json{}; // treat as protocol error, behave like EOF
    }

    // read body
    std::string body(static_cast<std::size_t>(content_length), '\0');
    std::cin.read(body.data(), content_length);
    if (std::cin.gcount() < content_length) {
        return au::Json{};
    }

    return au::Json::parse(body, nullptr, false);
}

/// Write one MCP message to stdout (stdio wire frame; must use the prefix-free raw channel to keep the Content-Length
/// header byte-exact).
auto write_message(const au::Json &msg) -> void {
    std::string body = msg.dump();
    AURORA_LOG_RAW("mcp", "Content-Length: ", body.size(), "\r\n\r\n", body);
}

/// Build a JSON-RPC 2.0 success response.
[[nodiscard]] auto rpc_result(const au::Json &id, const au::Json &result) -> au::Json {
    return au::Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
}

/// Build a JSON-RPC 2.0 error response.
[[nodiscard]] auto rpc_error(const au::Json &id, int code, const std::string &message) -> au::Json {
    return au::Json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
}

/// @brief Determine whether a persisted output path is confined within the server's working directory.
///
/// The MCP caller is an AI Agent whose arguments can be manipulated by any untrusted content it reads
/// (repo files, web pages, issue bodies) through prompt injection. If render_png's path were unconstrained,
/// it could write/overwrite arbitrary files (e.g. an autostart directory), forming a classic confused-deputy
/// arbitrary-write primitive. Hence we only accept relative paths inside the working directory.
[[nodiscard]] auto is_confined_output_path(const std::string &path) -> bool {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path p(path);
    if (p.is_absolute() || p.has_root_name()) {
        return false; // reject absolute paths, drive letters and UNC prefixes
    }
    return std::ranges::all_of(p, [](const auto &part) -> auto { return part != ".."; });
}

// ---------- MCP tool definitions ----------

/// Return the tool array for tools/list.
[[nodiscard]] auto tool_definitions() -> au::Json {
    au::Json tools = au::Json::array();

    // helper lambda: build inputSchema
    auto schema_obj = [](au::Json props, au::Json req) -> au::Json {
        au::Json s = au::Json::object();
        s["type"] = "object";
        s["properties"] = std::move(props);
        if (!req.empty()) {
            s["required"] = std::move(req);
        }
        return s;
    };
    auto str_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p["type"] = "string";
        p["description"] = desc;
        return p;
    };
    auto int_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p["type"] = "integer";
        p["description"] = desc;
        return p;
    };
    auto obj_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p["type"] = "object";
        p["description"] = desc;
        return p;
    };
    auto req_arr = [](std::initializer_list<const char *> items) -> au::Json {
        au::Json a = au::Json::array();
        for (const auto *i : items) {
            a.push_back(i);
        }
        return a;
    };

    // list_components
    {
        au::Json t = au::Json::object();
        t["name"] = "list_components";
        t["description"] = "List all registered Aurora component type names";
        t["inputSchema"] = schema_obj(au::Json::object(), au::Json::array());
        tools.push_back(std::move(t));
    }
    // describe_component
    {
        au::Json props = au::Json::object();
        props["name"] = str_prop("Component type name, e.g. Button");
        au::Json t = au::Json::object();
        t["name"] = "describe_component";
        t["description"] = "Return the full schema of a single component (props/events/children policy/examples)";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "name" }));
        tools.push_back(std::move(t));
    }
    // search_components
    {
        au::Json props = au::Json::object();
        props["query"] = str_prop("Search keyword");
        au::Json t = au::Json::object();
        t["name"] = "search_components";
        t["description"] = "Fuzzy-search registered components by name substring";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "query" }));
        tools.push_back(std::move(t));
    }
    // validate_tree
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON (to_json output format)");
        au::Json t = au::Json::object();
        t["name"] = "validate_tree";
        t["description"] = "Validate the legality of a UI-tree JSON (type/depth/empty children)";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // validate_ui (specification/08-tooling.md §7.1: schema static validation, structured errors with path/suggestions)
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON (to_json output format)");
        au::Json t = au::Json::object();
        t["name"] = "validate_ui";
        t["description"] =
            "Statically validate the UI tree against aurora_api.json schema: unknown type / missing required "
            "prop / type mismatch / children policy; errors include JSON path and fix suggestions (for AI auto-fix)";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // render_snapshot
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON");
        props["width"] = int_prop("Viewport width (default 800)");
        props["height"] = int_prop("Viewport height (default 600)");
        au::Json t = au::Json::object();
        t["name"] = "render_snapshot";
        t["description"] =
            "Run offscreen layout on the UI-tree JSON and return a logical snapshot (type + box + children)";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // render_png
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON");
        props["width"] = int_prop("Canvas width (default 800)");
        props["height"] = int_prop("Canvas height (default 600)");
        props["path"] = str_prop("Output PNG path; must be a relative path inside the working directory without '..' "
                                 "(default aurora_render.png)");
        au::Json t = au::Json::object();
        t["name"] = "render_png";
        t["description"] = "Run offscreen rendering on the UI-tree JSON and output a PNG file";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // to_code
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON");
        props["style"] = str_prop("Code style: fluent | step | di (default fluent)");
        au::Json t = au::Json::object();
        t["name"] = "to_code";
        t["description"] = "Convert a UI-tree JSON into compilable C++ code";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // to_yaml
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON object (with type/props/children)");
        au::Json t = au::Json::object();
        t["name"] = "to_yaml";
        t["description"] = "Convert a UI-tree JSON into a YAML-formatted string";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // get_schema
    {
        au::Json t = au::Json::object();
        t["name"] = "get_schema";
        t["description"] = "Return the full Aurora API schema (all components + enums)";
        t["inputSchema"] = schema_obj(au::Json::object(), au::Json::array());
        tools.push_back(std::move(t));
    }

    return tools;
}

// ---------- MCP tool execution ----------

/// Build the content array (text type) for an MCP tools/call.
[[nodiscard]] auto text_content(const std::string &text) -> au::Json {
    au::Json item = au::Json::object();
    item["type"] = "text";
    item["text"] = text;
    au::Json arr = au::Json::array();
    arr.push_back(std::move(item));
    return arr;
}

[[nodiscard]] auto json_content(const au::Json &j) -> au::Json { return text_content(j.dump(2)); }

/// Execute the named tool and return the MCP tools/call result.
// NOLINTNEXTLINE(*-function-cognitive-complexity)
[[nodiscard]] auto execute_tool(const std::string &name, const au::Json &args) -> au::Json {
    if (name == "list_components") {
        // Use the Inspector facade to fetch all component schemas and extract type names
        auto schemas = aurora::Inspector::components();
        au::Json arr = au::Json::array();
        for (const auto &s : schemas) {
            if (s.contains("type")) {
                arr.push_back(s["type"]);
            }
        }
        return au::Json{ { "content", json_content(arr) } };
    }

    if (name == "describe_component") {
        if (!args.contains("name") || !args["name"].is_string()) {
            return au::Json{ { "content", text_content("Error: missing 'name' parameter") }, { "isError", true } };
        }
        au::Json schema = aurora::Inspector::component_schema(args["name"].get<std::string>());
        return au::Json{ { "content", json_content(schema) } };
    }

    if (name == "search_components") {
        if (!args.contains("query") || !args["query"].is_string()) {
            return au::Json{ { "content", text_content("Error: missing 'query' parameter") }, { "isError", true } };
        }
        auto results = aurora::search_components(args["query"].get<std::string>());
        au::Json arr = au::Json::array();
        for (const auto &r : results) {
            arr.push_back(r);
        }
        return au::Json{ { "content", json_content(arr) } };
    }

    if (name == "validate_ui") {
        // specification/08-tooling.md §7.1: schema static validation (no widget construction, pure JSON against
        // aurora_api schema).
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        const au::Json report = aurora::validate_ui_tree_json(args["tree"]);
        au::Json out{ { "content", json_content(report) } };
        if (!report["valid"].get<bool>()) {
            out["isError"] = true;
        }
        return out;
    }

    if (name == "validate_tree") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            auto err = au::Json{ { "ok", false }, { "error", widget.error().to_json() } };
            return au::Json{ { "content", json_content(err) }, { "isError", true } };
        }
        aurora::Node root(std::move(widget.value()));
        auto diags = aurora::Inspector::validate(root);
        if (!diags.empty()) {
            au::Json err = au::Json::object();
            err["ok"] = false;
            au::Json diag_arr = au::Json::array();
            for (const auto &d : diags) {
                diag_arr.push_back(au::Json::parse(d.to_json_line(), nullptr, false));
            }
            err["diagnostics"] = diag_arr;
            return au::Json{ { "content", json_content(err) }, { "isError", true } };
        }
        return au::Json{ { "content", json_content(au::Json{ { "ok", true } }) } };
    }

    if (name == "render_snapshot") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            return au::Json{ { "content", text_content("Error: " + widget.error().message) }, { "isError", true } };
        }
        aurora::Node root(std::move(widget.value()));
        au::Json snapshot = render_to_logical_snapshot(root, w, h);
        return au::Json{ { "content", json_content(snapshot) } };
    }

    if (name == "render_png") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        std::string path = args.value("path", std::string("aurora_render.png"));
        if (!is_confined_output_path(path)) {
            return au::Json{ { "content",
                               text_content(
                                   "Error: 'path' must be a relative path inside the working directory (no '..')") },
                             { "isError", true } };
        }
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            return au::Json{ { "content", text_content("Error: " + widget.error().message) }, { "isError", true } };
        }
        aurora::Node root(std::move(widget.value()));
        auto ok = render_to_png(root, w, h, path.c_str());
        if (!ok) {
            return au::Json{ { "content", text_content("Error: " + ok.error().message) }, { "isError", true } };
        }
        return au::Json{ { "content", json_content(au::Json{ { "path", path }, { "width", w }, { "height", h } }) } };
    }

    if (name == "to_code") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        std::string style_str = args.value("style", std::string("fluent"));
        auto style = aurora::tools::parse_code_style(style_str);

        std::string code = to_code(args["tree"], style);
        return au::Json{ { "content", text_content(code) } };
    }

    if (name == "to_yaml") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        std::string yaml = aurora::serialization::to_yaml(args["tree"]);
        return au::Json{ { "content", text_content(yaml) } };
    }

    if (name == "get_schema") {
        au::Json api = aurora::tools::build_api_skeleton();
        return au::Json{ { "content", json_content(api) } };
    }

    // unknown tool
    return au::Json{ { "content", text_content("Error: unknown tool '" + name + "'") }, { "isError", true } };
}

// ---------- JSON-RPC dispatch ----------

[[nodiscard]] auto handle_request(const au::Json &req) -> au::Json {
    const au::Json id = req.contains("id") ? req["id"] : au::Json(nullptr);
    const std::string method = req.value("method", std::string(""));
    const au::Json params = req.value("params", au::Json::object());

    if (method == "initialize") {
        const auto result = au::Json{
            { "protocolVersion", "2024-11-05" },
            { "capabilities", { { "tools", au::Json::object() } } },
            { "serverInfo", { { "name", "aurora-mcp" }, { "version", AURORA_VERSION_STRING } } },
        };
        return rpc_result(id, result);
    }

    if (method == "notifications/initialized") {
        // notification, no response needed (JSON-RPC notifications have no id, so do not send a response)
        return au::Json{}; // empty means no response
    }

    if (method == "tools/list") {
        return rpc_result(id, au::Json{ { "tools", tool_definitions() } });
    }

    if (method == "tools/call") {
        const std::string tool_name = params.value("name", std::string(""));
        const au::Json args = params.value("arguments", au::Json::object());
        if (tool_name.empty()) {
            return rpc_error(id, -32602, "Missing tool name in params.name");
        }
        const au::Json result = execute_tool(tool_name, args);
        return rpc_result(id, result);
    }

    if (method == "ping") {
        return rpc_result(id, au::Json::object());
    }

    return rpc_error(id, -32601, "Method not found: " + method);
}

} // namespace

// ---------- main ----------

auto main() -> int { // NOLINT(*-exception-escape)
    aurora::serialization::register_core_widgets();

    // MCP stdio main loop
    while (true) {
        au::Json msg = read_message();
        if (msg.is_null() || msg.is_discarded()) {
            break; // EOF or parse failure
        }

        au::Json response = handle_request(msg);
        if (!response.is_null()) {
            write_message(response);
        }
    }

    return 0;
}

// tools/servers/aurora_mcp.cpp
//
// aurora-mcp — Aurora MCP Server (Model Context Protocol, stdio JSON-RPC 2.0).
//
// Provides AI Agents with runtime component discovery, UI-tree validation, offscreen rendering and code generation.
// Transport: stdin/stdout, message format Content-Length: <N>\r\n\r\n<JSON-RPC 2.0 body>.
//
// Usage: aurora_mcp (launched by an AI Agent in stdio mode; no human interaction required)
//
// Exposes 21 MCP Tools:
//   list_components / describe_component / search_components /
//   validate_tree / validate_ui / render_snapshot / render_png / compare_snapshot /
//   generate_ui / build_ui_prompt / repair_tree /
//   live_tree / live_widget_get / live_widget_set / live_patch / live_simulate /
//   to_code / to_yaml / get_schema / simulate_interaction /
//   list_commands / invoke_command
//
// 其中 `live_*` 五个面向**正在运行的应用**（经其 Inspector HTTP 服务，仅回环，端口见
// `AURORA_INSPECTOR_PORT`）；其余均为**离线无状态**——以 `tree` 入参在本进程内临时建树。

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "api_schema.h"
#include "aurora/aurora.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/yaml.h"
#include "code_style.h"
#include "command_listing.h"
#include "inspector_client.h"

// ---------- Known enums (single source of truth: tools/include/known_enums.h) ----------

namespace {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

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
            break;  // empty line ends the header
        }
        if (line.starts_with("Content-Length:")) {
            // Wire-protocol headers come from the peer and may be malformed ("Content-Length: abc").
            // The invalid_argument/out_of_range thrown by std::stoi are not caught here and would terminate the server.
            try {
                content_length = std::stoi(line.substr(15));
            } catch (const std::exception &) {
                return au::Json{};  // treat as protocol error, behave like EOF
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
        return au::Json{};  // treat as protocol error, behave like EOF
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
    return au::Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

/// Build a JSON-RPC 2.0 error response.
[[nodiscard]] auto rpc_error(const au::Json &id, int code, const std::string &message) -> au::Json {
    return au::Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
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
        return false;  // reject absolute paths, drive letters and UNC prefixes
    }
    return std::ranges::all_of(p, [](const auto &part) -> auto { return part != ".."; });
}

// ---------- 运行中应用的会话解析（Track A）----------
//
// 会话发现按「方案 ③」：不做进程注册表，只认
//   ① 工具入参 `session`（"6280" 或 "127.0.0.1:6280"）
//   ② 环境变量 `AURORA_INSPECTOR_PORT`
//   ③ 默认值 6280（与 InspectorServer::start() 的默认端口一致）
// 主机**永远**被 pin 到回环 —— 客户端自身也拒绝非 loopback 目标（见 inspector_client.h）。

struct InspectorSession {
    std::string host{"127.0.0.1"};
    std::uint16_t port{aurora::tools::inspector::kDefaultPort};
};

/// @brief 解析目标会话。解析失败返回空串原因（调用方据此回 isError）。
[[nodiscard]] auto resolve_session(const au::Json &args, std::string &error_out) -> InspectorSession {
    InspectorSession session;

    std::string raw = args.value("session", std::string{});
    if (raw.empty()) {
        // 环境变量兜底：便于本机固定一个非默认端口，省去每次传参。
        if (const char *env = std::getenv("AURORA_INSPECTOR_PORT"); (env != nullptr) && (*env != '\0')) {
            raw = env;
        }
    }

    if (!raw.empty()) {
        std::string port_text = raw;
        const auto colon = raw.find(':');
        if (colon != std::string::npos) {
            const std::string host_part = raw.substr(0, colon);
            if (!aurora::tools::inspector::is_loopback_host(host_part)) {
                error_out = "session host must be loopback (127.0.0.1 / localhost / ::1)";
                return session;
            }
            session.host = host_part;
            port_text = raw.substr(colon + 1);
        }
        try {
            const long parsed = std::stol(port_text);  // NOLINT
            if (parsed <= 0 || parsed > 65535) {
                error_out = "session port out of range: " + port_text;
                return session;
            }
            session.port = static_cast<std::uint16_t>(parsed);
        } catch (const std::exception &) {
            error_out = "session is not a valid port or host:port: " + raw;
            return session;
        }
    }
    return session;
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
    auto num_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p["type"] = "number";
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
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"name"}));
        tools.push_back(std::move(t));
    }
    // search_components
    {
        au::Json props = au::Json::object();
        props["query"] = str_prop("Search keyword");
        au::Json t = au::Json::object();
        t["name"] = "search_components";
        t["description"] = "Fuzzy-search registered components by name substring";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"query"}));
        tools.push_back(std::move(t));
    }
    // validate_tree
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON (to_json output format)");
        au::Json t = au::Json::object();
        t["name"] = "validate_tree";
        t["description"] = "Validate the legality of a UI-tree JSON (type/depth/empty children)";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
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
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
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
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
        tools.push_back(std::move(t));
    }
    // render_png
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON");
        props["width"] = int_prop("Canvas width (default 800)");
        props["height"] = int_prop("Canvas height (default 600)");
        props["path"] = str_prop(
            "Output PNG path; must be a relative path inside the working directory without '..' "
            "(default aurora_render.png)");
        au::Json t = au::Json::object();
        t["name"] = "render_png";
        t["description"] = "Run offscreen rendering on the UI-tree JSON and output a PNG file";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
        tools.push_back(std::move(t));
    }
    // compare_snapshot
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON");
        props["baseline_path"] = str_prop(
            "Golden baseline PNG to compare against; must be a relative path inside the working directory "
            "without '..'");
        props["width"] = int_prop("Canvas width (default 800)");
        props["height"] = int_prop("Canvas height (default 600)");
        props["tolerance"] = int_prop("Per-channel color tolerance 0..255 (default 0)");
        au::Json t = au::Json::object();
        t["name"] = "compare_snapshot";
        t["description"] =
            "Render the UI-tree JSON offscreen and compare it against a golden baseline PNG. Returns a "
            "semantic report: per-pixel statistics plus spatially clustered diff regions, each attributed "
            "to the widget that drew it (widget_path understood by GET/PUT /api/widget/{path}). Use this "
            "instead of eyeballing raw pixel counts when a visual regression fails.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree", "baseline_path"}));
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
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
        tools.push_back(std::move(t));
    }
    // to_yaml
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON object (with type/props/children)");
        au::Json t = au::Json::object();
        t["name"] = "to_yaml";
        t["description"] = "Convert a UI-tree JSON into a YAML-formatted string";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
        tools.push_back(std::move(t));
    }
    // generate_ui（Track B）
    {
        au::Json props = au::Json::object();
        props["description"] = str_prop("Natural-language description, e.g. \"a column with a button and a slider\"");
        au::Json t = au::Json::object();
        t["name"] = "generate_ui";
        t["description"] =
            "Keyword-match natural language to a UI-tree JSON (no LLM involved). Covers every registered "
            "component type; unmatched input falls back to a Text node. For LLM-backed generation, use "
            "build_ui_prompt instead.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"description"}));
        tools.push_back(std::move(t));
    }
    // build_ui_prompt（Track B）
    {
        au::Json props = au::Json::object();
        props["description"] = str_prop("What the end UI should be; used to narrow down the relevant types");
        au::Json t = au::Json::object();
        t["name"] = "build_ui_prompt";
        t["description"] =
            "Project the Aurora schema into a compact prompt for an EXTERNAL LLM. Aurora never calls any "
            "network service itself — hand this text to your own model, then feed the result to repair_tree.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"description"}));
        tools.push_back(std::move(t));
    }
    // repair_tree（Track B）
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON (node object; a {\"node\": ...} wrapper is accepted too)");
        au::Json t = au::Json::object();
        t["name"] = "repair_tree";
        t["description"] =
            "Deterministically repair a UI-tree JSON without any LLM: fix unknown type names, fill missing "
            "props from schema defaults, and drop children on types that declare children_policy=none. "
            "Returns the repaired tree plus the remaining validation errors.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree"}));
        tools.push_back(std::move(t));
    }
    // live_tree（Track A）
    {
        au::Json props = au::Json::object();
        props["session"] =
            str_prop(R"(Optional "6280" or "127.0.0.1:6280"; defaults to env AURORA_INSPECTOR_PORT, then 6280)");
        props["window"] = int_prop("Optional window id; omit for the main window");
        au::Json t = au::Json::object();
        t["name"] = "live_tree";
        t["description"] =
            "Read the LIVE widget tree of a running Aurora application (via its Inspector HTTP server). "
            "Unlike render_snapshot, this is the real UI with real runtime state. Requires the app to have "
            "started an InspectorServer.";
        t["inputSchema"] = schema_obj(std::move(props), au::Json::array());
        tools.push_back(std::move(t));
    }
    // live_widget_get（Track A）
    {
        au::Json props = au::Json::object();
        props["session"] = str_prop(R"(Optional "6280" or "127.0.0.1:6280")");
        props["path"] = str_prop(R"(Widget index path, e.g. "0" or "1/2"; empty string = root)");
        au::Json t = au::Json::object();
        t["name"] = "live_widget_get";
        t["description"] = "Read all properties of one widget in a running application.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"path"}));
        tools.push_back(std::move(t));
    }
    // live_widget_set（Track A）
    {
        au::Json props = au::Json::object();
        props["session"] = str_prop(R"(Optional "6280" or "127.0.0.1:6280")");
        props["path"] = str_prop("Widget index path, e.g. \"1\"; empty string = root");
        props["prop"] = str_prop(R"(Property name, e.g. "content" or "checked")");
        props["value"] = obj_prop("New value as JSON (number, string, boolean, object or array)");
        au::Json t = au::Json::object();
        t["name"] = "live_widget_set";
        t["description"] =
            "Write one property of a widget in a RUNNING application — this is how you edit live UI. "
            "On failure the response carries the reason so you can retry with a corrected value.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"path", "prop", "value"}));
        tools.push_back(std::move(t));
    }
    // live_patch（Track A）
    {
        au::Json props = au::Json::object();
        props["session"] = str_prop(R"(Optional "6280" or "127.0.0.1:6280")");
        props["ops"] =
            obj_prop(R"(JSON array of {"path": "/1/content", "value": <v>} (last path segment is the prop))");
        au::Json t = au::Json::object();
        t["name"] = "live_patch";
        t["description"] =
            "Apply a minimal property patch to a RUNNING application in ONE request. Prefer this over "
            "repeated live_widget_set: fewer round-trips and no whole-tree rebuild. Property-only — "
            "structural add/remove is not expressible and requires replacing the tree.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"ops"}));
        tools.push_back(std::move(t));
    }
    // live_simulate（Track A）
    {
        au::Json props = au::Json::object();
        props["session"] = str_prop(R"(Optional "6280" or "127.0.0.1:6280")");
        props["path"] = str_prop("Target widget index path, e.g. \"0\"");
        props["action"] = str_prop("Interaction to simulate: click | scroll | text");
        props["dx"] = num_prop("Horizontal scroll delta (action=scroll, default 0)");
        props["dy"] = num_prop("Vertical scroll delta (action=scroll, default 0; positive scrolls content up)");
        props["text"] = str_prop("UTF-8 text to insert (action=text)");
        au::Json t = au::Json::object();
        t["name"] = "live_simulate";
        t["description"] =
            "Dispatch a synthetic interaction (click / scroll / text) into a RUNNING application, going "
            "through the real hit-test and dispatch path.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"path", "action"}));
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
    // simulate_interaction
    {
        au::Json props = au::Json::object();
        props["tree"] = obj_prop("UI-tree JSON");
        props["path"] = str_prop(
            "Target widget index path, e.g. \"0/1\" = second child of the first child; empty string targets the tree "
            "root");
        props["action"] = str_prop("Interaction to simulate: click | scroll | text");
        props["dx"] = num_prop("Horizontal scroll delta (action=scroll, default 0)");
        props["dy"] = num_prop("Vertical scroll delta (action=scroll, default 0; positive scrolls content up)");
        props["text"] = str_prop("UTF-8 text to insert (action=text)");
        props["width"] = int_prop("Viewport width used for the layout pass (default 800)");
        props["height"] = int_prop("Viewport height used for the layout pass (default 600)");
        au::Json t = au::Json::object();
        t["name"] = "simulate_interaction";
        t["description"] =
            "Build the UI-tree JSON offscreen, lay it out, dispatch a synthetic interaction (click / scroll / "
            "text input) at the target widget, then return that widget's props and the post-interaction logical "
            "snapshot. This closes the generate -> interact -> assert loop without running an app. Observable "
            "state only: widgets built from JSON carry no user callbacks, so a click is verified through state "
            "changes (e.g. Checkbox.checked, focus movement) rather than a callback side effect. Scroll exposes "
            "no serialized state through this path: Scroll does not serialize its offset, and the containers "
            "that do (LazyList / GridView 'scroll_offset') build their items from a runtime builder that a JSON "
            "tree cannot supply. A successful scroll therefore only means the event reached a hit-testable "
            "target after layout; read the offset itself back in a C++ test. Returns isError when the target "
            "is not found or nothing at its centre is hit-testable (in that case no state is changed).";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"tree", "path", "action"}));
        tools.push_back(std::move(t));
    }
    // list_commands
    {
        au::Json props = au::Json::object();
        props["commands"] = obj_prop(
            "Command descriptors (the {\"commands\":[...]} envelope produced by CommandRegistry::to_json(); "
            "a bare array is also accepted)");
        props["query"] = str_prop("Optional fuzzy query over command titles; empty matches all");
        props["limit"] = int_prop("Maximum number of returned commands (default 50; negative = unlimited)");
        au::Json include_disabled = au::Json::object();
        include_disabled["type"] = "boolean";
        include_disabled["description"] = "Include commands whose enabled flag is false (default false)";
        props["include_disabled"] = std::move(include_disabled);
        au::Json t = au::Json::object();
        t["name"] = "list_commands";
        t["description"] =
            "Filter and rank a host-exported command list (CommandRegistry::to_json()): same fuzzy scoring and "
            "ordering as the in-app command palette, so AI-side discovery matches what a user sees. Stateless: the "
            "descriptors travel with the request, no running app is required.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"commands"}));
        tools.push_back(std::move(t));
    }
    // invoke_command
    {
        au::Json props = au::Json::object();
        props["commands"] = obj_prop(
            "Command descriptors (the {\"commands\":[...]} envelope produced by CommandRegistry::to_json(); "
            "a bare array is also accepted)");
        props["id"] = str_prop("Command identifier to resolve, e.g. \"file.open\"");
        au::Json t = au::Json::object();
        t["name"] = "invoke_command";
        t["description"] =
            "Resolve a command identifier against a host-exported command list and report whether it can be invoked: "
            "status is one of invocable / not-found / disabled / not-invocable. This server is stateless and cannot "
            "run the host process' command action, so it returns the invocation intent - the host performs the actual "
            "call. Never reports success for a command it cannot run.";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({"commands", "id"}));
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

/// @brief 向运行中的应用发一次请求，并把 HTTP 结果翻译成 MCP 工具结果。
///
/// 刻意放在 `text_content` / `json_content` 之后：它俩是工具结果的组装原语，先定义才能被复用
/// （匿名命名空间内不存在跨函数的隐式前向声明）。
[[nodiscard]] auto inspector_result(const InspectorSession &session, std::string_view method, const std::string &target,
                                    const std::string &body = {}) -> au::Json {
    const aurora::tools::inspector::HttpResponse r =
        aurora::tools::inspector::http_request(method, session.host, session.port, target, body);
    if (!r.ok()) {
        return au::Json{{"content", text_content("Error: " + r.error)}, {"isError", true}};
    }
    if (r.status < 200 || r.status >= 300) {
        return au::Json{{"content", text_content("Error: HTTP " + std::to_string(r.status) + " " + r.body)},
                        {"isError", true}};
    }
    // 成功时优先回结构化 JSON，解析不了才回落成纯文本。
    auto parsed = au::Json::parse(r.body, nullptr, false);
    if (parsed.is_discarded()) {
        return au::Json{{"content", json_content(au::Json{{"status", r.status}, {"body", r.body}})}};
    }
    if (parsed.is_object()) {
        parsed["http_status"] = r.status;
    }
    return au::Json{{"content", json_content(parsed)}};
}

/// Execute the named tool and return the MCP tools/call result.
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
        return au::Json{{"content", json_content(arr)}};
    }

    if (name == "describe_component") {
        if (!args.contains("name") || !args["name"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'name' parameter")}, {"isError", true}};
        }
        au::Json schema = aurora::Inspector::component_schema(args["name"].get<std::string>());
        return au::Json{{"content", json_content(schema)}};
    }

    if (name == "search_components") {
        if (!args.contains("query") || !args["query"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'query' parameter")}, {"isError", true}};
        }
        auto results = aurora::search_components(args["query"].get<std::string>());
        au::Json arr = au::Json::array();
        for (const auto &r : results) {
            arr.push_back(r);
        }
        return au::Json{{"content", json_content(arr)}};
    }

    if (name == "validate_ui") {
        // specification/08-tooling.md §7.1: schema static validation (no widget construction, pure JSON against
        // aurora_api schema).
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        const au::Json report = aurora::validate_ui_tree_json(args["tree"]);
        au::Json out{{"content", json_content(report)}};
        if (!report["valid"].get<bool>()) {
            out["isError"] = true;
        }
        return out;
    }

    if (name == "validate_tree") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            auto err = au::Json{{"ok", false}, {"error", widget.error().to_json()}};
            return au::Json{{"content", json_content(err)}, {"isError", true}};
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
            return au::Json{{"content", json_content(err)}, {"isError", true}};
        }
        return au::Json{{"content", json_content(au::Json{{"ok", true}})}};
    }

    if (name == "render_snapshot") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            return au::Json{{"content", text_content("Error: " + widget.error().message)}, {"isError", true}};
        }
        aurora::Node root(std::move(widget.value()));
        au::Json snapshot = render_to_logical_snapshot(root, w, h);
        return au::Json{{"content", json_content(snapshot)}};
    }

    if (name == "render_png") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        std::string path = args.value("path", std::string("aurora_render.png"));
        if (!is_confined_output_path(path)) {
            return au::Json{
                {"content",
                 text_content("Error: 'path' must be a relative path inside the working directory (no '..')")},
                {"isError", true}};
        }
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            return au::Json{{"content", text_content("Error: " + widget.error().message)}, {"isError", true}};
        }
        aurora::Node root(std::move(widget.value()));
        auto ok = render_to_png(root, w, h, path.c_str());
        if (!ok) {
            return au::Json{{"content", text_content("Error: " + ok.error().message)}, {"isError", true}};
        }
        return au::Json{{"content", json_content(au::Json{{"path", path}, {"width", w}, {"height", h}})}};
    }

    // 渲染 树 → 逐像素比对 golden 基线 → 产出「在哪儿 + 是谁画的」的语义报告。
    if (name == "compare_snapshot") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        if (!args.contains("baseline_path") || !args["baseline_path"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'baseline_path' parameter")}, {"isError", true}};
        }
        std::string baseline_path = args["baseline_path"].get<std::string>();
        if (!is_confined_output_path(baseline_path)) {
            return au::Json{
                {"content", text_content("Error: 'baseline_path' must be a relative path inside the working directory "
                                         "(no '..')")},
                {"isError", true}};
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        int tolerance = args.value("tolerance", 0);

        const au::Result<aurora::Image> baseline = aurora::Image::load(baseline_path);
        if (!baseline) {
            return au::Json{{"content", text_content("Error: cannot load baseline: " + baseline.error().message)},
                            {"isError", true}};
        }
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            return au::Json{{"content", text_content("Error: " + widget.error().message)}, {"isError", true}};
        }
        aurora::Node root(std::move(widget.value()));
        const aurora::Image current = render_to_image(root, w, h);

        // 归因需要布局盒；root 经过 render_to_image 后已 mount + layout + 落定几何。
        const std::vector<aurora::WidgetBox> boxes = aurora::collect_widget_boxes(root);
        const aurora::SnapshotDiffReport report =
            aurora::build_snapshot_diff_report(baseline.value(), current, boxes, tolerance);

        au::Json payload = report.to_json();
        payload["summary"] = report.to_text();
        payload["baseline"] = baseline_path;
        return au::Json{{"content", json_content(payload)}};
    }

    // ───────────────────── Track B：NL→UI ─────────────────────

    if (name == "generate_ui") {
        if (!args.contains("description") || !args["description"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'description' parameter")}, {"isError", true}};
        }
        const auto r = aurora::generate_ui(args["description"].get<std::string>());
        if (!r.ok()) {
            return au::Json{{"content", text_content("Error: " + r.error().message)}, {"isError", true}};
        }
        return au::Json{{"content", json_content(r.value())}};
    }

    if (name == "build_ui_prompt") {
        if (!args.contains("description") || !args["description"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'description' parameter")}, {"isError", true}};
        }
        // 只产出文本 —— 本库不发起任何网络请求，调用 LLM 的是调用方。
        const std::string prompt = aurora::ui_prompt_for(args["description"].get<std::string>());
        return au::Json{{"content", text_content(prompt)}};
    }

    if (name == "repair_tree") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        au::Json tree = args["tree"];
        if (tree.contains("node")) {
            tree = tree["node"];  // 兼容 generate_ui 的 {"node": ...} 包装
        }
        const au::Json repaired = aurora::repair_ui_tree(tree);
        const std::vector<aurora::ValidationError> errors = aurora::validate_ui_tree(repaired);

        au::Json out = au::Json::object();
        out["tree"] = repaired;
        out["valid"] = errors.empty();
        au::Json errs = au::Json::array();
        for (const aurora::ValidationError &e : errors) {
            errs.push_back(e.to_json());
        }
        out["errors"] = errs;
        return au::Json{{"content", json_content(out)}};
    }

    // ───────────────────── Track A：运行中的应用 ─────────────────────

    if (name == "live_tree") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return au::Json{{"content", text_content("Error: " + session_error)}, {"isError", true}};
        }
        std::string target = "/api/tree";
        const int window = args.value("window", 0);
        if (window > 0) {
            target += "?window=" + std::to_string(window);
        }
        return inspector_result(session, "GET", target);
    }

    if (name == "live_widget_get") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return au::Json{{"content", text_content("Error: " + session_error)}, {"isError", true}};
        }
        if (!args.contains("path") || !args["path"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'path' parameter")}, {"isError", true}};
        }
        return inspector_result(session, "GET", "/api/widget/" + args["path"].get<std::string>());
    }

    if (name == "live_widget_set") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return au::Json{{"content", text_content("Error: " + session_error)}, {"isError", true}};
        }
        for (const char *key : {"path", "prop"}) {
            if (!args.contains(key) || !args[key].is_string()) {
                return au::Json{{"content", text_content(std::string("Error: missing '") + key + "' parameter")},
                                {"isError", true}};
            }
        }
        if (!args.contains("value")) {
            return au::Json{{"content", text_content("Error: missing 'value' parameter")}, {"isError", true}};
        }
        const std::string path = args["path"].get<std::string>();
        const std::string prop = args["prop"].get<std::string>();
        // REST 约定：/api/widget/{tree_path}/{prop_name}；根节点的树路径为空。
        const std::string target = "/api/widget/" + (path.empty() ? std::string{} : path + "/") + prop;
        return inspector_result(session, "PUT", target, args["value"].dump());
    }

    if (name == "live_patch") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return au::Json{{"content", text_content("Error: " + session_error)}, {"isError", true}};
        }
        if (!args.contains("ops") || !args["ops"].is_array()) {
            return au::Json{{"content", text_content("Error: 'ops' must be a JSON array")}, {"isError", true}};
        }
        return inspector_result(session, "POST", "/api/patch", args["ops"].dump());
    }

    if (name == "live_simulate") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return au::Json{{"content", text_content("Error: " + session_error)}, {"isError", true}};
        }
        if (!args.contains("path") || !args["path"].is_string() || !args.contains("action") ||
            !args["action"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'path' or 'action' parameter")},
                            {"isError", true}};
        }
        const std::string action = args["action"].get<std::string>();
        if (action != "click" && action != "scroll" && action != "text") {
            return au::Json{{"content", text_content("Error: action must be click | scroll | text")},
                            {"isError", true}};
        }
        au::Json body = au::Json::object();
        body["path"] = args["path"].get<std::string>();
        if (action == "scroll") {
            body["dx"] = args.value("dx", 0.0F);
            body["dy"] = args.value("dy", 0.0F);
        }
        if (action == "text") {
            body["text"] = args.value("text", std::string{});
        }
        return inspector_result(session, "POST", "/api/input/" + action, body.dump());
    }

    if (name == "to_code") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        std::string style_str = args.value("style", std::string("fluent"));
        auto style = aurora::tools::parse_code_style(style_str);

        std::string code = to_code(args["tree"], style);
        return au::Json{{"content", text_content(code)}};
    }

    if (name == "to_yaml") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        std::string yaml = aurora::serialization::to_yaml(args["tree"]);
        return au::Json{{"content", text_content(yaml)}};
    }

    if (name == "get_schema") {
        au::Json api = aurora::tools::build_api_skeleton();
        return au::Json{{"content", json_content(api)}};
    }

    if (name == "simulate_interaction") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return au::Json{{"content", text_content("Error: missing 'tree' parameter")}, {"isError", true}};
        }
        if (!args.contains("path") || !args["path"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'path' parameter")}, {"isError", true}};
        }
        if (!args.contains("action") || !args["action"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'action' parameter")}, {"isError", true}};
        }
        const std::string path = args["path"].get<std::string>();
        const std::string action = args["action"].get<std::string>();
        if (action != "click" && action != "scroll" && action != "text") {
            return au::Json{{"content", text_content("Error: 'action' must be one of click | scroll | text")},
                            {"isError", true}};
        }
        // 参数判型前置：nlohmann 的 get<>/value<> 遇类型不符会抛 type_error，而本进程主循环
        // 不捕获异常（stdio 服务直接终止），故所有取值前先判型，类型错了回 isError。
        for (const char *key : {"dx", "dy", "width", "height"}) {
            if (const auto it = args.find(key); it != args.end() && !it->is_number()) {
                return au::Json{{"content", text_content(std::string("Error: '") + key + "' must be a number")},
                                {"isError", true}};
            }
        }
        if (const auto it = args.find("text"); it != args.end() && !it->is_string()) {
            return au::Json{{"content", text_content("Error: 'text' must be a string")}, {"isError", true}};
        }

        const int width = args.value("width", 800);
        const int height = args.value("height", 600);
        auto widget = aurora::serialization::from_json(args["tree"]);
        if (!widget) {
            return au::Json{{"content", text_content("Error: " + widget.error().message)}, {"isError", true}};
        }
        aurora::Node root(std::move(widget.value()));
        // 先布局一次：from_json 只构树不布局，而 simulate_* 要求目标控件有可命中区域
        // （滚动容器还须真正布局出非零视口）。此处复用无头快照的 mount + layout 路径，
        // 其结果仅用于确立几何，不回传（回传的是交互之后的那份）。
        (void)render_to_logical_snapshot(root, width, height);

        aurora::Node target = aurora::Inspector::find_node(root, path);
        if (!target) {
            return au::Json{{"content", text_content("Error: widget not found at path '" + path + "'")},
                            {"isError", true}};
        }

        std::string failure;
        if (action == "click") {
            const aurora::Result<void> r = aurora::Inspector::simulate_click(target.widget());
            failure = r ? std::string{} : r.error().message;
        } else if (action == "scroll") {
            const aurora::Result<void> r =
                aurora::Inspector::simulate_scroll(target.widget(), args.value("dx", 0.0F), args.value("dy", 0.0F));
            failure = r ? std::string{} : r.error().message;
        } else {
            const aurora::Result<void> r =
                aurora::Inspector::simulate_text_input(target.widget(), args.value("text", std::string("")));
            failure = r ? std::string{} : r.error().message;
        }
        if (!failure.empty()) {
            return au::Json{{"content", text_content("Error: " + failure)}, {"isError", true}};
        }

        au::Json out = au::Json::object();
        out["action"] = action;
        out["path"] = path;
        out["target"] = aurora::Inspector::get_prop(target.widget());
        out["snapshot"] = render_to_logical_snapshot(root, width, height);
        return au::Json{{"content", json_content(out)}};
    }

    if (name == "list_commands") {
        if (!args.contains("commands")) {
            return au::Json{{"content", text_content("Error: missing 'commands' parameter")}, {"isError", true}};
        }
        const au::Json *items = aurora::tools::command_descriptors(args["commands"]);
        if (items == nullptr) {
            return au::Json{
                {"content", text_content("Error: 'commands' must be an array or the {\"commands\": [...]} envelope")},
                {"isError", true}};
        }
        std::string query;
        if (const auto it = args.find("query"); it != args.end()) {
            if (!it->is_string()) {
                return au::Json{{"content", text_content("Error: 'query' must be a string")}, {"isError", true}};
            }
            query = it->get<std::string>();
        }
        int limit = 50;
        if (const auto it = args.find("limit"); it != args.end()) {
            if (!it->is_number_integer()) {
                return au::Json{{"content", text_content("Error: 'limit' must be an integer")}, {"isError", true}};
            }
            limit = it->get<int>();
        }
        bool include_disabled = false;
        if (const auto it = args.find("include_disabled"); it != args.end()) {
            if (!it->is_boolean()) {
                return au::Json{{"content", text_content("Error: 'include_disabled' must be a boolean")},
                                {"isError", true}};
            }
            include_disabled = it->get<bool>();
        }

        const aurora::tools::CommandListing listing = aurora::tools::list_commands(*items, query, include_disabled);
        au::Json listed = au::Json::array();
        int count = 0;
        for (const std::size_t index : listing.indices) {
            if (limit >= 0 && count >= limit) {
                break;
            }
            listed.push_back((*items)[index]);
            ++count;
        }
        au::Json out = au::Json::object();
        out["count"] = count;
        out["matched"] = static_cast<int>(listing.indices.size());
        out["considered"] = static_cast<int>(listing.considered);
        out["commands"] = std::move(listed);
        return au::Json{{"content", json_content(out)}};
    }

    if (name == "invoke_command") {
        if (!args.contains("commands")) {
            return au::Json{{"content", text_content("Error: missing 'commands' parameter")}, {"isError", true}};
        }
        if (!args.contains("id") || !args["id"].is_string()) {
            return au::Json{{"content", text_content("Error: missing 'id' parameter")}, {"isError", true}};
        }
        const au::Json *items = aurora::tools::command_descriptors(args["commands"]);
        if (items == nullptr) {
            return au::Json{
                {"content", text_content("Error: 'commands' must be an array or the {\"commands\": [...]} envelope")},
                {"isError", true}};
        }
        const std::string id = args["id"].get<std::string>();
        aurora::tools::CommandStatus status = aurora::tools::CommandStatus::NotFound;
        const au::Json *found = aurora::tools::resolve_command(*items, id, status);

        au::Json out = au::Json::object();
        out["id"] = id;
        out["resolved"] = found != nullptr;
        out["enabled"] = found != nullptr && aurora::tools::command_bool_field(*found, "enabled", true);
        out["invocable"] = found != nullptr && aurora::tools::command_bool_field(*found, "invocable", false);
        out["status"] = aurora::tools::command_status_name(status);
        if (found != nullptr) {
            if (const auto it = found->find("title"); it != found->end() && it->is_string()) {
                out["title"] = it->get<std::string>();
            }
            if (const auto it = found->find("when"); it != found->end() && it->is_string()) {
                out["when"] = it->get<std::string>();
            }
        }
        out["note"] = "Resolution only; the host performs the actual call.";
        return au::Json{{"content", json_content(out)}};
    }

    // unknown tool
    return au::Json{{"content", text_content("Error: unknown tool '" + name + "'")}, {"isError", true}};
}

// ---------- JSON-RPC dispatch ----------

[[nodiscard]] auto handle_request(const au::Json &req) -> au::Json {
    const au::Json id = req.contains("id") ? req["id"] : au::Json(nullptr);
    const std::string method = req.value("method", std::string(""));
    const au::Json params = req.value("params", au::Json::object());

    if (method == "initialize") {
        const auto result = au::Json{
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {{"tools", au::Json::object()}}},
            {"serverInfo", {{"name", "aurora-mcp"}, {"version", AURORA_VERSION_STRING}}},
        };
        return rpc_result(id, result);
    }

    if (method == "notifications/initialized") {
        // notification, no response needed (JSON-RPC notifications have no id, so do not send a response)
        return au::Json{};  // empty means no response
    }

    if (method == "tools/list") {
        return rpc_result(id, au::Json{{"tools", tool_definitions()}});
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

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace

// ---------- main ----------

auto main() -> int {  // NOLINT(*-exception-escape)
    aurora::serialization::register_core_widgets();

    // MCP stdio main loop
    while (true) {
        au::Json msg = read_message();
        if (msg.is_null() || msg.is_discarded()) {
            break;  // EOF or parse failure
        }

        au::Json response = handle_request(msg);
        if (!response.is_null()) {
            write_message(response);
        }
    }

    return 0;
}
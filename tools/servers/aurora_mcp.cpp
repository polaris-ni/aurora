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

    const auto parsed = au::json::parse(body);
    return parsed ? parsed.value() : au::Json{};
}

/// Write one MCP message to stdout (stdio wire frame; must use the prefix-free raw channel to keep the Content-Length
/// header byte-exact).
auto write_message(const au::Json &msg) -> void {
    const auto dumped = au::json::dump(msg);
    std::string body = dumped.ok() ? dumped.value() : std::string{};
    AURORA_LOG_RAW("mcp", "Content-Length: ", body.size(), "\r\n\r\n", body);
}

/// Build a JSON-RPC 2.0 success response.
[[nodiscard]] auto rpc_result(const au::Json &id, const au::Json &result) -> au::Json {
    au::Json r = au::Json::object();
    r.set("jsonrpc", "2.0");
    r.set("id", id);
    r.set("result", result);
    return r;
}

/// Build a JSON-RPC 2.0 error response.
[[nodiscard]] auto rpc_error(const au::Json &id, int code, const std::string &message) -> au::Json {
    au::Json err = au::Json::object();
    err.set("code", code);
    err.set("message", message);
    au::Json r = au::Json::object();
    r.set("jsonrpc", "2.0");
    r.set("id", id);
    r.set("error", std::move(err));
    return r;
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
    std::uint16_t port{aurora::tools::inspector::AURORA_DEFAULT_PORT};
};

/// @brief 解析目标会话。解析失败返回空串原因（调用方据此回 isError）。
[[nodiscard]] auto resolve_session(const au::Json &args, std::string &error_out) -> InspectorSession {
    InspectorSession session;

    auto raw = args.as_or<std::string>("session", std::string{});
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
        s.set("type", "object");
        s.set("properties", std::move(props));
        if (!req.empty()) {
            s.set("required", std::move(req));
        }
        return s;
    };
    auto str_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p.set("type", "string");
        p.set("description", desc);
        return p;
    };
    auto int_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p.set("type", "integer");
        p.set("description", desc);
        return p;
    };
    auto num_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p.set("type", "number");
        p.set("description", desc);
        return p;
    };
    auto obj_prop = [](const char *desc) -> au::Json {
        au::Json p = au::Json::object();
        p.set("type", "object");
        p.set("description", desc);
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
        t.set("name", "list_components");
        t.set("description", "List all registered Aurora component type names");
        t.set("inputSchema", schema_obj(au::Json::object(), au::Json::array()));
        tools.push_back(std::move(t));
    }
    // describe_component
    {
        au::Json props = au::Json::object();
        props.set("name", str_prop("Component type name, e.g. Button"));
        au::Json t = au::Json::object();
        t.set("name", "describe_component");
        t.set("description", "Return the full schema of a single component (props/events/children policy/examples)");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"name"})));
        tools.push_back(std::move(t));
    }
    // search_components
    {
        au::Json props = au::Json::object();
        props.set("query", str_prop("Search keyword"));
        au::Json t = au::Json::object();
        t.set("name", "search_components");
        t.set("description", "Fuzzy-search registered components by name substring");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"query"})));
        tools.push_back(std::move(t));
    }
    // validate_tree
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON (to_json output format)"));
        au::Json t = au::Json::object();
        t.set("name", "validate_tree");
        t.set("description", "Validate the legality of a UI-tree JSON (type/depth/empty children)");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // validate_ui (specification/08-tooling.md §7.1: schema static validation, structured errors with path/suggestions)
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON (to_json output format)"));
        au::Json t = au::Json::object();
        t.set("name", "validate_ui");
        t.set("description",
              "Statically validate the UI tree against aurora_api.json schema: unknown type / missing required "
              "prop / type mismatch / children policy; errors include JSON path and fix suggestions (for AI auto-fix)");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // render_snapshot
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON"));
        props.set("width", int_prop("Viewport width (default 800)"));
        props.set("height", int_prop("Viewport height (default 600)"));
        au::Json t = au::Json::object();
        t.set("name", "render_snapshot");
        t.set("description",
              "Run offscreen layout on the UI-tree JSON and return a logical snapshot (type + box + children)");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // render_png
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON"));
        props.set("width", int_prop("Canvas width (default 800)"));
        props.set("height", int_prop("Canvas height (default 600)"));
        props.set("path", str_prop("Output PNG path; must be a relative path inside the working directory without '..' "
                                   "(default aurora_render.png)"));
        au::Json t = au::Json::object();
        t.set("name", "render_png");
        t.set("description", "Run offscreen rendering on the UI-tree JSON and output a PNG file");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // compare_snapshot
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON"));
        props.set(
            "baseline_path",
            str_prop("Golden baseline PNG to compare against; must be a relative path inside the working directory "
                     "without '..'"));
        props.set("width", int_prop("Canvas width (default 800)"));
        props.set("height", int_prop("Canvas height (default 600)"));
        props.set("tolerance", int_prop("Per-channel color tolerance 0..255 (default 0)"));
        au::Json t = au::Json::object();
        t.set("name", "compare_snapshot");
        t.set("description",
              "Render the UI-tree JSON offscreen and compare it against a golden baseline PNG. Returns a "
              "semantic report: per-pixel statistics plus spatially clustered diff regions, each attributed "
              "to the widget that drew it (widget_path understood by GET/PUT /api/widget/{path}). Use this "
              "instead of eyeballing raw pixel counts when a visual regression fails.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree", "baseline_path"})));
        tools.push_back(std::move(t));
    }
    // to_code
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON"));
        props.set("style", str_prop("Code style: fluent | step | di (default fluent)"));
        au::Json t = au::Json::object();
        t.set("name", "to_code");
        t.set("description", "Convert a UI-tree JSON into compilable C++ code");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // to_yaml
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON object (with type/props/children)"));
        au::Json t = au::Json::object();
        t.set("name", "to_yaml");
        t.set("description", "Convert a UI-tree JSON into a YAML-formatted string");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // generate_ui（Track B）
    {
        au::Json props = au::Json::object();
        props.set("description",
                  str_prop("Natural-language description, e.g. \"a column with a button and a slider\""));
        au::Json t = au::Json::object();
        t.set("name", "generate_ui");
        t.set("description",
              "Keyword-match natural language to a UI-tree JSON (no LLM involved). Covers every registered "
              "component type; unmatched input falls back to a Text node. For LLM-backed generation, use "
              "build_ui_prompt instead.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"description"})));
        tools.push_back(std::move(t));
    }
    // build_ui_prompt（Track B）
    {
        au::Json props = au::Json::object();
        props.set("description", str_prop("What the end UI should be; used to narrow down the relevant types"));
        au::Json t = au::Json::object();
        t.set("name", "build_ui_prompt");
        t.set("description",
              "Project the Aurora schema into a compact prompt for an EXTERNAL LLM. Aurora never calls any "
              "network service itself — hand this text to your own model, then feed the result to repair_tree.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"description"})));
        tools.push_back(std::move(t));
    }
    // repair_tree（Track B）
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON (node object; a {\"node\": ...} wrapper is accepted too)"));
        au::Json t = au::Json::object();
        t.set("name", "repair_tree");
        t.set("description",
              "Deterministically repair a UI-tree JSON without any LLM: fix unknown type names, fill missing "
              "props from schema defaults, and drop children on types that declare children_policy=none. "
              "Returns the repaired tree plus the remaining validation errors.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree"})));
        tools.push_back(std::move(t));
    }
    // live_tree（Track A）
    {
        au::Json props = au::Json::object();
        props.set("session",
                  str_prop(R"(Optional "6280" or "127.0.0.1:6280"; defaults to env AURORA_INSPECTOR_PORT, then 6280)"));
        props.set("window", int_prop("Optional window id; omit for the main window"));
        au::Json t = au::Json::object();
        t.set("name", "live_tree");
        t.set("description",
              "Read the LIVE widget tree of a running Aurora application (via its Inspector HTTP server). "
              "Unlike render_snapshot, this is the real UI with real runtime state. Requires the app to have "
              "started an InspectorServer.");
        t.set("inputSchema", schema_obj(std::move(props), au::Json::array()));
        tools.push_back(std::move(t));
    }
    // live_widget_get（Track A）
    {
        au::Json props = au::Json::object();
        props.set("session", str_prop(R"(Optional "6280" or "127.0.0.1:6280")"));
        props.set("path", str_prop(R"(Widget index path, e.g. "0" or "1/2"; empty string = root)"));
        au::Json t = au::Json::object();
        t.set("name", "live_widget_get");
        t.set("description", "Read all properties of one widget in a running application.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"path"})));
        tools.push_back(std::move(t));
    }
    // live_widget_set（Track A）
    {
        au::Json props = au::Json::object();
        props.set("session", str_prop(R"(Optional "6280" or "127.0.0.1:6280")"));
        props.set("path", str_prop("Widget index path, e.g. \"1\"; empty string = root"));
        props.set("prop", str_prop(R"(Property name, e.g. "content" or "checked")"));
        props.set("value", obj_prop("New value as JSON (number, string, boolean, object or array)"));
        au::Json t = au::Json::object();
        t.set("name", "live_widget_set");
        t.set("description",
              "Write one property of a widget in a RUNNING application — this is how you edit live UI. "
              "On failure the response carries the reason so you can retry with a corrected value.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"path", "prop", "value"})));
        tools.push_back(std::move(t));
    }
    // live_patch（Track A）
    {
        au::Json props = au::Json::object();
        props.set("session", str_prop(R"(Optional "6280" or "127.0.0.1:6280")"));
        props.set("ops",
                  obj_prop(R"(JSON array of {"path": "/1/content", "value": <v>} (last path segment is the prop))"));
        au::Json t = au::Json::object();
        t.set("name", "live_patch");
        t.set("description",
              "Apply a minimal property patch to a RUNNING application in ONE request. Prefer this over "
              "repeated live_widget_set: fewer round-trips and no whole-tree rebuild. Property-only — "
              "structural add/remove is not expressible and requires replacing the tree.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"ops"})));
        tools.push_back(std::move(t));
    }
    // live_simulate（Track A）
    {
        au::Json props = au::Json::object();
        props.set("session", str_prop(R"(Optional "6280" or "127.0.0.1:6280")"));
        props.set("path", str_prop("Target widget index path, e.g. \"0\""));
        props.set("action", str_prop("Interaction to simulate: click | scroll | text"));
        props.set("dx", num_prop("Horizontal scroll delta (action=scroll, default 0)"));
        props.set("dy", num_prop("Vertical scroll delta (action=scroll, default 0; positive scrolls content up)"));
        props.set("text", str_prop("UTF-8 text to insert (action=text)"));
        au::Json t = au::Json::object();
        t.set("name", "live_simulate");
        t.set("description",
              "Dispatch a synthetic interaction (click / scroll / text) into a RUNNING application, going "
              "through the real hit-test and dispatch path.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"path", "action"})));
        tools.push_back(std::move(t));
    }
    // get_schema
    {
        au::Json t = au::Json::object();
        t.set("name", "get_schema");
        t.set("description", "Return the full Aurora API schema (all components + enums)");
        t.set("inputSchema", schema_obj(au::Json::object(), au::Json::array()));
        tools.push_back(std::move(t));
    }
    // simulate_interaction
    {
        au::Json props = au::Json::object();
        props.set("tree", obj_prop("UI-tree JSON"));
        props.set("path", str_prop("Target widget index path, e.g. \"0/1\" = second child of the first child; empty "
                                   "string targets the tree "
                                   "root"));
        props.set("action", str_prop("Interaction to simulate: click | scroll | text"));
        props.set("dx", num_prop("Horizontal scroll delta (action=scroll, default 0)"));
        props.set("dy", num_prop("Vertical scroll delta (action=scroll, default 0; positive scrolls content up)"));
        props.set("text", str_prop("UTF-8 text to insert (action=text)"));
        props.set("width", int_prop("Viewport width used for the layout pass (default 800)"));
        props.set("height", int_prop("Viewport height used for the layout pass (default 600)"));
        au::Json t = au::Json::object();
        t.set("name", "simulate_interaction");
        t.set("description",
              "Build the UI-tree JSON offscreen, lay it out, dispatch a synthetic interaction (click / scroll / "
              "text input) at the target widget, then return that widget's props and the post-interaction logical "
              "snapshot. This closes the generate -> interact -> assert loop without running an app. Observable "
              "state only: widgets built from JSON carry no user callbacks, so a click is verified through state "
              "changes (e.g. Checkbox.checked, focus movement) rather than a callback side effect. Scroll exposes "
              "no serialized state through this path: Scroll does not serialize its offset, and the containers "
              "that do (LazyList / GridView 'scroll_offset') build their items from a runtime builder that a JSON "
              "tree cannot supply. A successful scroll therefore only means the event reached a hit-testable "
              "target after layout; read the offset itself back in a C++ test. Returns isError when the target "
              "is not found or nothing at its centre is hit-testable (in that case no state is changed).");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"tree", "path", "action"})));
        tools.push_back(std::move(t));
    }
    // list_commands
    {
        au::Json props = au::Json::object();
        props.set(
            "commands",
            obj_prop("Command descriptors (the {\"commands\":[...]} envelope produced by CommandRegistry::to_json(); "
                     "a bare array is also accepted)"));
        props.set("query", str_prop("Optional fuzzy query over command titles; empty matches all"));
        props.set("limit", int_prop("Maximum number of returned commands (default 50; negative = unlimited)"));
        au::Json include_disabled = au::Json::object();
        include_disabled.set("type", "boolean");
        include_disabled.set("description", "Include commands whose enabled flag is false (default false)");
        props.set("include_disabled", std::move(include_disabled));
        au::Json t = au::Json::object();
        t.set("name", "list_commands");
        t.set("description",
              "Filter and rank a host-exported command list (CommandRegistry::to_json()): same fuzzy scoring and "
              "ordering as the in-app command palette, so AI-side discovery matches what a user sees. Stateless: the "
              "descriptors travel with the request, no running app is required.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"commands"})));
        tools.push_back(std::move(t));
    }
    // invoke_command
    {
        au::Json props = au::Json::object();
        props.set(
            "commands",
            obj_prop("Command descriptors (the {\"commands\":[...]} envelope produced by CommandRegistry::to_json(); "
                     "a bare array is also accepted)"));
        props.set("id", str_prop("Command identifier to resolve, e.g. \"file.open\""));
        au::Json t = au::Json::object();
        t.set("name", "invoke_command");
        t.set(
            "description",
            "Resolve a command identifier against a host-exported command list and report whether it can be invoked: "
            "status is one of invocable / not-found / disabled / not-invocable. This server is stateless and cannot "
            "run the host process' command action, so it returns the invocation intent - the host performs the actual "
            "call. Never reports success for a command it cannot run.");
        t.set("inputSchema", schema_obj(std::move(props), req_arr({"commands", "id"})));
        tools.push_back(std::move(t));
    }

    return tools;
}

// ---------- MCP tool execution ----------

/// Build the content array (text type) for an MCP tools/call.
[[nodiscard]] auto text_content(const std::string &text) -> au::Json {
    au::Json item = au::Json::object();
    item.set("type", "text");
    item.set("text", text);
    au::Json arr = au::Json::array();
    arr.push_back(std::move(item));
    return arr;
}

/// @brief 组装 MCP 工具结果信封：{"content": [...]}，is_error 时追加 {"isError": true}。
[[nodiscard]] auto tool_result(au::Json content, const bool is_error = false) -> au::Json {
    au::Json r = au::Json::object();
    r.set("content", std::move(content));
    if (is_error) {
        r.set("isError", au::Json{true});
    }
    return r;
}

[[nodiscard]] auto json_content(const au::Json &j) -> au::Json {
    const auto dumped = au::json::dump(j, {.indent = 2});
    return text_content(dumped.ok() ? dumped.value() : std::string{});
}

/// @brief 向运行中的应用发一次请求，并把 HTTP 结果翻译成 MCP 工具结果。
///
/// 刻意放在 `text_content` / `json_content` 之后：它俩是工具结果的组装原语，先定义才能被复用
/// （匿名命名空间内不存在跨函数的隐式前向声明）。
[[nodiscard]] auto inspector_result(const InspectorSession &session, std::string_view method, const std::string &target,
                                    const std::string &body = {}) -> au::Json {
    const aurora::tools::inspector::HttpResponse r =
        aurora::tools::inspector::http_request(method, session.host, session.port, target, body);
    if (!r.ok()) {
        return tool_result(text_content("Error: " + r.error), true);
    }
    if (r.status < 200 || r.status >= 300) {
        return tool_result(text_content("Error: HTTP " + std::to_string(r.status) + " " + r.body), true);
    }
    // 成功时优先回结构化 JSON，解析不了才回落成纯文本。
    au::Json payload;
    if (auto parsed = au::json::parse(r.body); parsed) {
        payload = std::move(parsed.value());
    } else {
        au::Json fallback = au::Json::object();
        fallback.set("status", r.status);
        fallback.set("body", r.body);
        return tool_result(json_content(fallback));
    }
    if (payload.is_object()) {
        payload.set("http_status", r.status);
    }
    return tool_result(json_content(payload));
}

/// Execute the named tool and return the MCP tools/call result.
[[nodiscard]] auto execute_tool(const std::string &name, const au::Json &args) -> au::Json {
    if (name == "list_components") {
        // Use the Inspector facade to fetch all component schemas and extract type names
        auto schemas = aurora::Inspector::components();
        au::Json arr = au::Json::array();
        for (const auto &s : schemas) {
            if (const auto *type = s.at("type"); type != nullptr) {
                arr.push_back(*type);
            }
        }
        return tool_result(json_content(arr));
    }

    if (name == "describe_component") {
        const auto *name_v = args.at("name");
        if (name_v == nullptr || !name_v->is_string()) {
            return tool_result(text_content("Error: missing 'name' parameter"), true);
        }
        au::Json schema = aurora::Inspector::component_schema(name_v->as_or<std::string>(""));
        return tool_result(json_content(schema));
    }

    if (name == "search_components") {
        const auto *query_v = args.at("query");
        if (query_v == nullptr || !query_v->is_string()) {
            return tool_result(text_content("Error: missing 'query' parameter"), true);
        }
        auto results = aurora::search_components(query_v->as_or<std::string>(""));
        au::Json arr = au::Json::array();
        for (const auto &r : results) {
            arr.push_back(r);
        }
        return tool_result(json_content(arr));
    }

    if (name == "validate_ui") {
        // specification/08-tooling.md §7.1: schema static validation (no widget construction, pure JSON against
        // aurora_api schema).
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        const au::Json report = aurora::validate_ui_tree_json(*tree_v);
        au::Json out = au::Json::object();
        out.set("content", json_content(report));
        if (!report.at("valid")->as_or<bool>(false)) {
            out.set("isError", au::Json{true});
        }
        return out;
    }

    if (name == "validate_tree") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        auto widget = aurora::serialization::from_json(*tree_v);
        if (!widget) {
            au::Json err = au::Json::object();
            err.set("ok", au::Json{false});
            err.set("error", widget.error().to_json());
            return tool_result(json_content(err), true);
        }
        aurora::Node root(std::move(widget.value()));
        auto diags = aurora::Inspector::validate(root);
        if (!diags.empty()) {
            au::Json err = au::Json::object();
            err.set("ok", au::Json{false});
            au::Json diag_arr = au::Json::array();
            for (const auto &d : diags) {
                if (auto parsed = au::json::parse(d.to_json_line()); parsed) {
                    diag_arr.push_back(std::move(parsed.value()));
                } else {
                    diag_arr.push_back(au::Json{});
                }
            }
            err.set("diagnostics", diag_arr);
            return tool_result(json_content(err), true);
        }
        au::Json ok_out = au::Json::object();
        ok_out.set("ok", au::Json{true});
        return tool_result(json_content(ok_out));
    }

    if (name == "render_snapshot") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        int w = args.as_or<int>("width", 800);
        int h = args.as_or<int>("height", 600);
        auto widget = aurora::serialization::from_json(*tree_v);
        if (!widget) {
            return tool_result(text_content("Error: " + widget.error().message), true);
        }
        aurora::Node root(std::move(widget.value()));
        au::Json snapshot = render_to_logical_snapshot(root, w, h);
        return tool_result(json_content(snapshot));
    }

    if (name == "render_png") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        int w = args.as_or<int>("width", 800);
        int h = args.as_or<int>("height", 600);
        auto path = args.as_or<std::string>("path", std::string("aurora_render.png"));
        if (!is_confined_output_path(path)) {
            return tool_result(
                text_content("Error: 'path' must be a relative path inside the working directory (no '..')"), true);
        }
        auto widget = aurora::serialization::from_json(*tree_v);
        if (!widget) {
            return tool_result(text_content("Error: " + widget.error().message), true);
        }
        aurora::Node root(std::move(widget.value()));
        auto ok = render_to_png(root, w, h, path.c_str());
        if (!ok) {
            return tool_result(text_content("Error: " + ok.error().message), true);
        }
        au::Json info = au::Json::object();
        info.set("path", path);
        info.set("width", w);
        info.set("height", h);
        return tool_result(json_content(info));
    }

    // 渲染 树 → 逐像素比对 golden 基线 → 产出「在哪儿 + 是谁画的」的语义报告。
    if (name == "compare_snapshot") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        const auto *baseline_v = args.at("baseline_path");
        if (baseline_v == nullptr || !baseline_v->is_string()) {
            return tool_result(text_content("Error: missing 'baseline_path' parameter"), true);
        }
        auto baseline_path = baseline_v->as_or<std::string>("");
        if (!is_confined_output_path(baseline_path)) {
            return tool_result(text_content("Error: 'baseline_path' must be a relative path inside the working "
                                            "directory (no '..')"),
                               true);
        }
        int w = args.as_or<int>("width", 800);
        int h = args.as_or<int>("height", 600);
        int tolerance = args.as_or<int>("tolerance", 0);

        const au::Result<aurora::Image> baseline = aurora::Image::load(baseline_path);
        if (!baseline) {
            return tool_result(text_content("Error: cannot load baseline: " + baseline.error().message), true);
        }
        auto widget = aurora::serialization::from_json(*tree_v);
        if (!widget) {
            return tool_result(text_content("Error: " + widget.error().message), true);
        }
        aurora::Node root(std::move(widget.value()));
        const aurora::Image current = render_to_image(root, w, h);

        // 归因需要布局盒；root 经过 render_to_image 后已 mount + layout + 落定几何。
        const std::vector<aurora::WidgetBox> boxes = aurora::collect_widget_boxes(root);
        const aurora::SnapshotDiffReport report =
            aurora::build_snapshot_diff_report(baseline.value(), current, boxes, tolerance);

        // 信封的三个字段直接在新容器上拼好后序列化：`json_content` 的终点是文本，
        // 无需再回到 MCP 层所用的 JSON 库做一次解析往返。
        auto payload = report.to_json();
        payload.set("summary", report.to_text());
        payload.set("baseline", baseline_path);
        const auto text = aurora::json::dump(payload, {.indent = 2});
        return tool_result(text_content(text.ok() ? text.value() : std::string{}));
    }

    // ───────────────────── Track B：NL→UI ─────────────────────

    if (name == "generate_ui") {
        const auto *description_v = args.at("description");
        if (description_v == nullptr || !description_v->is_string()) {
            return tool_result(text_content("Error: missing 'description' parameter"), true);
        }
        const auto r = aurora::generate_ui(description_v->as_or<std::string>(""));
        if (!r.ok()) {
            return tool_result(text_content("Error: " + r.error().message), true);
        }
        return tool_result(json_content(r.value()));
    }

    if (name == "build_ui_prompt") {
        const auto *description_v = args.at("description");
        if (description_v == nullptr || !description_v->is_string()) {
            return tool_result(text_content("Error: missing 'description' parameter"), true);
        }
        // 只产出文本 —— 本库不发起任何网络请求，调用 LLM 的是调用方。
        const std::string prompt = aurora::ui_prompt_for(description_v->as_or<std::string>(""));
        return tool_result(text_content(prompt));
    }

    if (name == "repair_tree") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        au::Json tree = *tree_v;
        if (const auto *node_v = tree.at("node"); node_v != nullptr) {
            tree = *node_v;  // 兼容 generate_ui 的 {"node": ...} 包装
        }
        const au::Json repaired = aurora::repair_ui_tree(tree);
        const std::vector<aurora::ValidationError> errors = aurora::validate_ui_tree(repaired);

        au::Json out = au::Json::object();
        out.set("tree", repaired);
        out.set("valid", au::Json{errors.empty()});
        au::Json errs = au::Json::array();
        for (const aurora::ValidationError &e : errors) {
            errs.push_back(e.to_json());
        }
        out.set("errors", errs);
        return tool_result(json_content(out));
    }

    // ───────────────────── Track A：运行中的应用 ─────────────────────

    if (name == "live_tree") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return tool_result(text_content("Error: " + session_error), true);
        }
        std::string target = "/api/tree";
        const int window = args.as_or<int>("window", 0);
        if (window > 0) {
            target += "?window=" + std::to_string(window);
        }
        return inspector_result(session, "GET", target);
    }

    if (name == "live_widget_get") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return tool_result(text_content("Error: " + session_error), true);
        }
        const auto *path_v = args.at("path");
        if (path_v == nullptr || !path_v->is_string()) {
            return tool_result(text_content("Error: missing 'path' parameter"), true);
        }
        return inspector_result(session, "GET", "/api/widget/" + path_v->as_or<std::string>(""));
    }

    if (name == "live_widget_set") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return tool_result(text_content("Error: " + session_error), true);
        }
        for (const char *key : {"path", "prop"}) {
            const auto *kv = args.at(key);
            if (kv == nullptr || !kv->is_string()) {
                return tool_result(text_content(std::string("Error: missing '") + key + "' parameter"), true);
            }
        }
        const auto *value_v = args.at("value");
        if (value_v == nullptr) {
            return tool_result(text_content("Error: missing 'value' parameter"), true);
        }
        const auto path = args.at("path")->as_or<std::string>("");
        const auto prop = args.at("prop")->as_or<std::string>("");
        // REST 约定：/api/widget/{tree_path}/{prop_name}；根节点的树路径为空。
        const std::string target = "/api/widget/" + (path.empty() ? std::string{} : path + "/") + prop;
        const auto dumped = au::json::dump(*value_v);
        return inspector_result(session, "PUT", target, dumped.ok() ? dumped.value() : std::string{});
    }

    if (name == "live_patch") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return tool_result(text_content("Error: " + session_error), true);
        }
        const auto *ops_v = args.at("ops");
        if (ops_v == nullptr || !ops_v->is_array()) {
            return tool_result(text_content("Error: 'ops' must be a JSON array"), true);
        }
        const auto dumped = au::json::dump(*ops_v);
        return inspector_result(session, "POST", "/api/patch", dumped.ok() ? dumped.value() : std::string{});
    }

    if (name == "live_simulate") {
        std::string session_error;
        const InspectorSession session = resolve_session(args, session_error);
        if (!session_error.empty()) {
            return tool_result(text_content("Error: " + session_error), true);
        }
        const auto *path_v = args.at("path");
        const auto *action_v = args.at("action");
        if (path_v == nullptr || !path_v->is_string() || action_v == nullptr || !action_v->is_string()) {
            return tool_result(text_content("Error: missing 'path' or 'action' parameter"), true);
        }
        const auto action = action_v->as_or<std::string>("");
        if (action != "click" && action != "scroll" && action != "text") {
            return tool_result(text_content("Error: action must be click | scroll | text"), true);
        }
        au::Json body = au::Json::object();
        body.set("path", path_v->as_or<std::string>(""));
        if (action == "scroll") {
            body.set("dx", args.as_or<float>("dx", 0.0F));
            body.set("dy", args.as_or<float>("dy", 0.0F));
        }
        if (action == "text") {
            body.set("text", args.as_or<std::string>("text", std::string{}));
        }
        const auto dumped = au::json::dump(body);
        return inspector_result(session, "POST", "/api/input/" + action, dumped.ok() ? dumped.value() : std::string{});
    }

    if (name == "to_code") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        auto style_str = args.as_or<std::string>("style", std::string("fluent"));
        auto style = aurora::tools::parse_code_style(style_str);

        std::string code = to_code(*tree_v, style);
        return tool_result(text_content(code));
    }

    if (name == "to_yaml") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        std::string yaml = aurora::serialization::to_yaml(*tree_v);
        return tool_result(text_content(yaml));
    }

    if (name == "get_schema") {
        au::Json api = aurora::tools::build_api_skeleton();
        return tool_result(json_content(api));
    }

    if (name == "simulate_interaction") {
        const auto *tree_v = args.at("tree");
        if (tree_v == nullptr || !tree_v->is_object()) {
            return tool_result(text_content("Error: missing 'tree' parameter"), true);
        }
        const auto *path_v = args.at("path");
        if (path_v == nullptr || !path_v->is_string()) {
            return tool_result(text_content("Error: missing 'path' parameter"), true);
        }
        const auto *action_v = args.at("action");
        if (action_v == nullptr || !action_v->is_string()) {
            return tool_result(text_content("Error: missing 'action' parameter"), true);
        }
        const auto path = path_v->as_or<std::string>("");
        const auto action = action_v->as_or<std::string>("");
        if (action != "click" && action != "scroll" && action != "text") {
            return tool_result(text_content("Error: 'action' must be one of click | scroll | text"), true);
        }
        // 参数判型前置：取值一律走宽容读（as_or），类型不符回退默认，绝不抛异常
        // （stdio 服务主循环不捕获异常，抛出即进程终止）。
        for (const char *key : {"dx", "dy", "width", "height"}) {
            if (const auto *it = args.find(key); it != nullptr && !it->is_number()) {
                return tool_result(text_content(std::string("Error: '") + key + "' must be a number"), true);
            }
        }
        if (const auto *it = args.find("text"); it != nullptr && !it->is_string()) {
            return tool_result(text_content("Error: 'text' must be a string"), true);
        }

        const int width = args.as_or<int>("width", 800);
        const int height = args.as_or<int>("height", 600);
        auto widget = aurora::serialization::from_json(*tree_v);
        if (!widget) {
            return tool_result(text_content("Error: " + widget.error().message), true);
        }
        aurora::Node root(std::move(widget.value()));
        // 先布局一次：from_json 只构树不布局，而 simulate_* 要求目标控件有可命中区域
        // （滚动容器还须真正布局出非零视口）。此处复用无头快照的 mount + layout 路径，
        // 其结果仅用于确立几何，不回传（回传的是交互之后的那份）。
        (void)render_to_logical_snapshot(root, width, height);

        // 寻址用 `find_widget`（而非 `find_node`）：① 与树枚举（`tree_json_full`）同源，
        // 同一路径在快照与单控件查询下指向同一控件；② 下降全程不构造 `Node` 副本——本工具是
        // 「先交互再快照」的流程，`Node` 副本析构会无条件清掉兄弟节点的 `layout_parent_`，
        // 破坏脏传播（`from_json` 构造的树虽不含虚拟化容器，这条副作用与树形无关）。
        aurora::Widget *target = aurora::Inspector::find_widget(root.widget(), path);
        if (target == nullptr) {
            return tool_result(text_content("Error: widget not found at path '" + path + "'"), true);
        }

        std::string failure;
        if (action == "click") {
            const aurora::Result<void> r = aurora::Inspector::simulate_click(*target);
            failure = r ? std::string{} : r.error().message;
        } else if (action == "scroll") {
            const aurora::Result<void> r = aurora::Inspector::simulate_scroll(*target, args.as_or<float>("dx", 0.0F),
                                                                              args.as_or<float>("dy", 0.0F));
            failure = r ? std::string{} : r.error().message;
        } else {
            const aurora::Result<void> r =
                aurora::Inspector::simulate_text_input(*target, args.as_or<std::string>("text", std::string("")));
            failure = r ? std::string{} : r.error().message;
        }
        if (!failure.empty()) {
            return tool_result(text_content("Error: " + failure), true);
        }

        au::Json out = au::Json::object();
        out.set("action", action);
        out.set("path", path);
        out.set("target", aurora::Inspector::get_prop(*target));
        out.set("snapshot", render_to_logical_snapshot(root, width, height));
        return tool_result(json_content(out));
    }

    if (name == "list_commands") {
        const auto *commands_v = args.at("commands");
        if (commands_v == nullptr) {
            return tool_result(text_content("Error: missing 'commands' parameter"), true);
        }
        const au::Json *items = aurora::tools::command_descriptors(*commands_v);
        if (items == nullptr) {
            return tool_result(text_content("Error: 'commands' must be an array or the {\"commands\": [...]} envelope"),
                               true);
        }
        std::string query;
        if (const auto *it = args.find("query"); it != nullptr) {
            if (!it->is_string()) {
                return tool_result(text_content("Error: 'query' must be a string"), true);
            }
            query = it->as_or<std::string>("");
        }
        int limit = 50;
        if (const auto *it = args.find("limit"); it != nullptr) {
            if (!it->is_int()) {
                return tool_result(text_content("Error: 'limit' must be an integer"), true);
            }
            limit = it->as_or<int>(0);
        }
        bool include_disabled = false;
        if (const auto *it = args.find("include_disabled"); it != nullptr) {
            if (!it->is_bool()) {
                return tool_result(text_content("Error: 'include_disabled' must be a boolean"), true);
            }
            include_disabled = it->as_or<bool>(false);
        }

        const aurora::tools::CommandListing listing = aurora::tools::list_commands(*items, query, include_disabled);
        au::Json listed = au::Json::array();
        int count = 0;
        for (const std::size_t index : listing.indices) {
            if (limit >= 0 && count >= limit) {
                break;
            }
            listed.push_back(*items->at(index));
            ++count;
        }
        au::Json out = au::Json::object();
        out.set("count", count);
        out.set("matched", static_cast<int>(listing.indices.size()));
        out.set("considered", static_cast<int>(listing.considered));
        out.set("commands", std::move(listed));
        return tool_result(json_content(out));
    }

    if (name == "invoke_command") {
        const auto *commands_v = args.at("commands");
        if (commands_v == nullptr) {
            return tool_result(text_content("Error: missing 'commands' parameter"), true);
        }
        const auto *id_v = args.at("id");
        if (id_v == nullptr || !id_v->is_string()) {
            return tool_result(text_content("Error: missing 'id' parameter"), true);
        }
        const au::Json *items = aurora::tools::command_descriptors(*commands_v);
        if (items == nullptr) {
            return tool_result(text_content("Error: 'commands' must be an array or the {\"commands\": [...]} envelope"),
                               true);
        }
        const auto id = id_v->as_or<std::string>("");
        aurora::tools::CommandStatus status = aurora::tools::CommandStatus::NotFound;
        const au::Json *found = aurora::tools::resolve_command(*items, id, status);

        au::Json out = au::Json::object();
        out.set("id", id);
        out.set("resolved", au::Json{found != nullptr});
        out.set("enabled", au::Json{found != nullptr && aurora::tools::command_bool_field(*found, "enabled", true)});
        out.set("invocable",
                au::Json{found != nullptr && aurora::tools::command_bool_field(*found, "invocable", false)});
        out.set("status", aurora::tools::command_status_name(status));
        if (found != nullptr) {
            if (const auto *it = found->find("title"); it != nullptr && it->is_string()) {
                out.set("title", it->as_or<std::string>(""));
            }
            if (const auto *it = found->find("when"); it != nullptr && it->is_string()) {
                out.set("when", it->as_or<std::string>(""));
            }
        }
        out.set("note", "Resolution only; the host performs the actual call.");
        return tool_result(json_content(out));
    }

    // unknown tool
    return tool_result(text_content("Error: unknown tool '" + name + "'"), true);
}

// ---------- JSON-RPC dispatch ----------

[[nodiscard]] auto handle_request(const au::Json &req) -> au::Json {
    const auto *id_v = req.at("id");
    const au::Json id = id_v != nullptr ? *id_v : au::Json{};
    const auto method = req.as_or<std::string>("method", std::string(""));
    const auto *params_v = req.at("params");
    const au::Json params = (params_v != nullptr && params_v->is_object()) ? *params_v : au::Json::object();

    if (method == "initialize") {
        au::Json result = au::Json::object();
        result.set("protocolVersion", "2024-11-05");
        au::Json capabilities = au::Json::object();
        capabilities.set("tools", au::Json::object());
        result.set("capabilities", std::move(capabilities));
        au::Json info = au::Json::object();
        info.set("name", "aurora-mcp");
        info.set("version", AURORA_VERSION_STRING);
        result.set("serverInfo", std::move(info));
        return rpc_result(id, result);
    }

    if (method == "notifications/initialized") {
        // notification, no response needed (JSON-RPC notifications have no id, so do not send a response)
        return au::Json{};  // empty means no response
    }

    if (method == "tools/list") {
        au::Json result = au::Json::object();
        result.set("tools", tool_definitions());
        return rpc_result(id, result);
    }

    if (method == "tools/call") {
        const auto tool_name = params.as_or<std::string>("name", std::string(""));
        const auto *args_v = params.at("arguments");
        const au::Json args = (args_v != nullptr && args_v->is_object()) ? *args_v : au::Json::object();
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
        if (msg.is_null()) {
            break;  // EOF or parse failure
        }

        au::Json response = handle_request(msg);
        if (!response.is_null()) {
            write_message(response);
        }
    }

    return 0;
}
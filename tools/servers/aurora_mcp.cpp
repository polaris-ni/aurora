// tools/servers/aurora_mcp.cpp
//
// aurora-mcp — Aurora MCP Server（Model Context Protocol，stdio JSON-RPC 2.0）。
//
// 为 AI Agent 提供运行时组件发现、UI 树校验、离屏渲染与代码生成能力。
// 传输：stdin/stdout，消息格式 Content-Length: <N>\r\n\r\n<JSON-RPC 2.0 body>。
//
// 用法：aurora_mcp（由 AI Agent 以 stdio 模式启动，无需人工交互）
//
// 暴露 MCP Tools（10 个）：
//   list_components / describe_component / search_components /
//   validate_tree / validate_ui / render_snapshot / render_png /
//   to_code / to_yaml / get_schema

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "aurora/app/validate.h"
#include "aurora/aurora.h"
#include "aurora/inspector/inspector_api.h"
#include "aurora/render/offscreen.h"
#include "aurora/widget/codegen.h"
#include "aurora/widget/yaml.h"
#include "known_enums.h"

#include "api_schema.h"
#include "code_style.h"

using namespace aurora;
using namespace aurora::serialization;
using namespace aurora::tools;

// ---------- 已知枚举（单一来源：tools/include/known_enums.h） ----------

namespace {

// ---------- MCP 协议 I/O ----------

/// 从 stdin 读取一条 MCP 消息（Content-Length 头 + JSON body）。返回空 Json 表示 EOF。
[[nodiscard]] auto read_message() -> Json {
    // 读 header 行
    std::string line;
    int content_length = -1;
    while (std::getline(std::cin, line)) {
        // 去除 \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            break; // 空行 = header 结束
        if (line.rfind("Content-Length:", 0) == 0) {
            // 线协议头来自对端，可能畸形（"Content-Length: abc"）。std::stoi 抛出的
            // invalid_argument/out_of_range 在此无人接住，会直接 terminate 掉服务器。
            try {
                content_length = std::stoi(line.substr(15));
            } catch (const std::exception &) {
                return Json{}; // 视作协议错误，按 EOF 处理
            }
        }
    }
    if (content_length < 0)
        return Json{};
    // 上限保护：畸形/恶意对端可声明超大 Content-Length，无上限的 resize 会因
    // bad_alloc / length_error 直接 terminate 掉服务器。合法 MCP 消息远小于 64MiB。
    constexpr int kMaxMessageBytes = 64 * 1024 * 1024;
    if (content_length > kMaxMessageBytes)
        return Json{}; // 视作协议错误，按 EOF 处理

    // 读 body
    std::string body(static_cast<std::size_t>(content_length), '\0');
    std::cin.read(body.data(), content_length);
    if (std::cin.gcount() < content_length)
        return Json{};

    return Json::parse(body, nullptr, false);
}

/// 向 stdout 写一条 MCP 消息（stdio 线协议帧，须经无前缀 raw 通道，保持 Content-Length 头字节精确）。
auto write_message(const Json &msg) -> void {
    std::string body = msg.dump();
    AURORA_LOG_RAW("mcp", "Content-Length: ", body.size(), "\r\n\r\n", body);
}

/// 构造 JSON-RPC 2.0 成功响应。
[[nodiscard]] auto rpc_result(const Json &id, const Json &result) -> Json {
    return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
}

/// 构造 JSON-RPC 2.0 错误响应。
[[nodiscard]] auto rpc_error(const Json &id, int code, const std::string &message) -> Json {
    return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
}

/// @brief 判断落盘路径是否被限制在服务器工作目录内。
///
/// MCP 的调用方是 AI Agent，其参数可被它读到的任意不可信内容（仓库文件、网页、issue 正文）
/// 通过提示注入操纵。若 `render_png` 的 `path` 不加约束，即可写/覆盖任意文件（如自启动目录），
/// 形成典型的 confused-deputy 任意写原语。故此处只接受工作目录内的相对路径。
[[nodiscard]] auto is_confined_output_path(const std::string &path) -> bool {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path p(path);
    if (p.is_absolute() || p.has_root_name()) {
        return false; // 绝对路径与 Windows 盘符/UNC 前缀一律拒绝
    }
    for (const auto &part : p) {
        if (part == "..") {
            return false; // 任何上跳段都可能逃逸出工作目录
        }
    }
    return true;
}

// ---------- MCP Tool 定义 ----------

/// 返回 tools/list 的工具数组。
[[nodiscard]] auto tool_definitions() -> Json {
    Json tools = Json::array();

    // 辅助 lambda：构建 inputSchema
    auto schema_obj = [](Json props, Json req) -> Json {
        Json s = Json::object();
        s["type"] = "object";
        s["properties"] = std::move(props);
        if (!req.empty())
            s["required"] = std::move(req);
        return s;
    };
    auto str_prop = [](const char *desc) -> Json {
        Json p = Json::object();
        p["type"] = "string";
        p["description"] = desc;
        return p;
    };
    auto int_prop = [](const char *desc) -> Json {
        Json p = Json::object();
        p["type"] = "integer";
        p["description"] = desc;
        return p;
    };
    auto obj_prop = [](const char *desc) -> Json {
        Json p = Json::object();
        p["type"] = "object";
        p["description"] = desc;
        return p;
    };
    auto req_arr = [](std::initializer_list<const char *> items) -> Json {
        Json a = Json::array();
        for (auto *i : items)
            a.push_back(i);
        return a;
    };

    // list_components
    {
        Json t = Json::object();
        t["name"] = "list_components";
        t["description"] = "列出所有已注册的 Aurora 组件类型名";
        t["inputSchema"] = schema_obj(Json::object(), Json::array());
        tools.push_back(std::move(t));
    }
    // describe_component
    {
        Json props = Json::object();
        props["name"] = str_prop("组件类型名，如 Button");
        Json t = Json::object();
        t["name"] = "describe_component";
        t["description"] = "返回单个组件的完整 schema（属性/事件/子节点策略/示例）";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "name" }));
        tools.push_back(std::move(t));
    }
    // search_components
    {
        Json props = Json::object();
        props["query"] = str_prop("搜索关键词");
        Json t = Json::object();
        t["name"] = "search_components";
        t["description"] = "按名称子串模糊搜索已注册组件";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "query" }));
        tools.push_back(std::move(t));
    }
    // validate_tree
    {
        Json props = Json::object();
        props["tree"] = obj_prop("UI 树 JSON（to_json 输出格式）");
        Json t = Json::object();
        t["name"] = "validate_tree";
        t["description"] = "校验 UI 树 JSON 的合法性（类型/深度/空子节点）";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // validate_ui（specification/08-tooling.md §7.1：schema 静态验证，带路径/建议的结构化错误）
    {
        Json props = Json::object();
        props["tree"] = obj_prop("UI 树 JSON（to_json 输出格式）");
        Json t = Json::object();
        t["name"] = "validate_ui";
        t["description"] =
            "按 aurora_api.json schema 静态验证 UI 树：未知类型/缺失必填属性/类型不匹配/子节点策略；错误含 JSON "
            "路径与修复建议（供 AI 自动修复）";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // render_snapshot
    {
        Json props = Json::object();
        props["tree"] = obj_prop("UI 树 JSON");
        props["width"] = int_prop("视口宽（默认 800）");
        props["height"] = int_prop("视口高（默认 600）");
        Json t = Json::object();
        t["name"] = "render_snapshot";
        t["description"] = "对 UI 树 JSON 做离屏布局，返回逻辑快照（type + box + children）";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // render_png
    {
        Json props = Json::object();
        props["tree"] = obj_prop("UI 树 JSON");
        props["width"] = int_prop("画布宽（默认 800）");
        props["height"] = int_prop("画布高（默认 600）");
        props["path"] = str_prop("输出 PNG 路径，须为工作目录内的相对路径且不含 '..'（默认 aurora_render.png）");
        Json t = Json::object();
        t["name"] = "render_png";
        t["description"] = "对 UI 树 JSON 做离屏渲染，输出 PNG 文件";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // to_code
    {
        Json props = Json::object();
        props["tree"] = obj_prop("UI 树 JSON");
        props["style"] = str_prop("代码风格：fluent | step | di（默认 fluent）");
        Json t = Json::object();
        t["name"] = "to_code";
        t["description"] = "将 UI 树 JSON 转换为可编译的 C++ 代码";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // to_yaml
    {
        Json props = Json::object();
        props["tree"] = obj_prop("UI 树 JSON 对象（含 type/props/children）");
        Json t = Json::object();
        t["name"] = "to_yaml";
        t["description"] = "将 UI 树 JSON 转换为 YAML 格式字符串";
        t["inputSchema"] = schema_obj(std::move(props), req_arr({ "tree" }));
        tools.push_back(std::move(t));
    }
    // get_schema
    {
        Json t = Json::object();
        t["name"] = "get_schema";
        t["description"] = "返回完整 Aurora API schema（所有组件 + 枚举）";
        t["inputSchema"] = schema_obj(Json::object(), Json::array());
        tools.push_back(std::move(t));
    }

    return tools;
}

// ---------- MCP Tool 执行 ----------

/// 构造 MCP tools/call 的 content 数组（text 类型）。
[[nodiscard]] auto text_content(const std::string &text) -> Json {
    Json item = Json::object();
    item["type"] = "text";
    item["text"] = text;
    Json arr = Json::array();
    arr.push_back(std::move(item));
    return arr;
}

[[nodiscard]] auto json_content(const Json &j) -> Json { return text_content(j.dump(2)); }

/// 执行指定 tool，返回 MCP tools/call 的 result。
[[nodiscard]] auto execute_tool(const std::string &name, const Json &args) -> Json {
    if (name == "list_components") {
        // 使用 Inspector 门面获取所有组件 schema，提取类型名
        auto schemas = Inspector::components();
        Json arr = Json::array();
        for (const auto &s : schemas) {
            if (s.contains("type"))
                arr.push_back(s["type"]);
        }
        return Json{ { "content", json_content(arr) } };
    }

    if (name == "describe_component") {
        if (!args.contains("name") || !args["name"].is_string()) {
            return Json{ { "content", text_content("Error: missing 'name' parameter") }, { "isError", true } };
        }
        Json schema = Inspector::component_schema(args["name"].get<std::string>());
        return Json{ { "content", json_content(schema) } };
    }

    if (name == "search_components") {
        if (!args.contains("query") || !args["query"].is_string()) {
            return Json{ { "content", text_content("Error: missing 'query' parameter") }, { "isError", true } };
        }
        auto results = search_components(args["query"].get<std::string>());
        Json arr = Json::array();
        for (const auto &r : results)
            arr.push_back(r);
        return Json{ { "content", json_content(arr) } };
    }

    if (name == "validate_ui") {
        // specification/08-tooling.md §7.1：schema 静态验证（不构造 widget，纯 JSON 对照 aurora_api schema）。
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        const Json report = validate_ui_tree_json(args["tree"]);
        Json out{ { "content", json_content(report) } };
        if (!report["valid"].get<bool>()) {
            out["isError"] = true;
        }
        return out;
    }

    if (name == "validate_tree") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        auto widget = from_json(args["tree"]);
        if (!widget) {
            Json err = Json{ { "ok", false }, { "error", widget.error().to_json() } };
            return Json{ { "content", json_content(err) }, { "isError", true } };
        }
        Node root(std::move(widget.value()));
        auto diags = Inspector::validate(root);
        if (!diags.empty()) {
            Json err = Json::object();
            err["ok"] = false;
            Json diag_arr = Json::array();
            for (const auto &d : diags) {
                diag_arr.push_back(Json::parse(d.to_json_line(), nullptr, false));
            }
            err["diagnostics"] = diag_arr;
            return Json{ { "content", json_content(err) }, { "isError", true } };
        }
        return Json{ { "content", json_content(Json{ { "ok", true } }) } };
    }

    if (name == "render_snapshot") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        auto widget = from_json(args["tree"]);
        if (!widget) {
            return Json{ { "content", text_content("Error: " + widget.error().message) }, { "isError", true } };
        }
        Node root(std::move(widget.value()));
        Json snapshot = render_to_logical_snapshot(root, w, h);
        return Json{ { "content", json_content(snapshot) } };
    }

    if (name == "render_png") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        int w = args.value("width", 800);
        int h = args.value("height", 600);
        std::string path = args.value("path", std::string("aurora_render.png"));
        if (!is_confined_output_path(path)) {
            return Json{ { "content", text_content("Error: 'path' 必须是工作目录内的相对路径（不含 '..'）") },
                         { "isError", true } };
        }
        auto widget = from_json(args["tree"]);
        if (!widget) {
            return Json{ { "content", text_content("Error: " + widget.error().message) }, { "isError", true } };
        }
        Node root(std::move(widget.value()));
        auto ok = render_to_png(root, w, h, path.c_str());
        if (!ok) {
            return Json{ { "content", text_content("Error: " + ok.error().message) }, { "isError", true } };
        }
        return Json{ { "content", json_content(Json{ { "path", path }, { "width", w }, { "height", h } }) } };
    }

    if (name == "to_code") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        std::string style_str = args.value("style", std::string("fluent"));
        auto style = aurora::tools::parse_code_style(style_str);

        std::string code = to_code(args["tree"], style);
        return Json{ { "content", text_content(code) } };
    }

    if (name == "to_yaml") {
        if (!args.contains("tree") || !args["tree"].is_object()) {
            return Json{ { "content", text_content("Error: missing 'tree' parameter") }, { "isError", true } };
        }
        std::string yaml = aurora::serialization::to_yaml(args["tree"]);
        return Json{ { "content", text_content(yaml) } };
    }

    if (name == "get_schema") {
        Json api = aurora::tools::build_api_skeleton();
        return Json{ { "content", json_content(api) } };
    }

    // 未知工具
    return Json{ { "content", text_content("Error: unknown tool '" + name + "'") }, { "isError", true } };
}

// ---------- JSON-RPC 分发 ----------

[[nodiscard]] auto handle_request(const Json &req) -> Json {
    Json id = req.contains("id") ? req["id"] : Json(nullptr);
    std::string method = req.value("method", std::string(""));
    Json params = req.value("params", Json::object());

    if (method == "initialize") {
        Json result = Json{
            { "protocolVersion", "2024-11-05" },
            { "capabilities", { { "tools", Json::object() } } },
            { "serverInfo", { { "name", "aurora-mcp" }, { "version", AURORA_VERSION_STRING } } },
        };
        return rpc_result(id, result);
    }

    if (method == "notifications/initialized") {
        // 通知，无需响应（但 JSON-RPC 通知无 id，不发响应）
        return Json{}; // 空表示不发
    }

    if (method == "tools/list") {
        return rpc_result(id, Json{ { "tools", tool_definitions() } });
    }

    if (method == "tools/call") {
        std::string tool_name = params.value("name", std::string(""));
        Json args = params.value("arguments", Json::object());
        if (tool_name.empty()) {
            return rpc_error(id, -32602, "Missing tool name in params.name");
        }
        Json result = execute_tool(tool_name, args);
        return rpc_result(id, result);
    }

    if (method == "ping") {
        return rpc_result(id, Json::object());
    }

    return rpc_error(id, -32601, "Method not found: " + method);
}

} // namespace

// ---------- main ----------

int main() {
    register_core_widgets();

    // MCP stdio 主循环
    while (true) {
        Json msg = read_message();
        if (msg.is_null() || msg.is_discarded())
            break; // EOF 或解析失败

        Json response = handle_request(msg);
        if (!response.is_null()) {
            write_message(response);
        }
    }

    return 0;
}

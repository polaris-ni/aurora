// =============================================================================
// aurora_lsp.cpp — Aurora 语言服务（LSP，stdio JSON-RPC 2.0）。
// -----------------------------------------------------------------------------
// 能力（specification/08-tooling.md §7.3）：completion / hover / diagnostics / codeAction。
// 消费库的 live API（describe_component + known_enums），并基于 lsp_schema.h /
// lsp_document.h / lsp_features.h 这套分层的 LSP 分析器
// 的轻量 C++ 源码扫描，对 au::<Type>Props{ .prop = ... } 等声明式写法提供辅助。
// 构建：与 aurora_mcp / aurora_cli 同模式（add_executable + link aurora）。
// =============================================================================

#include "aurora/core/platform.h"
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifdef AURORA_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

#include "aurora/aurora.h"
#include "aurora/widget/serialization.h"

#include "lsp_features.h" // 汇聚 lsp_schema.h + lsp_document.h
#include "nlohmann/json.hpp"
#include "known_enums.h"

using Json = nlohmann::json;
using namespace aurora::tools;
using aurora::describe_component;
using aurora::list_all_components;

// ----------------------------- 全局状态 --------------------------------------
static Schema g_schema;
static std::map<std::string, std::string> g_docs; // uri -> 文档全文

static auto build_schema() -> void {
    g_schema = Schema{};
    for (const auto &kv : aurora::tools::known_enums()) {
        EnumSchema e;
        e.name = kv.first;
        e.values = kv.second;
        g_schema.enums.push_back(std::move(e));
    }
    for (const std::string &t : list_all_components()) {
        try {
            Json j = describe_component(t);
            if (j.is_null() || j.empty())
                continue;
            ComponentSchema c;
            c.type = j.value("type", t);
            if (j.contains("category") && j["category"].is_string())
                c.category = j["category"].get<std::string>();
            if (j.contains("children_policy") && j["children_policy"].is_string())
                c.children_policy = j["children_policy"].get<std::string>();
            else if (j.contains("container") && j["container"].is_string())
                c.children_policy = j["container"].get<std::string>();
            if (j.contains("prop_descriptors") && j["prop_descriptors"].is_array()) {
                for (const auto &p : j["prop_descriptors"]) {
                    PropSchema ps;
                    if (p.contains("name") && p["name"].is_string())
                        ps.name = p["name"].get<std::string>();
                    if (p.contains("type") && p["type"].is_string())
                        ps.type = p["type"].get<std::string>();
                    if (p.contains("default")) {
                        const Json &d = p["default"];
                        ps.default_value = d.is_string() ? d.get<std::string>() : d.dump();
                    }
                    ps.required = p.value("required", false);
                    if (p.contains("note") && p["note"].is_string())
                        ps.note = p["note"].get<std::string>();
                    c.props.push_back(std::move(ps));
                }
            }
            if (j.contains("events") && j["events"].is_array())
                for (const auto &e : j["events"])
                    if (e.is_string())
                        c.events.push_back(e.get<std::string>());
            if (j.contains("examples") && j["examples"].is_array())
                for (const auto &ex : j["examples"])
                    if (ex.is_string())
                        c.examples.push_back(ex.get<std::string>());
            g_schema.components.push_back(std::move(c));
        } catch (const std::exception &e) {
            AURORA_LOG_ERROR("lsp", "[aurora-lsp] skip component ", t, ": ", e.what());
        }
    }
}

// ----------------------------- JSON-RPC 传输 --------------------------------
static auto read_message(std::string &out) -> bool {
    out.clear();
    int content_length = -1;
    std::string line;
    while (true) {
        line.clear();
        int c;
        while ((c = std::cin.get()) != EOF && c != '\n') {
            if (c != '\r')
                line.push_back(static_cast<char>(c));
        }
        if (c == EOF)
            return false;
        if (line.empty())
            break; // 空行结束头部
        if (line.rfind("Content-Length:", 0) == 0) {
            try {
                content_length = std::stoi(line.substr(std::string("Content-Length:").size()));
            } catch (...) {
            }
        }
    }
    if (content_length < 0)
        return false;
    // 上限保护：畸形/恶意对端可声明超大 Content-Length，无上限的 resize 会因
    // bad_alloc / length_error 直接 terminate 掉服务器。合法 LSP 消息远小于 64MiB。
    constexpr int kMaxMessageBytes = 64 * 1024 * 1024;
    if (content_length > kMaxMessageBytes)
        return false;
    out.resize(static_cast<size_t>(content_length));
    if (content_length > 0) {
        std::cin.read(&out[0], content_length);
        if (std::cin.gcount() != content_length)
            return false;
    }
    return true;
}

static auto send_message(const Json &j) -> void {
    const std::string s = j.dump();
    AURORA_LOG_RAW("lsp", "Content-Length: ", s.size(), "\r\n\r\n", s);
}

static auto send_response(const Json &id, const Json &result) -> void {
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = result;
    send_message(r);
}

static auto send_notification(const std::string &method, const Json &params) -> void {
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["method"] = method;
    r["params"] = params;
    send_message(r);
}

static auto range_json(size_t line, size_t col, size_t end_line, size_t end_col) -> Json {
    Json r = Json::object();
    Json s = Json::object();
    s["line"] = line;
    s["character"] = col;
    Json e = Json::object();
    e["line"] = end_line;
    e["character"] = end_col;
    r["start"] = s;
    r["end"] = e;
    return r;
}

static auto kind_to_int(const std::string &k) -> int {
    if (k == "Class")
        return 7;
    if (k == "Property")
        return 10;
    if (k == "Enum")
        return 13;
    if (k == "EnumMember")
        return 20;
    return 1;
}

// ----------------------------- 能力处理 --------------------------------------
static auto on_initialize(const Json & /*params*/) -> Json {
    Json caps = Json::object();
    caps["textDocumentSync"] = 1; // 全量同步
    Json comp = Json::object();
    Json trig = Json::array();
    trig.push_back(".");
    trig.push_back(":");
    comp["triggerCharacters"] = trig;
    caps["completionProvider"] = comp;
    caps["hoverProvider"] = true;
    caps["codeActionProvider"] = true;

    Json result = Json::object();
    result["capabilities"] = caps;
    Json info = Json::object();
    info["name"] = "aurora-lsp";
    info["version"] = AURORA_VERSION_STRING;
    result["serverInfo"] = info;
    return result;
}

static auto publish_diagnostics(const std::string &uri, const std::string &text) -> void {
    Document doc = analyze(text);
    std::vector<Diagnostic> diags = diagnostics(doc, g_schema);
    std::vector<Diagnostic> ev = validate_enum_values(text, g_schema);
    diags.insert(diags.end(), ev.begin(), ev.end());

    Json params = Json::object();
    params["uri"] = uri;
    Json arr = Json::array();
    for (const auto &d : diags) {
        Json dd = Json::object();
        dd["range"] = range_json(d.line, d.col, d.end_line, d.end_col);
        // LSP 诊断严重级别：1=Error, 2=Warning, 3=Info, 4=Hint。
        dd["severity"] = (d.severity == Diagnostic::Severity::Error) ? 1 : 2;
        dd["message"] = d.message;
        dd["source"] = "aurora-lsp";
        arr.push_back(dd);
    }
    params["diagnostics"] = arr;
    send_notification("textDocument/publishDiagnostics", params);
}

static auto doc_text(const Json &params) -> std::string {
    const std::string uri = params["textDocument"].value("uri", "");
    auto it = g_docs.find(uri);
    return (it == g_docs.end()) ? std::string{} : it->second;
}

static auto on_completion(const Json &params) -> Json {
    const std::string uri = params["textDocument"].value("uri", "");
    const std::string text = doc_text(params);
    if (text.empty() && g_docs.find(uri) == g_docs.end())
        return Json::object();
    const Json &pos = params["position"];
    const size_t line = pos["line"].get<size_t>();
    const size_t col = pos["character"].get<size_t>();

    Document doc = analyze(text);
    std::vector<CompletionItem> items = completions(text, doc, g_schema, line, col);

    Json result = Json::object();
    result["isIncomplete"] = false;
    Json arr = Json::array();
    for (const auto &it : items) {
        Json ci = Json::object();
        ci["label"] = it.label;
        ci["kind"] = kind_to_int(it.kind);
        if (!it.detail.empty())
            ci["detail"] = it.detail;
        if (!it.documentation.empty()) {
            Json doc = Json::object();
            doc["kind"] = "markdown";
            doc["value"] = it.documentation;
            ci["documentation"] = doc;
        }
        arr.push_back(ci);
    }
    result["items"] = arr;
    return result;
}

static auto on_hover(const Json &params) -> Json {
    const std::string text = doc_text(params);
    if (text.empty())
        return Json(); // null
    const Json &pos = params["position"];
    const size_t line = pos["line"].get<size_t>();
    const size_t col = pos["character"].get<size_t>();

    Document doc = analyze(text);
    auto h = hover(doc, g_schema, line, col);
    if (!h)
        return Json();

    Json result = Json::object();
    Json contents = Json::object();
    contents["kind"] = "markdown";
    contents["value"] = h->content;
    result["contents"] = contents;
    return result;
}

static auto on_code_action(const Json &params) -> Json {
    const std::string uri = params["textDocument"].value("uri", "");
    const std::string text = doc_text(params);
    if (text.empty())
        return Json::array();

    Document doc = analyze(text);
    std::vector<CodeAction> actions = code_actions(doc, g_schema);

    Json arr = Json::array();
    for (const auto &a : actions) {
        Json ca = Json::object();
        ca["title"] = a.title;
        ca["kind"] = "quickfix";
        Json edit = Json::object();
        Json changes = Json::object();
        Json edits = Json::array();
        for (const auto &e : a.edits) {
            Json te = Json::object();
            te["range"] = range_json(e.line, e.col, e.line, e.col);
            te["newText"] = e.new_text;
            edits.push_back(te);
        }
        changes[uri] = edits;
        edit["changes"] = changes;
        ca["edit"] = edit;
        arr.push_back(ca);
    }
    return arr;
}

// ----------------------------- 主循环 ----------------------------------------
int main() {
#ifdef AURORA_PLATFORM_WINDOWS
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    build_schema();

    std::string msg;
    while (read_message(msg)) {
        Json req;
        try {
            req = Json::parse(msg);
        } catch (...) {
            continue;
        }
        if (!req.contains("method"))
            continue;

        const std::string method = req.value("method", "");
        const Json id = req.contains("id") ? req["id"] : Json();
        const Json params = req.contains("params") ? req["params"] : Json::object();

        try {
            if (method == "initialize") {
                send_response(id, on_initialize(params));
            } else if (method == "initialized") {
                // 通知，无需响应
            } else if (method == "shutdown") {
                send_response(id, Json());
            } else if (method == "exit") {
                break;
            } else if (method == "textDocument/didOpen") {
                const std::string uri = params["textDocument"].value("uri", "");
                const std::string text = params["textDocument"].value("text", "");
                g_docs[uri] = text;
                publish_diagnostics(uri, text);
            } else if (method == "textDocument/didChange") {
                const std::string uri = params["textDocument"].value("uri", "");
                const Json &changes = params["contentChanges"];
                if (changes.is_array() && !changes.empty() && changes[0].contains("text")) {
                    g_docs[uri] = changes[0]["text"].value("text", "");
                }
                publish_diagnostics(uri, g_docs[uri]);
            } else if (method == "textDocument/didClose") {
                g_docs.erase(params["textDocument"].value("uri", ""));
            } else if (method == "textDocument/completion") {
                send_response(id, on_completion(params));
            } else if (method == "textDocument/hover") {
                send_response(id, on_hover(params));
            } else if (method == "textDocument/codeAction") {
                send_response(id, on_code_action(params));
            } else if (!id.is_null()) {
                send_response(id, Json()); // 未知方法：空结果
            }
        } catch (const std::exception &e) {
            AURORA_LOG_ERROR("lsp", "[aurora-lsp] error handling ", method, ": ", e.what());
            if (!id.is_null() && method != "exit")
                send_response(id, Json());
        }
    }
    return 0;
}

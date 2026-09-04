// =============================================================================
// aurora_lsp.cpp — Aurora language service (LSP, stdio JSON-RPC 2.0).
// -----------------------------------------------------------------------------
// Capabilities (specification/08-tooling.md §7.3): completion / hover / diagnostics / codeAction.
// Consumes the library's live API (describe_component + known_enums) and, via the lightweight
// C++ source scanning of the layered LSP analyzer (lsp_schema.h / lsp_document.h /
// lsp_features.h), assists declarative syntax such as au::<Type>Props{ .prop = ... }.
// Build: same pattern as aurora_mcp / aurora_cli (add_executable + link aurora).
// =============================================================================

#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

#include "aurora/aurora.h"
#include "aurora/widget/serialization.h"
#include "known_enums.h"
#include "lsp_features.h"
#include "nlohmann/json.hpp"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

// ----------------------------- global state ----------------------------------
static auto docs() -> std::map<std::string, std::string> & {
    static std::map<std::string, std::string> s;
    return s;
}

static auto schema() -> const au::tools::Schema & {
    static const au::tools::Schema S = []() -> au::tools::Schema {
        au::tools::Schema result;
        for (const auto &kv : au::tools::known_enums()) {
            au::tools::EnumSchema e;
            e.name = kv.first;
            e.values = kv.second;
            result.enums.push_back(std::move(e));
        }
        for (const std::string &t : au::list_all_components()) {
            try {
                au::Json j = au::describe_component(t);
                if (j.is_null() || j.empty()) {
                    continue;
                }
                au::tools::ComponentSchema c;
                c.type = j.value("type", t);
                if (j.contains("category") && j["category"].is_string()) {
                    c.category = j["category"].get<std::string>();
                }
                if (j.contains("children_policy") && j["children_policy"].is_string()) {
                    c.children_policy = j["children_policy"].get<std::string>();
                } else if (j.contains("container") && j["container"].is_string()) {
                    c.children_policy = j["container"].get<std::string>();
                }
                if (j.contains("prop_descriptors") && j["prop_descriptors"].is_array()) {
                    for (const auto &p : j["prop_descriptors"]) {
                        au::tools::PropSchema ps;
                        if (p.contains("name") && p["name"].is_string()) {
                            ps.name = p["name"].get<std::string>();
                        }
                        if (p.contains("type") && p["type"].is_string()) {
                            ps.type = p["type"].get<std::string>();
                        }
                        if (p.contains("default")) {
                            const au::Json &d = p["default"];
                            ps.default_value = d.is_string() ? d.get<std::string>() : d.dump();
                        }
                        ps.required = p.value("required", false);
                        if (p.contains("note") && p["note"].is_string()) {
                            ps.note = p["note"].get<std::string>();
                        }
                        c.props.push_back(std::move(ps));
                    }
                }
                if (j.contains("events") && j["events"].is_array()) {
                    for (const auto &e : j["events"]) {
                        if (e.is_string()) {
                            c.events.push_back(e.get<std::string>());
                        }
                    }
                }
                if (j.contains("examples") && j["examples"].is_array()) {
                    for (const auto &ex : j["examples"]) {
                        if (ex.is_string()) {
                            c.examples.push_back(ex.get<std::string>());
                        }
                    }
                }
                result.components.push_back(std::move(c));
            } catch (const std::exception &e) {
                AURORA_LOG_ERROR("lsp", "[aurora-lsp] skip component ", t, ": ", e.what());
            }
        }
        return result;
    }();
    return S;
}

// ----------------------------- JSON-RPC transport ---------------------------
static auto read_message(std::string &out) -> bool {
    out.clear();
    int content_length = -1;
    std::string line;
    while (true) {
        line.clear();
        int c = 0;
        while ((c = std::cin.get()) != EOF && c != '\n') {
            if (c != '\r') {
                line.push_back(static_cast<char>(c));
            }
        }
        if (c == EOF) {
            return false;
        }
        if (line.empty()) {
            break;  // empty line ends the header
        }
        if (line.starts_with("Content-Length:")) {
            try {
                content_length = std::stoi(line.substr(std::string("Content-Length:").size()));
            } catch (...) {  // NOLINT(*-empty-catch)
            }
        }
    }
    if (content_length < 0) {
        return false;
    }
    // Upper-bound guard: a malformed/malicious peer may declare an enormous Content-Length; an unbounded
    // resize would terminate the server via bad_alloc / length_error. Legitimate LSP messages are far below 64MiB.
    constexpr int max_message_bytes = 64 * 1024 * 1024;
    if (content_length > max_message_bytes) {
        return false;
    }
    out.resize(static_cast<size_t>(content_length));
    if (content_length > 0) {
        std::cin.read(out.data(), content_length);
        if (std::cin.gcount() != content_length) {
            return false;
        }
    }
    return true;
}

static auto send_message(const au::Json &j) -> void {
    const std::string s = j.dump();
    AURORA_LOG_RAW("lsp", "Content-Length: ", s.size(), "\r\n\r\n", s);
}

static auto send_response(const au::Json &id, const au::Json &result) -> void {
    au::Json r = au::Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = result;
    send_message(r);
}

static auto send_notification(const std::string &method, const au::Json &params) -> void {
    au::Json r = au::Json::object();
    r["jsonrpc"] = "2.0";
    r["method"] = method;
    r["params"] = params;
    send_message(r);
}

static auto range_json(size_t line, size_t col, size_t end_line, size_t end_col) -> au::Json {
    au::Json r = au::Json::object();
    au::Json s = au::Json::object();
    s["line"] = line;
    s["character"] = col;
    au::Json e = au::Json::object();
    e["line"] = end_line;
    e["character"] = end_col;
    r["start"] = s;
    r["end"] = e;
    return r;
}

static auto kind_to_int(const std::string &k) -> int {
    if (k == "Class") {
        return 7;
    }
    if (k == "Property") {
        return 10;
    }
    if (k == "Enum") {
        return 13;
    }
    if (k == "EnumMember") {
        return 20;
    }
    return 1;
}

// ----------------------------- capability handling ---------------------------
static auto on_initialize(const au::Json & /*params*/) -> au::Json {
    au::Json caps = au::Json::object();
    caps["textDocumentSync"] = 1;  // full sync
    au::Json comp = au::Json::object();
    au::Json trig = au::Json::array();
    trig.push_back(".");
    trig.push_back(":");
    comp["triggerCharacters"] = trig;
    caps["completionProvider"] = comp;
    caps["hoverProvider"] = true;
    caps["codeActionProvider"] = true;

    au::Json result = au::Json::object();
    result["capabilities"] = caps;
    au::Json info = au::Json::object();
    info["name"] = "aurora-lsp";
    info["version"] = AURORA_VERSION_STRING;
    result["serverInfo"] = info;
    return result;
}

static auto publish_diagnostics(const std::string &uri, const std::string &text) -> void {
    const au::tools::Document doc = au::tools::analyze(text);
    std::vector<au::tools::Diagnostic> diags = diagnostics(doc, schema());
    std::vector<au::tools::Diagnostic> ev = validate_enum_values(text, schema());
    diags.insert(diags.end(), ev.begin(), ev.end());

    au::Json params = au::Json::object();
    params["uri"] = uri;
    au::Json arr = au::Json::array();
    for (const auto &d : diags) {
        au::Json dd = au::Json::object();
        dd["range"] = range_json(d.line, d.col, d.end_line, d.end_col);
        // LSP diagnostic severity: 1=Error, 2=Warning, 3=Info, 4=Hint.
        dd["severity"] = (d.severity == au::tools::Diagnostic::Severity::Error) ? 1 : 2;
        dd["message"] = d.message;
        dd["source"] = "aurora-lsp";
        arr.push_back(dd);
    }
    params["diagnostics"] = arr;
    send_notification("textDocument/publishDiagnostics", params);
}

static auto doc_text(const au::Json &params) -> std::string {
    const std::string uri = params["textDocument"].value("uri", "");
    const auto it = docs().find(uri);
    return it == docs().end() ? std::string{} : it->second;
}

static auto on_completion(const au::Json &params) -> au::Json {
    const std::string uri = params["textDocument"].value("uri", "");
    const std::string text = doc_text(params);
    if (text.empty() && !docs().contains(uri)) {
        return au::Json::object();
    }
    const au::Json &pos = params["position"];
    const size_t line = pos["line"].get<size_t>();
    const size_t col = pos["character"].get<size_t>();

    const au::tools::Document doc = au::tools::analyze(text);
    const std::vector<au::tools::CompletionItem> items = completions(text, doc, schema(), line, col);

    au::Json result = au::Json::object();
    result["isIncomplete"] = false;
    au::Json arr = au::Json::array();
    for (const auto &it : items) {
        au::Json ci = au::Json::object();
        ci["label"] = it.label;
        ci["kind"] = kind_to_int(it.kind);
        if (!it.detail.empty()) {
            ci["detail"] = it.detail;
        }
        if (!it.documentation.empty()) {
            au::Json doc_json = au::Json::object();
            doc_json["kind"] = "markdown";
            doc_json["value"] = it.documentation;
            ci["documentation"] = doc_json;
        }
        arr.push_back(ci);
    }
    result["items"] = arr;
    return result;
}

static auto on_hover(const au::Json &params) -> au::Json {
    const std::string text = doc_text(params);
    if (text.empty()) {
        return {};  // null
    }
    const au::Json &pos = params["position"];
    const size_t line = pos["line"].get<size_t>();
    const size_t col = pos["character"].get<size_t>();

    const au::tools::Document doc = au::tools::analyze(text);
    auto h = hover(doc, schema(), line, col);
    if (!h) {
        return {};
    }

    au::Json result = au::Json::object();
    au::Json contents = au::Json::object();
    contents["kind"] = "markdown";
    contents["value"] = h->content;
    result["contents"] = contents;
    return result;
}

static auto on_code_action(const au::Json &params) -> au::Json {
    const std::string uri = params["textDocument"].value("uri", "");
    const std::string text = doc_text(params);
    if (text.empty()) {
        return au::Json::array();
    }

    au::tools::Document doc = au::tools::analyze(text);
    std::vector<au::tools::CodeAction> actions = code_actions(doc, schema());

    au::Json arr = au::Json::array();
    for (const auto &a : actions) {
        au::Json ca = au::Json::object();
        ca["title"] = a.title;
        ca["kind"] = "quickfix";
        au::Json edit = au::Json::object();
        au::Json changes = au::Json::object();
        au::Json edits = au::Json::array();
        for (const auto &e : a.edits) {
            au::Json te = au::Json::object();
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

// ----------------------------- main loop -------------------------------------
auto main() -> int {  // NOLINT(*-exception-escape, *-function-cognitive-complexity)
#ifdef AURORA_PLATFORM_WINDOWS
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string msg;
    while (read_message(msg)) {
        au::Json req;
        try {
            req = au::Json::parse(msg);
        } catch (...) {
            continue;
        }
        if (!req.contains("method")) {
            continue;
        }

        const std::string method = req.value("method", "");
        const au::Json id = req.contains("id") ? req["id"] : au::Json();
        const au::Json params = req.contains("params") ? req["params"] : au::Json::object();

        try {
            if (method == "initialize") {
                send_response(id, on_initialize(params));
            } else if (method == "initialized") {
                // notification, no response
            } else if (method == "exit") {
                break;
            } else if (method == "textDocument/didOpen") {
                const std::string uri = params["textDocument"].value("uri", "");
                const std::string text = params["textDocument"].value("text", "");
                docs()[uri] = text;
                publish_diagnostics(uri, text);
            } else if (method == "textDocument/didChange") {
                const std::string uri = params["textDocument"].value("uri", "");
                const au::Json &changes = params["contentChanges"];
                if (changes.is_array() && !changes.empty() && changes[0].contains("text")) {
                    docs()[uri] = changes[0]["text"].value("text", "");
                }
                publish_diagnostics(uri, docs()[uri]);
            } else if (method == "textDocument/didClose") {
                docs().erase(params["textDocument"].value("uri", ""));
            } else if (method == "textDocument/completion") {
                send_response(id, on_completion(params));
            } else if (method == "textDocument/hover") {
                send_response(id, on_hover(params));
            } else if (method == "textDocument/codeAction") {
                send_response(id, on_code_action(params));
            } else if (method == "shutdown" || !id.is_null()) {
                send_response(id, au::Json());  // shutdown or unknown method
            }
        } catch (const std::exception &e) {
            AURORA_LOG_ERROR("lsp", "[aurora-lsp] error handling ", method, ": ", e.what());
            if (!id.is_null() && method != "exit") {
                send_response(id, au::Json());
            }
        }
    }
    return 0;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
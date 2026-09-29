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
                c.type = j.as_or<std::string>("type", t);
                if (const auto *v = j.at("category"); v != nullptr && v->is_string()) {
                    c.category = v->as_or<std::string>("");
                }
                if (const auto *v = j.at("children_policy"); v != nullptr && v->is_string()) {
                    c.children_policy = v->as_or<std::string>("");
                } else if (const auto *v = j.at("container"); v != nullptr && v->is_string()) {
                    c.children_policy = v->as_or<std::string>("");
                }
                if (const auto *descriptors = j.at("prop_descriptors");
                    descriptors != nullptr && descriptors->is_array()) {
                    for (const auto &descriptor : *descriptors) {
                        au::tools::PropSchema ps;
                        if (const auto *v = descriptor.at("name"); v != nullptr && v->is_string()) {
                            ps.name = v->as_or<std::string>("");
                        }
                        if (const auto *v = descriptor.at("type"); v != nullptr && v->is_string()) {
                            ps.type = v->as_or<std::string>("");
                        }
                        if (const auto *d = descriptor.at("default"); d != nullptr) {
                            if (d->is_string()) {
                                ps.default_value = d->as_or<std::string>("");
                            } else {
                                const auto dumped = au::json::dump(*d);
                                ps.default_value = dumped.ok() ? dumped.value() : std::string{};
                            }
                        }
                        ps.required = descriptor.as_or<bool>("required", false);
                        if (const auto *v = descriptor.at("note"); v != nullptr && v->is_string()) {
                            ps.note = v->as_or<std::string>("");
                        }
                        c.props.push_back(std::move(ps));
                    }
                }
                if (const auto *events = j.at("events"); events != nullptr && events->is_array()) {
                    for (const auto &event : *events) {
                        if (event.is_string()) {
                            c.events.push_back(event.as_or<std::string>(""));
                        }
                    }
                }
                if (const auto *examples = j.at("examples"); examples != nullptr && examples->is_array()) {
                    for (const auto &example : *examples) {
                        if (example.is_string()) {
                            c.examples.push_back(example.as_or<std::string>(""));
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
    const auto dumped = au::json::dump(j);
    const std::string s = dumped.ok() ? dumped.value() : std::string{};
    AURORA_LOG_RAW("lsp", "Content-Length: ", s.size(), "\r\n\r\n", s);
}

static auto send_response(const au::Json &id, const au::Json &result) -> void {
    au::Json r = au::Json::object();
    r.set("jsonrpc", "2.0");
    r.set("id", id);
    r.set("result", result);
    send_message(r);
}

static auto send_notification(const std::string &method, const au::Json &params) -> void {
    au::Json r = au::Json::object();
    r.set("jsonrpc", "2.0");
    r.set("method", method);
    r.set("params", params);
    send_message(r);
}

static auto range_json(size_t line, size_t col, size_t end_line, size_t end_col) -> au::Json {
    au::Json r = au::Json::object();
    au::Json s = au::Json::object();
    s.set("line", line);
    s.set("character", col);
    au::Json e = au::Json::object();
    e.set("line", end_line);
    e.set("character", end_col);
    r.set("start", s);
    r.set("end", e);
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

/// @brief 读取 params.textDocument.<key>（宽容：缺键 / 非对象 / 类型不符回空串，不抛异常）。
static auto text_document_field(const au::Json &params, const char *key) -> std::string {
    const auto *td = params.at("textDocument");
    return td != nullptr ? td->as_or<std::string>(key, std::string{}) : std::string{};
}

/// @brief 读取 params.position 的行/列（宽容：缺键回 0）。
static auto position_line_col(const au::Json &params, size_t &line, size_t &col) -> void {
    line = 0;
    col = 0;
    const auto *pos = params.at("position");
    if (pos == nullptr) {
        return;
    }
    line = pos->as_or<std::size_t>("line", std::size_t{0});
    col = pos->as_or<std::size_t>("character", std::size_t{0});
}

// ----------------------------- capability handling ---------------------------
static auto on_initialize(const au::Json & /*params*/) -> au::Json {
    au::Json caps = au::Json::object();
    caps.set("textDocumentSync", 1);  // full sync
    au::Json comp = au::Json::object();
    au::Json trig = au::Json::array();
    trig.push_back(".");
    trig.push_back(":");
    comp.set("triggerCharacters", trig);
    caps.set("completionProvider", comp);
    caps.set("hoverProvider", au::Json{true});
    caps.set("codeActionProvider", au::Json{true});

    au::Json result = au::Json::object();
    result.set("capabilities", caps);
    au::Json info = au::Json::object();
    info.set("name", "aurora-lsp");
    info.set("version", AURORA_VERSION_STRING);
    result.set("serverInfo", info);
    return result;
}

static auto publish_diagnostics(const std::string &uri, const std::string &text) -> void {
    const au::tools::Document doc = au::tools::analyze(text);
    std::vector<au::tools::Diagnostic> diags = diagnostics(doc, schema());
    std::vector<au::tools::Diagnostic> ev = validate_enum_values(text, schema());
    diags.insert(diags.end(), ev.begin(), ev.end());

    au::Json params = au::Json::object();
    params.set("uri", uri);
    au::Json arr = au::Json::array();
    for (const auto &d : diags) {
        au::Json dd = au::Json::object();
        dd.set("range", range_json(d.line, d.col, d.end_line, d.end_col));
        // LSP diagnostic severity: 1=Error, 2=Warning, 3=Info, 4=Hint.
        dd.set("severity", (d.severity == au::tools::Diagnostic::Severity::Error) ? 1 : 2);
        dd.set("message", d.message);
        dd.set("source", "aurora-lsp");
        arr.push_back(dd);
    }
    params.set("diagnostics", arr);
    send_notification("textDocument/publishDiagnostics", params);
}

static auto doc_text(const au::Json &params) -> std::string {
    const std::string uri = text_document_field(params, "uri");
    const auto it = docs().find(uri);
    return it == docs().end() ? std::string{} : it->second;
}

static auto on_completion(const au::Json &params) -> au::Json {
    const std::string uri = text_document_field(params, "uri");
    const std::string text = doc_text(params);
    if (text.empty() && !docs().contains(uri)) {
        return au::Json::object();
    }
    size_t line = 0;
    size_t col = 0;
    position_line_col(params, line, col);

    const au::tools::Document doc = au::tools::analyze(text);
    const std::vector<au::tools::CompletionItem> items = completions(text, doc, schema(), line, col);

    au::Json result = au::Json::object();
    result.set("isIncomplete", au::Json{false});
    au::Json arr = au::Json::array();
    for (const auto &it : items) {
        au::Json ci = au::Json::object();
        ci.set("label", it.label);
        ci.set("kind", kind_to_int(it.kind));
        if (!it.detail.empty()) {
            ci.set("detail", it.detail);
        }
        if (!it.documentation.empty()) {
            au::Json doc_json = au::Json::object();
            doc_json.set("kind", "markdown");
            doc_json.set("value", it.documentation);
            ci.set("documentation", doc_json);
        }
        arr.push_back(ci);
    }
    result.set("items", arr);
    return result;
}

static auto on_hover(const au::Json &params) -> au::Json {
    const std::string text = doc_text(params);
    if (text.empty()) {
        return {};  // null
    }
    size_t line = 0;
    size_t col = 0;
    position_line_col(params, line, col);

    const au::tools::Document doc = au::tools::analyze(text);
    auto h = hover(doc, schema(), line, col);
    if (!h) {
        return {};
    }

    au::Json result = au::Json::object();
    au::Json contents = au::Json::object();
    contents.set("kind", "markdown");
    contents.set("value", h->content);
    result.set("contents", contents);
    return result;
}

static auto on_code_action(const au::Json &params) -> au::Json {
    const std::string uri = text_document_field(params, "uri");
    const std::string text = doc_text(params);
    if (text.empty()) {
        return au::Json::array();
    }

    au::tools::Document doc = au::tools::analyze(text);
    std::vector<au::tools::CodeAction> actions = code_actions(doc, schema());

    au::Json arr = au::Json::array();
    for (const auto &a : actions) {
        au::Json ca = au::Json::object();
        ca.set("title", a.title);
        ca.set("kind", "quickfix");
        au::Json edit = au::Json::object();
        au::Json changes = au::Json::object();
        au::Json edits = au::Json::array();
        for (const auto &e : a.edits) {
            au::Json te = au::Json::object();
            te.set("range", range_json(e.line, e.col, e.line, e.col));
            te.set("newText", e.new_text);
            edits.push_back(te);
        }
        changes.set(uri, edits);
        edit.set("changes", changes);
        ca.set("edit", edit);
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
        if (auto parsed = au::json::parse(msg); parsed) {
            req = std::move(parsed.value());
        } else {
            continue;
        }
        if (!req.contains("method")) {
            continue;
        }

        const auto method = req.as_or<std::string>("method", "");
        const auto *id_v = req.at("id");
        const au::Json id = id_v != nullptr ? *id_v : au::Json{};
        const auto *params_v = req.at("params");
        const au::Json params = (params_v != nullptr && params_v->is_object()) ? *params_v : au::Json::object();

        try {
            if (method == "initialize") {
                send_response(id, on_initialize(params));
            } else if (method == "initialized") {
                // notification, no response
            } else if (method == "exit") {
                break;
            } else if (method == "textDocument/didOpen") {
                const std::string uri = text_document_field(params, "uri");
                const std::string text = text_document_field(params, "text");
                docs()[uri] = text;
                publish_diagnostics(uri, text);
            } else if (method == "textDocument/didChange") {
                const std::string uri = text_document_field(params, "uri");
                const auto *changes = params.at("contentChanges");
                if (changes != nullptr && changes->is_array() && !changes->empty() && changes->at(0) != nullptr &&
                    changes->at(0)->contains("text")) {
                    docs()[uri] = changes->at(0)->as_or<std::string>("text", std::string{});
                }
                publish_diagnostics(uri, docs()[uri]);
            } else if (method == "textDocument/didClose") {
                docs().erase(text_document_field(params, "uri"));
            } else if (method == "textDocument/completion") {
                send_response(id, on_completion(params));
            } else if (method == "textDocument/hover") {
                send_response(id, on_hover(params));
            } else if (method == "textDocument/codeAction") {
                send_response(id, on_code_action(params));
            } else if (method == "shutdown" || !id.is_null()) {
                send_response(id, au::Json{});  // shutdown or unknown method
            }
        } catch (const std::exception &e) {
            AURORA_LOG_ERROR("lsp", "[aurora-lsp] error handling ", method, ": ", e.what());
            if (!id.is_null() && method != "exit") {
                send_response(id, au::Json{});
            }
        }
    }
    return 0;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

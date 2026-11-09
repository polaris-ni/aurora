// =============================================================================
// lsp_features.h — Semantic service layer of the LSP analyzer (header-only, decoupled from the library).
// -----------------------------------------------------------------------------
// Built on the Document model from lsp_document.h and the Schema from lsp_schema.h, providing
// completion / hover / diagnostics / enum-value validation / code actions, plus the helpers they
// depend on (innermost_block / innermost_chain / line_prefix). It does not do full C++ parsing; it
// only targets common Aurora declarative-UI idioms.
//
// The property surface works on the chain form `au::TProps().set_<key>(...)`. A completion label IS
// the text the server inserts, so the label is the writer name itself; matching accepts both the
// writer name and the bare key, because the key is what the schema and the diagnostics speak.
// Brace property blocks are an illegal form: they are reported as one error per block and offered a
// whole-block rewrite. Brace child-node lists are legal and are not validated as property blocks.
// Convention: lines and columns are 0-based, consistent with the LSP protocol.
// =============================================================================

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lsp_document.h"
#include "lsp_schema.h"

namespace aurora::tools {

// --------------------------- completion / hover / diagnostics ------------------------------
struct CompletionItem {
    std::string label;  // completion text (inserted verbatim: no insertText / snippet is negotiated)
    std::string kind;  // "Method" / "Class" / "Enum" / "EnumMember"
    std::string detail;  // type / default value
    std::string documentation;
};

struct HoverInfo {
    std::string content;  // markdown-flavored plain text
};

struct Diagnostic {
    enum class Severity : std::uint8_t { Error = 1, Warning = 2, Information = 3, Hint = 4 };
    size_t line = 0;
    size_t col = 0;
    size_t end_line = 0;
    size_t end_col = 0;
    Severity severity = Severity::Error;
    std::string message;
};

struct TextEdit {
    size_t line = 0;  // start line (0-based)
    size_t col = 0;  // start column
    size_t end_line = 0;  // replacement end; equal to (line,col) means a pure insertion
    size_t end_col = 0;
    std::string new_text;
};

struct CodeAction {
    std::string title;
    std::vector<TextEdit> edits;
};

// Find the innermost block containing (line,col) (the deeper the nesting, the later it appears;
// take the last match).
[[nodiscard]] inline auto innermost_block(const Document &doc, size_t line, size_t col) -> const Block * {
    const Block *best = nullptr;
    for (const auto &b : doc.blocks) {
        if (pos_inside(b, line, col)) {
            if ((best == nullptr) || b.start_line > best->start_line ||
                (b.start_line == best->start_line && b.start_col > best->start_col)) {
                best = &b;
            }
        }
    }
    return best;
}

// Same selection rule for Props chains.
[[nodiscard]] inline auto innermost_chain(const Document &doc, size_t line, size_t col) -> const Chain * {
    const Chain *best = nullptr;
    for (const auto &c : doc.chains) {
        if (pos_inside(c, line, col)) {
            if ((best == nullptr) || c.start_line > best->start_line ||
                (c.start_line == best->start_line && c.start_col > best->start_col)) {
                best = &c;
            }
        }
    }
    return best;
}

[[nodiscard]] inline auto line_prefix(const std::string &text, size_t pos) -> std::string {
    // Take the content of the current line before pos.
    size_t start = pos;
    while (start > 0 && text[start - 1] != '\n') {
        --start;
    }
    return text.substr(start, pos - start);
}

// ------------------------------- shared property helpers -----------------------------------
[[nodiscard]] inline auto find_prop(const ComponentSchema &comp, const std::string &key) -> const PropSchema * {
    for (const auto &p : comp.props) {
        if (p.name == key) {
            return &p;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto prop_used(const std::vector<PropUse> &uses, const std::string &key) -> bool {
    return std::ranges::any_of(uses, [&key](const PropUse &u) { return u.name == key; });
}

// Required rows that have a Props-side writer and have not been loaded yet. A required row without a
// writer cannot be loaded in a chain at all, so it is not reported as missing (there is no fix).
[[nodiscard]] inline auto missing_required(const ComponentSchema &comp, const std::vector<PropUse> &uses)
    -> std::vector<const PropSchema *> {
    std::vector<const PropSchema *> out;
    for (const auto &p : comp.props) {
        if (p.required && !p.writer.empty() && !prop_used(uses, p.name)) {
            out.push_back(&p);
        }
    }
    return out;
}

[[nodiscard]] inline auto prop_doc(const PropSchema &p) -> std::string {
    return (p.default_value.empty() ? "" : ("default: " + p.default_value + "\n")) + p.note;
}

// Placeholder argument for a writer inserted by a code action. Every required row in the authority
// table is string-valued (Button.label / Text.content / TextInput.value, all taking const char *), so
// the empty string compiles; a row that has a native default keeps it.
[[nodiscard]] inline auto prop_placeholder(const PropSchema &p) -> std::string {
    return p.default_value.empty() ? std::string{"\"\""} : p.default_value;
}

[[nodiscard]] inline auto prop_hover(const PropSchema &p, const std::string &writer) -> HoverInfo {
    HoverInfo h;
    h.content = "**" + (writer.empty() ? p.name : writer) + "**: `" + p.type + "`" + (p.required ? " (required)" : "") +
                "\n" + (p.default_value.empty() ? "" : ("default: `" + p.default_value + "`\n")) + p.note;
    return h;
}

// Provide completions at (line,col). text is used to derive the trigger context.
[[nodiscard]] inline auto completions(const std::string &text, const Document &doc, const Schema &schema, size_t line,
                                      size_t col) -> std::vector<CompletionItem> {
    std::vector<CompletionItem> out;
    const size_t pos = text_pos_of(text, line, col);
    const std::string prefix = line_prefix(text, pos);

    // The identifier currently being typed.
    size_t wend = prefix.size();
    size_t wstart = wend;
    while (wstart > 0 && is_ident_char(prefix[wstart - 1])) {
        --wstart;
    }
    const std::string word = prefix.substr(wstart, wend - wstart);

    const bool after_au = [&]() -> bool {
        // prefix starts with au:: and there is no '.' or '::' between it and word.
        auto p = prefix.rfind("au::");
        if (p == std::string::npos) {
            return false;
        }
        // Check that between au:: (4 characters, [p, p+3]) and word there is only whitespace,
        // or they are directly adjacent.
        for (size_t x = p + 4; x < wstart; ++x) {
            if (prefix[x] != ' ' && prefix[x] != '\t') {
                return false;
            }
        }
        return true;
    }();

    const bool after_dot = (wstart > 0 && prefix[wstart - 1] == '.');

    // 1) after au:: -> complete widget types + enum types. The label is the C++ spelling, which is
    // what compiles after au:: (Image is serialized as "Image" but declared as au::ImageView, and
    // WebView lives in au::webview::).
    if (after_au) {
        for (const auto &c : schema.components) {
            const std::string spell = c.cpp_type.empty() ? c.type : c.cpp_type;
            if (word.empty() || spell.starts_with(word)) {
                CompletionItem it;
                it.label = spell;
                it.kind = "Class";
                it.detail = c.category;
                it.documentation = "children_policy: " + c.children_policy +
                                   (c.props.empty() ? "" : ("\nproperties: " + std::to_string(c.props.size())));
                out.push_back(std::move(it));
            }
        }
        for (const auto &e : schema.enums) {
            if (word.empty() || e.name.starts_with(word)) {
                CompletionItem it;
                it.label = e.name;
                it.kind = "Enum";
                it.detail = "enum";
                it.documentation = "values: " + join(e.values, ", ");
                out.push_back(std::move(it));
            }
        }
        return out;
    }

    // 2) inside a Props chain after '.' -> complete the set_<key>() writers (excluding used ones).
    // A brace property block gets nothing here: it is an illegal form, and the diagnostic plus the
    // whole-block rewrite own it. Emitting `.key = ` fragments there is what made the old surface
    // suggest code that does not compile.
    if (after_dot) {
        const Chain *ch = innermost_chain(doc, line, col);
        if (ch == nullptr) {
            return out;
        }
        const auto *comp = schema.find_component(ch->type);
        if (comp == nullptr || !comp->chain_writers) {
            return out;
        }
        for (const auto &p : comp->props) {
            if (p.writer.empty() || prop_used(ch->props, p.name)) {
                continue;
            }
            // Dual matching: the label is the writer name, but typing the bare key must still hit it.
            if (!word.empty() && !p.writer.starts_with(word) && !p.name.starts_with(word)) {
                continue;
            }
            CompletionItem it;
            it.label = p.writer;
            it.kind = "Method";
            it.detail = p.type + (p.required ? " (required)" : "");
            it.documentation = prop_doc(p);
            out.push_back(std::move(it));
        }
        return out;
    }

    // 3) Enum member completion: au::Enum:: or, inside a writer call, the argument of an enum row.
    // 3a) the au::Enum:: form.
    auto dcolon = prefix.rfind("::");
    if (dcolon != std::string::npos) {
        // Take the identifier before :: as the enum name (it may carry au::).
        size_t es = dcolon;
        while (es > 0 && is_ident_char(prefix[es - 1])) {
            --es;
        }
        std::string ename = prefix.substr(es, dcolon - es);
        const auto *en = schema.find_enum_qualified(ename);
        if (en != nullptr) {
            for (const auto &v : en->values) {
                if (word.empty() || v.starts_with(word)) {
                    CompletionItem it;
                    it.label = v;
                    it.kind = "EnumMember";
                    it.detail = en->name;
                    out.push_back(std::move(it));
                }
            }
            return out;
        }
    }

    // 3b) Inside `.set_<key>(` of an enum-typed row.
    {
        const Chain *ch = innermost_chain(doc, line, col);
        if (ch != nullptr) {
            const auto *comp = schema.find_component(ch->type);
            const std::string key = writer_arg_key(text, pos);
            if (comp != nullptr && !key.empty()) {
                if (const auto *p = find_prop(*comp, key); p != nullptr) {
                    if (const auto *en = schema.find_enum(p->type); en != nullptr) {
                        for (const auto &v : en->values) {
                            if (word.empty() || v.starts_with(word)) {
                                CompletionItem it;
                                it.label = v;
                                it.kind = "EnumMember";
                                it.detail = en->name;
                                out.push_back(std::move(it));
                            }
                        }
                    }
                }
            }
        }
    }

    return out;
}

// Hover: on an au::Type -> widget summary; on a set_<key>() writer or a brace designator -> the row.
[[nodiscard]] inline auto hover(const Document &doc, const Schema &schema, size_t line, size_t col)
    -> std::optional<HoverInfo> {
    // Widget type reference.
    for (const auto &r : doc.refs) {
        const size_t name_start = r.col + 3;
        if (line == r.line && col >= name_start && col <= name_start + r.name.size()) {
            if (const auto *c = schema.find_component(r.name); c != nullptr) {
                HoverInfo h;
                h.content = "**" + c->type + "** (" + c->category + ")\n" + "children_policy: " + c->children_policy +
                            "\n" + "properties: " + std::to_string(c->props.size()) +
                            " / events: " + std::to_string(c->events.size());
                return h;
            }
            if (const auto *e = schema.find_enum_qualified(r.name); e != nullptr) {
                HoverInfo h;
                h.content = "**enum " + e->name + "**\nvalues: " + join(e->values, ", ");
                return h;
            }
        }
    }
    // Writer call inside a Props chain.
    if (const Chain *ch = innermost_chain(doc, line, col); ch != nullptr) {
        if (const auto *comp = schema.find_component(ch->type); comp != nullptr) {
            for (const auto &u : ch->props) {
                if (line == u.line && col >= u.col && col <= prop_use_end(u)) {
                    if (const auto *p = find_prop(*comp, u.name); p != nullptr) {
                        return prop_hover(*p, p->writer);
                    }
                }
            }
        }
    }
    // Designator inside a brace block (the block itself is reported; the row info is still useful).
    if (const Block *b = innermost_block(doc, line, col); b != nullptr) {
        if (const auto *comp = schema.find_component(b->type); comp != nullptr) {
            for (const auto &u : b->props) {
                if (line == u.line && col >= u.col && col <= prop_use_end(u)) {
                    if (const auto *p = find_prop(*comp, u.name); p != nullptr) {
                        return prop_hover(*p, p->writer);
                    }
                }
            }
        }
    }
    return std::nullopt;
}

// Diagnostics: unknown type, illegal brace property form, unknown / misplaced writer, missing
// required property.
[[nodiscard]] inline auto diagnostics(const Document &doc, const Schema &schema) -> std::vector<Diagnostic> {
    std::vector<Diagnostic> out;

    for (const auto &r : doc.refs) {
        if (schema.is_known_ref(r.name)) {
            continue;
        }
        Diagnostic d;
        d.line = r.line;
        d.col = r.col + 3;
        d.end_line = r.line;
        d.end_col = r.col + 3 + r.name.size();
        d.severity = Diagnostic::Severity::Error;
        d.message = "Unknown Aurora type: au::" + r.name;
        out.push_back(d);
    }

    for (const auto &ch : doc.chains) {
        const auto *comp = schema.find_component(ch.type);
        if (comp == nullptr) {
            continue;  // unknown widget: the reference diagnostic already reported it
        }
        const std::string ctype = comp->type;
        if (!comp->chain_writers) {
            Diagnostic d;
            d.line = ch.start_line;
            d.col = ch.start_col + 3;
            d.end_line = ch.start_line;
            d.end_col = ch.start_col + 3 + ch.type.size();
            d.severity = Diagnostic::Severity::Error;
            d.message = "Widget " + ctype + " has no Props aggregate; au::" + ch.type + "() does not exist";
            out.push_back(d);
            continue;
        }
        for (const auto &u : ch.props) {
            const auto *p = find_prop(*comp, u.name);
            Diagnostic d;
            d.line = u.line;
            d.col = u.col;
            d.end_line = u.line;
            d.end_col = prop_use_end(u);
            d.severity = Diagnostic::Severity::Error;
            if (p == nullptr) {
                d.message = "Widget " + ctype + " has no property writer: set_" + u.name;
            } else if (p->writer.empty()) {
                d.message = "Property " + u.name + " of " + ctype +
                            " lives on the widget, not on its Props aggregate; set it after construction";
            } else {
                continue;
            }
            out.push_back(std::move(d));
        }
        // Missing required property: only for a finished construction argument. A standalone
        // `auto p = au::TProps();` is configured later, and an open chain or an open outer call is
        // still being typed (both carry sentinels, recomputed by didChange once closed).
        if (!ch.is_complete_construction()) {
            continue;
        }
        for (const auto *p : missing_required(*comp, ch.props)) {
            Diagnostic d;
            d.line = ch.end_line;
            d.col = ch.end_col - 1;
            d.end_line = ch.end_line;
            d.end_col = ch.end_col;
            d.severity = Diagnostic::Severity::Warning;
            d.message = "Widget " + ctype + " missing required property: " + p->name;
            out.push_back(d);
        }
    }

    for (const auto &b : doc.blocks) {
        if (!b.is_property_block) {
            continue;  // a child-node initializer list is legal: it loads no properties
        }
        const auto *comp = schema.find_component(b.type);
        if (comp == nullptr) {
            continue;  // unknown widget: the reference diagnostic already reported it
        }
        // One error per block: the form itself is illegal, so per-key errors would only be noise.
        // After the rewrite the chain diagnostics above report unknown or misplaced keys precisely.
        Diagnostic d;
        d.line = b.start_line;
        d.col = b.start_col;
        d.end_line = b.start_line;
        d.end_col = b.start_col + 4 + b.type.size();
        d.severity = Diagnostic::Severity::Error;
        d.message = "Widget " + comp->type + " does not accept designated initializers; ";
        d.message += comp->props_type.empty() ? std::string{"load its properties after construction"}
                                              : ("use au::" + comp->props_type + "().set_<key>(...)");
        out.push_back(std::move(d));
    }

    return out;
}

// Enum value literal validation (needs the original text): au::Enum::Value or Enum::Value.
[[nodiscard]] inline auto validate_enum_values(const std::string &text, const Schema &schema)
    -> std::vector<Diagnostic> {
    std::vector<Diagnostic> out;
    const size_t n = text.size();
    size_t line = 0;
    for (size_t i = 0; i < n;) {
        if (text[i] == '\n') {
            ++line;
            ++i;
            continue;
        }
        const size_t s = skip_trivia(text, i);
        if (s != i) {
            for (size_t k = i; k < s; ++k) {
                if (text[k] == '\n') {
                    ++line;
                }
            }
            i = s;
            if (i >= n) {
                break;
            }
        }
        // Find the identifier before :: (the enum name).
        if (text[i] == ':' && i + 1 < n && text[i + 1] == ':') {
            size_t es = i;
            while (es > 0 && is_ident_char(text[es - 1])) {
                --es;
            }
            std::string ename = text.substr(es, i - es);
            size_t vs = i + 2;
            while (vs < n && is_ident_char(text[vs])) {
                ++vs;
            }
            std::string vname = text.substr(i + 2, vs - (i + 2));
            const auto *en = schema.find_enum_qualified(ename);
            if ((en != nullptr) && !vname.empty()) {
                bool ok = false;
                for (const auto &v : en->values) {
                    if (v == vname) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    Diagnostic d;
                    d.line = line;
                    d.col = i + 2;
                    d.end_line = line;
                    d.end_col = vs;
                    d.severity = Diagnostic::Severity::Error;
                    d.message = "Enum " + en->name + " has no values: " + vname;
                    out.push_back(d);
                }
            }
            i = vs;
            continue;
        }
        ++i;
    }
    return out;
}

// Code actions: append the missing required writers to a chain, and rewrite an illegal brace property
// block into the chain form.
[[nodiscard]] inline auto code_actions(const Document &doc, const Schema &schema) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;

    for (const auto &ch : doc.chains) {
        if (!ch.is_complete_construction()) {
            continue;  // the insertion point is the end of a finished construction argument
        }
        const auto *comp = schema.find_component(ch.type);
        if (comp == nullptr || !comp->chain_writers) {
            continue;
        }
        const std::vector<const PropSchema *> missing = missing_required(*comp, ch.props);
        if (missing.empty()) {
            continue;
        }
        std::string insert;
        for (const auto *p : missing) {
            insert += "." + p->writer + "(" + prop_placeholder(*p) + ")";
        }
        CodeAction ca;
        ca.title = "Add missing required properties for " + comp->type;
        ca.edits.push_back(TextEdit{.line = ch.end_line,
                                    .col = ch.end_col,
                                    .end_line = ch.end_line,
                                    .end_col = ch.end_col,
                                    .new_text = insert});
        out.push_back(std::move(ca));
    }

    for (const auto &b : doc.blocks) {
        if (!b.is_property_block || !b.is_closed) {
            continue;  // the rewrite replaces a whole block, so it needs both braces
        }
        const auto *comp = schema.find_component(b.type);
        if (comp == nullptr || comp->props_type.empty()) {
            continue;  // no aggregate to rewrite into
        }
        // `au::TProps{ ... }` becomes the Props chain; `au::T{ ... }` becomes the widget construction
        // carrying that chain, so the expression keeps constructing the same thing. Keys without a
        // Props-side writer are carried over verbatim: the chain diagnostics then name them, instead
        // of the rewrite silently dropping a property the user asked for.
        const auto sep = b.type.rfind("::");
        const std::string seg = sep == std::string::npos ? b.type : b.type.substr(sep + 2);
        const bool props_spell = seg.ends_with("Props");
        std::string replacement = "au::" + comp->props_type + "()";
        for (const auto &u : b.props) {
            replacement += ".set_";
            replacement += u.name;
            replacement += "(";
            replacement += u.value;
            replacement += ")";
        }
        if (!props_spell) {
            std::string wrapped = "au::";
            wrapped += comp->cpp_type;
            wrapped += '(';
            wrapped += replacement;
            wrapped += ')';
            replacement = std::move(wrapped);
        }
        CodeAction ca;
        ca.title = "Rewrite " + comp->type + " properties as set_<key>() chain writers";
        ca.edits.push_back(TextEdit{.line = b.start_line,
                                    .col = b.start_col,
                                    .end_line = b.end_line,
                                    .end_col = b.end_col + 1,
                                    .new_text = replacement});
        out.push_back(std::move(ca));
    }

    return out;
}

}  // namespace aurora::tools

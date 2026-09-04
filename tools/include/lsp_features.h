// =============================================================================
// lsp_features.h — Semantic service layer of the LSP analyzer (header-only, decoupled from the library).
// -----------------------------------------------------------------------------
// Built on the Document model from lsp_document.h and the Schema from lsp_schema.h, providing
// completion / hover / diagnostics / enum-value validation / code actions, plus the helpers they
// depend on (innermost_block / line_prefix). It does not do full C++ parsing; it only targets
// common Aurora declarative-UI idioms.
// Convention: lines and columns are 0-based, consistent with the LSP protocol.
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "lsp_document.h"
#include "lsp_schema.h"

namespace aurora::tools {

// --------------------------- completion / hover / diagnostics ------------------------------
struct CompletionItem {
    std::string label;  // completion text
    std::string kind;  // "Property" / "Class" / "Enum" / "EnumMember"
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
    size_t line = 0;  // insertion line (0-based)
    size_t col = 0;  // insertion column
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

[[nodiscard]] inline auto line_prefix(const std::string &text, size_t pos) -> std::string {
    // Take the content of the current line before pos.
    size_t start = pos;
    while (start > 0 && text[start - 1] != '\n') {
        --start;
    }
    return text.substr(start, pos - start);
}

// Provide completions at (line,col). text is used to derive the trigger context.
[[nodiscard]] inline auto completions(const std::string &text, const Document &doc, const Schema &schema, size_t line,
                                      size_t col) -> std::vector<CompletionItem> {
    std::vector<CompletionItem> out;
    const std::string prefix = line_prefix(text, text_pos_of(text, line, col));

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

    // 1) after au:: -> complete widget types + enum types.
    if (after_au) {
        for (const auto &c : schema.components) {
            if (word.empty() || c.type.starts_with(word)) {
                CompletionItem it;
                it.label = c.type;
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

    // 2) inside a block after '.' -> complete properties (excluding already-used ones).
    if (after_dot) {
        const Block *b = innermost_block(doc, line, col);
        if (b != nullptr) {
            const std::string ctype = Schema::strip_props(b->type);
            const auto *comp = schema.find_component(ctype);
            if (comp != nullptr) {
                for (const auto &p : comp->props) {
                    bool used = false;
                    for (const auto &u : b->props) {
                        if (u.name == p.name) {
                            used = true;
                            break;
                        }
                    }
                    if (used) {
                        continue;
                    }
                    if (!word.empty() && !p.name.starts_with(word)) {
                        continue;
                    }
                    CompletionItem it;
                    it.label = p.name;
                    it.kind = "Property";
                    it.detail = p.type + (p.required ? " (required)" : "");
                    it.documentation = (p.default_value.empty() ? "" : ("default: " + p.default_value + "\n")) + p.note;
                    out.push_back(std::move(it));
                }
            }
        }
        return out;
    }

    // 3) Enum member completion: au::Enum:: or, inside a block, enum property =
    // 3a) the au::Enum:: form.
    auto dcolon = prefix.rfind("::");
    if (dcolon != std::string::npos) {
        // Take the identifier before :: as the enum name (it may carry au::).
        size_t es = dcolon;
        while (es > 0 && is_ident_char(prefix[es - 1])) {
            --es;
        }
        std::string ename = prefix.substr(es, dcolon - es);
        const auto *en = schema.find_enum(ename);
        if (en == nullptr) {
            en = schema.find_enum(Schema::strip_props(ename));
        }
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

    // 3b) Inside a block, after enum property =.
    {
        const Block *b = innermost_block(doc, line, col);
        if (b != nullptr) {
            const std::string ctype = Schema::strip_props(b->type);
            const auto *comp = schema.find_component(ctype);
            if (comp != nullptr) {
                // Find the nearest .prop before '='.
                size_t eq = prefix.rfind('=');
                if (eq != std::string::npos) {
                    size_t ds = prefix.rfind('.', eq);
                    if (ds != std::string::npos) {
                        size_t pe = ds + 1;
                        while (pe < eq && is_ident_char(prefix[pe])) {
                            ++pe;
                        }
                        std::string pname = prefix.substr(ds + 1, pe - (ds + 1));
                        for (const auto &p : comp->props) {
                            if (p.name == pname) {
                                const auto *en = schema.find_enum(p.type);
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
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    return out;
}

// Hover: (line,col) on an au::Type -> widget summary; on a .prop inside a block -> property description.
[[nodiscard]] inline auto hover(const Document &doc, const Schema &schema, size_t line, size_t col)
    -> std::optional<HoverInfo> {
    // Widget type reference.
    for (const auto &r : doc.refs) {
        const size_t name_start = r.col + 3;
        if (line == r.line && col >= name_start && col <= name_start + r.name.size()) {
            if (const auto *c = schema.find_component(Schema::strip_props(r.name))) {
                HoverInfo h;
                h.content = "**" + c->type + "** (" + c->category + ")\n" + "children_policy: " + c->children_policy +
                            "\n" + "properties: " + std::to_string(c->props.size()) +
                            " / events: " + std::to_string(c->events.size());
                return h;
            }
            if (const auto *e = schema.find_enum(r.name)) {
                HoverInfo h;
                h.content = "**enum " + e->name + "**\nvalues: " + join(e->values, ", ");
                return h;
            }
        }
    }
    // Property inside a block.
    const Block *b = innermost_block(doc, line, col);
    if (b != nullptr) {
        const std::string ctype = Schema::strip_props(b->type);
        const auto *comp = schema.find_component(ctype);
        if (comp != nullptr) {
            for (const auto &u : b->props) {
                if (line == u.line && col >= u.col && col <= u.col + u.name.size()) {
                    for (const auto &p : comp->props) {
                        if (p.name == u.name) {
                            HoverInfo h;
                            h.content =
                                "**" + p.name + "**: `" + p.type + "`" + (p.required ? " (required)" : "") + "\n" +
                                (p.default_value.empty() ? "" : ("default: `" + p.default_value + "`\n")) + p.note;
                            return h;
                        }
                    }
                }
            }
        }
    }
    return std::nullopt;
}

// Diagnostics: unknown type, unknown property, unknown enum value, missing required property.
[[nodiscard]] inline auto diagnostics(const Document &doc, const Schema &schema) -> std::vector<Diagnostic> {
    std::vector<Diagnostic> out;

    for (const auto &r : doc.refs) {
        if (schema.is_known_component(r.name) || schema.is_known_enum(r.name)) {
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

    for (const auto &b : doc.blocks) {
        const std::string ctype = Schema::strip_props(b.type);
        const auto *comp = schema.find_component(ctype);
        if (comp == nullptr) {
            continue;  // unknown widget: do not validate properties
        }
        // Unknown property.
        for (const auto &u : b.props) {
            bool known = false;
            for (const auto &p : comp->props) {
                if (p.name == u.name) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                Diagnostic d;
                d.line = u.line;
                d.col = u.col;
                d.end_line = u.line;
                d.end_col = u.col + u.name.size();
                d.severity = Diagnostic::Severity::Error;
                d.message = "Widget " + ctype + " has no properties: " + u.name;
                out.push_back(d);
            }
        }
        // Missing required property (warning; only for closed blocks: unclosed blocks have a sentinel
        // end and are recomputed by didChange once closed).
        for (const auto &p : comp->props) {
            if (!p.required) {
                continue;
            }
            bool used = false;
            for (const auto &u : b.props) {
                if (u.name == p.name) {
                    used = true;
                    break;
                }
            }
            if (!used && b.is_closed) {
                Diagnostic d;
                d.line = b.end_line;
                d.col = b.end_col;
                d.end_line = b.end_line;
                d.end_col = b.end_col + 1;
                d.severity = Diagnostic::Severity::Warning;
                d.message = "Widget " + ctype + " missing required property: " + p.name;
                out.push_back(d);
            }
        }
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
            const auto *en = schema.find_enum(ename);
            if (en == nullptr) {
                en = schema.find_enum(Schema::strip_props(ename));
            }
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

// Code actions: insert .prop = <default> for blocks missing required properties.
[[nodiscard]] inline auto code_actions(const Document &doc, const Schema &schema) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;
    for (const auto &b : doc.blocks) {
        if (!b.is_closed) {
            continue;  // only offer insertion for closed blocks (the insertion point is before '}')
        }
        const std::string ctype = Schema::strip_props(b.type);
        const auto *comp = schema.find_component(ctype);
        if (comp == nullptr) {
            continue;
        }
        std::vector<const PropSchema *> missing;
        for (const auto &p : comp->props) {
            if (!p.required) {
                continue;
            }
            bool used = false;
            for (const auto &u : b.props) {
                if (u.name == p.name) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                missing.push_back(&p);
            }
        }
        if (missing.empty()) {
            continue;
        }

        std::string insert;
        for (const auto &p : missing) {
            insert += "  ." + p->name + " = " + (p->default_value.empty() ? "\"\"" : p->default_value) + ",\n";
        }
        CodeAction ca;
        ca.title = "Add missing required properties for " + ctype;
        ca.edits.push_back(TextEdit{.line = b.end_line, .col = b.end_col, .new_text = insert});
        out.push_back(std::move(ca));
    }
    return out;
}

}  // namespace aurora::tools

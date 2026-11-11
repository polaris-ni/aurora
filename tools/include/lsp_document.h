// =============================================================================
// lsp_document.h — Document model layer of the LSP analyzer (header-only, decoupled from the library).
// -----------------------------------------------------------------------------
// Parses source text into a Document model made of Chain / Block / TypeRef, together with the lexical
// helpers needed for parsing (pos_inside / is_ident_char / join / text_pos_of / skip_trivia).
// Depends on the type definitions in lsp_schema.h and the semantic services in lsp_features.h
// (see those files).
//
// Two property-loading forms are modelled, and they are not interchangeable:
//   Chain — `au::TProps().set_x(...).set_y(...)`, the only form the migrated controls accept. Their
//           fields are private, so this is what completion / hover / diagnostics work on.
//   Block — `au::T{ ... }` / `au::TProps{ ... }`. A brace body holding child nodes is legal for the
//           types that take an initializer list; a brace body holding `.key = value` designators is
//           not legal for any registered type (the Props aggregates are gone or non-aggregate), so it
//           is still parsed — only to report it and to offer a whole-block rewrite.
// Only `set_<key>` writer calls are modelled inside a chain; other member calls (`clear(row)`, ...)
// are skipped rather than reported, because they are legal and carry no property key.
// =============================================================================

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace aurora::tools {

// ----------------------------- Document model ---------------------------------
struct PropUse {
    std::string name;  // property key: the designator name in a block, the writer root in a chain
    size_t line = 0;
    size_t col = 0;  // starting column of the name as written (the dot is before it)
    size_t len = 0;  // span of the identifier as written; 0 means name.size() (block designators)
    std::string value;  // raw initializer text, trimmed; only filled for block designators
};

struct Block {
    std::string type;  // the raw name after au::, e.g. "Button" / "ButtonProps"
    // True when the brace body loads properties (the type ends with Props, or a `.key = value`
    // designator was found inside). False means the body is a child-node initializer list, which is
    // legal and must not be validated as a property block.
    bool is_property_block = false;
    bool is_closed =
        false;  // whether a matching closing brace was found (unclosed blocks are used for completion context)
    size_t start_line = 0, start_col = 0;
    size_t end_line = 0, end_col = 0;
    size_t start_depth_was = 0;  // brace depth before the opening brace
    std::vector<PropUse> props;  // designated-initializer properties appearing inside the block
};

struct Chain {
    std::string type;  // the Props name as written, e.g. "ButtonProps" / "webview::WebViewProps"
    bool is_closed = false;  // whether the constructor call and every writer call were closed
    // True when the chain is an argument of an au::<Widget>(...) construction. A standalone
    // `auto p = au::TProps();` is configured later, so it must not be reported as missing required
    // properties at its declaration.
    bool ctor_arg = false;
    bool ctor_closed = false;  // whether that enclosing construction has its closing ')'
    size_t start_line = 0, start_col = 0;
    size_t end_line = 0, end_col = 0;  // one past the last ')' (or the sentinel when unclosed)
    std::vector<PropUse> props;  // writer roots, in written order

    // Whether this chain is a finished construction argument — the only state in which a missing
    // required property is a fact about the code rather than an artifact of typing. Both closures
    // are needed: in `au::Button(au::ButtonProps()` the Props constructor call is closed while the
    // widget call is not, and the writer list is still growing.
    [[nodiscard]] auto is_complete_construction() const -> bool { return ctor_arg && ctor_closed && is_closed; }
};

struct TypeRef {
    std::string name;  // name after au:: (without au::), namespace segments included
    size_t line = 0;
    size_t col = 0;  // starting column of au:: (the name starts at col+3)
};

struct Document {
    std::vector<Block> blocks;
    std::vector<Chain> chains;
    std::vector<TypeRef> refs;
};

// One past the last column of a property use, accounting for the `set_` prefix kept in chains.
[[nodiscard]] inline auto prop_use_end(const PropUse &u) -> size_t {
    return u.col + (u.len != 0 ? u.len : u.name.size());
}

// Whether (line,col) falls inside a source span. An unclosed span carries the (-1,-1) sentinel and is
// treated as extending to the end of the document.
[[nodiscard]] inline auto span_contains(size_t start_line, size_t start_col, size_t end_line, size_t end_col,
                                        bool is_closed, size_t line, size_t col) -> bool {
    if (line < start_line || (is_closed && line > end_line)) {
        return false;
    }
    if (line == start_line && col < start_col) {
        return false;
    }
    if (is_closed && line == end_line && col > end_col) {
        return false;
    }
    return true;
}

[[nodiscard]] inline auto pos_inside(const Block &b, size_t line, size_t col) -> bool {
    return span_contains(b.start_line, b.start_col, b.end_line, b.end_col, b.is_closed, line, col);
}

[[nodiscard]] inline auto pos_inside(const Chain &c, size_t line, size_t col) -> bool {
    return span_contains(c.start_line, c.start_col, c.end_line, c.end_col, c.is_closed, line, col);
}

[[nodiscard]] inline auto is_ident_char(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] inline auto is_space(char c) -> bool { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// NOLINTBEGIN(*-pro-bounds-avoid-unchecked-container-access)

// Small helper: join strings.
[[nodiscard]] inline auto join(const std::vector<std::string> &vs, const std::string &sep) -> std::string {
    std::string r;
    for (size_t i = 0; i < vs.size(); ++i) {
        if (i != 0U) {
            r += sep;
        }
        r += vs[i];
    }
    return r;
}

// Map (line,col) to a byte offset in text (0-based, counted in characters).
[[nodiscard]] inline auto text_pos_of(const std::string &text, size_t line, size_t col) -> size_t {
    size_t cur_line = 0;
    size_t cur_col = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (cur_line == line && cur_col == col) {
            return i;
        }
        if (text[i] == '\n') {
            ++cur_line;
            cur_col = 0;
        } else {
            ++cur_col;
        }
    }
    return text.size();
}

// Skip comments and string literals to avoid mistaking "au::" inside a string for a reference.
// Returns the advanced index (pointing at the start of non-skipped content, or text.size()).
[[nodiscard]] inline auto skip_trivia(const std::string &text, size_t i) -> size_t {
    const size_t n = text.size();
    if (i + 1 < n) {
        if (text[i] == '/' && text[i + 1] == '/') {
            size_t j = i + 2;
            while (j < n && text[j] != '\n') {
                ++j;
            }
            return j;
        }
        if (text[i] == '/' && text[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && (text[j] != '*' || text[j + 1] != '/')) {
                ++j;
            }
            return j + 1 < n ? j + 2 : n;
        }
        if (text[i] == '"') {
            size_t j = i + 1;
            while (j < n && text[j] != '"') {
                if (text[j] == '\\') {
                    ++j;  // skip escapes
                }
                ++j;
            }
            return (j < n) ? j + 1 : n;
        }
        if (text[i] == '\'') {
            size_t j = i + 1;
            while (j < n && text[j] != '\'') {
                if (text[j] == '\\') {
                    ++j;
                }
                ++j;
            }
            return (j < n) ? j + 1 : n;
        }
    }
    return i;
}

// Index of the delimiter matching the one at `open`, or text.size() when it is never closed.
// Comments and literals are skipped so that a bracket inside them does not pair up.
[[nodiscard]] inline auto match_delim(const std::string &text, size_t open, char oc, char cc) -> size_t {
    const size_t n = text.size();
    size_t depth = 0;
    for (size_t i = open; i < n;) {
        const size_t s = skip_trivia(text, i);
        if (s != i) {
            i = s;
            continue;
        }
        if (text[i] == oc) {
            ++depth;
        } else if (text[i] == cc) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
        ++i;
    }
    return n;
}

// End of a designated initializer's value: the first top-level ',' or the block's closing '}'.
[[nodiscard]] inline auto designated_value_end(const std::string &text, size_t start) -> size_t {
    const size_t n = text.size();
    size_t depth = 0;
    for (size_t i = start; i < n;) {
        const size_t s = skip_trivia(text, i);
        if (s != i) {
            i = s;
            continue;
        }
        const char c = text[i];
        if (depth == 0 && (c == ',' || c == '}')) {
            return i;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            --depth;
        }
        ++i;
    }
    return n;
}

[[nodiscard]] inline auto trim(const std::string &s) -> std::string {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(s[b])) {
        ++b;
    }
    while (e > b && is_space(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

// Read a qualified name starting at `start`, consuming `::` segments (au::webview::WebViewProps,
// au::Alignment::Center). Returns one past the last character consumed.
[[nodiscard]] inline auto read_qualified_name(const std::string &text, size_t start, std::string &name) -> size_t {
    const size_t n = text.size();
    size_t i = start;
    while (true) {
        size_t e = i;
        while (e < n && is_ident_char(text[e])) {
            ++e;
        }
        if (e == i) {
            break;
        }
        if (!name.empty()) {
            name += "::";
        }
        name += text.substr(i, e - i);
        i = e;
        if (i + 2 >= n || text[i] != ':' || text[i + 1] != ':' || !is_ident_char(text[i + 2])) {
            break;
        }
        i += 2;
    }
    return i;
}

// Key of the writer call whose argument list encloses `pos`, or empty when there is none. Used to
// complete enum values inside `.set_align(`. Scans backwards for the innermost unmatched '('.
[[nodiscard]] inline auto writer_arg_key(const std::string &text, size_t pos) -> std::string {
    size_t depth = 0;
    for (size_t i = pos; i > 0; --i) {
        const char c = text[i - 1];
        if (c == ')') {
            ++depth;
        } else if (c == '(') {
            if (depth > 0) {
                --depth;
                continue;
            }
            size_t e = i - 1;
            while (e > 0 && is_space(text[e - 1])) {
                --e;
            }
            size_t s = e;
            while (s > 0 && is_ident_char(text[s - 1])) {
                --s;
            }
            const std::string ident = text.substr(s, e - s);
            if (s == 0 || text[s - 1] != '.' || ident.size() <= 4 || !ident.starts_with("set_")) {
                return {};
            }
            return ident.substr(4);
        }
    }
    return {};
}

// Parse the document and build chains / blocks / refs.
[[nodiscard]] inline auto analyze(const std::string &text) -> Document {
    Document doc;
    const size_t n = text.size();
    std::vector<Block> open;  // nesting stack
    size_t depth = 0;
    // Open au::<Widget>( ... ) constructions, innermost last. A Props chain anywhere in such an
    // argument list is a construction argument — including the second position of the kids-first
    // form au::Column({ ... }, au::ColumnProps().set_gap(8)). Tracking it forward (rather than
    // scanning backwards from the chain) keeps comments and literals out of the decision.
    struct WidgetCall {
        std::string name;
        size_t depth_was = 0;  // paren depth before the call's '('
        size_t paren = 0;  // index of the call's '(' — lets a chain test whether the call is closed
    };
    std::vector<WidgetCall> calls;
    size_t paren_depth = 0;
    [[maybe_unused]] size_t line = 0;

    // Widget spelling carried by a Props name: ImageViewProps -> ImageView, webview::WebViewProps ->
    // WebView. Purely textual, so the analyzer needs no library link.
    auto widget_of_props = [](const std::string &props_name) -> std::string {
        const auto sep = props_name.rfind("::");
        const std::string seg = sep == std::string::npos ? props_name : props_name.substr(sep + 2);
        if (seg.size() <= 5 || !seg.ends_with("Props")) {
            return {};
        }
        return seg.substr(0, seg.size() - 5);
    };

    auto line_col_of = [&](size_t idx) -> std::pair<size_t, size_t> {
        // Simple line number: count from the start (the caller uses it once, the cost is acceptable).
        size_t ln = 0;
        size_t col = 0;
        for (size_t k = 0; k < idx && k < n; ++k) {
            if (text[k] == '\n') {
                ++ln;
                col = 0;
            } else {
                ++col;
            }
        }
        return {ln, col};
    };

    for (size_t i = 0; i < n;) {
        size_t const s = skip_trivia(text, i);
        if (s != i) {
            // The skipped content may span lines; the line number is updated approximately
            // (line counting inside comments/strings has negligible impact).
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

        // Line number advance (handle newlines in ordinary characters).
        if (text[i] == '\n') {
            ++line;
            ++i;
            continue;
        }

        if (text[i] == '(') {
            ++paren_depth;
            ++i;
            continue;
        }
        if (text[i] == ')') {
            if (paren_depth > 0) {
                --paren_depth;
            }
            while (!calls.empty() && paren_depth <= calls.back().depth_was) {
                calls.pop_back();
            }
            ++i;
            continue;
        }

        if (text[i] == '{') {
            ++depth;
            ++i;
            continue;
        }
        if (text[i] == '}') {
            if (!open.empty() && depth == open.back().start_depth_was + 1) {
                // Close the innermost block (its opening brace raised the depth from start_depth_was to +1).
                Block b = open.back();
                open.pop_back();
                b.is_closed = true;
                auto [el, ec] = line_col_of(i);
                b.end_line = el;
                b.end_col = ec;
                doc.blocks.push_back(std::move(b));
            }
            if (depth > 0) {
                --depth;
            }
            ++i;
            continue;
        }

        // Detect au:: references / block openings / Props chains.
        if (i + 3 < n && text[i] == 'a' && text[i + 1] == 'u' && text[i + 2] == ':' && text[i + 3] == ':') {
            std::string name;
            const size_t j = read_qualified_name(text, i + 4, name);
            // After skipping whitespace, determine whether '{' or '(' follows.
            size_t k = j;
            while (k < n && is_space(text[k])) {
                ++k;
            }
            if (k < n && text[k] == '{') {
                Block b;
                b.type = name;
                b.is_property_block = name.ends_with("Props");
                auto [sl, sc] = line_col_of(i);
                b.start_line = sl;
                b.start_col = sc;
                b.start_depth_was = depth;  // record the depth before the opening brace
                open.push_back(std::move(b));
                // handle this '{': depth +1, i jumps to k+1.
                ++depth;
                i = k + 1;
                continue;
            }
            if (k < n && text[k] == '(' && name.ends_with("Props")) {
                Chain ch;
                ch.type = name;
                ch.ctor_arg = !calls.empty() && calls.back().name == widget_of_props(name);
                ch.ctor_closed = ch.ctor_arg && match_delim(text, calls.back().paren, '(', ')') < n;
                auto [sl, sc] = line_col_of(i);
                ch.start_line = sl;
                ch.start_col = sc;
                const size_t close = match_delim(text, k, '(', ')');
                ch.is_closed = close < n;
                // The Props constructor itself takes no arguments; the writers follow its ')'.
                size_t p = close < n ? close + 1 : n;
                size_t end_idx = p;
                while (p < n) {
                    size_t q = p;
                    while (q < n) {
                        const size_t t = skip_trivia(text, q);
                        if (t != q) {
                            q = t;
                            continue;
                        }
                        if (is_space(text[q])) {
                            ++q;
                            continue;
                        }
                        break;
                    }
                    if (q >= n || text[q] != '.') {
                        break;
                    }
                    size_t e = q + 1;
                    while (e < n && is_ident_char(text[e])) {
                        ++e;
                    }
                    if (e == q + 1) {
                        // A trailing '.' with no name yet: keep it inside the span so that completion
                        // still resolves the chain while the user is typing.
                        end_idx = q + 1;
                        break;
                    }
                    const std::string ident = text.substr(q + 1, e - (q + 1));
                    if (ident.size() > 4 && ident.starts_with("set_")) {
                        auto [pl, pc] = line_col_of(q + 1);
                        ch.props.push_back(
                            {.name = ident.substr(4), .line = pl, .col = pc, .len = ident.size(), .value = {}});
                    }
                    size_t a = e;
                    while (a < n && is_space(text[a])) {
                        ++a;
                    }
                    if (a >= n || text[a] != '(') {
                        end_idx = e;
                        break;
                    }
                    const size_t ac = match_delim(text, a, '(', ')');
                    if (ac >= n) {
                        ch.is_closed = false;
                        end_idx = n;
                        break;
                    }
                    p = ac + 1;
                    end_idx = p;
                }
                if (ch.is_closed) {
                    auto [el, ec] = line_col_of(end_idx);
                    ch.end_line = el;
                    ch.end_col = ec;
                } else {
                    ch.end_line = static_cast<size_t>(-1);
                    ch.end_col = static_cast<size_t>(-1);
                }
                doc.chains.push_back(std::move(ch));
                auto [rl, rc] = line_col_of(i);
                doc.refs.push_back({.name = name, .line = rl, .col = rc});
                // Resume at the end of the name so that nested refs / chains inside the arguments are
                // still visited by the main loop.
                i = j;
                continue;
            }
            if (k < n && text[k] == '(' && !name.empty()) {
                // A widget construction (or any other au:: call): remember it so that a Props chain
                // in its argument list is recognized as a construction argument.
                const auto sep = name.rfind("::");
                calls.push_back({.name = sep == std::string::npos ? name : name.substr(sep + 2),
                                 .depth_was = paren_depth,
                                 .paren = k});
            }
            if (!name.empty()) {
                auto [rl, rc] = line_col_of(i);
                doc.refs.push_back({.name = name, .line = rl, .col = rc});
            }
            i = j;
            continue;
        }

        // Designated initializer .key = value (only inside a brace block). A bare `.member` or a
        // `.set_x(...)` call is not a designator and must not turn a child list into a property block.
        if (text[i] == '.' && !open.empty()) {
            size_t j = i + 1;
            while (j < n && is_ident_char(text[j])) {
                ++j;
            }
            if (j > i + 1) {
                size_t k = j;
                while (k < n && is_space(text[k])) {
                    ++k;
                }
                if (k < n && text[k] == '=' && (k + 1 >= n || text[k + 1] != '=')) {
                    std::string const p_name = text.substr(i + 1, j - (i + 1));
                    auto [pl, pc] = line_col_of(i + 1);
                    const size_t ve = designated_value_end(text, k + 1);
                    open.back().props.push_back({.name = p_name,
                                                 .line = pl,
                                                 .col = pc,
                                                 .len = 0,
                                                 .value = trim(text.substr(k + 1, ve - (k + 1)))});
                    open.back().is_property_block = true;
                }
                i = j;
                continue;
            }
        }

        ++i;
    }
    // Blocks still unclosed at the end of the document: mark them unclosed (end is a sentinel) and add
    // them to the document model, so completion contexts (the user is still typing and has not entered
    // '}') can still locate the block.
    for (auto &b : open) {
        b.is_closed = false;
        b.end_line = static_cast<size_t>(-1);
        b.end_col = static_cast<size_t>(-1);
        doc.blocks.push_back(std::move(b));
    }
    return doc;
}

// NOLINTEND(*-pro-bounds-avoid-unchecked-container-access)

}  // namespace aurora::tools

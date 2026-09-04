// =============================================================================
// lsp_document.h — Document model layer of the LSP analyzer (header-only, decoupled from the library).
// -----------------------------------------------------------------------------
// Parses source text into a Document model made of Block / TypeRef, together with the lexical
// helpers needed for parsing (pos_inside / is_ident_char / join / text_pos_of / skip_trivia).
// Depends on the type definitions in lsp_schema.h and the semantic services in lsp_features.h
// (see those files).
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace aurora::tools {

// ----------------------------- Document model ---------------------------------
struct PropUse {
    std::string name;
    size_t line = 0;
    size_t col = 0;  // starting column of the property name (the dot is before name)
};

struct Block {
    std::string type;  // the raw name after au::, e.g. "Button" / "ButtonProps"
    bool is_props = false;  // whether type ends with Props
    bool is_closed =
        false;  // whether a matching closing brace was found (unclosed blocks are used for completion context)
    size_t start_line = 0, start_col = 0;
    size_t end_line = 0, end_col = 0;
    size_t start_depth_was = 0;  // brace depth before the opening brace
    std::vector<PropUse> props;  // designated-initializer properties appearing inside the block
};

struct TypeRef {
    std::string name;  // name after au:: (without au::)
    size_t line = 0;
    size_t col = 0;  // starting column of au:: (the name starts at col+3)
};

struct Document {
    std::vector<Block> blocks;
    std::vector<TypeRef> refs;
};

// Whether (line,col) falls inside the block range (inclusive).
[[nodiscard]] inline auto pos_inside(const Block &b, size_t line, size_t col) -> bool {
    if (line < b.start_line || line > b.end_line) {
        return false;
    }
    if (line == b.start_line && col < b.start_col) {
        return false;
    }
    // An unclosed block (end is a sentinel) is treated as extending to the end of the document.
    if (b.is_closed && line == b.end_line && col > b.end_col) {
        return false;
    }
    return true;
}

[[nodiscard]] inline auto is_ident_char(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

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

// Parse the document and build blocks / refs.
[[nodiscard]] inline auto analyze(const std::string &text) -> Document {
    Document doc;
    const size_t n = text.size();
    std::vector<Block> open;  // nesting stack
    size_t depth = 0;
    [[maybe_unused]] size_t line = 0;

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
        size_t s = skip_trivia(text, i);
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

        // Detect au:: references / block openings.
        if (i + 3 < n && text[i] == 'a' && text[i + 1] == 'u' && text[i + 2] == ':' && text[i + 3] == ':') {
            size_t j = i + 4;
            while (j < n && is_ident_char(text[j])) {
                ++j;
            }
            std::string name = text.substr(i + 4, j - (i + 4));
            // After skipping whitespace, determine whether '{' follows.
            size_t k = j;
            while (k < n && (text[k] == ' ' || text[k] == '\t')) {
                ++k;
            }
            if (k < n && text[k] == '{') {
                Block b;
                b.type = name;
                b.is_props = (name.size() > 5 && name.ends_with("Props"));
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
            if (!name.empty()) {
                auto [rl, rc] = line_col_of(i);
                doc.refs.push_back({.name = name, .line = rl, .col = rc});
            }
            i = j;
            continue;
        }

        // Designated initializer .prop (only inside a block).
        if (text[i] == '.' && !open.empty()) {
            size_t j = i + 1;
            while (j < n && is_ident_char(text[j])) {
                ++j;
            }
            if (j > i + 1) {
                std::string p_name = text.substr(i + 1, j - (i + 1));
                auto [pl, pc] = line_col_of(i + 1);
                open.back().props.push_back({.name = p_name, .line = pl, .col = pc});
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

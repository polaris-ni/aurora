// =============================================================================
// lsp_document.h — LSP 分析器的 Document 模型层（header-only，与库解耦）。
// -----------------------------------------------------------------------------
// 将源码文本解析为 Block / TypeRef 构成的 Document 模型，并附带解析所需的词法辅助
// （pos_inside / is_ident_char / join / text_pos_of / skip_trivia）。
// 依赖 lsp_schema.h 的类型定义与 lsp_features.h 的语义服务（见各文件）。
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace aurora::tools {

// ----------------------------- Document 模型 ---------------------------------
struct PropUse {
    std::string name;
    size_t line = 0;
    size_t col = 0; // 属性名起始列（点在 name 之前）
};

struct Block {
    std::string type;       // 原始 au:: 后的名字，如 "Button" / "ButtonProps"
    bool is_props = false;  // type 是否以 Props 结尾
    bool is_closed = false; // 是否遇到匹配的右括号（未闭合块用于补全上下文）
    size_t start_line = 0, start_col = 0;
    size_t end_line = 0, end_col = 0;
    size_t start_depth_was = 0; // 开括号前的括号深度
    std::vector<PropUse> props; // 块内出现的指定初始化器属性
};

struct TypeRef {
    std::string name; // au:: 后的名字（不含 au::）
    size_t line = 0;
    size_t col = 0; // au:: 起始列（名字从 col+3 开始）
};

struct Document {
    std::vector<Block> blocks;
    std::vector<TypeRef> refs;
};

// 判断 (line,col) 是否落在 block 区间内（闭区间）。
[[nodiscard]] inline auto pos_inside(const Block &b, size_t line, size_t col) -> bool {
    if (line < b.start_line || line > b.end_line) return false;
    if (line == b.start_line && col < b.start_col) return false;
    // 未闭合块（end 为哨兵）视为延续到文档末尾。
    if (b.is_closed && line == b.end_line && col > b.end_col) return false;
    return true;
}

[[nodiscard]] inline auto is_ident_char(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// 小工具：连接字符串。
[[nodiscard]] inline auto join(const std::vector<std::string> &vs, const std::string &sep) -> std::string {
    std::string r;
    for (size_t i = 0; i < vs.size(); ++i) {
        if (i) r += sep;
        r += vs[i];
    }
    return r;
}

// 将 (line,col) 映射为 text 中的字节偏移（0-based，按字符计）。
[[nodiscard]] inline auto text_pos_of(const std::string &text, size_t line, size_t col) -> size_t {
    size_t cur_line = 0, cur_col = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (cur_line == line && cur_col == col) return i;
        if (text[i] == '\n') {
            ++cur_line;
            cur_col = 0;
        } else {
            ++cur_col;
        }
    }
    return text.size();
}

// 跳过注释与字符串字面量，避免误把字符串里的 "au::" 当成引用。
// 返回推进后的索引（指向非跳过内容起始，或 text.size()）。
[[nodiscard]] inline auto skip_trivia(const std::string &text, size_t i) -> size_t {
    const size_t n = text.size();
    if (i + 1 < n) {
        if (text[i] == '/' && text[i + 1] == '/') {
            size_t j = i + 2;
            while (j < n && text[j] != '\n')
                ++j;
            return j;
        }
        if (text[i] == '/' && text[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(text[j] == '*' && text[j + 1] == '/'))
                ++j;
            return (j + 1 < n) ? j + 2 : n;
        }
        if (text[i] == '"') {
            size_t j = i + 1;
            while (j < n && text[j] != '"') {
                if (text[j] == '\\') ++j; // 跳过转义
                ++j;
            }
            return (j < n) ? j + 1 : n;
        }
        if (text[i] == '\'') {
            size_t j = i + 1;
            while (j < n && text[j] != '\'') {
                if (text[j] == '\\') ++j;
                ++j;
            }
            return (j < n) ? j + 1 : n;
        }
    }
    return i;
}

// 解析文档，构建 blocks / refs。
[[nodiscard]] inline auto analyze(const std::string &text) -> Document {
    Document doc;
    const size_t n = text.size();
    std::vector<Block> open; // 嵌套栈
    size_t depth = 0;
    [[maybe_unused]] size_t line = 0;

    auto line_col_of = [&](size_t idx) -> std::pair<size_t, size_t> {
        // 简单行号：从头计数（调用方单次使用，开销可接受）。
        size_t ln = 0, col = 0;
        for (size_t k = 0; k < idx && k < n; ++k) {
            if (text[k] == '\n') {
                ++ln;
                col = 0;
            } else {
                ++col;
            }
        }
        return { ln, col };
    };

    for (size_t i = 0; i < n;) {
        size_t s = skip_trivia(text, i);
        if (s != i) {
            // 跳过的内容可能跨行，更新行号近似（仅用于注释/字符串内的行计数影响很小）。
            for (size_t k = i; k < s; ++k)
                if (text[k] == '\n') ++line;
            i = s;
            if (i >= n) break;
        }

        // 行号推进（处理普通字符换行）。
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
                // 关闭最内层块（其开括号使深度从 start_depth_was 升到 +1）。
                Block b = open.back();
                open.pop_back();
                b.is_closed = true;
                auto [el, ec] = line_col_of(i);
                b.end_line = el;
                b.end_col = ec;
                doc.blocks.push_back(std::move(b));
            }
            if (depth > 0) --depth;
            ++i;
            continue;
        }

        // 检测 au:: 引用 / 块开启。
        if (i + 3 < n && text[i] == 'a' && text[i + 1] == 'u' && text[i + 2] == ':' && text[i + 3] == ':') {
            size_t j = i + 4;
            while (j < n && is_ident_char(text[j]))
                ++j;
            std::string name = text.substr(i + 4, j - (i + 4));
            // 跳过空白后判断是否接 '{'。
            size_t k = j;
            while (k < n && (text[k] == ' ' || text[k] == '\t'))
                ++k;
            if (k < n && text[k] == '{') {
                Block b;
                b.type = name;
                b.is_props = (name.size() > 5 && name.compare(name.size() - 5, 5, "Props") == 0);
                auto [sl, sc] = line_col_of(i);
                b.start_line = sl;
                b.start_col = sc;
                b.start_depth_was = depth; // 记录开括号前的深度
                open.push_back(std::move(b));
                // 处理这个 '{'：深度 +1，i 跳到 k+1。
                ++depth;
                i = k + 1;
                continue;
            } else {
                if (!name.empty()) {
                    auto [rl, rc] = line_col_of(i);
                    doc.refs.push_back({ name, rl, rc });
                }
                i = j;
                continue;
            }
        }

        // 指定初始化器 .prop（仅在块内）。
        if (text[i] == '.' && !open.empty()) {
            size_t j = i + 1;
            while (j < n && is_ident_char(text[j]))
                ++j;
            if (j > i + 1) {
                std::string pname = text.substr(i + 1, j - (i + 1));
                auto [pl, pc] = line_col_of(i + 1);
                open.back().props.push_back({ pname, pl, pc });
                i = j;
                continue;
            }
        }

        ++i;
    }
    // 文档结束仍有未闭合块：标记为未闭合（end 为哨兵），并加入文档模型，
    // 以便补全上下文（用户正在输入、尚未敲下 '}'）仍能定位到块内。
    for (auto &b : open) {
        b.is_closed = false;
        b.end_line = static_cast<size_t>(-1);
        b.end_col = static_cast<size_t>(-1);
        doc.blocks.push_back(std::move(b));
    }
    return doc;
}

} // namespace aurora::tools

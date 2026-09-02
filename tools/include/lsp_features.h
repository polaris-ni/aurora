// =============================================================================
// lsp_features.h — LSP 分析器的语义服务层（header-only，与库解耦）。
// -----------------------------------------------------------------------------
// 基于 lsp_document.h 的 Document 模型与 lsp_schema.h 的 Schema，提供
// 补全 / 悬停 / 诊断 / 枚举值校验 / 代码动作，以及它们依赖的辅助
// （innermost_block / line_prefix）。不做完整 C++ 解析，仅针对 Aurora 声明式 UI 的常见写法。
// 约定：行/列均为 0-based，与 LSP 协议一致。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "lsp_document.h"
#include "lsp_schema.h"

namespace aurora::tools {

// --------------------------- 补全 / 悬停 / 诊断 ------------------------------
struct CompletionItem {
    std::string label;  // 补全文本
    std::string kind;   // "Property" / "Class" / "Enum" / "EnumMember"
    std::string detail; // 类型 / 默认值
    std::string documentation;
};

struct HoverInfo {
    std::string content; // markdown 风格纯文本
};

struct Diagnostic {
    enum class Severity { Error = 1, Warning = 2, Information = 3, Hint = 4 };
    size_t line = 0;
    size_t col = 0;
    size_t end_line = 0;
    size_t end_col = 0;
    Severity severity = Severity::Error;
    std::string message;
};

struct TextEdit {
    size_t line = 0; // 插入行（0-based）
    size_t col = 0;  // 插入列
    std::string new_text;
};

struct CodeAction {
    std::string title;
    std::vector<TextEdit> edits;
};

// 找出包含 (line,col) 的最内层块（嵌套越深越靠后，取最后一个匹配）。
[[nodiscard]] inline auto innermost_block(const Document &doc, size_t line, size_t col) -> const Block * {
    const Block *best = nullptr;
    for (const auto &b : doc.blocks) {
        if (pos_inside(b, line, col)) {
            if (!best || b.start_line > best->start_line ||
                (b.start_line == best->start_line && b.start_col > best->start_col))
                best = &b;
        }
    }
    return best;
}

[[nodiscard]] inline auto line_prefix(const std::string &text, size_t pos) -> std::string {
    // 取 pos 之前的本行内容。
    size_t start = pos;
    while (start > 0 && text[start - 1] != '\n')
        --start;
    return text.substr(start, pos - start);
}

// 在 (line,col) 处提供补全。text 用于推导触发上下文。
[[nodiscard]] inline auto completions(const std::string &text, const Document &doc, const Schema &schema, size_t line,
                                      size_t col) -> std::vector<CompletionItem> {
    std::vector<CompletionItem> out;
    const std::string prefix = line_prefix(text, text_pos_of(text, line, col));

    // 正在键入的标识符。
    size_t wend = prefix.size();
    size_t wstart = wend;
    while (wstart > 0 && is_ident_char(prefix[wstart - 1]))
        --wstart;
    const std::string word = prefix.substr(wstart, wend - wstart);

    const bool after_au = [&]() -> bool {
        // prefix 中以 au:: 开头且与 word 之间无 '.' 或 '::'。
        auto p = prefix.rfind("au::");
        if (p == std::string::npos) return false;
        // 检查 au::（占 4 个字符，[p, p+3]）与 word 之间只有空白或就是紧挨着。
        for (size_t x = p + 4; x < wstart; ++x)
            if (prefix[x] != ' ' && prefix[x] != '\t') return false;
        return true;
    }();

    const bool after_dot = (wstart > 0 && prefix[wstart - 1] == '.');

    // 1) au:: 之后 → 补控件类型 + 枚举类型。
    if (after_au) {
        for (const auto &c : schema.components) {
            if (word.empty() || c.type.rfind(word, 0) == 0) {
                CompletionItem it;
                it.label = c.type;
                it.kind = "Class";
                it.detail = c.category;
                it.documentation = "children_policy: " + c.children_policy +
                                   (c.props.empty() ? "" : ("\n属性 " + std::to_string(c.props.size()) + " 个"));
                out.push_back(std::move(it));
            }
        }
        for (const auto &e : schema.enums) {
            if (word.empty() || e.name.rfind(word, 0) == 0) {
                CompletionItem it;
                it.label = e.name;
                it.kind = "Enum";
                it.detail = "enum";
                it.documentation = "取值: " + join(e.values, ", ");
                out.push_back(std::move(it));
            }
        }
        return out;
    }

    // 2) 块内 '.' 之后 → 补属性（排除已用的）。
    if (after_dot) {
        const Block *b = innermost_block(doc, line, col);
        if (b) {
            const std::string ctype = Schema::strip_props(b->type);
            const auto *comp = schema.find_component(ctype);
            if (comp) {
                for (const auto &p : comp->props) {
                    bool used = false;
                    for (const auto &u : b->props)
                        if (u.name == p.name) {
                            used = true;
                            break;
                        }
                    if (used) continue;
                    if (!word.empty() && p.name.rfind(word, 0) != 0) continue;
                    CompletionItem it;
                    it.label = p.name;
                    it.kind = "Property";
                    it.detail = p.type + (p.required ? " (必填)" : "");
                    it.documentation = (p.default_value.empty() ? "" : ("默认: " + p.default_value + "\n")) + p.note;
                    out.push_back(std::move(it));
                }
            }
        }
        return out;
    }

    // 3) 枚举成员补全：au::Enum:: 或 块内 枚举属性 =
    // 3a) au::Enum:: 形态。
    auto dcolon = prefix.rfind("::");
    if (dcolon != std::string::npos) {
        // 取 :: 前的标识符作为枚举名（可能带 au::）。
        size_t es = dcolon;
        while (es > 0 && is_ident_char(prefix[es - 1]))
            --es;
        std::string ename = prefix.substr(es, dcolon - es);
        const auto *en = schema.find_enum(ename);
        if (!en) en = schema.find_enum(Schema::strip_props(ename));
        if (en) {
            for (const auto &v : en->values) {
                if (word.empty() || v.rfind(word, 0) == 0) {
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

    // 3b) 块内枚举属性 = 之后。
    {
        const Block *b = innermost_block(doc, line, col);
        if (b) {
            const std::string ctype = Schema::strip_props(b->type);
            const auto *comp = schema.find_component(ctype);
            if (comp) {
                // 找 = 之前最近的 .prop。
                size_t eq = prefix.rfind('=');
                if (eq != std::string::npos) {
                    size_t ds = prefix.rfind('.', eq);
                    if (ds != std::string::npos) {
                        size_t pe = ds + 1;
                        while (pe < eq && is_ident_char(prefix[pe]))
                            ++pe;
                        std::string pname = prefix.substr(ds + 1, pe - (ds + 1));
                        for (const auto &p : comp->props) {
                            if (p.name == pname) {
                                const auto *en = schema.find_enum(p.type);
                                if (en) {
                                    for (const auto &v : en->values) {
                                        if (word.empty() || v.rfind(word, 0) == 0) {
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

// 悬停：(line,col) 落在 au::Type 上 → 控件概要；落在块内 .prop 上 → 属性说明。
[[nodiscard]] inline auto hover(const Document &doc, const Schema &schema, size_t line, size_t col)
    -> std::optional<HoverInfo> {
    // 控件类型引用。
    for (const auto &r : doc.refs) {
        const size_t name_start = r.col + 3;
        if (line == r.line && col >= name_start && col <= name_start + r.name.size()) {
            if (const auto *c = schema.find_component(Schema::strip_props(r.name))) {
                HoverInfo h;
                h.content = "**" + c->type + "** (" + c->category + ")\n" + "children_policy: " + c->children_policy +
                            "\n" + "属性 " + std::to_string(c->props.size()) + " / 事件 " +
                            std::to_string(c->events.size());
                return h;
            }
            if (const auto *e = schema.find_enum(r.name)) {
                HoverInfo h;
                h.content = "**enum " + e->name + "**\n取值: " + join(e->values, ", ");
                return h;
            }
        }
    }
    // 块内属性。
    const Block *b = innermost_block(doc, line, col);
    if (b) {
        const std::string ctype = Schema::strip_props(b->type);
        const auto *comp = schema.find_component(ctype);
        if (comp) {
            for (const auto &u : b->props) {
                if (line == u.line && col >= u.col && col <= u.col + u.name.size()) {
                    for (const auto &p : comp->props) {
                        if (p.name == u.name) {
                            HoverInfo h;
                            h.content = "**" + p.name + "**: `" + p.type + "`" + (p.required ? " (必填)" : "") + "\n" +
                                        (p.default_value.empty() ? "" : ("默认: `" + p.default_value + "`\n")) + p.note;
                            return h;
                        }
                    }
                }
            }
        }
    }
    return std::nullopt;
}

// 诊断：未知类型、未知属性、未知枚举值、缺失必填属性。
[[nodiscard]] inline auto diagnostics(const Document &doc, const Schema &schema) -> std::vector<Diagnostic> {
    std::vector<Diagnostic> out;

    for (const auto &r : doc.refs) {
        if (schema.is_known_component(r.name) || schema.is_known_enum(r.name)) continue;
        Diagnostic d;
        d.line = r.line;
        d.col = r.col + 3;
        d.end_line = r.line;
        d.end_col = r.col + 3 + r.name.size();
        d.severity = Diagnostic::Severity::Error;
        d.message = "未知 Aurora 类型: au::" + r.name;
        out.push_back(d);
    }

    for (const auto &b : doc.blocks) {
        const std::string ctype = Schema::strip_props(b.type);
        const auto *comp = schema.find_component(ctype);
        if (!comp) continue; // 未知控件不校验属性
        // 未知属性。
        for (const auto &u : b.props) {
            bool known = false;
            for (const auto &p : comp->props)
                if (p.name == u.name) {
                    known = true;
                    break;
                }
            if (!known) {
                Diagnostic d;
                d.line = u.line;
                d.col = u.col;
                d.end_line = u.line;
                d.end_col = u.col + u.name.size();
                d.severity = Diagnostic::Severity::Error;
                d.message = "控件 " + ctype + " 无属性: " + u.name;
                out.push_back(d);
            }
        }
        // 缺失必填属性（warning，仅对已闭合块：未闭合块 end 为哨兵，待闭合后由 didChange 重算）。
        for (const auto &p : comp->props) {
            if (!p.required) continue;
            bool used = false;
            for (const auto &u : b.props)
                if (u.name == p.name) {
                    used = true;
                    break;
                }
            if (!used && b.is_closed) {
                Diagnostic d;
                d.line = b.end_line;
                d.col = b.end_col;
                d.end_line = b.end_line;
                d.end_col = b.end_col + 1;
                d.severity = Diagnostic::Severity::Warning;
                d.message = "控件 " + ctype + " 缺少必填属性: " + p.name;
                out.push_back(d);
            }
        }
    }

    return out;
}

// 枚举值字面量校验（需原文）：au::Enum::Value 或 Enum::Value。
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
        size_t s = skip_trivia(text, i);
        if (s != i) {
            for (size_t k = i; k < s; ++k)
                if (text[k] == '\n') ++line;
            i = s;
            if (i >= n) break;
        }
        // 找 :: 之前的标识（枚举名）。
        if (text[i] == ':' && i + 1 < n && text[i + 1] == ':') {
            size_t es = i;
            while (es > 0 && is_ident_char(text[es - 1]))
                --es;
            std::string ename = text.substr(es, i - es);
            size_t vs = i + 2;
            while (vs < n && is_ident_char(text[vs]))
                ++vs;
            std::string vname = text.substr(i + 2, vs - (i + 2));
            const auto *en = schema.find_enum(ename);
            if (!en) en = schema.find_enum(Schema::strip_props(ename));
            if (en && !vname.empty()) {
                bool ok = false;
                for (const auto &v : en->values)
                    if (v == vname) {
                        ok = true;
                        break;
                    }
                if (!ok) {
                    Diagnostic d;
                    d.line = line;
                    d.col = static_cast<size_t>(i + 2);
                    d.end_line = line;
                    d.end_col = static_cast<size_t>(vs);
                    d.severity = Diagnostic::Severity::Error;
                    d.message = "枚举 " + en->name + " 无取值: " + vname;
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

// 代码动作：为缺失必填属性的块插入 .prop = <default>。
[[nodiscard]] inline auto code_actions(const Document &doc, const Schema &schema) -> std::vector<CodeAction> {
    std::vector<CodeAction> out;
    for (const auto &b : doc.blocks) {
        if (!b.is_closed) continue; // 仅对已闭合块提供插入（插入点在 '}' 之前）
        const std::string ctype = Schema::strip_props(b.type);
        const auto *comp = schema.find_component(ctype);
        if (!comp) continue;
        std::vector<const PropSchema *> missing;
        for (const auto &p : comp->props) {
            if (!p.required) continue;
            bool used = false;
            for (const auto &u : b.props)
                if (u.name == p.name) {
                    used = true;
                    break;
                }
            if (!used) missing.push_back(&p);
        }
        if (missing.empty()) continue;

        std::string insert;
        for (const auto &p : missing) {
            insert += "  ." + p->name + " = " + (p->default_value.empty() ? "\"\"" : p->default_value) + ",\n";
        }
        CodeAction ca;
        ca.title = "为 " + ctype + " 补全缺失必填属性";
        ca.edits.push_back(TextEdit{ b.end_line, b.end_col, insert });
        out.push_back(std::move(ca));
    }
    return out;
}

} // namespace aurora::tools

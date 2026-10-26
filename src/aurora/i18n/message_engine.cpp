#include "aurora/i18n/detail/message_engine.h"

#include <string>
#include <string_view>
#include <utility>

#include "aurora/i18n/plural.h"

namespace aurora::detail {
namespace {

constexpr int AURORA_MAX_DEPTH = 8;  // 选择器嵌套深度上限（生成器同口径校验）

/// @brief 十进制整数字符串 → long long：与 `std::strtoll` 对纯数字输入同结果（前导空白与 `+`/`-`
/// 号照收，首个非数字处停下），但不需要 NUL 结尾，故调用侧不必为切出来的子串再分配一份 string。
/// @param text 待解析的十进制片段。
/// @return 解析值；空串或无数字时为 0（与 strtoll 同口径）。
auto parse_decimal_int(std::string_view text) -> long long {
    std::size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    const bool negative = pos < text.size() && text[pos] == '-';
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
        ++pos;
    }
    unsigned long long magnitude = 0;
    for (; pos < text.size(); ++pos) {
        if (text[pos] < '0' || text[pos] > '9') {
            break;
        }
        if (magnitude <= 900000000000000000ULL) {  // 饱和防溢出（与 plural.cpp 的操作数解析同口径）
            magnitude = (magnitude * 10U) + static_cast<unsigned long long>(text[pos] - '0');
        }
    }
    return negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
}

/// @brief 找到与 start 配平的 '}'。
auto find_closing_brace(std::string_view s, std::size_t start) -> std::size_t {
    int depth = 0;
    for (std::size_t k = start; k < s.size(); ++k) {
        if (s[k] == '{') {
            ++depth;
        } else if (s[k] == '}') {
            --depth;
            if (depth == 0) {
                return k;
            }
        }
    }
    return std::string_view::npos;
}

/// @brief MF1 引号子集（ICU 口径）：`''` → 字面 `'`；`'` 后跟 `{`/`}`/`#` → 转义该字符；
///        `'` 后跟其它字符 → 引用段，段内原文照抄（段内 `''` 记字面 `'` 并续段），收段引号不落地。
auto consume_quoted(std::string_view s, std::size_t &pos, std::string &out) -> void {
    if (pos + 1 >= s.size()) {
        out += '\'';  // 末尾孤立引号：无可转义对象，按字面保留
        ++pos;
        return;
    }
    const char next = s[pos + 1];
    if (next == '\'' || next == '{' || next == '}' || next == '#') {
        out += next;
        pos += 2;
        return;
    }
    ++pos;  // 越过开引号，进入引用段
    while (pos < s.size()) {
        if (s[pos] == '\'') {
            if (pos + 1 < s.size() && s[pos + 1] == '\'') {
                out += '\'';
                pos += 2;
                continue;
            }
            ++pos;  // 收段引号
            return;
        }
        out += s[pos];
        ++pos;
    }
}

/// @brief 按深度 0 的逗号切分参数内文（选择器分支的花括号不参与切分）。
auto split_top_level(std::string_view s) -> std::vector<std::string_view> {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '{') {
            ++depth;
        } else if (s[k] == '}') {
            --depth;
        } else if (s[k] == ',' && depth == 0) {
            parts.push_back(s.substr(start, k - start));
            start = k + 1;
        }
    }
    parts.push_back(s.substr(start));
    return parts;
}

/// @brief 修剪 ASCII 空白。
auto trim(std::string_view s) -> std::string_view {
    std::size_t lo = 0;
    std::size_t hi = s.size();
    while (lo < hi && (s[lo] == ' ' || s[lo] == '\t')) {
        ++lo;
    }
    while (hi > lo && (s[hi - 1] == ' ' || s[hi - 1] == '\t')) {
        --hi;
    }
    return s.substr(lo, hi - lo);
}

// 前向声明（parse 内递归消费分支体）
auto parse_into(MessageNode &out, std::string_view tmpl, int depth, bool in_selector) -> bool;

/// @brief 解析选择器分支序列（`offset:` 前缀已由调用方消费）。
auto parse_branches(MessageNode &node, std::string_view rest, int depth) -> bool {
    std::size_t pos = 0;
    while (pos < rest.size()) {
        if (rest[pos] == ' ' || rest[pos] == '\t') {
            ++pos;
            continue;
        }
        const std::size_t open = rest.find('{', pos);
        if (open == std::string_view::npos) {
            break;
        }
        std::string key(trim(rest.substr(pos, open - pos)));
        while (!key.empty() && key.front() == ',') {  // func 后的分隔逗号残留在键首（split 后逐段起点）
            key = std::string(trim(std::string_view(key).substr(1)));
        }
        if (key.empty()) {
            pos = open;  // 无键分支（防御口径）：跳过该 '{'，交由主循环按文本处理
            ++pos;
            continue;
        }
        const std::size_t close = find_closing_brace(rest, open);
        if (close == std::string_view::npos || depth + 1 > AURORA_MAX_DEPTH) {
            return false;
        }
        auto body = std::make_unique<MessageNode>();
        if (!parse_into(*body, rest.substr(open + 1, close - open - 1), depth + 1, true)) {
            return false;
        }
        node.branches.emplace_back(key, std::move(body));
        pos = close + 1;
    }
    return pos >= rest.size();
}

auto parse_into(MessageNode &out, std::string_view tmpl, int depth, bool in_selector) -> bool {
    std::size_t pos = 0;
    std::size_t seg_start = 0;  // 当前文本段起点（与扫描位置解耦：终扫时 pos==end 也要能取到段）
    auto flush_text = [&](std::size_t end) {
        if (end > seg_start) {
            auto text_node = std::make_unique<MessageNode>();
            text_node->kind = NodeKind::Text;
            text_node->text = std::string(tmpl.substr(seg_start, end - seg_start));
            out.children.push_back(std::move(text_node));
        }
        seg_start = end;
    };
    while (pos < tmpl.size()) {
        const char ch = tmpl[pos];
        if (ch == '\'') {
            flush_text(pos);
            std::string quoted;
            consume_quoted(tmpl, pos, quoted);  // pos 前进越过引号内容
            auto text_node = std::make_unique<MessageNode>();
            text_node->kind = NodeKind::Text;
            text_node->text = std::move(quoted);
            out.children.push_back(std::move(text_node));
            seg_start = pos;
            continue;
        }
        if (ch == '#' && in_selector) {
            flush_text(pos);
            auto hash_node = std::make_unique<MessageNode>();
            hash_node->kind = NodeKind::Hash;
            out.children.push_back(std::move(hash_node));
            ++pos;
            seg_start = pos;
            continue;
        }
        if (ch == '{') {
            if (depth + 1 > AURORA_MAX_DEPTH) {
                return false;
            }
            const std::size_t close = find_closing_brace(tmpl, pos);
            if (close == std::string_view::npos) {
                return false;
            }
            flush_text(pos);
            const std::string_view inner = tmpl.substr(pos + 1, close - pos - 1);
            const auto parts = split_top_level(inner);
            auto node = std::make_unique<MessageNode>();
            node->arg_name = std::string(trim(parts[0]));
            if (node->arg_name.empty()) {
                return false;
            }
            if (parts.size() == 1) {
                node->kind = NodeKind::Arg;  // 纯文本占位 {name}
            } else {
                const std::string func(trim(parts[1]));
                const bool is_selector = func == "plural" || func == "selectordinal" || func == "select";
                if (is_selector) {
                    node->kind = NodeKind::Selector;
                    node->selector_kind = func;
                    const std::size_t func_pos = inner.find(func);
                    // rest 起点先剥掉 func 与后继 token 之间的空白与分隔逗号，再判 offset 前缀。
                    std::string_view rest = trim(inner.substr(func_pos + func.size()));
                    if (!rest.empty() && rest.front() == ',') {
                        rest = trim(rest.substr(1));
                    }
                    if (rest.starts_with("offset:")) {
                        const std::size_t colon = rest.find(':');
                        const std::size_t space = rest.find_first_of(" \t", colon);
                        const std::string_view num = trim(rest.substr(
                            colon + 1, space == std::string_view::npos ? rest.size() - colon - 1 : space - colon - 1));
                        node->offset = parse_decimal_int(num);
                        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space);
                    }
                    if (!parse_branches(*node, rest, depth + 1) || node->branches.empty()) {
                        return false;
                    }
                } else {
                    node->kind = NodeKind::Arg;
                    node->func = func;
                    if (parts.size() >= 3) {
                        node->style = std::string(trim(parts[2]));
                    }
                }
            }
            out.children.push_back(std::move(node));
            pos = close + 1;
            seg_start = pos;
            continue;
        }
        ++pos;  // 普通字符：留在当前文本段内
    }
    flush_text(tmpl.size());
    return true;
}

/// @brief 取参数值（缺参返回 nullptr）。
auto find_arg(const std::vector<MessageArg> &args, std::string_view name) -> const MessageArg * {
    for (const MessageArg &arg : args) {
        if (arg.name == name) {
            return &arg;
        }
    }
    return nullptr;
}

/// @brief 参数值的文本形态：string 原样、整数 to_string、浮点按 locale 分组（缺省 2 位小数）。
auto message_arg_text(const MessageArg &arg, const Locale &loc) -> std::string {
    if (const auto *s = std::get_if<std::string>(&arg.value)) {
        return *s;
    }
    if (const auto *n = std::get_if<std::int64_t>(&arg.value)) {
        return std::to_string(*n);
    }
    if (const auto *d = std::get_if<double>(&arg.value)) {
        return format_number(*d, loc, 2);
    }
    return {};
}

/// @brief 选择器数值：int64 / double 直取；string 按十进制解析（非法 → 0）。
auto selector_number(const MessageArg &arg) -> double {
    if (const auto *n = std::get_if<std::int64_t>(&arg.value)) {
        return static_cast<double>(*n);
    }
    if (const auto *d = std::get_if<double>(&arg.value)) {
        return *d;
    }
    const auto *s = std::get_if<std::string>(&arg.value);
    return s != nullptr ? plural_operands_from_decimal(*s).n : 0.0;
}

/// @brief 货币 ISO 码 → Currency（生成器把未知码拦在构建期，此处兜底 USD）。
auto currency_from_code(std::string_view code) -> Currency {
    if (code == "EUR") {
        return Currency::EUR;
    }
    if (code == "JPY") {
        return Currency::JPY;
    }
    if (code == "CNY") {
        return Currency::CNY;
    }
    if (code == "GBP") {
        return Currency::GBP;
    }
    return Currency::USD;
}

/// @brief 日期风格 token → DateStyle。
auto date_style_from_token(std::string_view token) -> DateStyle {
    if (token == "short") {
        return DateStyle::Short;
    }
    if (token == "long") {
        return DateStyle::Long;
    }
    return DateStyle::Medium;
}

/// @brief 选择器生效值（应用 offset 后）的展示串（`#` 的取数口径，与选择判定同源）。
auto effective_number_text(const MessageArg &arg, std::int64_t offset, const Locale &loc) -> std::string {
    if (const auto *n = std::get_if<std::int64_t>(&arg.value)) {
        return format_number(static_cast<double>(*n - offset), loc, 0);
    }
    if (const auto *d = std::get_if<double>(&arg.value)) {
        return format_number(*d - static_cast<double>(offset), loc, 2);
    }
    const auto *s = std::get_if<std::string>(&arg.value);
    if (s == nullptr) {
        return {};
    }
    const PluralOperands ops = plural_operands_from_decimal(*s);
    return format_number(ops.n - static_cast<double>(offset), loc, ops.v > 0 ? 2 : 0);
}

/// @brief 从参数值构造生效操作数（offset 只对整数语义精确；小数 + offset 按数值近似，申报口径）。
auto effective_operands(const MessageArg &arg, std::int64_t offset) -> PluralOperands {
    if (const auto *n = std::get_if<std::int64_t>(&arg.value)) {
        return plural_operands_from_int(*n - offset);
    }
    if (const auto *d = std::get_if<double>(&arg.value)) {
        return plural_operands_from_double(*d - static_cast<double>(offset));
    }
    const auto *s = std::get_if<std::string>(&arg.value);
    PluralOperands ops = s != nullptr ? plural_operands_from_decimal(*s) : PluralOperands{};
    ops.n -= static_cast<double>(offset);
    ops.i -= offset;
    return ops;
}

/// @brief 按 AST 递归格式化。
///
/// `hash_value` 是最近一层数值选择器（plural / selectordinal）的生效值展示串，select 层原样透传；
/// 为空指针时当前没有数值上下文，`#` 按字面输出（与 ICU 同口径：`#` 只在复数分支里被替换）。
/// @param node 当前节点。
/// @param args 具名参数表。
/// @param loc 取代表决所用的 locale。
/// @param hash_value 生效数值展示串；无数值上下文时为空指针。
/// @param out 结果累加缓冲。
auto format_into(const MessageNode &node, const std::vector<MessageArg> &args, const Locale &loc,
                 const std::string *hash_value, std::string &out) -> void {
    if (!node.children.empty()) {  // 序列节点（根节点 / 选择器分支体）：依序展开子节点
        for (const auto &child : node.children) {
            format_into(*child, args, loc, hash_value, out);
        }
        return;
    }
    switch (node.kind) {
        case NodeKind::Text:
            out += node.text;
            return;
        case NodeKind::Hash:
            out += hash_value != nullptr ? *hash_value : std::string_view{"#"};
            return;
        case NodeKind::Arg: {
            const MessageArg *arg = find_arg(args, node.arg_name);
            if (arg == nullptr) {
                return;  // 缺参 → 空串（与旧版占位越界回空一致）
            }
            if (node.func.empty() || node.func == "#") {
                out += message_arg_text(*arg, loc);
                return;
            }
            if (node.func == "number") {
                if (const auto *n = std::get_if<std::int64_t>(&arg->value)) {
                    out += format_number(static_cast<double>(*n), loc, 0);  // 整数无小数位
                } else if (const auto *d = std::get_if<double>(&arg->value)) {
                    out += format_number(*d, loc, node.style == "integer" ? 0 : 2);
                } else {
                    out += message_arg_text(*arg, loc);
                }
                return;
            }
            if (node.func == "currency") {
                const auto *n = std::get_if<std::int64_t>(&arg->value);
                const auto *d = std::get_if<double>(&arg->value);
                if (n != nullptr || d != nullptr) {
                    const double value = n != nullptr ? static_cast<double>(*n) : *d;
                    out += format_currency(value, currency_from_code(node.style), loc);
                }
                return;
            }
            if (node.func == "date") {
                if (const auto *date = std::get_if<CivilDate>(&arg->value)) {
                    out += format_date(date->year, date->month, date->day, loc, date_style_from_token(node.style));
                }
                return;
            }
            return;  // 未知函数：生成器已拦，运行期按空串降级
        }
        case NodeKind::Selector: {
            const MessageArg *arg = find_arg(args, node.arg_name);
            const MessageNode *chosen = nullptr;
            // select 不产出数值：分支内的 `#` 沿用外层最近一个数值 selector 的生效值（与 ICU 同口径）；
            // 外层也没有数值 selector 时 hash_value 本就是空指针，`#` 落回字面。
            const std::string *hash_pass = node.selector_kind == "select" ? hash_value : nullptr;
            std::string hash_text;
            if (arg != nullptr) {
                if (node.selector_kind == "select") {
                    const std::string text = message_arg_text(*arg, loc);
                    for (const auto &branch : node.branches) {
                        if (branch.first != "other" && branch.first == text) {
                            chosen = branch.second.get();
                            break;
                        }
                    }
                } else {
                    const bool ordinal = node.selector_kind == "selectordinal";
                    const PluralOperands ops = effective_operands(*arg, node.offset);
                    const double effective = selector_number(*arg) - static_cast<double>(node.offset);
                    for (const auto &branch : node.branches) {
                        if (branch.first.size() > 1 && branch.first[0] == '=') {
                            if (parse_decimal_int(std::string_view{branch.first}.substr(1)) ==
                                static_cast<long long>(effective)) {
                                chosen = branch.second.get();
                                break;
                            }
                        }
                    }
                    if (chosen == nullptr) {
                        const PluralCategory cat =
                            ordinal ? plural_category_ordinal(ops, loc) : plural_category(ops, loc);
                        const char *keyword = cat == PluralCategory::Zero   ? "zero"
                                              : cat == PluralCategory::One  ? "one"
                                              : cat == PluralCategory::Two  ? "two"
                                              : cat == PluralCategory::Few  ? "few"
                                              : cat == PluralCategory::Many ? "many"
                                                                            : "other";
                        for (const auto &branch : node.branches) {
                            if (branch.first == keyword) {
                                chosen = branch.second.get();
                                break;
                            }
                        }
                    }
                    hash_text = effective_number_text(*arg, node.offset, loc);
                    hash_pass = &hash_text;  // 本层生效值替换外层值（含 offset 后的口径）
                }
            }
            if (chosen == nullptr) {
                // 参数缺失 / 类别无分支 / select 无精确值 → other 兜底（与旧版回退 other= 同口径）。
                for (const auto &branch : node.branches) {
                    if (branch.first == "other") {
                        chosen = branch.second.get();
                        break;
                    }
                }
            }
            if (chosen != nullptr) {
                format_into(*chosen, args, loc, hash_pass, out);
            }
            return;
        }
    }
}

}  // namespace

auto parse_message_template(std::string_view tmpl) -> std::unique_ptr<MessageNode> {
    auto root = std::make_unique<MessageNode>();
    root->kind = NodeKind::Text;  // 根节点借 Text 位作为「序列节点」标记（kind 仅在分支判定时区分）
    if (!parse_into(*root, tmpl, 0, false)) {
        return nullptr;
    }
    return root;
}

auto format_message_node(const MessageNode &node, const std::vector<MessageArg> &args, const Locale &loc)
    -> std::string {
    std::string out;
    out.reserve(node.text.size() + (args.size() * 8));
    format_into(node, args, loc, /*hash_value=*/nullptr, out);
    return out;
}

}  // namespace aurora::detail

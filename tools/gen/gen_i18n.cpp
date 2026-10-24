// ============================================================================
// gen_i18n — Aurora 消息目录生成器（i18n 构建期管线）
// ----------------------------------------------------------------------------
// 产出两件：薄生成头（强类型 Id 常量 + 每 key 一枚具名形参的 inline 构造函数）与 AUM1 二进制
// 词表（由 `MessageCatalog::load_binary` 载入）。译文迭代只改 JSON、只重出 bin，不触碰 C++ 编译。
//
// 用法：
//   gen_i18n <out.hpp> <out.bin> <cpp-namespace> [--id-base N] <base.json> [locale.json...]
//   gen_i18n --emit-builtin <out.hpp> <builtin.json>
//
// 输入形态（ARB 口径；词条可直接置于顶层，也可包在 `messages` 里）：
//   基准文件 {"version":1, "locale":"en", "id_base":1024, "messages":{
//              "app.welcome": "Hi {name}, {count, plural, one {# file} other {# files}}",
//              "@app.welcome": {"placeholders": {"name": {"type": "string"},
//                                                 "count": {"type": "int64"}},
//                                "description": "欢迎语"}}}
//   译文文件 {"locale":"zh-Hans-CN", "messages":{"app.welcome":"…"}}  // 纯词条，元数据只在基准侧
//
// 校验口径：模板过运行期同一个解析器（引擎源文件直接编入本工具，构建期与运行期同源）；跨语言做
// 结构树比对；形参元数据必填且与占位函数 / 类型匹配；selector 分支对照 CLDR 可达档覆盖；另查
// UTF-8、key 形态、`#` 作用域与嵌套深度。任何 error 都不落盘（退出码 1），warn 照常产出并逐条打印。
//
// Note: 构建期工具，**不链接 aurora 库**（JSON 容器走 aurora_json 静态库；i18n 引擎与复数判定表按
//       源文件编入本 target），与 gen_error_codes 同口径，可在 Emscripten 交叉下走宿主原生子配置。
// ============================================================================
#include <aurora/core/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/i18n/builtin_messages.gen.h"
#include "aurora/i18n/detail/message_engine.h"
#include "aurora/i18n/locale.h"
#include "aurora/i18n/plural.h"

namespace {

using Json = aurora::json::Value;  // 值类型别名（公共侧的 aurora::Json 住在 widget 层，本工具够不到）
using aurora::detail::MessageNode;
using aurora::detail::NodeKind;

/// @brief 内置保留区的规模：`[AURORA_BUILTIN_MESSAGE_ID_BASE, +本值)` 归库内置词表独占。
///
/// 应用词表基址的**硬下限**，与 `--id-base` / JSON `id_base` 的缺省值同源（缺省即紧跟保留区）。
/// 允许下调进保留区等于让应用词条与库内置词条抢同一槽位：眼下内置词条只占了前几格，落进
/// `[7, 1024)` 看似相安无事，库一旦增补内置词条就静默撞号——故保留区按规模整块划走，
/// `--id-base` 只能上调（多词表各自的区间由应用自己排开）。
constexpr std::uint32_t AURORA_BUILTIN_RESERVED_SIZE = 1024;

/// @brief 诊断输出统一出口（工具不链接 aurora，够不到 AURORA_LOG_*；口径同 gen_error_codes）。
auto err(const std::string &msg) -> void { std::cerr << "[gen_i18n] " << msg << "\n"; }

/// @brief 生成物横幅里的来源路径写法（反斜杠归一成正斜杠）。
///
/// 横幅是产物的一部分，会被 golden 逐字节比对：Windows 下 `tests\fixtures\...` 与 Linux /
/// 正斜杠写法下的 `tests/fixtures/...` 会产出两份内容相同、字节不同的头，判据「同输入逐字节
/// 同输出」就只在单平台成立。路径在此只作展示（打开文件用原样入参），归一不影响读写。
/// @param path 调用方给定的原始路径。
/// @return 正斜杠形态的路径。
auto display_path(std::string path) -> std::string {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

/// @brief 多段文本拼接：按各段总长预留一次分配，再逐段追加。
///
/// 诊断消息与生成物横幅都由 4~6 段文本拼成，`a + b + c` 形态的逐段 `operator+` 每步各造一枚
/// 临时串；收进本助手后调用点保持单表达式，却只分配一次。
/// @param parts 依次追加的片段（std::string / std::string_view / C 串均可）。
/// @return 拼接后的文本。
template <typename... Parts>
auto concat_text(const Parts &...parts) -> std::string {
    std::string out;
    out.reserve((std::string_view{parts}.size() + ... + 0U));
    ((out.append(std::string_view{parts})), ...);
    return out;
}

/// @brief 校验诊断累积器：error 拦产出，warn 只报不拦。
///
/// 同一条基准模板会被走两遍（基准侧元数据校验 + 基准 locale 的译文比对），故按消息文本去重，
/// 免得一处缺陷刷两行、把诊断计数带偏。
struct Diags {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    auto error(const std::string &file, const std::string &what) -> void {
        push(errors, seen_errors, file + ": " + what);
    }
    auto warn(const std::string &file, const std::string &what) -> void {
        push(warnings, seen_warnings, file + ": " + what);
    }

    /// @brief 追加尚未出现过的诊断。
    auto push(std::vector<std::string> &sink, std::set<std::string> &seen, const std::string &line) -> void {
        if (seen.insert(line).second) {
            sink.push_back(line);
        }
    }

    std::set<std::string> seen_errors;  ///< 已记过的 error 文本
    std::set<std::string> seen_warnings;  ///< 已记过的 warn 文本

    /// @brief 先 warn 后 error 全量打印。
    /// @return 无 error 时为 true。
    [[nodiscard]] auto report() const -> bool {
        for (const std::string &w : warnings) {
            err("warn: " + w);
        }
        for (const std::string &e : errors) {
            err("error: " + e);
        }
        return errors.empty();
    }
};

/// @brief 词条形参元数据（`@key.placeholders.<name>.type`）。
struct Param {
    std::string name;
    std::string type;  ///< "string" / "int64" / "double" / "date"
};

/// @brief 一条模板的结构形状（跨语言比对用）。
struct TemplateShape {
    std::string sig;  ///< 含 select 分支键的归一签名
    std::string sig_bare;  ///< 不含 select 分支键的归一签名
    std::vector<std::string> select_keys;  ///< select 分支键集合（`形参=键`，升序去重）
};

/// @brief 一条基准词条。
struct Entry {
    std::string key;  ///< 规范名（点分段）
    std::string text;  ///< 基准模板
    std::vector<Param> params;  ///< 形参元数据（按名升序）
    std::string description;  ///< 可选译员注释
    std::uint32_t id = 0;  ///< 词表内 Id
    TemplateShape shape;  ///< 基准模板结构形状（跨语言比对用）
};

/// @brief 一个 locale 的译文集合（tag → 规范名 → 模板）。
using LocaleTexts = std::pair<std::string, std::map<std::string, std::string>>;

// ---- 基础判定 ----

/// @brief 小写化（就地）。
auto to_lower(std::string &s) -> void {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

auto to_lower(std::string_view s) -> std::string {
    std::string out(s);
    to_lower(out);
    return out;
}

auto to_upper(std::string_view s) -> std::string {
    std::string out(s);
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// @brief 书写系统形态（首字母大写 + 其余小写，如 "latin" → "Latn"）。
auto to_title(std::string_view s) -> std::string {
    std::string out(s);
    for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] = k == 0 ? static_cast<char>(std::toupper(static_cast<unsigned char>(out[k])))
                        : static_cast<char>(std::tolower(static_cast<unsigned char>(out[k])));
    }
    return out;
}

/// @brief 按分隔符切段。
auto split(std::string_view text, char sep) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t hit = text.find(sep, pos);
        const std::size_t end = hit == std::string_view::npos ? text.size() : hit;
        out.push_back(text.substr(pos, end - pos));
        if (hit == std::string_view::npos) {
            break;
        }
        pos = hit + 1;
    }
    return out;
}

/// @brief C++ 保留字（点分段映射为 namespace / 函数名后必须可用）。
constexpr std::string_view AURORA_CPP_KEYWORDS[] = {"alignas",
                                                    "alignof",
                                                    "and",
                                                    "and_eq",
                                                    "asm",
                                                    "auto",
                                                    "bitand",
                                                    "bitor",
                                                    "bool",
                                                    "break",
                                                    "case",
                                                    "catch",
                                                    "char",
                                                    "char8_t",
                                                    "char16_t",
                                                    "char32_t",
                                                    "class",
                                                    "concept",
                                                    "const",
                                                    "consteval",
                                                    "constexpr",
                                                    "constinit",
                                                    "const_cast",
                                                    "continue",
                                                    "co_await",
                                                    "co_return",
                                                    "co_yield",
                                                    "decltype",
                                                    "default",
                                                    "delete",
                                                    "do",
                                                    "double",
                                                    "dynamic_cast",
                                                    "else",
                                                    "enum",
                                                    "explicit",
                                                    "export",
                                                    "extern",
                                                    "false",
                                                    "float",
                                                    "for",
                                                    "friend",
                                                    "goto",
                                                    "if",
                                                    "inline",
                                                    "int",
                                                    "long",
                                                    "mutable",
                                                    "namespace",
                                                    "new",
                                                    "noexcept",
                                                    "not",
                                                    "not_eq",
                                                    "nullptr",
                                                    "operator",
                                                    "or",
                                                    "or_eq",
                                                    "private",
                                                    "protected",
                                                    "public",
                                                    "register",
                                                    "reinterpret_cast",
                                                    "requires",
                                                    "return",
                                                    "short",
                                                    "signed",
                                                    "sizeof",
                                                    "static",
                                                    "static_assert",
                                                    "static_cast",
                                                    "struct",
                                                    "switch",
                                                    "template",
                                                    "this",
                                                    "thread_local",
                                                    "throw",
                                                    "true",
                                                    "try",
                                                    "typedef",
                                                    "typeid",
                                                    "typename",
                                                    "union",
                                                    "unsigned",
                                                    "using",
                                                    "variant",
                                                    "virtual",
                                                    "void",
                                                    "volatile",
                                                    "wchar_t",
                                                    "while",
                                                    "xor",
                                                    "xor_eq"};

/// @brief 是否 C++ 保留字。
auto is_cpp_keyword(std::string_view seg) -> bool {
    return std::find(std::begin(AURORA_CPP_KEYWORDS), std::end(AURORA_CPP_KEYWORDS), seg) !=
           std::end(AURORA_CPP_KEYWORDS);
}

/// @brief 标识符段判定（首字符字母或下划线，其后字母数字下划线，且非保留字）。
auto is_identifier_segment(std::string_view seg) -> bool {
    if (seg.empty() || (std::isalpha(static_cast<unsigned char>(seg[0])) == 0 && seg[0] != '_')) {
        return false;
    }
    for (char c : seg) {
        const bool ok = (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
        if (!ok) {
            return false;
        }
    }
    return !is_cpp_keyword(seg);
}

/// @brief 规范名形态：`[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*`，且各段可作 C++ 标识符。
auto canonical_name_ok(std::string_view name) -> bool {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(split(name, '.'), [](std::string_view seg) -> bool {
        if (seg.empty() || std::isalpha(static_cast<unsigned char>(seg[0])) == 0) {
            return false;
        }
        if (to_lower(seg) != std::string(seg)) {
            return false;  // 规范名一律小写，避免同一 key 的大小写二义
        }
        return is_identifier_segment(seg);
    });
}

/// @brief 点分段以下划线连写（Id 常量名）。
auto flatten_name(std::string_view key) -> std::string {
    std::string out(key);
    for (char &c : out) {
        if (c == '.') {
            c = '_';
        }
    }
    return out;
}

/// @brief Id 常量名：全大写 + `AURORA_` 前缀（本库公共常量的命名惯例，命名门禁同口径）。
///
/// 规范名本身（`command_palette.placeholder`）仍是外部契约与 bin 里的索引，常量只是它的 C++
/// 形态；已自带 `AURORA_` 起头的键（内置词表的 `aurora.*` 段）不重复加前缀。
auto id_constant_name(std::string_view key) -> std::string {
    std::string out = to_upper(flatten_name(key));
    if (!out.starts_with("AURORA_")) {
        out = "AURORA_" + out;
    }
    return out;
}

/// @brief 点分段映射为嵌套 namespace 前缀（末段是函数名，其余作 namespace）。
auto ns_prefix(std::string_view key) -> std::string {
    const std::size_t dot = key.rfind('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string out;
    for (std::string_view seg : split(key.substr(0, dot), '.')) {
        if (!out.empty()) {
            out += "::";
        }
        out += seg;
    }
    return out;
}

/// @brief 末段（函数名）。
auto leaf_name(std::string_view key) -> std::string {
    const std::size_t dot = key.rfind('.');
    return dot == std::string::npos ? std::string(key) : std::string(key.substr(dot + 1));
}

/// @brief UTF-8 结构校验（拒过长序列 / 代理区码点 / 超 U+10FFFF / 落单续字节 / 截断）。
auto is_utf8(std::string_view s) -> bool {
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        unsigned long cp = 0;
        if (b0 < 0x80U) {
            ++i;
            continue;
        }
        if ((b0 & 0xE0U) == 0xC0U) {
            if (b0 < 0xC2U) {
                return false;  // C0 / C1 前导 = 过长
            }
            extra = 1;
            cp = b0 & 0x1FU;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            extra = 2;
            cp = b0 & 0x0FU;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            extra = 3;
            cp = b0 & 0x07U;
        } else {
            return false;  // 落单续字节 / 非法前导
        }
        if (i + extra >= n) {
            return false;  // 截断
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto bk = static_cast<unsigned char>(s[i + k]);
            if ((bk & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (bk & 0x3FU);
        }
        if (extra == 2 && (cp < 0x800UL || (cp >= 0xD800UL && cp <= 0xDFFFUL))) {
            return false;  // 过长 / 代理区
        }
        if (extra == 3 && (cp < 0x10000UL || cp > 0x10FFFFUL)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

/// @brief 由 BCP-47 形态标签拆出 Locale（语言 + 可选 4 字母文字 + 可选 2 字母 / 3 数字地区）。
auto locale_from_tag(std::string_view tag) -> std::optional<aurora::Locale> {
    const std::vector<std::string_view> parts = split(tag, '-');
    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }
    const auto alpha = [](std::string_view v) {
        return std::all_of(v.begin(), v.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
    };
    const auto digit = [](std::string_view v) {
        return std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    };
    if (parts[0].size() < 2 || parts[0].size() > 3 || !alpha(parts[0])) {
        return std::nullopt;
    }
    aurora::Locale loc;
    loc.language = to_lower(parts[0]);
    std::size_t next = 1;
    if (next < parts.size() && parts[next].size() == 4 && alpha(parts[next])) {
        loc.script = to_title(parts[next]);
        ++next;
    }
    if (next < parts.size()) {
        const std::string_view region = parts[next];
        const bool ok = (region.size() == 2 && alpha(region)) || (region.size() == 3 && digit(region));
        if (!ok) {
            return std::nullopt;
        }
        loc.region = to_upper(region);
        ++next;
    }
    if (next != parts.size()) {
        return std::nullopt;  // 变体 / 扩展子标签不在词表口径内
    }
    return loc;
}

// ---- 模板结构分析 ----

/// @brief 占位使用记录。
struct PlaceholderUse {
    std::string name;
    std::string func;  ///< "" / number / currency / date / 其它（不支持）
    std::string style;
};

/// @brief selector 使用记录。
struct SelectorUse {
    std::string kind;  ///< plural / selectordinal / select
    std::string name;
    std::int64_t offset = 0;
    std::vector<std::string> keys;
};

/// @brief AST 走查收集到的事实。
struct TemplateFacts {
    std::vector<PlaceholderUse> placeholders;
    std::vector<SelectorUse> selectors;
    std::vector<std::string> select_keys;  ///< select 分支键（`形参=键` 形态，跨层合并）
    bool hash_without_numeric_context = false;  ///< `#` 所在分支链上没有 plural / selectordinal
};

/// @brief selector 是否产出 `#` 可引用的生效数值。
auto is_numeric_selector(std::string_view kind) -> bool { return kind == "plural" || kind == "selectordinal"; }

/// @brief 递归收集占位 / selector / `#` 事实（in_plural 为外层是否已有数值 selector）。
/// @param node 当前节点。
/// @param in_plural 进入本节点时外层是否已有 plural / selectordinal。
/// @param out 事实累积器。
auto collect_facts(const MessageNode &node, bool in_plural, TemplateFacts &out) -> void {
    if (!node.children.empty()) {  // 序列节点（根 / 分支体）
        for (const auto &child : node.children) {
            collect_facts(*child, in_plural, out);
        }
        return;
    }
    switch (node.kind) {
        case NodeKind::Text:
            return;
        case NodeKind::Hash:
            // 解析器只在 selector 分支内产出 Hash 节点，顶层 `#` 是文本段（字面输出，与 ICU 一致）；
            // 这里剩下的就是「select 链上没有数值 selector」这一种可疑写法。
            if (!in_plural) {
                out.hash_without_numeric_context = true;
            }
            return;
        case NodeKind::Arg:
            out.placeholders.push_back({.name = node.arg_name, .func = node.func, .style = node.style});
            return;
        case NodeKind::Selector: {
            SelectorUse use;
            use.kind = node.selector_kind;
            use.name = node.arg_name;
            use.offset = node.offset;
            // `#` 取的是**最近的外层数值 selector** 生效值：select 不压数值栈，故 select 套在
            // plural 里时分支内的 `#` 仍指向那个 plural —— 只有整条链上没有数值 selector 才无值。
            const bool child_in_plural = in_plural || is_numeric_selector(node.selector_kind);
            for (const auto &branch : node.branches) {
                use.keys.push_back(branch.first);
                if (!is_numeric_selector(node.selector_kind)) {
                    out.select_keys.push_back(node.arg_name + "=" + branch.first);
                }
                collect_facts(*branch.second, child_in_plural, out);
            }
            out.selectors.push_back(std::move(use));
            return;
        }
    }
}

/// @brief 跨语言结构签名：只留骨架，措辞与语序不进串。
///
/// 三条归一口径都由「译文本就与基准不同」推导，不是把校验放宽：
///   - 字面文本段不进签名：各语言的措辞与占位相对位置自由（`Due {d}` 与 `{d} due` 同构）；
///   - `plural` / `selectordinal` 的分支键不进签名：分支集由该语言的 CLDR 可达档决定（en 只有
///     one / other，ru 另需 few / many），故按「分支体签名去重升序」比对；分支是否覆盖可达档
///     由 CLDR 对照单独判（warn 级）；纯文字分支（不带 `#`，如 `=0 {none}`）签名为空、不入集合，
///     免得译文省掉这类分支就报结构不符；
///   - `select` 的分支键进签名：那是值域契约（`admin` 在任何语言都匹配 `admin`），须逐键对齐。
///     `with_select_keys=false` 的裸签名把键位也去掉，只留各分支体的形状——两份签名相等而全签名
///     不等，就说明差异**只在 select 键的有无**，交由键集合分别定级（少给 warn、多给 error）。
/// @param node 模板 AST。
/// @param with_select_keys 是否把 select 分支键写进签名。
/// @return 归一后的结构签名串。
auto signature_of(const MessageNode &node, bool with_select_keys = true) -> std::string {
    if (!node.children.empty()) {  // 序列节点（根 / 分支体）：子节点串接，文本段产出空串自然消失
        std::string out;
        for (const auto &child : node.children) {
            out += signature_of(*child, with_select_keys);
        }
        return out;
    }
    switch (node.kind) {
        case NodeKind::Text:
            return {};
        case NodeKind::Hash:
            return "#";
        case NodeKind::Arg:
            return "A[" + node.arg_name + "|" + node.func + "|" + node.style + "]";
        case NodeKind::Selector: {
            const bool plural = is_numeric_selector(node.selector_kind);
            std::vector<std::string> parts;
            for (const auto &branch : node.branches) {
                const std::string body = signature_of(*branch.second, with_select_keys);
                if (plural) {
                    if (body.empty()) {
                        continue;  // 纯文字分支不进集合
                    }
                    parts.push_back(body);
                } else {
                    parts.push_back(with_select_keys ? branch.first + "=" + body : body);
                }
            }
            std::sort(parts.begin(), parts.end());
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
            std::string out = "S[" + node.selector_kind + "|" + node.arg_name + "|" + std::to_string(node.offset) + "{";
            for (const std::string &part : parts) {
                out += part + ";";
            }
            return out + "}]";
        }
    }
    return "?";
}

/// @brief 复数类别名（与 AST 分支键同形态）。
auto category_name(aurora::PluralCategory cat) -> std::string_view {
    switch (cat) {
        case aurora::PluralCategory::Zero:
            return "zero";
        case aurora::PluralCategory::One:
            return "one";
        case aurora::PluralCategory::Two:
            return "two";
        case aurora::PluralCategory::Few:
            return "few";
        case aurora::PluralCategory::Many:
            return "many";
        case aurora::PluralCategory::Other:
            return "other";
    }
    return "other";
}

/// @brief 探测某 locale 在其 CLDR 规则下可达的复数类别（与运行期同表：整档 + 小数档抽样）。
auto reachable_categories(const aurora::Locale &loc, bool ordinal) -> std::set<std::string> {
    std::set<std::string> out;
    const auto add = [&](std::string_view text) {
        const aurora::PluralOperands ops = aurora::plural_operands_from_decimal(text);
        const aurora::PluralCategory cat =
            ordinal ? aurora::plural_category_ordinal(ops, loc) : aurora::plural_category(ops, loc);
        out.insert(std::string(category_name(cat)));
    };
    for (int v = 0; v <= 100; ++v) {
        add(std::to_string(v));
    }
    constexpr std::string_view decimal_probes[] = {"1000", "10000", "100000", "1000000", "0.0",  "0.5",
                                                   "1.0",  "1.5",   "2.0",    "10.0",    "100.0"};
    for (std::string_view probe : decimal_probes) {
        add(probe);
    }
    return out;
}

/// @brief 支持的占位函数（与引擎 format_into 同口径）。
constexpr std::string_view AURORA_SUPPORTED_FUNCS[] = {"number", "currency", "date"};
/// @brief 支持的货币码（与 format.h 的 Currency 枚举同口径）。
constexpr std::string_view AURORA_SUPPORTED_CURRENCIES[] = {"USD", "EUR", "JPY", "CNY", "GBP"};
/// @brief CLDR 复数类别分支键（`=N` 精确分支另判）。
constexpr std::string_view AURORA_PLURAL_KEYS[] = {"zero", "one", "two", "few", "many", "other"};
/// @brief 日期风格 token（空串走缺省 medium）。
constexpr std::string_view AURORA_DATE_STYLES[] = {"", "short", "medium", "long"};
/// @brief 形参类型集合（与 MessageArg 的值域一一对应）。
constexpr std::string_view AURORA_PARAM_TYPES[] = {"string", "int64", "double", "date"};

/// @brief 数值型形参（int64 / double）。
auto is_numeric(std::string_view type) -> bool { return type == "int64" || type == "double"; }

/// @brief `=N` 精确分支键判定。
auto is_exact_key(std::string_view key) -> bool {
    if (key.size() < 2 || key[0] != '=') {
        return false;
    }
    const std::size_t start = key[1] == '-' ? 2 : 1;
    if (start >= key.size()) {
        return false;
    }
    return std::all_of(key.begin() + static_cast<std::ptrdiff_t>(start), key.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

/// @brief 骨架校验：selector 类型约束、分支键形态、`other` 必备、`#` 作用域、函数 / 风格集合。
auto check_skeleton(const std::string &file, const std::string &key, const TemplateFacts &facts,
                    const std::map<std::string, std::string> &param_types, Diags &diags) -> void {
    const std::string where = "entry '" + key + "'";
    if (facts.hash_without_numeric_context) {
        // 不是结构错误而是可疑写法：`#` 在没有外层复数的 select 分支里按字面输出（ICU 同口径），
        // 译员多半是想引用数值。报 warn 提醒，不拦产出。
        diags.warn(file, where +
                             ": `#` here has no enclosing plural / selectordinal, so it renders as a literal '#'; "
                             "move it into a plural branch or use a named placeholder if you meant the number");
    }
    for (const SelectorUse &sel : facts.selectors) {
        const bool plural = sel.kind == "plural" || sel.kind == "selectordinal";
        if (sel.keys.empty()) {
            diags.error(file, where + ": " + sel.kind + " has no branch");
        }
        bool has_other = false;
        for (const std::string &branch : sel.keys) {
            if (branch == "other") {
                has_other = true;
            }
            if (!plural) {
                continue;
            }
            const bool known = std::find(std::begin(AURORA_PLURAL_KEYS), std::end(AURORA_PLURAL_KEYS),
                                         std::string_view(branch)) != std::end(AURORA_PLURAL_KEYS);
            if (!known && !is_exact_key(branch)) {
                diags.error(file, concat_text(where, ": ", sel.kind, " branch key '", branch,
                                              "' is neither a CLDR category nor an =N exact value "
                                              "(offset must be written as an offset:N prefix)"));
            }
        }
        if (!has_other) {
            diags.error(file, where + ": " + sel.kind + " is missing the 'other' fallback branch");
        }
        if (sel.offset < 0) {
            diags.error(file, where + ": " + sel.kind + " offset must not be negative");
        }
        const auto declared = param_types.find(sel.name);
        if (declared != param_types.end()) {
            if (plural && !is_numeric(declared->second)) {
                diags.error(file, where + ": " + sel.kind + " selector operand '" + sel.name +
                                      "' must be int64 or double, got " + declared->second);
            }
            if (!plural && declared->second != "string") {
                diags.error(file, where + ": select operand '" + sel.name + "' must be string (exact match), got " +
                                      declared->second);
            }
        }
    }
    for (const PlaceholderUse &ph : facts.placeholders) {
        const auto declared = param_types.find(ph.name);
        if (ph.func.empty()) {
            if (declared != param_types.end() && declared->second == "date") {
                diags.error(file, where + ": date param '" + ph.name +
                                      "' must be written as {name, date[, short|medium|long]}; "
                                      "a bare placeholder renders nothing");
            }
            continue;
        }
        if (std::find(std::begin(AURORA_SUPPORTED_FUNCS), std::end(AURORA_SUPPORTED_FUNCS),
                      std::string_view(ph.func)) == std::end(AURORA_SUPPORTED_FUNCS)) {
            diags.error(file, where + ": unsupported placeholder function '" + ph.func +
                                  "' (available: number / currency / date)");
            continue;
        }
        if (declared != param_types.end()) {
            const std::string &type = declared->second;
            if (ph.func == "date" && type != "date") {
                diags.error(file, concat_text(where, ": {, date} param '", ph.name, "' must be date, got ", type));
            }
            if ((ph.func == "number" || ph.func == "currency") && !is_numeric(type) && type != "string") {
                diags.error(
                    file, concat_text(where, ": {, ", ph.func, "} param '", ph.name, "' must be numeric, got ", type));
            }
        }
        if (ph.func == "number" && !ph.style.empty() && ph.style != "integer") {
            diags.error(file, where + ": number style only supports integer (no percent etc.), got '" + ph.style + "'");
        }
        if (ph.func == "currency" &&
            std::find(std::begin(AURORA_SUPPORTED_CURRENCIES), std::end(AURORA_SUPPORTED_CURRENCIES),
                      std::string_view(ph.style)) == std::end(AURORA_SUPPORTED_CURRENCIES)) {
            diags.error(file, where + ": currency must be a 3-letter ISO code inside this library's " +
                                  "Currency enum (USD/EUR/JPY/CNY/GBP), got '" + ph.style + "'");
        }
        if (ph.func == "date" && std::find(std::begin(AURORA_DATE_STYLES), std::end(AURORA_DATE_STYLES),
                                           std::string_view(ph.style)) == std::end(AURORA_DATE_STYLES)) {
            diags.error(file, where + ": date style only supports short / medium / long, got '" + ph.style + "'");
        }
    }
}

/// @brief 解析并校验一条模板。
/// @param file 诊断归属的文件名。
/// @param key 规范名。
/// @param text 模板原文。
/// @param loc 该模板所在 locale（CLDR 覆盖对照用）。
/// @param param_types 形参名 → 类型（基准元数据；译文复用基准的）。
/// @param check_meta 是否比对元数据覆盖（只在基准侧做）。
/// @param check_coverage 是否做 CLDR 可达档覆盖提醒。
/// @param diags 诊断累积器。
/// @return 结构形状；解析 / 编码失败返回空 optional（错误已记）。
auto analyze_template(const std::string &file, const std::string &key, const std::string &text,
                      const aurora::Locale &loc, const std::map<std::string, std::string> &param_types, bool check_meta,
                      bool check_coverage, Diags &diags) -> std::optional<TemplateShape> {
    if (!is_utf8(text)) {
        diags.error(file, "entry '" + key + "': not valid UTF-8");
        return std::nullopt;
    }
    const std::unique_ptr<MessageNode> ast = aurora::detail::parse_message_template(text);
    if (ast == nullptr) {
        diags.error(file, "entry '" + key +
                              "': template parse failed (unbalanced braces / nesting over 8 levels / "
                              "empty placeholder name)");
        return std::nullopt;
    }
    TemplateFacts facts;
    collect_facts(*ast, /*in_plural=*/false, facts);
    check_skeleton(file, key, facts, param_types, diags);
    std::set<std::string> used;
    for (const PlaceholderUse &ph : facts.placeholders) {
        used.insert(ph.name);
    }
    for (const SelectorUse &sel : facts.selectors) {
        used.insert(sel.name);
    }
    if (check_meta) {
        for (const std::string &name : used) {
            if (!param_types.contains(name)) {
                diags.error(file, concat_text("entry '", key, "': placeholder '", name,
                                              "' has no @key.placeholders metadata (param type is required)"));
            }
        }
        for (const auto &[name, type] : param_types) {
            if (!used.contains(name)) {
                diags.warn(file,
                           concat_text("entry '", key, "': metadata param '", name, "' is never used by the template"));
            }
        }
    }
    if (check_coverage) {
        for (const SelectorUse &sel : facts.selectors) {
            if (sel.kind != "plural" && sel.kind != "selectordinal") {
                continue;
            }
            const std::set<std::string> reachable = reachable_categories(loc, sel.kind == "selectordinal");
            const std::set<std::string> declared(sel.keys.begin(), sel.keys.end());
            for (const std::string &cat : reachable) {
                if (cat == "other" || declared.contains(cat)) {
                    continue;
                }
                diags.warn(file, concat_text("entry '", key, "': locale '", loc.tag(), "' reaches the ", cat,
                                             " category under CLDR rules; the branch is uncovered and falls to other"));
            }
        }
    }
    TemplateShape shape;
    shape.sig = signature_of(*ast, true);
    shape.sig_bare = signature_of(*ast, false);
    shape.select_keys = facts.select_keys;
    std::sort(shape.select_keys.begin(), shape.select_keys.end());
    shape.select_keys.erase(std::unique(shape.select_keys.begin(), shape.select_keys.end()), shape.select_keys.end());
    return shape;
}

/// @brief 跨语言结构比对：骨架差异是 error，select 分支键的少给是 warn、多给是 error。
/// @param file 诊断归属的文件名。
/// @param base_name 词条规范名。
/// @param origin 基准侧称呼（应用词表说 base，内置 key 的译文说 library template）。
/// @param base 基准模板形状。
/// @param loc_tag 译文所属 locale 标签。
/// @param shape 译文模板形状。
/// @param diags 诊断累积器。
auto compare_structure(const std::string &file, const std::string &base_name, std::string_view origin,
                       const TemplateShape &base, const std::string &loc_tag, const TemplateShape &shape, Diags &diags)
    -> void {
    const std::string where = "entry '" + base_name + "'";
    if (base.sig.empty() || shape.sig == base.sig) {
        return;  // 基准模板自身没解析出来（错误已在基准侧记过），不在这儿重复报
    }
    if (shape.sig_bare != base.sig_bare) {
        diags.error(file, concat_text(where, " has a different structure in locale '", loc_tag, "' than the ", origin,
                                      " does\n    ", origin, ": ", base.sig, "\n    ", loc_tag, ": ", shape.sig));
        return;
    }
    // 裸签名一致而全签名不一致：差异只在 select 分支键的有无。少给键有 other 兜底（可达，译员的
    // 措辞选择），多给的值在基准侧永远不出现（该分支匹配不到，是死码）。
    for (const std::string &branch : base.select_keys) {
        if (std::find(shape.select_keys.begin(), shape.select_keys.end(), branch) == shape.select_keys.end()) {
            diags.warn(file, concat_text(where, ": locale '", loc_tag, "' has no branch for select value '", branch,
                                         "'; those values render the other branch"));
        }
    }
    for (const std::string &branch : shape.select_keys) {
        if (std::find(base.select_keys.begin(), base.select_keys.end(), branch) == base.select_keys.end()) {
            diags.error(file, concat_text(where, ": locale '", loc_tag, "' adds select value '", branch, "' that the ",
                                          origin, " does not have; that branch can never match"));
        }
    }
}

// ---- 输入读取 ----

/// @brief 读文件原始字节；失败记 error。
auto read_bytes(const std::string &path, Diags &diags) -> std::optional<std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diags.error(path, "cannot open file");
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// @brief 读并解析 JSON 对象；失败记 error。
auto read_json(const std::string &path, Diags &diags) -> std::optional<Json> {
    const auto raw = read_bytes(path, diags);
    if (!raw) {
        return std::nullopt;
    }
    const auto parsed = aurora::json::parse(*raw);
    if (!parsed) {
        diags.error(path, "JSON parse failed");
        return std::nullopt;
    }
    if (!parsed.value().is_object()) {
        diags.error(path, "top level must be an object");
        return std::nullopt;
    }
    return parsed.value();
}

/// @brief 取词条对象：优先 `messages` 子对象，否则用顶层。
auto messages_object(const Json &root) -> const Json * {
    const Json *wrapped = root.at("messages");
    return wrapped != nullptr && wrapped->is_object() ? wrapped : &root;
}

/// @brief 取字符串字段。
auto string_field(const Json &obj, std::string_view key) -> std::optional<std::string> {
    const Json *v = obj.at(key);
    if (v == nullptr) {
        return std::nullopt;
    }
    // 形态判定与取值一次完成：`is_string()` 后再 `as_string()` 是同一条不变量问两遍，且跨语句的
    // 守卫让静态分析看不出解引用为何安全（Emscripten 的 libc++ optional 尤其如此）。
    const std::optional<std::string_view> text = v->as_string();
    if (!text.has_value()) {
        return std::nullopt;
    }
    return std::string{*text};
}

/// @brief 基准文件的装载结果。
struct BaseFile {
    std::vector<Entry> entries;  ///< 按规范名升序
    std::string locale = "en";  ///< 基准 locale（`"locale"` 字段，缺省 en）
    std::uint32_t id_base = AURORA_BUILTIN_RESERVED_SIZE;  ///< 文件内声明的应用词表基址
};

/// @brief 读基准文件：词条 + `@key` 元数据。
///
/// `@key` 块整体可选（只给译员注释也行）；「占位必须有形参元数据」由 analyze_template 的
/// 元数据覆盖检查判定——那才是生成 API 的真正依赖。
/// @param path 基准 JSON 路径。
/// @param diags 诊断累积器。
/// @return 词条集合（按规范名升序）；读失败为空 optional（错误已记）。
auto load_base(const std::string &path, Diags &diags) -> std::optional<BaseFile> {
    const auto root = read_json(path, diags);
    if (!root) {
        return std::nullopt;
    }
    BaseFile out;
    if (const Json *version = root->at("version"); version != nullptr && !version->is_int()) {
        diags.error(path, "version must be an integer");
    } else if (version != nullptr) {
        const auto v = version->as_or<std::int64_t>(1);
        if (v != 1) {
            diags.error(path, "unsupported version: " + std::to_string(v));
        }
    }
    if (const std::optional<std::string> loc = string_field(*root, "locale")) {
        out.locale = *loc;
    }
    if (const Json *declared = root->at("id_base"); declared != nullptr) {
        if (!declared->is_int()) {
            diags.error(path, "id_base must be an integer");
        } else {
            out.id_base = static_cast<std::uint32_t>(
                declared->as_or<std::int64_t>(AURORA_BUILTIN_RESERVED_SIZE));  // 缺省即紧跟保留区
        }
    }
    const Json *messages = messages_object(*root);
    std::map<std::string, const Json *> meta;
    std::set<std::string> keys;
    for (const auto e : messages->entries()) {
        const std::string key(e.key);
        if (!e.key.empty() && e.key[0] == '@') {
            if (!e.value.is_object()) {
                diags.error(path, "metadata '@" + key + "' must be an object");
            }
            meta[key] = &e.value;
            continue;
        }
        if (!canonical_name_ok(e.key)) {
            diags.error(path,
                        "key shape is invalid (must be [a-z][a-z0-9_]*(\\.[a-z][a-z0-9_]*)*, every segment usable "
                        "as a C++ identifier): " +
                            key);
            continue;
        }
        const std::optional<std::string_view> text = e.value.as_string();
        if (!text.has_value()) {
            diags.error(path, "entry '" + key + "' value must be a string");
            continue;
        }
        // 键的唯一性由 JSON 容器保证（core/json 解析器对重复键后值覆盖、位置保首次），故此处
        // 不再重复检一遍「不可能出现」的输入——校验清单只列可达规则（见 08-tooling.md 的 gen_i18n 节）。
        keys.insert(key);  // 供下方 '@key' 元数据反查
        out.entries.push_back(
            {.key = key, .text = std::string{*text}, .params = {}, .description = {}, .id = 0, .shape = {}});
    }
    for (const auto &[name, ignored] : meta) {
        if (!keys.contains(name.substr(1))) {
            diags.error(path, "metadata '" + name + "' has no matching entry");
        }
    }
    for (Entry &entry : out.entries) {
        const auto found = meta.find("@" + entry.key);
        if (found == meta.end()) {
            continue;
        }
        const Json &m = *found->second;
        if (const std::optional<std::string> desc = string_field(m, "description")) {
            entry.description = *desc;
        }
        const Json *placeholders = m.at("placeholders");
        if (placeholders == nullptr) {
            continue;
        }
        if (!placeholders->is_object()) {
            diags.error(path, "entry '" + entry.key + "': placeholders must be an object");
            continue;
        }
        for (const auto p : placeholders->entries()) {
            const std::string pname(p.key);
            if (!p.value.is_object()) {
                diags.error(path, "entry '" + entry.key + "': param '" + pname + "' metadata must be an object");
                continue;
            }
            const std::optional<std::string> type = string_field(p.value, "type");
            if (!type) {
                diags.error(path, "entry '" + entry.key + "': param '" + pname + "' has no type");
                continue;
            }
            if (std::find(std::begin(AURORA_PARAM_TYPES), std::end(AURORA_PARAM_TYPES), std::string_view(*type)) ==
                std::end(AURORA_PARAM_TYPES)) {
                diags.error(path, "entry '" + entry.key + "': param '" + pname + "' has an invalid type: " + *type +
                                      " (available string / int64 / double / date)");
                continue;
            }
            if (!is_identifier_segment(pname)) {
                diags.error(path, "entry '" + entry.key + "': param name cannot be used as a C++ identifier: " + pname);
                continue;
            }
            entry.params.push_back({.name = pname, .type = *type});
        }
        // 参数名同样由 JSON 容器的键唯一性保证，无需再检重复（口径见 load_base 的注释）。
        std::sort(entry.params.begin(), entry.params.end(),
                  [](const Param &a, const Param &b) { return a.name < b.name; });
    }
    std::sort(out.entries.begin(), out.entries.end(), [](const Entry &a, const Entry &b) { return a.key < b.key; });
    if (out.entries.empty()) {
        diags.error(path, "base file holds no entry");
    }
    return out;
}

/// @brief 读译文文件（纯词条）。
auto load_locale_file(const std::string &path, Diags &diags) -> std::optional<LocaleTexts> {
    const auto root = read_json(path, diags);
    if (!root) {
        return std::nullopt;
    }
    const std::optional<std::string> tag = string_field(*root, "locale");
    if (!tag) {
        diags.error(path, "translation file has no \"locale\" tag");
        return std::nullopt;
    }
    if (!locale_from_tag(*tag)) {
        diags.error(path, "locale tag shape is invalid (language[-script][-region]): " + *tag);
        return std::nullopt;
    }
    std::map<std::string, std::string> texts;
    for (const auto e : messages_object(*root)->entries()) {
        const std::string key(e.key);
        if (!e.key.empty() && e.key[0] == '@') {
            diags.warn(path, "translation files carry no metadata; ignored '" + key + "'");
            continue;
        }
        const std::optional<std::string_view> text = e.value.as_string();
        if (!text.has_value()) {
            diags.error(path, "translation '" + key + "' value must be a string");
            continue;
        }
        texts[key] = std::string{*text};  // 键唯一性由 JSON 容器保证（见 load_base）
    }
    return std::make_pair(*tag, std::move(texts));
}

/// @brief 内置保留区规范名 → Id。
auto builtin_id_map() -> std::map<std::string, std::uint32_t> {
    std::map<std::string, std::uint32_t> out;
    std::uint32_t k = 0;
    for (std::string_view name : aurora::detail::AURORA_BUILTIN_MESSAGE_NAMES) {
        out[std::string(name)] = aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE + k;
        ++k;
    }
    return out;
}

// ---- 产出：AUM1 bin ----

/// @brief 小端写入助手。
auto put_u16(std::vector<std::byte> &out, std::uint32_t v) -> void {
    out.push_back(static_cast<std::byte>(v & 0xFFU));
    out.push_back(static_cast<std::byte>((v >> 8U) & 0xFFU));
}

auto put_u32(std::vector<std::byte> &out, std::uint32_t v) -> void {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::byte>((v >> shift) & 0xFFU));
    }
}

auto put_str(std::vector<std::byte> &out, std::string_view text) -> void {
    for (char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

/// @brief 组装 AUM1 v1 词表缓冲。
///
/// 名字表按 Id 升序，空名是保留区空洞占位（载入侧不进名字索引）；字符串池按「locale 标签升序 ×
/// Id 升序」追加。同输入逐字节同输出。
/// @param id_base 应用词表基址。
/// @param app_names 应用规范名（已按字典序，不含内置 key）。
/// @param merge_builtin 是否并入内置保留区（任一输入文件命中内置名时为真）。
/// @param locales locale 档（tag → 规范名 → 模板），按 tag 升序传入。
/// @param diags 诊断累积器。
/// @return bin 字节；失败为空。
auto build_bin(std::uint32_t id_base, const std::vector<std::string> &app_names, bool merge_builtin,
               const std::vector<LocaleTexts> &locales, Diags &diags) -> std::vector<std::byte> {
    constexpr std::uint32_t bin_missing = 0xFFFFFFFFU;
    const std::map<std::string, std::uint32_t> builtin =
        merge_builtin ? builtin_id_map() : std::map<std::string, std::uint32_t>{};
    const std::uint32_t block_base = merge_builtin ? aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE : id_base;
    // 区间不重叠由调用方兜住（main 拦下 `id_base < 保留区上界`），此处只做布局，不再重复报同一条规则。
    const std::uint32_t id_count =
        id_base + static_cast<std::uint32_t>(app_names.size()) - block_base;  // 区间 [block_base, id_base+app)

    std::vector<std::string> names(id_count);
    if (merge_builtin) {
        for (std::uint32_t k = 0; k < aurora::detail::AURORA_BUILTIN_MESSAGE_NAMES.size(); ++k) {
            names.at(k) = std::string(aurora::detail::AURORA_BUILTIN_MESSAGE_NAMES.at(k));
        }
    }
    for (std::size_t k = 0; k < app_names.size(); ++k) {
        names[id_base - block_base + k] = app_names[k];
    }
    std::map<std::string, std::uint32_t> slot_of;
    for (std::uint32_t k = 0; k < id_count; ++k) {
        if (!names[k].empty()) {
            slot_of[names[k]] = k;
        }
    }

    std::vector<std::byte> pool;
    std::vector<std::pair<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>>> tables;
    for (const LocaleTexts &locale : locales) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> slots(
            id_count, std::make_pair(bin_missing, 0U));  // {offset, length}
        for (const auto &[key, text] : locale.second) {
            const auto found = slot_of.find(key);
            if (found == slot_of.end()) {
                continue;  // 未知 key 由调用侧判错，这里不重复报
            }
            if (pool.size() + text.size() > 0xFFFFFFFFUL) {
                diags.error("<bin>", "string pool exceeds the u32 offset domain");
                return {};
            }
            slots[found->second] = {static_cast<std::uint32_t>(pool.size()), static_cast<std::uint32_t>(text.size())};
            put_str(pool, text);
        }
        tables.emplace_back(locale.first, std::move(slots));
    }

    std::vector<std::byte> out;
    put_str(out, "AUM1");
    put_u32(out, 1);
    put_u32(out, block_base);
    put_u32(out, id_count);
    put_u32(out, static_cast<std::uint32_t>(tables.size()));
    for (const std::string &name : names) {
        if (name.size() > 0xFFFFUL) {
            diags.error("<bin>", "canonical name exceeds the u16 length domain: " + name);
            return {};
        }
        put_u16(out, static_cast<std::uint32_t>(name.size()));
        put_str(out, name);
    }
    for (const auto &[tag, slots] : tables) {
        if (tag.size() > 0xFFUL) {
            diags.error("<bin>", "locale tag exceeds the 1-byte length domain: " + tag);
            return {};
        }
        out.push_back(static_cast<std::byte>(tag.size()));
        put_str(out, tag);
        for (const auto &[offset, length] : slots) {
            put_u32(out, offset);
            put_u32(out, length);
        }
    }
    out.insert(out.end(), pool.begin(), pool.end());
    return out;
}

// ---- 产出：生成头 ----

/// @brief 写文本文件；失败记 error。
auto write_file(const std::string &path, const std::string &content, Diags &diags) -> void {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        diags.error(path, "cannot write the output file");
        return;
    }
    out << content;
}

/// @brief 写字节文件；失败记 error。
auto write_file(const std::string &path, const std::vector<std::byte> &content, Diags &diags) -> void {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        diags.error(path, "cannot write the output file");
        return;
    }
    // std::byte 与 char 之间没有静态转换路径，
    // ofstream::write 只收 const char*；此处不改字节值，只换视图。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    out.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
}

/// @brief C++ 字符串字面量转义。
auto cpp_escape(std::string_view text) -> std::string {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

/// @brief 形参类型 → C++ 类型名。
auto param_cpp_type(const std::string &type) -> std::string {
    if (type == "int64") {
        return "std::int64_t";
    }
    if (type == "double") {
        return "double";
    }
    if (type == "date") {
        return "aurora::CivilDate";
    }
    return "std::string";
}

/// @brief 应用词表生成头（Id 常量 + 每 key 一枚 inline 构造函数）。
/// @param base_path 基准 JSON 路径。
/// @param cpp_namespace 生成 API 的 C++ namespace。
/// @param id_base 应用词表 Id 起点。
/// @param entries 基准词条（内置 key 已剔除）。
/// @param locale_inputs 译文文件路径（按 locale tag 字典序，与 bin 块序同源，故命令行顺序不改产物）。
auto emit_app_header(const std::string &base_path, const std::string &cpp_namespace, std::uint32_t id_base,
                     const std::vector<Entry> &entries, const std::vector<std::string> &locale_inputs) -> std::string {
    const std::string source = display_path(base_path);  // 横幅按正斜杠写，产物跨平台逐字节一致
    std::string h = "// AUTO-GENERATED by gen_i18n from " + source + " - DO NOT EDIT.\n";
    h += "// Regenerate: gen_i18n <out.hpp> <out.bin> " + cpp_namespace + " --id-base " + std::to_string(id_base) +
         " " + source;
    for (const std::string &path : locale_inputs) {
        h += " " + display_path(path);
    }
    h += "\n\n#pragma once\n\n";
    h += "#include <cstdint>\n#include <string>\n#include <utility>\n#include <vector>\n\n";
    h += "#include \"aurora/i18n/localized_string.h\"\n#include \"aurora/i18n/message_id.h\"\n\n";
    h += "namespace " + cpp_namespace + "::id {\n\n";
    h += "/// @internal Vocabulary Id constants (bin order; dots become underscores, uppercased with the"
         " AURORA_ prefix).\n";
    for (const Entry &e : entries) {
        h += "inline constexpr aurora::MessageId " + id_constant_name(e.key) + "{" + std::to_string(e.id) + "};\n";
    }
    h += "\n}  // namespace " + cpp_namespace + "::id\n\n";
    // 点分段 → 嵌套 namespace（末段作函数名）；同前缀词条并到一个块里，前缀按字典序产出。
    std::map<std::string, std::vector<const Entry *>> groups;
    for (const Entry &e : entries) {
        groups[ns_prefix(e.key)].push_back(&e);
    }
    for (const auto &[prefix, list] : groups) {
        const std::string ns = prefix.empty() ? cpp_namespace : concat_text(cpp_namespace, "::", prefix);
        h += "namespace " + ns + " {\n\n";
        for (const Entry *e : list) {
            h += "/// @brief Message for \"" + e->key + "\".\n";
            if (!e->description.empty() && e->description.size() + 8 <= 120) {
                h += "///\n/// " + e->description + "\n";
            }
            std::string decl = "inline auto " + leaf_name(e->key) + "(";
            for (std::size_t k = 0; k < e->params.size(); ++k) {
                if (k != 0) {
                    decl += ", ";
                }
                decl += param_cpp_type(e->params[k].type) + " " + e->params[k].name;
            }
            h += decl + ") -> aurora::LocalizedString {\n";
            h +=
                "    return aurora::LocalizedString::tr(" + cpp_namespace + "::id::" + id_constant_name(e->key) + ", {";
            if (e->params.empty()) {
                h += "});\n";
            } else {
                h += "\n";
                for (const Param &p : e->params) {
                    const std::string value = p.type == "string" ? "std::move(" + p.name + ")" : p.name;
                    h += "        {\"" + p.name + "\", " + value + "},\n";
                }
                h += "    });\n";
            }
            h += "}\n\n";
        }
        h += "}  // namespace " + ns + "\n";
    }
    return h;
}

/// @brief 内置保留区生成头（codespec/i18n_builtin.json 的产物形态：Id 常量 + 名字 / 模板平行数组）。
auto emit_builtin_header(const std::string &source_path, const std::vector<Entry> &entries) -> std::string {
    std::string h = "// AUTO-GENERATED from " + display_path(source_path) + " — DO NOT EDIT.\n";
    h += "// Regenerate: gen_i18n --emit-builtin <the header above> " + display_path(source_path) + "\n";
    h += "/// @brief Built-in message vocabulary: reserved-region Id constants, canonical names and English\n";
    h += "/// fallback templates (the run-time defaults the library ships without translations).\n";
    h += "/// @file\n";
    h += "#pragma once\n\n";
    h += "#include \"aurora/i18n/message_id.h\"\n\n";
    h += "#include <array>\n#include <cstddef>\n#include <string_view>\n\n";
    h += "namespace aurora::msg {\n\n";
    // 每个 Id 常量带一条尾注（规范名即其身份），既不挤占组注释，也满足公共数据成员须有文档的门禁。
    for (const Entry &e : entries) {
        h += "inline constexpr MessageId " + id_constant_name(e.key) + "{" + std::to_string(e.id) + "};  ///< " +
             e.key + "\n";
    }
    h += "}  // namespace aurora::msg\n\n";
    h += "namespace aurora::detail {\n\n";
    h += "/// @brief Base of the built-in reserved region (application vocabularies start at 1024).\n";
    h += "inline constexpr std::uint32_t AURORA_BUILTIN_MESSAGE_ID_BASE = 0;\n";
    h += "/// @brief Built-in canonical names, sorted by name (Id order follows the source order).\n";
    h += "inline constexpr std::array<std::string_view, " + std::to_string(entries.size()) +
         "> AURORA_BUILTIN_MESSAGE_NAMES{\n";
    for (const Entry &e : entries) {
        h += "    \"" + cpp_escape(e.key) + "\",\n";
    }
    h += "};\n";
    h += "/// @brief Built-in English templates, parallel with the names (call-site fallbacks, not translations).\n";
    h += "inline constexpr std::array<std::string_view, " + std::to_string(entries.size()) +
         "> AURORA_BUILTIN_MESSAGE_TEMPLATES{\n";
    for (const Entry &e : entries) {
        h += "    \"" + cpp_escape(e.text) + "\",\n";
    }
    h += "};\n\n";
    h += "/// @brief Built-in canonical name for an Id.\n";
    h += "/// @param id Message Id, expected inside the reserved region.\n";
    h += "/// @return Canonical name; empty view when the Id lies outside the region.\n";
    h += "/// @note Thread: thread-safe (pure lookup)\n";
    h += "/// @note Side-effects: none\n";
    h += "/// @note Rebuildable: yes\n";
    h += "constexpr auto builtin_name(aurora::MessageId id) -> std::string_view {\n";
    h += "    if (id.value < AURORA_BUILTIN_MESSAGE_ID_BASE\n";
    h += "        || id.value >= AURORA_BUILTIN_MESSAGE_ID_BASE + AURORA_BUILTIN_MESSAGE_NAMES.size()) {\n";
    h += "        return {};\n";
    h += "    }\n";
    h += "    return AURORA_BUILTIN_MESSAGE_NAMES.at(id.value - AURORA_BUILTIN_MESSAGE_ID_BASE);\n";
    h += "}\n\n";
    h += "}  // namespace aurora::detail\n";
    return h;
}

// ---- 模式入口 ----

/// @brief `--emit-builtin` 模式：由内置狗粮源产出保留区头。
auto run_emit_builtin(const std::string &out_hpp, const std::string &source, Diags &diags) -> int {
    auto base = load_base(source, diags);
    if (!base) {
        (void)diags.report();
        return 1;
    }
    const std::string tag = base->locale;
    const aurora::Locale loc = locale_from_tag(tag).value_or(aurora::Locale{});
    for (std::size_t k = 0; k < base->entries.size(); ++k) {
        Entry &e = base->entries[k];
        e.id = aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE + static_cast<std::uint32_t>(k);
        std::map<std::string, std::string> types;
        for (const Param &p : e.params) {
            types[p.name] = p.type;
        }
        // 内置源不带元数据，只做骨架 / 编码校验（形参类型由控件调用点保证）。
        analyze_template(source, e.key, e.text, loc, types, false, false, diags);
    }
    if (base->id_base != aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE) {
        diags.error(source, "builtin source id_base must equal the reserved-region base (" +
                                std::to_string(base->id_base) +
                                " != " + std::to_string(aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE) + ")");
    }
    if (!diags.report()) {
        return 1;
    }
    write_file(out_hpp, emit_builtin_header(source, base->entries), diags);
    return diags.errors.empty() ? 0 : 1;
}

/// @brief 应用词表模式：基准 + 译文 → 生成头 + bin。
auto run_app(const std::vector<std::string> &args, Diags &diags) -> int {
    const std::string &out_hpp = args[0];
    const std::string &out_bin = args[1];
    const std::string &cpp_namespace = args[2];
    std::uint32_t id_base = AURORA_BUILTIN_RESERVED_SIZE;  // 紧跟内置保留区之后
    bool id_base_from_cli = false;
    std::vector<std::string> inputs;
    for (std::size_t k = 3; k < args.size(); ++k) {
        if (args[k] == "--id-base") {
            if (k + 1 >= args.size()) {
                err("--id-base needs a number");
                return 2;
            }
            id_base = static_cast<std::uint32_t>(std::strtoul(args[++k].c_str(), nullptr, 10));
            id_base_from_cli = true;
        } else if (args[k].starts_with("--id-base=")) {
            id_base = static_cast<std::uint32_t>(std::strtoul(args[k].substr(10).c_str(), nullptr, 10));
            id_base_from_cli = true;
        } else {
            inputs.push_back(args[k]);
        }
    }
    if (inputs.empty()) {
        err("at least one base.json is required");
        return 2;
    }
    for (std::string_view seg : split(cpp_namespace, ':')) {
        if (seg.empty()) {
            continue;  // `a::::b` 之类由段数校验兜住
        }
        if (!is_identifier_segment(seg)) {
            err("cpp-namespace segment cannot be a C++ identifier: " + std::string(seg));
            return 2;
        }
    }

    auto base = load_base(inputs[0], diags);
    if (!base) {
        (void)diags.report();
        return 1;
    }
    if (!id_base_from_cli) {
        id_base = base->id_base;
    }
    const aurora::Locale base_loc = locale_from_tag(base->locale).value_or(aurora::Locale{});
    if (!locale_from_tag(base->locale)) {
        diags.error(inputs[0], "base locale tag shape is invalid: " + base->locale);
    }

    std::vector<LocaleTexts> locales;
    std::map<std::string, std::string> path_of_tag;  // 译文 tag -> 来源路径（生成头复现注释按 tag 序取路径）
    for (std::size_t k = 1; k < inputs.size(); ++k) {
        auto loaded = load_locale_file(inputs[k], diags);
        if (loaded) {
            path_of_tag[loaded->first] = inputs[k];
            locales.push_back(std::move(*loaded));
        }
    }
    {
        LocaleTexts base_texts;
        base_texts.first = base_loc.tag();
        for (const Entry &e : base->entries) {
            base_texts.second[e.key] = e.text;
        }
        locales.push_back(std::move(base_texts));
    }
    {
        std::set<std::string> seen;
        for (const LocaleTexts &locale : locales) {
            if (!seen.insert(locale.first).second) {
                diags.error(inputs[0], "duplicate locale tag: " + locale.first);
            }
        }
    }

    const std::map<std::string, std::uint32_t> builtin = builtin_id_map();
    bool merge_builtin = false;
    std::vector<std::string> app_names;
    for (const Entry &e : base->entries) {
        if (builtin.contains(e.key)) {
            // 内置 key 的英文名归库（保留区由 `--emit-builtin` 单一来源产出），应用侧只能给译文。
            merge_builtin = true;
            diags.error(inputs[0], "entry '" + e.key +
                                       "' is a library builtin message name; override it in a locale "
                                       "file instead of redefining it in the base vocabulary");
        } else {
            app_names.push_back(e.key);  // base->entries 已按字典序
        }
    }
    for (const LocaleTexts &locale : locales) {
        for (const auto &[key, text] : locale.second) {
            if (builtin.contains(key)) {
                merge_builtin = true;
            }
        }
    }
    const std::uint32_t reserved_end = aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE + AURORA_BUILTIN_RESERVED_SIZE;
    if (id_base < reserved_end) {
        diags.error(inputs[0], "application vocabulary base must not fall inside the builtin reserved region [" +
                                   std::to_string(aurora::detail::AURORA_BUILTIN_MESSAGE_ID_BASE) + ", " +
                                   std::to_string(reserved_end) + ")");
    }

    std::map<std::string, std::uint32_t> id_of;
    for (const auto &[name, bid] : builtin) {
        id_of[name] = bid;
    }
    for (std::size_t k = 0; k < app_names.size(); ++k) {
        id_of[app_names[k]] = id_base + static_cast<std::uint32_t>(k);
    }

    // 基准模板校验（元数据 + CLDR 覆盖）
    for (Entry &e : base->entries) {
        e.id = id_of.at(e.key);
        std::map<std::string, std::string> types;
        for (const Param &p : e.params) {
            types[p.name] = p.type;
        }
        if (const std::optional<TemplateShape> shape =
                analyze_template(inputs[0], e.key, e.text, base_loc, types, true, true, diags)) {
            e.shape = *shape;
        }
    }

    // 译文：骨架 + 跨语言结构树比对
    std::map<std::string, const Entry *> by_key;
    for (const Entry &e : base->entries) {
        by_key[e.key] = &e;
    }
    // 内置 key 的译文走同一比对通道：基准骨架取自工具内嵌的同一份词表（「内置词表同源锁」的另一半
    // ——内嵌副本与生成头同源，译文漂移同样在这一条腿上红灯）。
    std::map<std::string, TemplateShape> builtin_shape;
    for (std::size_t k = 0; k < aurora::detail::AURORA_BUILTIN_MESSAGE_NAMES.size(); ++k) {
        const std::unique_ptr<MessageNode> ast =
            aurora::detail::parse_message_template(aurora::detail::AURORA_BUILTIN_MESSAGE_TEMPLATES.at(k));
        if (ast != nullptr) {
            TemplateFacts facts;
            collect_facts(*ast, /*in_plural=*/false, facts);
            TemplateShape shape;
            shape.sig = signature_of(*ast, true);
            shape.sig_bare = signature_of(*ast, false);
            shape.select_keys = facts.select_keys;
            std::sort(shape.select_keys.begin(), shape.select_keys.end());
            shape.select_keys.erase(std::unique(shape.select_keys.begin(), shape.select_keys.end()),
                                    shape.select_keys.end());
            builtin_shape[std::string(aurora::detail::AURORA_BUILTIN_MESSAGE_NAMES.at(k))] = std::move(shape);
        }
    }
    for (const LocaleTexts &locale : locales) {
        const aurora::Locale loc = locale_from_tag(locale.first).value_or(aurora::Locale{});
        for (const auto &[key, text] : locale.second) {
            const auto found = by_key.find(key);
            if (found == by_key.end()) {
                const auto reserved = builtin_shape.find(key);
                if (reserved == builtin_shape.end()) {
                    diags.error(inputs[0], "translation '" + key + "' of locale '" + locale.first +
                                               "' is absent from the base vocabulary");
                } else {
                    // 内置词条无应用侧元数据：形参类型由控件调用点保证，这里只比骨架。
                    if (const std::optional<TemplateShape> shape =
                            analyze_template(inputs[0], key, text, loc, {}, false, true, diags)) {
                        compare_structure(inputs[0], key, "library template", reserved->second, locale.first, *shape,
                                          diags);
                    }
                }
                continue;
            }
            std::map<std::string, std::string> types;
            for (const Param &p : found->second->params) {
                types[p.name] = p.type;
            }
            if (const std::optional<TemplateShape> shape =
                    analyze_template(inputs[0], key, text, loc, types, false, true, diags)) {
                compare_structure(inputs[0], key, "base", found->second->shape, locale.first, *shape, diags);
            }
        }
        if (locale.first != base_loc.tag()) {
            for (const Entry &e : base->entries) {
                if (!locale.second.contains(e.key)) {
                    diags.warn(inputs[0], "locale '" + locale.first + "' has no translation for entry '" + e.key + "'");
                }
            }
        }
    }

    // Id 常量名 / 函数全名冲突检查
    {
        std::vector<std::string> constants;
        for (const Entry &e : base->entries) {
            constants.push_back(id_constant_name(e.key));
        }
        std::sort(constants.begin(), constants.end());
        const auto dup = std::adjacent_find(constants.begin(), constants.end());
        if (dup != constants.end()) {
            diags.error(inputs[0], "Id constant name collides after flattening the dots: " + *dup);
        }
    }

    if (!diags.report()) {
        return 1;
    }

    std::sort(locales.begin(), locales.end(),
              [](const LocaleTexts &a, const LocaleTexts &b) { return a.first < b.first; });
    const std::vector<std::byte> bin = build_bin(id_base, app_names, merge_builtin, locales, diags);
    if (bin.empty()) {
        (void)diags.report();
        return 1;
    }

    std::vector<Entry> header_entries;
    for (const Entry &e : base->entries) {
        if (!builtin.contains(e.key)) {
            header_entries.push_back(e);  // 内置 key 的覆盖项由 aurora::msg 常量表达
        }
    }
    std::vector<std::string> locale_inputs;
    // 复现注释按 tag 字典序取来源路径（locales 刚排过序），命令行换序不换产物字节。
    for (const LocaleTexts &locale : locales) {
        const auto origin = path_of_tag.find(locale.first);
        if (origin != path_of_tag.end()) {
            locale_inputs.push_back(origin->second);
        }
    }
    write_file(out_hpp, emit_app_header(inputs[0], cpp_namespace, id_base, header_entries, locale_inputs), diags);
    write_file(out_bin, bin, diags);
    err("generated: " + std::to_string(base->entries.size()) + " entries / " + std::to_string(locales.size()) +
        " locales (builtin reserved region " + std::string(merge_builtin ? "merged" : "not referenced") + ") -> " +
        out_hpp + ", " + out_bin);
    return 0;
}

}  // namespace

// 工具刻意使用异常（string 分配 / ofstream），故不强求 main noexcept（口径同 gen_error_codes）。
auto main(int argc, char **argv) -> int {  // NOLINT(bugprone-exception-escape)
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::vector<std::string> args(argv + 1, argv + argc);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    Diags diags;
    if (args.empty()) {
        err("usage: gen_i18n <out.hpp> <out.bin> <cpp-namespace> [--id-base N] <base.json> [locale.json...]");
        err("       gen_i18n --emit-builtin <out.hpp> <builtin.json>");
        return 2;
    }
    if (args[0] == "--emit-builtin") {
        if (args.size() != 3) {
            err("usage: gen_i18n --emit-builtin <out.hpp> <builtin.json>");
            return 2;
        }
        return run_emit_builtin(args[1], args[2], diags);
    }
    if (args.size() < 4) {
        err("usage: gen_i18n <out.hpp> <out.bin> <cpp-namespace> [--id-base N] <base.json> [locale.json...]");
        return 2;
    }
    return run_app(args, diags);
}

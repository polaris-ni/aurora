#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/i18n/locale.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/i18n/plural.h"

namespace aurora {

/**
 * @brief i18n 字符串表（运行时，specification/07-environment-modifier.md §6 国际化）：按 Locale 查 key →
 * 模板，并格式化。
 *
 * 模板语法：
 * - 位置占位：`{0}` `{1}` … 用 args[i] 替换。
 * - CLDR 六类复数：`{0, plural, zero=… one=… two=… few=… many=… other=…}`，按 `plural_category(args[0], loc)`
 * 选分支（详见 plural.h）。
 *
 * 用法：
 * @code
 *   auto& t = default_string_table();
 *   t.add(Locale{"en"}, "greeting", "Hello {0}");
 *   t.add(Locale{"zh"}, "greeting", "你好 {0}");
 *   Text{ .content = LocalizedString::tr("greeting", {LocalizedString{"Aurora"}}) };
 * @endcode
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
class StringTable {
  public:
    /// @brief 为某区域设置 key → 模板。
    auto add(const Locale &loc, const std::string &key, const std::string &tmpl) -> void {
        data_[loc.tag()][key] = tmpl;
    }

    /// @brief 设置默认区域（查表缺省回退）。
    auto set_default_locale(const Locale &loc) -> void { default_ = loc; }

    /// @brief 查找某 Locale 下 key 的模板；缺失则回退默认 Locale；再缺失返回 nullopt。
    [[nodiscard]] auto lookup(const std::string &key, const Locale &loc) const -> std::optional<std::string> {
        if (const auto lit = data_.find(loc.tag()); lit != data_.end()) {
            if (const auto kit = lit->second.find(key); kit != lit->second.end()) {
                return kit->second;
            }
        }
        if (const auto dit = data_.find(default_.tag()); dit != data_.end()) {
            if (const auto kit = dit->second.find(key); kit != dit->second.end()) {
                return kit->second;
            }
        }
        return std::nullopt;
    }

    /// @brief 解析一个本地化字符串（查表 + 递归格式化参数）。
    [[nodiscard]] auto resolve(const LocalizedString &ls, const Locale &loc) const -> std::string {
        if (!ls.localize) {
            return ls.text;
        }
        auto r = lookup(ls.key, loc);
        if (!r) {
            return ls.text;  // 查表失败回退字面量
        }
        std::vector<std::string> as;
        as.reserve(ls.args.size());
        for (const auto &a : ls.args) {
            as.push_back(resolve(a, loc));
        }
        return format(*r, as, loc);
    }

    /// @brief 从 open 位置的 '{' 起，找到与之配平（考虑嵌套 {}）的 '}' 下标。
    [[nodiscard]] static auto find_closing_brace(const std::string &s, std::size_t open) -> std::size_t {
        int depth = 0;
        for (std::size_t k = open; k < s.size(); ++k) {
            if (s[k] == '{') {
                ++depth;
            } else if (s[k] == '}') {
                --depth;
                if (depth == 0) {
                    return k;
                }
            }
        }
        return std::string::npos;
    }

    /// @brief 格式化模板：替换 `{i}` 占位与 `{n, plural, one=… other=…}` 复数块。
    [[nodiscard]] static auto format(const std::string &tmpl, const std::vector<std::string> &args,
                                     const Locale &loc = Locale{}) -> std::string {
        std::string out;
        out.reserve(tmpl.size());
        const std::size_t n = tmpl.size();
        std::size_t i = 0;
        while (i < n) {
            if (tmpl[i] == '{') {
                const std::size_t j = find_closing_brace(tmpl, i);
                if (j == std::string::npos) {
                    out += tmpl[i];
                    ++i;
                    continue;
                }
                const std::string inner = tmpl.substr(i + 1, j - i - 1);
                const std::size_t ppos = inner.find(", plural,");
                if (ppos != std::string::npos) {
                    int idx = 0;
                    try {
                        idx = std::stoi(inner.substr(0, ppos));
                    } catch (...) {
                        idx = 0;
                    }
                    const std::string rest = inner.substr(ppos + 9);  // 跳过 ", plural,"
                    const std::string val = (idx >= 0 && std::cmp_less(idx, args.size())) ? args[idx] : "";
                    // CLDR 六类复数：依 loc.language 的规则表把数值映射到类别，再选对应分支。
                    // 旧式 one=/other= 模板在缺类别时回退 other=，与既有 num==1→one 行为向后兼容。
                    const PluralCategory cat = plural_category(to_double(val), loc);
                    std::string chosen = select_plural_branch(rest, cat);
                    // 分支值被模板写成 {…} 整段包裹（如 "other={many items}"）时去掉包裹花括号后再递归格式化，
                    // 否则 "{many items}" 会被当成 {0} 占位再次替换成参数值。
                    if (!chosen.empty() && chosen.front() == '{' && chosen.back() == '}') {
                        chosen = chosen.substr(1, chosen.size() - 2);
                    }
                    // 复数分支内可能含 {0} 占位（如 "one={0} item"），需递归格式化（带 locale 透传）。
                    out += format(chosen, args, loc);
                } else {
                    int idx = 0;
                    try {
                        idx = std::stoi(inner);
                    } catch (...) {
                        idx = 0;
                    }
                    if (idx >= 0 && std::cmp_less(idx, args.size())) {
                        out += args[idx];
                    }
                }
                i = j + 1;
            } else {
                out += tmpl[i];
                ++i;
            }
        }
        return out;
    }

  private:
    static auto trim(std::string &s) -> void {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.erase(s.begin());
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
    }

    static auto to_int(const std::string &s) -> int {
        try {
            return std::stoi(s);
        } catch (...) {
            return 0;
        }
    }

    /// @brief 把字符串解析为 double（用于复数类别判定）；失败回退 0。
    static auto to_double(const std::string &s) -> double {
        try {
            return std::stod(s);
        } catch (...) {
            return 0.0;
        }
    }

    /// @brief 把复数类别映射到模板关键字串（zero/one/two/few/many/other）。
    static auto category_keyword(PluralCategory cat) -> const char * {
        switch (cat) {
            case PluralCategory::Zero:
                return "zero";
            case PluralCategory::One:
                return "one";
            case PluralCategory::Two:
                return "two";
            case PluralCategory::Few:
                return "few";
            case PluralCategory::Many:
                return "many";
            case PluralCategory::Other:
                return "other";
        }
        return "other";
    }

    /// @brief 从复数块（rest）提取 `kw=` 之后的分支值，直到下一个分支关键字（或结尾）。
    static auto extract_branch(const std::string &rest, const std::string &kw) -> std::string {
        const std::string token = kw + "=";
        const std::size_t pos = rest.find(token);
        if (pos == std::string::npos) {
            return "";
        }
        const std::size_t start = pos + token.size();
        static const char *kws[] = {"zero=", "one=", "two=", "few=", "many=", "other="};
        std::size_t end = std::string::npos;
        for (const char *k : kws) {
            const std::size_t p = rest.find(k, start);
            if (p != std::string::npos && (end == std::string::npos || p < end)) {
                end = p;
            }
        }
        std::string val = (end == std::string::npos) ? rest.substr(start) : rest.substr(start, end - start);
        trim(val);
        return val;
    }

    /// @brief 按类别选择复数分支：先取该类别，缺则回退 other=，再缺则回退 one=；皆无返回空串。
    static auto select_plural_branch(const std::string &rest, PluralCategory cat) -> std::string {
        std::string v = extract_branch(rest, category_keyword(cat));
        if (!v.empty()) {
            return v;
        }
        v = extract_branch(rest, "other");
        if (!v.empty()) {
            return v;
        }
        return extract_branch(rest, "one");
    }

    std::map<std::string, std::map<std::string, std::string>> data_;  ///< localeTag → key → 模板
    Locale default_;  ///< 默认区域（回退）
};

/// @brief 进程级默认字符串表（供 widget 渲染时就地查表）。
[[nodiscard]] inline auto default_string_table() -> StringTable & {
    // 惰性构造的函数内 static：首次调用才建，跨 TU 初始化顺序问题在此不存在（本检查的担心面）。
    // 仅浏览器口径命中——native 遍同一份代码不报（CODING_STANDARDS.md §5.2 的口径差异）。
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static StringTable instance;
    return instance;
}

inline auto LocalizedString::resolve(const StringTable *table, const Locale &loc) const -> std::string {
    if (!localize || table == nullptr) {
        return text;
    }
    return table->resolve(*this, loc);
}

}  // namespace aurora

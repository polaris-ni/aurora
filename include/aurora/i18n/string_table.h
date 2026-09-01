#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aurora/i18n/locale.h"
#include "aurora/i18n/localized_string.h"

namespace aurora {

/**
 * @brief i18n 字符串表（运行时，规格 §8 / Q3）：按 Locale 查 key → 模板，并格式化。
 *
 * 模板语法：
 * - 位置占位：`{0}` `{1}` … 用 args[i] 替换。
 * - 简版复数：`{0, plural, one=1 项 other={0} 项}`，按 args[0] 数值选 `one`/`other`。
 *
 * 用法：
 * @code
 *   auto& t = defaultStringTable();
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
        m_data[loc.tag()][key] = tmpl;
    }

    /// @brief 设置默认区域（查表缺省回退）。
    auto set_default_locale(const Locale &loc) -> void { m_default = loc; }

    /// @brief 查找某 Locale 下 key 的模板；缺失则回退默认 Locale；再缺失返回 nullopt。
    [[nodiscard]] auto lookup(const std::string &key, const Locale &loc) const -> std::optional<std::string> {
        if (const auto lit = m_data.find(loc.tag()); lit != m_data.end()) {
            if (const auto kit = lit->second.find(key); kit != lit->second.end()) {
                return kit->second;
            }
        }
        if (const auto dit = m_data.find(m_default.tag()); dit != m_data.end()) {
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
            return ls.text; // 查表失败回退字面量
        }
        std::vector<std::string> as;
        as.reserve(ls.args.size());
        for (const auto &a : ls.args) {
            as.push_back(resolve(a, loc));
        }
        return format(*r, as);
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
    // NOLINTNEXTLINE(*-function-cognitive-complexity)
    [[nodiscard]] static auto format(const std::string &tmpl, const std::vector<std::string> &args) -> std::string {
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
                    const std::string rest = inner.substr(ppos + 9); // 跳过 ", plural,"
                    const std::size_t oi = rest.find("one=");
                    const std::size_t oti = rest.find("other=");
                    if (oi != std::string::npos && oti != std::string::npos && oti > oi) {
                        std::string one = rest.substr(oi + 4, oti - (oi + 4));
                        std::string other = rest.substr(oti + 6);
                        trim(one);
                        trim(other);
                        const std::string val = (idx >= 0 && std::cmp_less(idx, args.size())) ? args[idx] : "";
                        const int num = to_int(val);
                        std::string chosen = (num == 1) ? one : other;
                        // 分支值被模板写成 one={...} / other={...}，去掉包裹花括号后再递归格式化，
                        // 否则 "{one item}" 会被当成 {0} 占位再次替换成参数值。
                        if (!chosen.empty() && chosen.front() == '{' && chosen.back() == '}') {
                            chosen = chosen.substr(1, chosen.size() - 2);
                        }
                        // 复数分支内可能含 {0} 占位（如 "one={0} item"），需递归格式化。
                        out += format(chosen, args);
                    }
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

    std::map<std::string, std::map<std::string, std::string>> m_data; ///< localeTag → key → 模板
    Locale m_default;                                                 ///< 默认区域（回退）
};

/// @brief 进程级默认字符串表（供 widget 渲染时就地查表）。
[[nodiscard]] inline auto default_string_table() -> StringTable & {
    static StringTable instance;
    return instance;
}

inline auto LocalizedString::resolve(const StringTable *table, const Locale &loc) const -> std::string {
    if (!localize || table == nullptr) {
        return text;
    }
    return table->resolve(*this, loc);
}

} // namespace aurora

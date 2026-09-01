#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "aurora/i18n/locale.h"

namespace aurora {

class StringTable; // 前向声明：resolve 的定义在 string_table.h（避免循环包含）

/**
 * @brief 可本地化字符串（i18n 运行时，specification/07-environment-modifier.md §6 国际化）。
 *
 * - 非本地化：直接持文本（`text`）
 * - 本地化：以 `key` 查 `StringTable`，按 `Locale` 取模板并用 `args` 格式化
 *   （支持 `{0}`/`{1}` 占位与 `{n, plural, one=… other=…}` 简版复数）。
 * 提供从 `std::string_view` 的隐式构造，便于 AI 直接写 `Text{ .content = "Hi" }`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: yes, via from_json
 */
struct LocalizedString {
    std::string text;                  ///< 字面文本（非本地化值 / 本地化查表失败时的回退）
    std::string key;                   ///< 本地化键（localize 为 true 时使用）
    std::vector<LocalizedString> args; ///< 模板参数（可嵌套本地化）
    bool localize = false;             ///< 是否走查表解析

    LocalizedString() = default;
    LocalizedString(const char *t) : text(t) {}
    LocalizedString(std::string t) : text(std::move(t)) {}
    LocalizedString(std::string_view t) : text(std::string(t)) {}

    /// @brief 构造一个待本地化的字符串（按 key 查表 + 格式化 args）。
    static auto tr(std::string key, std::vector<LocalizedString> a = {}) -> LocalizedString {
        LocalizedString s;
        s.key = std::move(key);
        s.args = std::move(a);
        s.localize = true;
        return s;
    }

    [[nodiscard]] auto c_str() const -> const char * { return text.c_str(); }

    auto operator==(const LocalizedString &o) const -> bool { return text == o.text && key == o.key; }

    /// @brief 解析为最终显示字符串：localize 且表中有条目 → 格式化模板；否则回退 `text`。
    /// `args` 递归解析（支持嵌套本地化参数）。
    [[nodiscard]] auto resolve(const StringTable *table, const Locale &loc) const -> std::string;
};

} // namespace aurora

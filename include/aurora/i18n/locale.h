#pragma once

#include <string>

namespace aurora {

/**
 * @brief 区域设置：语言/地区代码，用于 i18n 字符串解析。
 *
 * @note Thread: thread-safe (pure value type)
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct Locale {
    std::string language = "en";
    std::string region;

    [[nodiscard]] auto tag() const -> std::string { return region.empty() ? language : (language + "-" + region); }
};

}  // namespace aurora

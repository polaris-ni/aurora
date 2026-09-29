#pragma once

#include <string>

namespace aurora {

/// @brief 区域设置：语言/地区代码，用于 i18n 字符串解析。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct Locale {
    std::string language = "en";  ///< 语言子标签（如 "en"），默认英文
    std::string region;  ///< 地区子标签（如 "CN"），空串表示仅有语言

    /// @brief 拼装完整语言标签。
    /// @return region 为空时仅返回 language，否则返回 "language-region"。
    [[nodiscard]] auto tag() const -> std::string { return region.empty() ? language : (language + "-" + region); }
};

}  // namespace aurora

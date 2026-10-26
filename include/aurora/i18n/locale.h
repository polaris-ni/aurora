#pragma once

#include <string>

namespace aurora {

/// @brief 区域设置：语言 / 文字 / 地区代码，用于 i18n 字符串解析。
///
/// 子标签按 BCP-47 语义命名：`language`（必填）、`script`（文字，如 "Hans"/"Hant"）、
/// `region`（地区，如 "CN"/"US"）。回退阶梯（载入期物化，见 `MessageCatalog`）按
/// 「先丢 region 后丢 script」的 CLDR 右删次序展开。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
/// @note Rebuildable: no
struct Locale {
    std::string language = "en";  ///< 语言子标签（如 "en"），默认英文
    std::string script;  ///< 文字子标签（如 "Hans"/"Hant"），空串表示不区分文字
    std::string region;  ///< 地区子标签（如 "CN"），空串表示不限地区

    /// @brief 拼装完整语言标签。
    /// @return 按 `language[-script][-region]` 拼接，空子标签跳过。
    [[nodiscard]] auto tag() const -> std::string {
        std::string out = language;
        if (!script.empty()) {
            out += "-" + script;
        }
        if (!region.empty()) {
            out += "-" + region;
        }
        return out;
    }
};

}  // namespace aurora

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aurora::a11y {

/// @brief 文本单位（UIA `TextUnit` / AT-SPI2 文本边界的共享语义）。
enum class TextUnit : std::uint8_t {
    Character,  ///< Unicode 码点
    Word,       ///< 空白 / 标点启发式分词（零依赖；CJK 整段一词为已知退化，设计 §13 R6）
    Line,       ///< `\n` 分界
    Document,   ///< 全文
};

namespace detail {

/// @brief UTF-8 序列长度（首字节决定；非法首字节按 1 处理，与解码退化一致）。
[[nodiscard]] inline auto seq_len(unsigned char c) -> std::size_t {
    if ((c & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((c & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((c & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

/// @brief 解码 `text[i]` 起的单个码点（非法序列按单字节返回，永不越界）。
[[nodiscard]] inline auto decode_cp(std::string_view text, std::size_t i) -> std::pair<std::uint32_t, std::size_t> {
    if (i >= text.size()) {
        return {0, 0};
    }
    const auto c = static_cast<unsigned char>(text[i]);
    const std::size_t n = seq_len(c);
    if (i + n > text.size()) {
        return {c, 1};
    }
    std::uint32_t cp = 0;
    switch (n) {
        case 1:
            cp = c;
            break;
        case 2:
            cp = static_cast<std::uint32_t>(c & 0x1FU);
            break;
        case 3:
            cp = static_cast<std::uint32_t>(c & 0x0FU);
            break;
        default:
            cp = static_cast<std::uint32_t>(c & 0x07U);
            break;
    }
    for (std::size_t k = 1; k < n; ++k) {
        cp = (cp << 6U) | static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + k]) & 0x3FU);
    }
    return {cp, n};
}

}  // namespace detail

/// @brief UTF-8 ↔ UTF-16 偏移映射（A4 / §7.4）。
///
/// UIA 文本偏移的单位是 **UTF-16 code unit**，而 Aurora 内部（含 `Widget::accessibility_text()`
/// 等钩子）一律 **UTF-8 字节偏移**；换算只存在于平台桥边界，且由本表一次性 O(n) 构建。
///
/// 关键纪律（G9）：emoji 等非 BMP 字符占 2 个 UTF-16 单元，`MoveEndpointByUnit(Character)`
/// 会把端点落到代理对的**第二单元**——该索引在 UTF-8 侧无对应码点起点。故 `to_utf8()`
/// 一律**向下夹紧到码点起点**（高代理处），绝不产生指向码点中部的偏移。
/// @note Thread: main-thread only
/// @note Side-effects: pure
class UtfOffsetMap {
  public:
    UtfOffsetMap() = default;
    explicit UtfOffsetMap(std::string_view text) { build(text); }

    /// @brief 按文本重建映射（O(n) 一次；文本变化时须重建）。
    auto build(std::string_view text) -> void {
        text_ = text;
        starts_.clear();
        utf16_of_.clear();
        std::size_t utf8 = 0;
        std::size_t utf16 = 0;
        while (utf8 < text.size()) {
            const auto [cp, len] = detail::decode_cp(text, utf8);
            starts_.push_back(utf8);
            utf16_of_.push_back(utf16);
            utf16 += (cp > 0xFFFFU) ? 2U : 1U;  // 非 BMP：代理对占 2 个 UTF-16 单元
            utf8 += (len == 0) ? 1 : len;       // len==0 仅越界时出现；兜底前进 1 防死循环
        }
        // 末尾哨兵：使「全文长度」这一端点可被映射（如选区终点 = 文末）。
        starts_.push_back(utf8);
        utf16_of_.push_back(utf16);
    }

    [[nodiscard]] auto utf8_length() const -> std::size_t { return text_.size(); }
    [[nodiscard]] auto utf16_length() const -> std::size_t {
        return utf16_of_.empty() ? 0 : utf16_of_.back();
    }

    /// @brief UTF-16 偏移 → UTF-8 偏移（向下夹紧到码点起点，G9）。
    [[nodiscard]] auto to_utf8(std::size_t utf16_index) const -> std::size_t {
        if (utf16_of_.empty()) {
            return 0;
        }
        if (utf16_index >= utf16_of_.back()) {
            return text_.size();
        }
        // upper_bound 取第一个 utf16 偏移 > 目标者的前一位 ⇒ 该码点起点。
        const auto it = std::upper_bound(utf16_of_.begin(), utf16_of_.end(), utf16_index);
        const std::size_t idx = (it == utf16_of_.begin()) ? 0 : static_cast<std::size_t>(it - utf16_of_.begin()) - 1;
        return starts_.at(idx);
    }

    /// @brief UTF-8 偏移 → UTF-16 偏移（非法/中部偏移向下夹紧到码点起点）。
    [[nodiscard]] auto to_utf16(std::size_t utf8_index) const -> std::size_t {
        if (starts_.empty()) {
            return 0;
        }
        if (utf8_index >= text_.size()) {
            return utf16_of_.back();
        }
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), utf8_index);
        const std::size_t idx = (it == starts_.begin()) ? 0 : static_cast<std::size_t>(it - starts_.begin()) - 1;
        return utf16_of_.at(idx);
    }

    /// @brief 从 `utf8_index` 起按码点前进/后退 `count` 个（夹紧到 [0, size]）。
    [[nodiscard]] auto advance_utf8(std::size_t utf8_index, int count) const -> std::size_t {
        if (count == 0 || starts_.empty()) {
            return utf8_index;
        }
        std::size_t idx = 0;
        const auto it = std::lower_bound(starts_.begin(), starts_.end(), utf8_index);
        idx = static_cast<std::size_t>(it - starts_.begin());
        if (count > 0) {
            idx += static_cast<std::size_t>(count);
            if (idx >= starts_.size()) {
                return text_.size();
            }
            return starts_.at(idx);
        }
        const std::size_t back = static_cast<std::size_t>(-count);
        if (idx <= back) {
            return 0;
        }
        return starts_.at(idx - back);
    }

    /// @brief 按 UTF-16 单元前进/后退（UIA `MoveEndpointByUnit(Character)` 语义）。
    [[nodiscard]] auto advance_utf16(std::size_t utf16_index, int count) const -> std::size_t {
        const std::size_t target =
            (count < 0 && utf16_index < static_cast<std::size_t>(-count)) ? 0 : utf16_index + static_cast<std::size_t>(count);
        return to_utf8(target);
    }

  private:
    std::string_view text_;
    std::vector<std::size_t> starts_;   ///< 各码点的 UTF-8 起点（含末尾哨兵）
    std::vector<std::size_t> utf16_of_;  ///< 各码点起点对应的 UTF-16 偏移（含末尾哨兵）
};

/// @brief UTF-8 → UTF-16（`char16_t` 序列；代理对按原生单元，非 BMP 占 2）。
[[nodiscard]] inline auto utf8_to_utf16(std::string_view text) -> std::u16string {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto [cp, len] = detail::decode_cp(text, i);
        i += (len == 0) ? 1 : len;
        if (cp > 0xFFFFU) {
            const std::uint32_t v = cp - 0x10000U;
            out.push_back(static_cast<char16_t>(0xD800U + (v >> 10U)));
            out.push_back(static_cast<char16_t>(0xDC00U + (v & 0x3FFU)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

/// @brief UTF-16 → UTF-8（孤立代理按 U+FFFD 替换，不崩溃）。
[[nodiscard]] inline auto utf16_to_utf8(std::u16string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto u = static_cast<std::uint32_t>(text[i]);
        std::uint32_t cp = u;
        if (u >= 0xD800U && u <= 0xDBFFU) {
            // 高代理：后随低代理则合成非 BMP 码点；否则（孤立 / 位于串尾）按 U+FFFD 替换——
            // 若放任其落进下面的通用编码分支，会产出 CESU-8 式的非法 UTF-8（0xED 0xA0 0xBD），
            // 下游控件（文本编辑、读屏文本朗读）解到该字节序列即行为未定义。
            cp = 0xFFFDU;
            if (i + 1 < text.size()) {
                const auto lo = static_cast<std::uint32_t>(text[i + 1]);
                if (lo >= 0xDC00U && lo <= 0xDFFFU) {
                    cp = 0x10000U + ((u - 0xD800U) << 10U) + (lo - 0xDC00U);
                    ++i;
                }
            }
        } else if (u >= 0xDC00U && u <= 0xDFFFU) {
            cp = 0xFFFDU;  // 孤立低代理
        }
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }
    return out;
}

/// @brief UTF-8 文本的 UTF-16 单元数（不建映射的便捷口径）。
[[nodiscard]] inline auto utf16_length_of(std::string_view text) -> std::size_t {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto [cp, len] = detail::decode_cp(text, i);
        i += (len == 0) ? 1 : len;
        n += (cp > 0xFFFFU) ? 2 : 1;
    }
    return n;
}

// ---- 文本单位（Word / Line / Document 启发式；零依赖，三桥共用）----

/// @brief 该码点是否为「词内」字符（非空白、非 ASCII 标点）。
[[nodiscard]] inline auto is_word_char(std::uint32_t cp) -> bool {
    if (cp == 0) {
        return false;
    }
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f') {
        return false;
    }
    // ASCII 标点视为分隔符；其余（含 CJK 全角标点本属 U+3000 段）按词内处理——
    // CJK 无空格分词，退化成「整段一词」是可接受代价（设计 §13 R6）。
    if (cp < 0x80U) {
        const bool punct = (cp >= '!' && cp <= '/') || (cp >= ':' && cp <= '@') || (cp >= '[' && cp <= '`') ||
                           (cp >= '{' && cp <= '~');
        return !punct;
    }
    return true;
}

/// @brief 把 `index` 所在位置按 `unit` 展开为 `[start, end)`（UTF-8 字节偏移）。
[[nodiscard]] inline auto expand_to_unit(std::string_view text, std::size_t index, TextUnit unit)
    -> std::pair<std::size_t, std::size_t> {
    if (index > text.size()) {
        index = text.size();
    }
    switch (unit) {
        case TextUnit::Document:
            return {0, text.size()};
        case TextUnit::Line: {
            std::size_t start = text.rfind('\n', index == 0 ? 0 : index - 1);
            start = (start == std::string_view::npos) ? 0 : start + 1;
            std::size_t end = text.find('\n', index);
            end = (end == std::string_view::npos) ? text.size() : end;
            return {start, end};
        }
        case TextUnit::Word: {
            // 先夹紧到码点起点，再向两侧扫同类字符。
            UtfOffsetMap map{text};
            const std::size_t at = map.advance_utf8(index, 0);
            std::size_t start = at;
            while (start > 0) {
                const std::size_t prev = map.advance_utf8(start, -1);
                if (!is_word_char(detail::decode_cp(text, prev).first)) {
                    break;
                }
                start = prev;
            }
            std::size_t end = at;
            while (end < text.size() && is_word_char(detail::decode_cp(text, end).first)) {
                const auto [cp_unused, len] = detail::decode_cp(text, end);
                (void)cp_unused;
                end += (len == 0) ? 1 : len;
            }
            return {start, end};
        }
        case TextUnit::Character:
        default: {
            const auto [cp_unused, len] = detail::decode_cp(text, index);
            (void)cp_unused;
            return {index, index + ((len == 0) ? 0 : len)};
        }
    }
}

}  // namespace aurora::a11y

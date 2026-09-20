// `ime_composition.h` 的实现：平台组合索引 → Aurora 码点下标的纯折算。
//
// 转换纪律见该头文件注释。整文件零平台依赖，可在无头 CI 逐用例断言（含 emoji / 代理对 /
// 孤立代理 / 属性数组缺尾等边界）。

#include "ime_composition.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "aurora/core/a11y_text.h"  // utf16_to_utf8（通用 UTF-16→UTF-8，孤立代理替换为 U+FFFD）

namespace aurora::ime {

namespace {

/// @brief 单元 `u` 归属的码点序号（`u` 落在代理对的两个单元之间时，夹紧到该码点起点）。
[[nodiscard]] auto cp_at(std::u16string_view text, std::size_t u) -> std::size_t {
    std::size_t cp = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto hi = static_cast<std::uint32_t>(text[i]);
        const bool paired = (hi >= 0xD800U && hi <= 0xDBFFU) && (i + 1 < text.size()) &&
                            static_cast<std::uint32_t>(text[i + 1]) >= 0xDC00U &&
                            static_cast<std::uint32_t>(text[i + 1]) <= 0xDFFFU;
        const std::size_t next = i + (paired ? 2U : 1U);
        if (u <= i) {
            break;  // 已达/越过目标单元：该码点不计
        }
        if (u < next) {
            return cp;  // u 落在本码点内部（仅代理对可能）→ 夹紧到其起点，计数不含它
        }
        i = next;
        ++cp;
    }
    return cp;
}

/// @brief 总码点数（按代理对合成计数，孤立代理各计一个——与 `utf16_to_utf8` 的 U+FFFD 一一对应）。
[[nodiscard]] auto cp_total(std::u16string_view text) -> std::size_t { return cp_at(text, text.size()); }

/// @brief 该属性值是否属于「待转换目标段」。
[[nodiscard]] auto is_target(std::uint8_t attr) -> bool {
    return attr == static_cast<std::uint8_t>(Attr::kTargetConverted) ||
           attr == static_cast<std::uint8_t>(Attr::kTargetNotConverted);
}

}  // namespace

auto utf16_index_to_cp_index(std::u16string_view text, std::size_t utf16_index) -> std::size_t {
    const std::size_t total = cp_total(text);
    if (utf16_index >= text.size()) {
        return total;
    }
    return std::min(cp_at(text, utf16_index), total);
}

auto target_selection(std::u16string_view text, const std::vector<std::uint8_t> &attrs) -> CpRange {
    // 属性数组按「较短者」生效：输入法给缺尾（attrs < 串长）时不越界，多余（异常）时忽略。
    const std::size_t n = std::min(attrs.size(), text.size());
    CpRange range;
    std::size_t first = 0;
    std::size_t last = 0;
    bool found = false;
    for (std::size_t u = 0; u < n; ++u) {
        if (!is_target(attrs[u])) {
            continue;
        }
        if (!found) {
            found = true;
            first = u;
            last = u;
            continue;
        }
        // 只取起始最靠前的**连续**段（IMM32 正常只给一段目标）。
        if (u == last + 1U) {
            last = u;
        } else {
            break;
        }
    }
    if (!found) {
        return range;
    }
    range.start = utf16_index_to_cp_index(text, first);
    range.end = utf16_index_to_cp_index(text, last);
    if (range.end < range.start) {
        range.end = range.start;  // 端点倒置（异常输入）退化为单点区间
    }
    return range;
}

auto make_preedit_state(std::u16string_view comp, std::size_t caret, const std::vector<std::uint8_t> &attrs)
    -> TextCompositionEvent {
    TextCompositionEvent e;
    e.preedit = a11y::utf16_to_utf8(comp);
    const std::size_t cp_count = cp_total(comp);
    e.cursor_index = std::min(utf16_index_to_cp_index(comp, caret), cp_count);
    const CpRange sel = target_selection(comp, attrs);
    if (sel.has_selection()) {
        e.sel_start = std::min(sel.start, cp_count);
        // 目标段延伸到串尾时，含尾下标不得超出最后一个码点。
        e.sel_end = std::min(sel.end, cp_count == 0 ? 0 : cp_count - 1);
    } else {
        e.sel_start = 0;
        e.sel_end = TextCompositionEvent::AURORA_NO_SELECTION;
    }
    return e;
}

auto utf8_byte_to_cp_index(std::string_view text, std::size_t byte_index) -> std::size_t {
    // 只统计**完整落在游标之前**的码点；游标切进某多字节码点中间时停在它起点（向下夹紧），
    // 与 utf16_index_to_cp_index 对代理对的处理同纪律。byte_index ≥ 串尾即总码点数。
    std::size_t cp = 0;
    std::size_t i = 0;
    while (i < byte_index && i < text.size()) {
        const auto [unused_cp, len] = a11y::detail::decode_cp(text, i);
        (void)unused_cp;
        const std::size_t n = (len == 0) ? 1 : len;  // len==0 仅越界出现；兜底前进 1 防死循环
        if (i + n > byte_index) {
            break;  // 游标落在本码点内部 → 夹紧到其起点（不 ++cp）
        }
        i += n;
        ++cp;
    }
    return cp;
}

auto make_preedit_state_utf8(std::string_view preedit, std::int64_t cursor_byte, std::int64_t sel_begin_byte,
                             std::int64_t sel_end_byte) -> TextCompositionEvent {
    TextCompositionEvent e;
    e.preedit.assign(preedit);
    const std::size_t total = utf8_byte_to_cp_index(preedit, preedit.size());  // 总码点数
    // cursor_byte<0 = 输入法未给游标 → 退化到串尾（新组合通常整体待选，串尾即候选插入点）。
    const std::size_t caret =
        (cursor_byte < 0) ? total : utf8_byte_to_cp_index(preedit, static_cast<std::size_t>(cursor_byte));
    e.cursor_index = std::min(caret, total);
    // 选区半开区间 [begin, end)（IBus/Fcitx index 口径）→ 契约的含尾码点区间 [start, end]。
    if (sel_begin_byte >= 0 && sel_end_byte > sel_begin_byte) {
        std::size_t start = utf8_byte_to_cp_index(preedit, static_cast<std::size_t>(sel_begin_byte));
        std::size_t end = utf8_byte_to_cp_index(preedit, static_cast<std::size_t>(sel_end_byte));
        // 半开区间的尾端点回退一个码点即含尾；退化（begin 与 end 落在同一码点）时 end=start。
        if (end > start) {
            --end;
        }
        e.sel_start = std::min(start, total);
        e.sel_end = std::min(end, total == 0 ? 0 : total - 1);
    } else {
        e.sel_start = 0;
        e.sel_end = TextCompositionEvent::AURORA_NO_SELECTION;
    }
    return e;
}

}  // namespace aurora::ime

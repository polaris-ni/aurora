// Aurora — 双向文本（bidi）辅助实现（UBA-lite，见 bidi.h）。
// 纯函数，不依赖 FreeType/HarfBuzz，可独立单测。
#include "aurora/render/bidi.h"

#include <cstdint>

namespace aurora::render::detail {

namespace {
// 最小 UTF-8 解码：写入 *cp，返回已消费的字节数（非法序列按 1 字节回退，保证不越界）。
[[nodiscard]] auto utf8_next(std::string_view s, std::size_t i) -> std::pair<unsigned, std::size_t> {
    if (i >= s.size()) {
        return {0U, 0U};
    }
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80U) {
        return {b0, 1U};
    }
    if ((b0 & 0xE0U) == 0xC0U && i + 1U < s.size()) {
        const unsigned lo = static_cast<unsigned char>(s[i + 1U]) & 0x3FU;
        return {((b0 & 0x1FU) << 6U) | lo, 2U};
    }
    if ((b0 & 0xF0U) == 0xE0U && i + 2U < s.size()) {
        const unsigned b1 = static_cast<unsigned char>(s[i + 1U]) & 0x3FU;
        const unsigned b2 = static_cast<unsigned char>(s[i + 2U]) & 0x3FU;
        return {((b0 & 0x0FU) << 12U) | (b1 << 6U) | b2, 3U};
    }
    if ((b0 & 0xF8U) == 0xF0U && i + 3U < s.size()) {
        const unsigned b1 = static_cast<unsigned char>(s[i + 1U]) & 0x3FU;
        const unsigned b2 = static_cast<unsigned char>(s[i + 2U]) & 0x3FU;
        const unsigned b3 = static_cast<unsigned char>(s[i + 3U]) & 0x3FU;
        return {((b0 & 0x07U) << 18U) | (b1 << 12U) | (b2 << 6U) | b3, 4U};
    }
    return {b0, 1U};  // 回退：当作单字节处理
}

// 单个码点的双向类型粗分类：强 LTR / 强 RTL / 中性（数字等）。
enum class BidiClass { L, R, Neutral };

[[nodiscard]] auto classify(unsigned cp) -> BidiClass {
    // 阿拉伯文及相关（含 Presentation Forms）。
    if ((cp >= 0x0600U && cp <= 0x06FFU) || (cp >= 0x0750U && cp <= 0x077FU) ||
        (cp >= 0x08A0U && cp <= 0x08FFU) || (cp >= 0xFB50U && cp <= 0xFDFFU) ||
        (cp >= 0xFE70U && cp <= 0xFEFFU) || (cp >= 0x1EC70U && cp <= 0x1ECBFU)) {
        return BidiClass::R;
    }
    // 希伯来文。
    if (cp >= 0x0590U && cp <= 0x05FFU) {
        return BidiClass::R;
    }
    // 西里尔、希腊、基本拉丁与拉丁-1 字母 → LTR。
    if ((cp >= 0x0041U && cp <= 0x005AU) || (cp >= 0x0061U && cp <= 0x007AU) ||
        (cp >= 0x00C0U && cp <= 0x024FU) || (cp >= 0x0370U && cp <= 0x03FFU) ||
        (cp >= 0x0400U && cp <= 0x04FFU)) {
        return BidiClass::L;
    }
    // 数字（含阿拉伯-印度数字）与标点视为中性，不决定段落基准方向。
    return BidiClass::Neutral;
}
}  // namespace

auto guess_paragraph_direction(std::string_view text) -> TextDirection {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto [cp, adv] = utf8_next(text, i);
        if (adv == 0U) {
            break;
        }
        const BidiClass cls = classify(cp);
        if (cls == BidiClass::R) {
            return TextDirection::RTL;
        }
        if (cls == BidiClass::L) {
            return TextDirection::LTR;
        }
        // Neutral：继续看下一个字符（UBA P2 仅首个强方向字符决定基准）。
        i += adv;
    }
    return TextDirection::LTR;  // 全中性/空串 → 回退 LTR
}

auto bidi_visual_run_order(std::size_t run_count, TextDirection base) -> std::vector<std::size_t> {
    std::vector<std::size_t> order;
    order.reserve(run_count);
    if (base == TextDirection::RTL) {
        // 段落 RTL：run 整体右→左翻转，使逻辑首 run 落在右缘。
        for (std::size_t k = run_count; k-- > 0;) {
            order.push_back(k);
        }
    } else {
        for (std::size_t k = 0; k < run_count; ++k) {
            order.push_back(k);
        }
    }
    return order;
}

}  // namespace aurora::render::detail

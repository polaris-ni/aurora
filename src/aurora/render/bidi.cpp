// Aurora — 双向文本（bidi）辅助实现（见 bidi.h）。
// 纯函数，不依赖 FreeType/HarfBuzz，可独立单测。
#include "aurora/render/bidi.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>

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

// UAX #9 双向字符类型（覆盖 Aurora 文本流会遇到的码点区间；未列出的码点一律 ON）。
enum class Bc : std::uint8_t {
    L,    // 强 LTR（拉丁/希腊/西里尔等字母）
    R,    // 强 RTL（希伯来等）
    AL,   // 阿拉伯字母（右到左阿拉伯）
    EN,   // 欧洲数字（0-9）
    AN,   // 阿拉伯-印度数字
    ES,   // 欧洲数字分隔符（+ -）
    ET,   // 欧洲数字终止符（# $ % 等）
    CS,   // 数字分隔符（, . : 等）
    WS,   // 空白
    ON,   // 其余中性
    BN,   // 边界中性（格式字符，参与解析但不参与显示）
    LRE,  // 显式嵌入控制（202A-202E）
    RLE,
    PDF,
    LRO,
    RLO,
    LRI,  // 隔离控制（2066-2069）
    RLI,
    FSI,
    PDI,
};

// 单个码点的双向类型（UAX #9 第 4-5 节主要区间的务实覆盖）。
[[nodiscard]] auto bidi_class_of(char32_t cp) -> Bc {
    // 显式嵌入与隔离控制。
    if (cp == 0x202AU) return Bc::LRE;
    if (cp == 0x202BU) return Bc::RLE;
    if (cp == 0x202CU) return Bc::PDF;
    if (cp == 0x202DU) return Bc::LRO;
    if (cp == 0x202EU) return Bc::RLO;
    if (cp == 0x2066U) return Bc::LRI;
    if (cp == 0x2067U) return Bc::RLI;
    if (cp == 0x2068U) return Bc::FSI;
    if (cp == 0x2069U) return Bc::PDI;
    // 边界中性（格式字符）。
    if (cp == 0x00ADU || cp == 0x200BU || cp == 0x200CU || cp == 0x200DU || cp == 0x2060U || cp == 0xFEFFU) {
        return Bc::BN;
    }
    // 强方向标记 LRM/RLM。
    if (cp == 0x200EU) return Bc::L;
    if (cp == 0x200FU) return Bc::R;
    // 强 RTL：希伯来。
    if (cp >= 0x0590U && cp <= 0x05FFU) return Bc::R;
    // 阿拉伯区：先摘出数字/分隔特例，其余一律 AL。
    if (cp >= 0x0600U && cp <= 0x06FFU) {
        if (cp >= 0x0660U && cp <= 0x0669U) return Bc::AN;
        if (cp >= 0x06F0U && cp <= 0x06F9U) return Bc::EN;
        if (cp == 0x060CU || cp == 0x066BU || cp == 0x066CU) return Bc::CS;
        return Bc::AL;
    }
    if ((cp >= 0x0750U && cp <= 0x077FU) || (cp >= 0x08A0U && cp <= 0x08FFU) ||
        (cp >= 0xFB50U && cp <= 0xFDFFU) || (cp >= 0xFE70U && cp <= 0xFEFFU) ||
        (cp >= 0x1EC70U && cp <= 0x1ECBFU)) {
        return Bc::AL;
    }
    // 强 LTR：拉丁/希腊/西里尔/亚美尼亚字母区。
    if ((cp >= 0x0041U && cp <= 0x005AU) || (cp >= 0x0061U && cp <= 0x007AU) || cp == 0x00AAU || cp == 0x00B5U ||
        cp == 0x00BAU || (cp >= 0x00C0U && cp <= 0x024FU) || (cp >= 0x0370U && cp <= 0x03FFU) ||
        (cp >= 0x0400U && cp <= 0x04FFU) || (cp >= 0x0531U && cp <= 0x058AU) || (cp >= 0x1E00U && cp <= 0x1EFFU)) {
        return Bc::L;
    }
    // 欧洲数字与阿拉伯-印度数字（阿区已摘）。
    if (cp >= 0x0030U && cp <= 0x0039U) return Bc::EN;
    // 数字分隔/终止符。
    if (cp == 0x002BU || cp == 0x002DU || cp == 0x2212U) return Bc::ES;
    if (cp == 0x0023U || cp == 0x0024U || cp == 0x0025U || (cp >= 0x00A2U && cp <= 0x00A5U) ||
        (cp >= 0x20A0U && cp <= 0x20CFU)) {
        return Bc::ET;
    }
    if (cp == 0x002CU || cp == 0x002EU || cp == 0x002FU || cp == 0x003AU) return Bc::CS;
    // 空白。
    if (cp == 0x0020U || cp == 0x00A0U || (cp >= 0x2000U && cp <= 0x200AU) || cp == 0x2028U || cp == 0x2029U ||
        cp == 0x202FU || cp == 0x205FU || cp == 0x3000U) {
        return Bc::WS;
    }
    return Bc::ON;
}

[[nodiscard]] auto is_strong(Bc bc) -> bool {
    return bc == Bc::L || bc == Bc::R || bc == Bc::AL;
}

[[nodiscard]] auto is_isolate(Bc bc) -> bool {
    return bc == Bc::LRI || bc == Bc::RLI || bc == Bc::FSI;
}

// FSI 的方向判定（BD14/P2/P3 于隔离内容内：首个强方向字符，找不到则按段落基准 RTL 视之）。
// 在 text [begin, end) 内找与 start 配对的 PDI（嵌套计数），扫描其中首强字符。
[[nodiscard]] auto fsi_is_rtl(const std::vector<char32_t> &text, std::size_t begin, std::size_t start) -> bool {
    std::size_t depth = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        const Bc bc = bidi_class_of(text[i]);
        if (is_isolate(bc)) {
            ++depth;
        } else if (bc == Bc::PDI) {
            if (depth == 0) {
                break;  // 配对结束仍未找到强方向字符
            }
            --depth;
            continue;
        }
        if (depth == 0 && is_strong(bc)) {
            return bc != Bc::L;  // R/AL → RTL
        }
    }
    (void)begin;
    return true;  // FSI 缺省：RTL
}

// X 阶段（X1-X9）：显式嵌入/隔离解析。产出每码点的 embedding 层级、embedding 方向、
// override 强制方向与控制符占位标记（X9：控制符视为 BN）。
struct XResult {
    std::vector<std::uint8_t> level;       // embedding 层级
    std::vector<bool> rtl_embedding;       // 所在 embedding 方向（true=RTL）
    std::vector<std::uint8_t> override_dir;  // 0=无 override，1=强制 L（LRO），2=强制 R（RLO）
    std::vector<bool> control_placeholder;   // 控制符（X9 后为 BN，不参与 W/N/I 的类型传播）
};

[[nodiscard]] auto resolve_x(const std::vector<char32_t> &text, std::uint8_t base_level) -> XResult {
    const std::size_t n = text.size();
    XResult out;
    out.level.assign(n, base_level);
    out.rtl_embedding.assign(n, (base_level % 2U) != 0U);
    out.override_dir.assign(n, 0U);
    out.control_placeholder.assign(n, false);

    // embedding 栈：{level, 是否 RTL 方向, 是否 override（L/R）, 是否 isolate}。
    struct Embedding {
        std::uint8_t level;
        bool rtl;
        bool has_override;
        bool override_rtl;
        bool isolate;
    };
    std::vector<Embedding> stack;
    stack.push_back(
        Embedding{.level = base_level, .rtl = (base_level % 2U) != 0U, .has_override = false,
                  .override_rtl = false, .isolate = false});
    constexpr std::uint8_t kMaxLevel = 125U;

    for (std::size_t i = 0; i < n; ++i) {
        const Bc bc = bidi_class_of(text[i]);
        switch (bc) {
        case Bc::RLE:  // X2
        case Bc::LRE:  // X3
        case Bc::RLO:  // X4
        case Bc::LRO: {  // X5
            const Embedding top = stack.back();  // 拷贝：push_back 可能 reallocate 使引用悬空
            std::uint8_t nl = top.level;
            // 新层级 = 严格大于当前层级的最小奇数（RLE/RLO）或偶数（LRE/LRO）。
            if (bc == Bc::RLE || bc == Bc::RLO) {
                nl = static_cast<std::uint8_t>((nl % 2U == 0U) ? nl + 1U : nl + 2U);
            } else {
                nl = static_cast<std::uint8_t>((nl % 2U == 0U) ? nl + 2U : nl + 1U);
            }
            if (nl > kMaxLevel) {
                nl = kMaxLevel;
            }
            stack.push_back(Embedding{.level = nl, .rtl = (nl % 2U) != 0U,
                                      .has_override = (bc == Bc::LRO || bc == Bc::RLO),
                                      .override_rtl = (bc == Bc::RLO), .isolate = false});
            out.level[i] = top.level;  // X9：控制符占位（所在 embedding 层级）
            out.rtl_embedding[i] = top.rtl;
            out.control_placeholder[i] = true;
            break;
        }
        case Bc::PDF: {  // X7
            const Embedding top = stack.back();
            if (stack.size() > 1U && !top.isolate) {
                stack.pop_back();
            }
            out.level[i] = top.level;
            out.rtl_embedding[i] = top.rtl;
            out.control_placeholder[i] = true;
            break;
        }
        case Bc::LRI:  // X6a
        case Bc::RLI:
        case Bc::FSI: {
            const Embedding top = stack.back();
            bool iso_rtl = (bc == Bc::RLI);
            if (bc == Bc::FSI) {
                iso_rtl = fsi_is_rtl(text, i, i + 1U);
            }
            // 隔离不增层级（层级沿用当前 embedding），仅切换方向与重置 override。
            stack.push_back(Embedding{.level = top.level, .rtl = iso_rtl, .has_override = false,
                                      .override_rtl = false, .isolate = true});
            out.level[i] = top.level;
            out.rtl_embedding[i] = top.rtl;
            out.control_placeholder[i] = true;
            break;
        }
        case Bc::PDI: {
            const Embedding top = stack.back();
            if (stack.size() > 1U && top.isolate) {
                stack.pop_back();
            }
            out.level[i] = top.level;
            out.rtl_embedding[i] = top.rtl;
            out.control_placeholder[i] = true;
            break;
        }
        default: {  // X5a/X6：普通字符取栈顶 embedding；X4/X5 override 覆盖强类型
            const Embedding &top = stack.back();  // 只读，无 push，引用安全
            out.level[i] = top.level;
            out.rtl_embedding[i] = top.rtl;
            out.override_dir[i] = top.has_override ? (top.override_rtl ? 2U : 1U) : 0U;
            out.control_placeholder[i] = false;
            break;
        }
        }
    }
    return out;
}

// W + N + I 阶段：在 X 结果上做弱类型/中性解析与隐式层级。
[[nodiscard]] auto resolve_wni(const std::vector<char32_t> &text, const XResult &x) -> std::vector<std::uint8_t> {
    const std::size_t n = text.size();
    if (n == 0) {
        return {};
    }
    std::vector<Bc> t(n);
    for (std::size_t i = 0; i < n; ++i) {
        t[i] = bidi_class_of(text[i]);
        // X4/X5 override（X9 后处理）：LRO/RLO 把覆盖范围内的字符强类型强制为 L/R。
        if (x.override_dir[i] == 1U) {
            t[i] = Bc::L;
        } else if (x.override_dir[i] == 2U) {
            t[i] = Bc::R;
        }
    }

    // ---- W 阶段 ----
    // W2/W7 的「最近强类型」（W2: AL 使 EN→AN；W7: L 使 EN→L）。
    Bc last_strong = Bc::L;  // sor 语义：段落层级偶 → L、奇 → R（由调用方 base_level 决定不了，
                             // 这里以首字符 embedding 方向近似，对无显式控制的常规文本等价）。
    last_strong = x.rtl_embedding[0] ? Bc::R : Bc::L;
    // W2：EN 若最近强类型为 AL → AN（先做，W3 才把 AL 变 R）。
    for (std::size_t i = 0; i < n; ++i) {
        if (x.control_placeholder[i]) {
            continue;
        }
        if (is_strong(t[i])) {
            last_strong = t[i];
        } else if (t[i] == Bc::EN && last_strong == Bc::AL) {
            t[i] = Bc::AN;
        }
    }
    // W3：AL → R。
    for (std::size_t i = 0; i < n; ++i) {
        if (!x.control_placeholder[i] && t[i] == Bc::AL) {
            t[i] = Bc::R;
        }
    }
    // W4：EN (ES|CS) EN → EN；AN CS AN → AN（单遍三窗口）。
    for (std::size_t i = 1U; i + 1U < n; ++i) {
        if (x.control_placeholder[i]) {
            continue;
        }
        if (t[i] == Bc::ES || t[i] == Bc::CS) {
            const Bc prev = t[i - 1U];
            const Bc next = t[i + 1U];
            if (t[i] == Bc::CS && prev == Bc::AN && next == Bc::AN) {
                t[i] = Bc::AN;
            } else if (prev == Bc::EN && next == Bc::EN) {
                t[i] = Bc::EN;
            }
        }
    }
    // W5：ET 序列与 EN 相邻 → EN（含连续 ET 串；两遍：先左邻后右邻）。
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i] == Bc::ET && i > 0U && t[i - 1U] == Bc::EN) {
            t[i] = Bc::EN;
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        if (t[i] == Bc::ET && i + 1U < n && t[i + 1U] == Bc::EN) {
            t[i] = Bc::EN;
        }
    }
    // W6：剩余 ES/ET/CS → ON。
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i] == Bc::ES || t[i] == Bc::ET || t[i] == Bc::CS) {
            t[i] = Bc::ON;
        }
    }
    // W7：EN 若最近强类型为 L → L。
    last_strong = x.rtl_embedding[0] ? Bc::R : Bc::L;
    for (std::size_t i = 0; i < n; ++i) {
        if (x.control_placeholder[i]) {
            continue;
        }
        if (is_strong(t[i])) {
            last_strong = t[i];
        } else if (t[i] == Bc::EN && last_strong == Bc::L) {
            t[i] = Bc::L;
        }
    }

    // ---- N1/N2 阶段（N0 括号未实现，括号按普通中性参与 N1/N2）----
    // 中性 = ON/WS（BN 已被 X9 占位排除）。EN/AN 在 N1 中视作 R。
    auto neutral_of = [&](Bc bc) -> bool { return bc == Bc::ON || bc == Bc::WS; };
    auto strong_side_of = [&](Bc bc) -> int {
        // N1 语境下的方向语义：L → 0；R/AL/EN/AN → 1。
        if (bc == Bc::L) return 0;
        if (bc == Bc::R || bc == Bc::EN || bc == Bc::AN) return 1;
        return -1;
    };
    std::size_t i = 0;
    while (i < n) {
        if (x.control_placeholder[i] || !neutral_of(t[i])) {
            ++i;
            continue;
        }
        const std::size_t j = i;
        while (i < n && !x.control_placeholder[i] && neutral_of(t[i])) {
            ++i;
        }
        // 中性段 [j, i)。找左右边界的有效方向。
        int left = -1;
        if (j > 0U) {
            left = strong_side_of(t[j - 1U]);
        }
        int right = -1;
        if (i < n) {
            right = strong_side_of(t[i]);
        }
        // 越界侧用 sor/eor = 段落方向（以首字符 embedding 方向近似）。
        const int para = x.rtl_embedding[0] ? 1 : 0;
        if (left < 0) {
            left = para;
        }
        if (right < 0) {
            right = para;
        }
        const int dir = (left == right) ? left : para;  // N1 一致 → 该方向；N2 → embedding 方向
        for (std::size_t k = j; k < i; ++k) {
            t[k] = (dir == 1) ? Bc::R : Bc::L;
        }
    }

    // ---- I1/I2 阶段：隐式层级 ----
    // 嵌入方向 RTL（奇向）：L/EN/AN → 层+1（变偶）；嵌入方向 LTR（偶向）：R → 层+1（变奇）。
    // BN（控制符占位）保持所在 embedding 层级。
    std::vector<std::uint8_t> levels(n);
    for (std::size_t k = 0; k < n; ++k) {
        const std::uint8_t lvl = x.level[k];
        if (x.control_placeholder[k]) {
            levels[k] = lvl;
            continue;
        }
        const bool rtl_emb = x.rtl_embedding[k];
        if (rtl_emb) {
            levels[k] = (t[k] == Bc::R) ? lvl : static_cast<std::uint8_t>(lvl + 1U);
        } else {
            levels[k] = (t[k] == Bc::R) ? static_cast<std::uint8_t>(lvl + 1U) : lvl;
        }
    }
    return levels;
}
}  // namespace

auto guess_paragraph_direction(std::string_view text) -> TextDirection {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto [cp, adv] = utf8_next(text, i);
        if (adv == 0U) {
            break;
        }
        const Bc cls = bidi_class_of(cp);
        if (cls == Bc::R || cls == Bc::AL) {
            return TextDirection::RTL;
        }
        if (cls == Bc::L) {
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

auto uba_levels(const std::vector<char32_t> &text, std::uint8_t base_level) -> std::vector<std::uint8_t> {
    if (text.empty()) {
        return {};
    }
    const XResult x = resolve_x(text, base_level);
    return resolve_wni(text, x);
}

auto uba_visual_order(const std::vector<std::uint8_t> &levels) -> std::vector<std::size_t> {
    const std::size_t n = levels.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (n == 0U) {
        return order;
    }
    const auto max_it = std::max_element(levels.begin(), levels.end());
    int top = static_cast<int>(*max_it);
    // L2：从最高层降到最低奇数层（≥1），逐层反转所有「层级 ≥ L」的连续区段。
    // 反转基于当前序列位置处的元素层级（order 变化后查 levels[order[i]]）。
    for (int lv = top; lv >= 1; --lv) {
        std::size_t i = 0;
        while (i < n) {
            if (levels[order[i]] >= lv) {
                std::size_t j = i;
                while (j < n && levels[order[j]] >= lv) {
                    ++j;
                }
                std::reverse(order.begin() + static_cast<long>(i), order.begin() + static_cast<long>(j));
                i = j;
            } else {
                ++i;
            }
        }
    }
    return order;
}

}  // namespace aurora::render::detail

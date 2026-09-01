#include "aurora/render/font_engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ft2build.h>
#include <list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include FT_FREETYPE_H
// 第三方 harfbuzz 头在 -Wall 下触发 -Wstringop-overread 误报（hb-algs.hh:1146 的 memcmp 边界
// 分析误判），对三方头无意义；已在 CMakeLists 对 harfbuzz target 整体关闭（-Wno-stringop-overread）。
#include <hb-ft.h>
#include <hb.h>

#include "aurora/core/log.h"
#include "aurora/core/utf8.h" // aurora::utf8_cp_len（收口 dup-1 重复实现）
#include "aurora/perf/counters.h"
#include "aurora/render/bitmap_font.h"
#include "aurora/render/font_discovery.h"
#include "aurora/render/glyph_atlas.h"

namespace aurora::render {

namespace {

// 字体引擎需要跨调用保持状态；收敛为函数局部静态变量，避免命名空间作用域全局可变对象告警。
[[nodiscard]] auto atlas() -> GlyphAtlas & {
    static GlyphAtlas a;
    return a;
}
[[nodiscard]] auto aa_mode() -> TextAAMode & {
    static TextAAMode m = TextAAMode::Supersample;
    return m;
}
class ShapeCache; // 前向声明：定义在下方，函数体随后置。
[[nodiscard]] auto shape_cache() -> ShapeCache &;

constexpr float AURORA_LOGICAL_DPI = 96.0f;
constexpr float AURORA_POINTS_PER_INCH = 72.0f;

// ---------- UTF-8 工具（utf8_cp_len 已收口到 aurora::utf8_cp_len，见 core/utf8.h） ----------
auto utf8_decode(const std::string &s, std::size_t &i, unsigned &cp) -> int {
    const auto c0 = static_cast<unsigned char>(s.at(i));
    int len = utf8_cp_len(c0);
    // 序列长度只由首字节决定，故截断的多字节序列（"…\xF0" 之类）会让下方后续字节访问
    // 越过字符串末尾读堆内存。文本可来自 JSON / 剪贴板 / 按字节截断的字段，属不可信输入：
    // 长度不足时退化为单字节处理（等价 U+FFFD 替换，不改变合法文本的行为）。
    if (i + static_cast<std::size_t>(len) > s.size()) {
        len = 1;
    }
    const auto byte = [&](std::size_t k) -> unsigned char { return static_cast<unsigned char>(s.at(i + k)); };
    unsigned u = 0;
    switch (len) {
    case 1: u = c0; break;
    case 2: u = ((c0 & 0x1FU) << 6U) | (byte(1) & 0x3FU); break;
    case 3: u = ((c0 & 0x0FU) << 12U) | ((byte(1) & 0x3FU) << 6U) | (byte(2) & 0x3FU); break;
    default:
        u = ((c0 & 0x07U) << 18U) | ((byte(1) & 0x3FU) << 12U) | ((byte(2) & 0x3FU) << 6U) | (byte(3) & 0x3FU);
        break;
    }
    cp = u;
    i += static_cast<std::size_t>(len);
    return len;
}

[[nodiscard]] auto cp_count(const std::string &s) -> std::size_t {
    std::size_t c = 0;
    std::size_t i = 0;
    const std::string::size_type n = s.size();
    while (i < n) {
        unsigned cp = 0;
        utf8_decode(s, i, cp);
        ++c;
    }
    return c;
}

[[nodiscard]] auto cp_substr(const std::string &s, std::size_t count) -> std::string {
    std::string out;
    std::size_t i = 0;
    std::size_t got = 0;
    const std::string::size_type n = s.size();
    while (i < n && got < count) {
        const std::size_t start = i;
        unsigned cp = 0;
        utf8_decode(s, i, cp);
        out.append(s, start, i - start);
        ++got;
    }
    return out;
}

// ---------- FreeType 辅助 ----------
[[nodiscard]] auto px_measure(const Font &f) -> int {
    return std::max(1, static_cast<int>(std::lround(f.size_pt * AURORA_LOGICAL_DPI / AURORA_POINTS_PER_INCH)));
}

auto find_glyph(const std::vector<FontFace *> &faces, unsigned cp, FontFace *&ff, FT_UInt &gi) -> bool {
    for (auto *fc : faces) {
        const FT_UInt g = FT_Get_Char_Index(fc->face, cp);
        if (g != 0) {
            ff = fc;
            gi = g;
            return true;
        }
    }
    ff = faces.front();
    gi = FT_Get_Char_Index(ff->face, cp);
    return false;
}

[[nodiscard]] auto line_height_px(const std::vector<FontFace *> &faces, int px) -> float {
    FT_Face face = faces.front()->face;
    FT_Set_Pixel_Sizes(face, 0, px);
    return static_cast<float>(face->size->metrics.height) / 64.0f;
}

// 主 face 的基线上升（ascender，物理 px）：draw_text 的 r.origin.y 是行盒顶（GDI TA_TOP
// 历史语义，全库调用方均按此传值），首行基线 = 顶 + ascender；回退 face 的字形统一按
// 主 face 基线对齐，保证混排（拉丁+CJK）同行基线一致。
[[nodiscard]] auto ascender_px(const std::vector<FontFace *> &faces, int px) -> float {
    FT_Face face = faces.front()->face;
    FT_Set_Pixel_Sizes(face, 0, px);
    return static_cast<float>(face->size->metrics.ascender) / 64.0f;
}

// key 布局（42 bit 有效）：
// [0:15]  glyph_index  (16 bit, max 65535)
// [16:31] px           (16 bit, max 65535)
// [32:39] face_id      (8 bit, max 255)
// [40]    mode         (1 bit, 0=normal/Gray, 1=Lcd)
// [41]    italic       (1 bit)
[[nodiscard]] constexpr auto make_key(std::uint32_t face_id, std::uint32_t glyph_index, std::uint32_t px, bool mode,
                                      bool italic) -> std::uint64_t {
    return static_cast<std::uint64_t>(face_id) << 32U | static_cast<std::uint64_t>(glyph_index) |
           static_cast<std::uint64_t>(px) << 16U | static_cast<std::uint64_t>(mode ? 1 : 0) << 40U |
           static_cast<std::uint64_t>(italic ? 1 : 0) << 41U;
}

auto apply_italic(FT_Face face, bool italic) -> void {
    if (italic) {
        // 仿斜（oblique）：FT_Matrix 变换为 x' = xx·x + xy·y，y' = yx·x + yy·y（字形空间 y 向上）。
        // 正常斜体是「越高的点越向右」：x' = x + slant·y → 斜量设在 xy 分量；
        // 若误设在 yx（y' = y + slant·x）则变成竖向歪斜（字形逆时针翻转、基线在字内爬坡）。
        // xy 剪切不改变 advance.x（advance 向量 (adv,0) 的 x' = adv），度量/命中不受影响。
        FT_Matrix shear{};
        shear.xx = 0x10000;
        shear.xy = static_cast<FT_Fixed>(0.22 * 0x10000);
        shear.yx = 0;
        shear.yy = 0x10000;
        FT_Set_Transform(face, &shear, nullptr);
    } else {
        FT_Set_Transform(face, nullptr, nullptr);
    }
}

// ---------- HarfBuzz shaping ----------
// 单字形 shaping 结果（px 尺寸，已换算为 float）：hb_shape 输出字形序号、placement 偏移与推进。
struct ShapedGlyph {
    FontFace *face; // 来源面（回退）
    FT_UInt gi;     // 字形序号（= hb codepoint）
    unsigned cp;    // 原始码点（用于空格字距判定）
    float x_off;    // 字形水平放置偏移（px）
    float y_off;    // 字形垂直放置偏移（px）
    float x_adv;    // 水平推进（px，hb 已含 kerning / OT 特性与 hinting 取整）
};

struct ShapedLine {
    std::vector<ShapedGlyph> glyphs;
};

// ---------- 文本 shaping 缓存 ----------
// `shape_line(line, faces, px, opts)` 是纯函数：输出只依赖这四个输入。缓存命中即跳过
// hb_shape（每帧纯 CPU 空烧约 10ms 的主因），返回与重算逐位一致的字形序列——不改变任何
// 输出像素（golden 零影响），故无条件开启（非 AURORA_ENABLE_PROFILING 门控）。
struct ShapeCacheKey {
    std::string line;
    int px = 0;
    TextLayoutOpts opts;
    std::uint64_t faces_key = 0;
    auto operator==(const ShapeCacheKey &o) const noexcept -> bool {
        return px == o.px && opts == o.opts && faces_key == o.faces_key && line == o.line;
    }
};

struct ShapeCacheKeyHash {
    auto operator()(const ShapeCacheKey &k) const noexcept -> std::size_t {
        auto h = std::hash<std::string>{}(k.line);
        const auto mix = [&](std::uint64_t v) -> void {
            h ^= static_cast<std::size_t>(v) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
        };
        mix(static_cast<std::uint64_t>(k.px));
        std::uint32_t u = 0;
        std::memcpy(&u, &k.opts.letter_spacing, 4);
        mix(u);
        std::memcpy(&u, &k.opts.word_spacing, 4);
        mix(u);
        mix(k.opts.italic ? 0x1001ULL : 0ULL);
        mix(k.faces_key);
        return h;
    }
};

// LRU + 双限（条目数 / 估算字节数）淘汰。单线程 UI 模型，无锁。
class ShapeCache {
  public:
    ShapeCache(std::size_t max_entries, std::size_t max_bytes) noexcept
        : m_max_entries(max_entries), m_max_bytes(max_bytes) {}

    [[nodiscard]] auto find(const ShapeCacheKey &k) -> const std::vector<ShapedGlyph> * {
        const auto it = m_map.find(k);
        if (it == m_map.end()) {
            ++m_stats.misses;
            return nullptr;
        }
        m_lru.splice(m_lru.begin(), m_lru, it->second.second); // 提到 MRU
        ++m_stats.hits;
        return &it->second.first;
    }

    auto insert(ShapeCacheKey k, std::vector<ShapedGlyph> glyphs) -> void {
        const std::size_t bytes = glyph_bytes(glyphs);
        evict_to_fit(1, bytes);
        const auto node = m_lru.insert(m_lru.begin(), std::move(k));
        m_map.emplace(*node, std::make_pair(std::move(glyphs), node));
        sync_stats();
    }

    auto clear() -> void {
        m_map.clear();
        m_lru.clear();
        m_stats = ShapeCacheStats{};
    }

    [[nodiscard]] auto stats() const -> ShapeCacheStats { return m_stats; }

  private:
    static auto glyph_bytes(const std::vector<ShapedGlyph> &g) -> std::size_t {
        return (g.size() * sizeof(ShapedGlyph)) + 32u; // 估算（含对齐）
    }
    auto evict_to_fit(std::size_t extra, std::size_t extra_bytes) -> void {
        while ((m_map.size() + extra > m_max_entries || m_stats.bytes + extra_bytes > m_max_bytes) && !m_lru.empty()) {
            const auto &old = m_lru.back();
            auto it = m_map.find(old);
            if (it != m_map.end()) {
                m_stats.bytes -= glyph_bytes(it->second.first);
                m_map.erase(it);
            }
            m_lru.pop_back();
        }
        sync_stats();
    }
    auto sync_stats() -> void { m_stats.entries = m_map.size(); }
    std::list<ShapeCacheKey> m_lru; // MRU 在前
    std::unordered_map<ShapeCacheKey, std::pair<std::vector<ShapedGlyph>, std::list<ShapeCacheKey>::iterator>,
                       ShapeCacheKeyHash>
        m_map;
    std::size_t m_max_entries;
    std::size_t m_max_bytes;
    ShapeCacheStats m_stats;
};

// 全局实例。容量：4096 条目 / 8 MiB 估算字节，双限 LRU。
[[nodiscard]] auto shape_cache() -> ShapeCache & {
    static ShapeCache c(4096u, static_cast<std::size_t>(8u * 1024u * 1024u));
    return c;
}

[[nodiscard]] auto faces_key_of(const std::vector<FontFace *> &faces) -> std::uint64_t {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto *fc : faces) {
        h ^= static_cast<std::uint64_t>(fc->id);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// 前向声明（定义见下方）：_uncached 是真正跑 hb_shape 的纯函数体。
[[nodiscard]] auto shape_line_uncached(const std::string &line, const std::vector<FontFace *> &faces, int px,
                                       const TextLayoutOpts &opts) -> ShapedLine;

// 带缓存的 shape_line：命中直接返回（拷贝字形序列，跳过 hb_shape）；未命中走 _uncached 并回写。
[[nodiscard]] auto shape_line(const std::string &line, const std::vector<FontFace *> &faces, int px,
                              const TextLayoutOpts &opts) -> ShapedLine {
    ShapedLine out;
    if (line.empty()) {
        return out;
    }
    const ShapeCacheKey key{ .line = line, .px = px, .opts = opts, .faces_key = faces_key_of(faces) };
    if (const auto *cached = shape_cache().find(key)) {
        out.glyphs = *cached; // 命中：跳过 hb_shape。纯函数输出，与重算逐位一致（golden 零影响）。
        AURORA_PROFILE_COUNT(shape_cache_hits, 1);
        return out;
    }
    ShapedLine fresh = shape_line_uncached(line, faces, px, opts);
    shape_cache().insert(key, fresh.glyphs);
    AURORA_PROFILE_COUNT(shape_cache_misses, 1);
    return fresh;
}

// 把一行文本按「面回退（find_glyph）」切成连续同面 run；每个 run 经 hb 做 OpenType shaping，
// 得到字形序号 / 偏移 / 推进，按绘制顺序合并为整行 ShapedGlyph 序列。
[[nodiscard]] auto shape_line_uncached(const std::string &line, const std::vector<FontFace *> &faces, int px,
                                       const TextLayoutOpts &opts) -> ShapedLine {
    ShapedLine out;
    if (line.empty()) {
        return out;
    }
    // 1) 解码整行码点并标注每个码点所属面。
    struct CpInfo {
        unsigned cp;
        FontFace *face;
        std::size_t byte_start;
        std::size_t byte_end;
    };
    std::vector<CpInfo> cps;
    {
        std::size_t i = 0;
        const std::string::size_type n = line.size();
        while (i < n) {
            const std::size_t bs = i;
            unsigned cp = 0;
            utf8_decode(line, i, cp);
            FontFace *ff = nullptr;
            FT_UInt gi = 0;
            find_glyph(faces, cp, ff, gi);
            cps.push_back({ .cp = cp, .face = ff, .byte_start = bs, .byte_end = i });
        }
    }
    // 2) 把连续同面码点切为 run，逐 run 调 hb_shape。
    for (std::size_t k = 0; k < cps.size();) {
        const auto &cp = cps.at(k);
        FontFace *rf = cp.face;
        const std::size_t run_byte_start = cp.byte_start;
        std::size_t run_byte_end = cp.byte_end;
        std::size_t kk = k + 1;
        while (kk < cps.size() && cps.at(kk).face == rf) {
            run_byte_end = cps.at(kk).byte_end;
            ++kk;
        }
        k = kk;
        const std::string run_str = line.substr(run_byte_start, run_byte_end - run_byte_start);
        if (run_str.empty()) {
            continue;
        }
        const FT_Face face = rf->face;
        FT_Set_Pixel_Sizes(face, 0, px);
        apply_italic(face, opts.italic); // 决定字形变换，使 shaping 与绘制变换一致（斜体剪切不改变 x 推进）
        hb_font_t *hb_font = hb_ft_font_create(face, nullptr);
        // hb-ft 默认 FT_LOAD_NO_HINTING（advance 为设计空间线性值），而绘制侧 FT_Load_Glyph
        // 用 FT_LOAD_DEFAULT（hinted）。二者不一致会让度量与实绘像素错位（缩放屏下行尾差数 px）。
        // 切到 FT_LOAD_DEFAULT，使 hb 的 x_advance 与绘制侧 slot->advance 逐位一致。
        hb_ft_font_set_load_flags(hb_font, FT_LOAD_DEFAULT);
        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, run_str.data(), static_cast<int>(run_str.size()), 0, static_cast<int>(run_str.size()));
        hb_buffer_guess_segment_properties(buf);
        hb_shape(hb_font, buf, nullptr, 0);
        unsigned int ng = 0;
        const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buf, &ng);
        const hb_glyph_position_t *poss = hb_buffer_get_glyph_positions(buf, &ng);
        // HarfBuzz 返回 C 风格数组，此处是三方 C API 的必经指针遍历；用 NOLINT 块收口。
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        for (unsigned int j = 0; j < ng; ++j) {
            ShapedGlyph sg{};
            sg.face = rf;
            sg.gi = infos[j].codepoint;
            // cluster 为 run_str 中的字节偏移，解码出该字形起始码点（用于空格判定）。
            std::size_t off = infos[j].cluster;
            if (off >= run_str.size()) {
                off = run_str.empty() ? 0 : run_str.size() - 1;
            }
            unsigned tmp = 0;
            utf8_decode(run_str, off, tmp);
            sg.cp = tmp;
            sg.x_off = static_cast<float>(poss[j].x_offset) / 64.0f;
            sg.y_off = static_cast<float>(poss[j].y_offset) / 64.0f;
            sg.x_adv = static_cast<float>(poss[j].x_advance) / 64.0f;
            out.glyphs.push_back(sg);
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        hb_buffer_destroy(buf);
        hb_font_destroy(hb_font);
    }
    return out;
}

// 按 '\n' 切分为行（'\n' 为单字节 0x0A，绝不出现在多字节序列内）。
[[nodiscard]] auto split_lines(const std::string &text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::string cur;
    for (const char ch : text) {
        if (ch == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    lines.push_back(cur);
    return lines;
}

// 整行前缀推进：前 count 个字形（绘制顺序）的 pen 推进，与 draw_text_impl 逐位同源
// （同 px、同 hb 推进、同 letter/word_spacing 语义）。spacing_scale 把 dp 间距换算到 px 空间。
[[nodiscard]] auto line_prefix(const std::vector<ShapedGlyph> &glyphs, const TextLayoutOpts &opts, float spacing_scale,
                               std::size_t count) -> float {
    float w = 0.0f;
    const std::size_t m = std::min(count, glyphs.size());
    for (std::size_t j = 0; j < m; ++j) {
        if (j > 0) {
            w += opts.letter_spacing * spacing_scale; // 相邻字形间加字距，整串共 (n-1) 次
        }
        const auto &g = glyphs.at(j);
        w += g.x_adv;
        if (g.cp == ' ' && opts.word_spacing != 0.0f) {
            w += opts.word_spacing * spacing_scale;
        }
    }
    return w;
}

// 前缀推进量（px 像素）：前 char_index 个码点所在行的 pen 推进，与 draw_text_impl 逐位同源
// （同 px、同 hb 推进、同 letter/word_spacing 语义）。spacing_scale 把逻辑 dp 间距换算到 px 空间。
// 实显度量必须按物理尺寸真算（lround(px*scale)），不得用自然度量线性近似，避免跨字符累计误差
// 造成命中 off-by-one。
[[nodiscard]] auto prefix_advance_px(const std::string &text, std::size_t char_index,
                                     const std::vector<FontFace *> &faces, int px, const TextLayoutOpts &opts,
                                     float spacing_scale) -> float {
    const auto lines = split_lines(text);
    std::size_t acc = 0;
    for (const auto &line : lines) {
        const std::size_t lcount = cp_count(line);
        if (char_index <= acc + lcount) {
            const std::size_t local = char_index - acc;
            const auto sl = shape_line(line, faces, px, opts);
            return line_prefix(sl.glyphs, opts, spacing_scale, local);
        }
        acc += lcount + 1; // +1 计 '\n'
    }
    const auto sl = shape_line(lines.back(), faces, px, opts);
    return line_prefix(sl.glyphs, opts, spacing_scale, sl.glyphs.size());
}

// 整串宽度（px 像素，多行取最宽行）：与 prefix_advance_px 同源的整串版。
[[nodiscard]] auto max_line_width_px(const std::string &text, const std::vector<FontFace *> &faces, int px,
                                     const TextLayoutOpts &opts, float spacing_scale) -> float {
    const auto lines = split_lines(text);
    float max_w = 0.0f;
    for (const auto &line : lines) {
        const auto sl = shape_line(line, faces, px, opts);
        max_w = std::max(max_w, line_prefix(sl.glyphs, opts, spacing_scale, sl.glyphs.size()));
    }
    return max_w;
}

// 实显像素尺寸：与 draw_text_impl 的 px 计算完全一致（lround(px_measure * scale)）。
[[nodiscard]] auto px_display(const Font &f, float scale) -> int {
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(px_measure(f)) * scale)));
}

// 单趟字符命中：在一次前缀推进扫描内完成命中，避免「逐边界重算前缀」的 O(n²)
// （63 码点单次命中 ≈1ms，252 码点 ≈15ms，全屏不折行长行拖选卡顿的主因）。
// 累加顺序与 line_prefix 逐位相同，每个边界除以 divisor 后与 x 比较（自然版
// divisor=1，实显版 divisor=scale，与逐次调用 caret_x / display_caret_x 的边界值逐位一致）。
// inclusive=false：caret 语义（相邻边界中点取舍，返回 0..total）；
// inclusive=true：含头含尾（x ≤ 右边界即命中该字符，行尾右侧命中末字符，返回 0..total-1）。
[[nodiscard]] auto hit_test_single_pass(const std::string &text, float x, const std::vector<FontFace *> &faces, int px,
                                        const TextLayoutOpts &opts, float spacing_scale, float divisor, bool inclusive)
    -> std::size_t {
    const std::size_t total = cp_count(text);
    if (total == 0 || x <= 0.0f) {
        return 0;
    }
    const auto lines = split_lines(text);
    float w = 0.0f;
    float prev_boundary = 0.0f;
    std::size_t line_char_offset = 0; // 当前行首在全局文本中的字符下标（含前导 '\n'）
    for (const auto &line_str : lines) {
        const auto sl = shape_line(line_str, faces, px, opts);
        w = 0.0f; // 每行 pen 推进归零（与 draw_text_impl 每行重置 pen_x 一致）
        for (std::size_t j = 0; j < sl.glyphs.size(); ++j) {
            if (j > 0) {
                w += opts.letter_spacing * spacing_scale;
            }
            const auto &g = sl.glyphs.at(j);
            w += g.x_adv;
            if (g.cp == ' ' && opts.word_spacing != 0.0f) {
                w += opts.word_spacing * spacing_scale;
            }
            const float boundary = w / divisor; // 与 caret_x/display_caret_x 返回值逐位一致
            if (inclusive) {
                if (x <= boundary) {
                    return line_char_offset + j;
                }
            } else {
                const float mid = (prev_boundary + boundary) * 0.5f;
                if (x < mid) {
                    return line_char_offset + j;
                }
                prev_boundary = boundary;
            }
        }
        line_char_offset += cp_count(line_str) + 1; // +1 计 '\n'
    }
    // 行尾右侧：caret 语义返回末 caret（total）；含头含尾返回末字符（消除行尾漏选）。
    return inclusive ? total - 1u : total;
}

// ---------- 位图字体兜底（FT 不可用时） ----------
[[nodiscard]] auto bitmap_pixel(const Glyph &g, int x, int y) -> bool {
    return std::string_view(g.rows.at(static_cast<std::size_t>(y)), 8).at(static_cast<std::size_t>(x)) == '#';
}

auto draw_text_bitmap_fallback(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                               const TextLayoutOpts &opts) -> void {
    const int ps = BitmapFont::pixel_size(f.size_pt);
    float pen_x = r.origin.x;
    float pen_y = r.origin.y;
    std::size_t i = 0;
    const std::string::size_type n = text.size();
    while (i < n) {
        unsigned cp = 0;
        const int len = utf8_decode(text, i, cp);
        (void)len;
        if (cp == '\n') {
            pen_x = r.origin.x;
            pen_y += static_cast<float>(BitmapFont::AURORA_CELL) * static_cast<float>(ps);
            continue;
        }
        if (cp < 128) {
            AURORA_PROFILE_COUNT(glyphs_rendered, 1);
            const Glyph &g = BitmapFont::glyph(static_cast<char>(cp));
            for (int y = 0; y < BitmapFont::AURORA_CELL; ++y) {
                for (int x = 0; x < BitmapFont::AURORA_CELL; ++x) {
                    if (bitmap_pixel(g, x, y)) {
                        p.blend_pixel(static_cast<int>(std::floor(pen_x)) + (x * ps),
                                      static_cast<int>(std::floor(pen_y)) + (y * ps), c);
                    }
                }
            }
        }
        pen_x += static_cast<float>(BitmapFont::AURORA_CELL) * static_cast<float>(ps);
        if (cp == ' ' && opts.word_spacing != 0.0f) {
            pen_x += opts.word_spacing;
        }
        pen_x += opts.letter_spacing;
    }
}

// ---------- 真·FreeType 绘制 ----------
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto draw_text_impl(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c, TextAAMode aa,
                    const TextLayoutOpts &opts) -> void {
    const auto &faces = resolve_faces(f.family);
    if (faces.empty()) {
        draw_text_bitmap_fallback(p, r, text, f, c, opts);
        return;
    }
    // 与度量（px_measure）保持一致的逻辑像素尺寸，再按设备缩放；保证绘制字形尺寸 == 布局度量尺寸。
    const int px = std::max(1, static_cast<int>(std::lround(static_cast<float>(px_measure(f)) * p.scale())));
    const GlyphAtlas::Mode mode =
        (aa == TextAAMode::ClearType && c.m_a == 255) ? GlyphAtlas::Mode::Lcd : GlyphAtlas::Mode::Gray;
    const float line_h = line_height_px(faces, px);

    // 与度量/绘制逐位同源：整段按行切分，逐行调用 shape_line（hb_shape）得到字形序列，
    // 再按 shaped run 推进 pen 并合成为像素——度量（measure/caret）与命中测试复用同一逻辑。
    const auto lines = split_lines(text);
    // r.origin.y 为行盒顶（历史 GDI TA_TOP 语义），FreeType 以基线定位字形：
    // 首行基线 = 顶 + ascender，否则整体上移一个 ascent（顶部控件文字被裁出窗外）。
    // 将行首 snap 到整数物理像素：hinted 字形 advance 为整像素，从整数坐标开始绘制
    // 可避免高 DPI/非整数列宽造成的半像素模糊（如 125% DPI 下 GridView 列宽为半整数
    // 时，1、3 列清晰而 2、4 列发虚）。
    float pen_y = std::floor(r.origin.y + ascender_px(faces, px) + 0.5f);
    for (const auto &line_str : lines) {
        const auto sl = shape_line(line_str, faces, px, opts);
        float pen_x = std::floor(r.origin.x + 0.5f); // 每行 pen 推进归零并 snap 到整数像素
        for (std::size_t j = 0; j < sl.glyphs.size(); ++j) {
            const auto &sg = sl.glyphs.at(j);
            const FT_Face face = sg.face->face;
            const std::uint64_t key =
                make_key(static_cast<std::uint32_t>(sg.face->id), static_cast<std::uint32_t>(sg.gi),
                         static_cast<std::uint32_t>(px), mode == GlyphAtlas::Mode::Lcd, opts.italic);
            const GlyphAtlas::Entry *e = atlas().find(key);
            AURORA_PROFILE_COUNT(glyphs_rendered, 1);
            AURORA_PROFILE_COUNT(glyph_cache_hits, e != nullptr ? 1 : 0);
            AURORA_PROFILE_COUNT(glyph_cache_misses, e == nullptr ? 1 : 0);
            if (e == nullptr) {
                // atlas 未命中才加载/光栅化字形槽；命中时 left/top/advance 已在 atlas 中，
                // blit 只用 atlas 字段，跳过 FT_Load_Glyph 省下每字形 ~10µs（文本快路径关键）。
                FT_Set_Pixel_Sizes(face, 0, px);
                apply_italic(face, opts.italic); // 与首绘同字形变换，保证逐位一致
                FT_Load_Glyph(face, sg.gi, FT_LOAD_DEFAULT);
                const FT_GlyphSlot slot = face->glyph;
                FT_Render_Glyph(slot, mode == GlyphAtlas::Mode::Lcd ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL);
                GlyphAtlas::Entry ne;
                ne.mode = mode;
                ne.left = slot->bitmap_left;
                ne.top = slot->bitmap_top;
                // LCD 模式下 FT 位图宽度为逻辑宽 ×3（RGB 子像素），但绘制循环按逻辑列遍历，
                // 故 ne.width 存逻辑宽（bitmap.width/3）；Gray 模式两者一致。
                ne.width =
                    static_cast<int>(mode == GlyphAtlas::Mode::Lcd ? slot->bitmap.width / 3 : slot->bitmap.width);
                ne.rows = static_cast<int>(slot->bitmap.rows);
                ne.pitch = slot->bitmap.pitch;
                ne.advance = static_cast<float>(slot->advance.x) / 64.0f;
                // copy_w 为 FT 位图每行真实字节数（LCD 已含 3× 子像素宽度），与绘制循环的行步长一致。
                const int copy_w = static_cast<int>(slot->bitmap.width);
                ne.buf.resize(static_cast<std::size_t>(copy_w) * static_cast<std::size_t>(ne.rows));
                const std::span<const std::uint8_t> src_span(slot->bitmap.buffer,
                                                             static_cast<std::size_t>(slot->bitmap.pitch) *
                                                                 static_cast<std::size_t>(ne.rows));
                for (int y = 0; y < ne.rows; ++y) {
                    const std::size_t src_off =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(slot->bitmap.pitch);
                    const std::size_t dst_off = static_cast<std::size_t>(y) * static_cast<std::size_t>(copy_w);
                    std::copy_n(src_span.subspan(src_off, static_cast<std::size_t>(copy_w)).begin(),
                                static_cast<std::size_t>(copy_w),
                                ne.buf.begin() + static_cast<std::ptrdiff_t>(dst_off));
                }
                atlas().insert(key, std::move(ne));
                e = atlas().find(key);
            }

            // hb placement 偏移（x_off/y_off）已实现连字/复杂脚本的字形微位移。
            const int dx0 = static_cast<int>(std::floor(pen_x + sg.x_off)) + e->left;
            const int dy0 = static_cast<int>(std::floor(pen_y + sg.y_off)) - e->top;
            if (mode == GlyphAtlas::Mode::Gray) {
                const std::span<const std::uint8_t> buf(e->buf);
                for (int y = 0; y < e->rows; ++y) {
                    const int py = dy0 + y;
                    const std::size_t off = static_cast<std::size_t>(y) * static_cast<std::size_t>(e->width);
                    // 批处理整行：裁剪只判一次，内联 gamma 混合，消除逐像素开销。
                    p.blend_subpixel_span(dx0, py, c, buf.subspan(off, static_cast<std::size_t>(e->width)).data(),
                                          e->width, false, static_cast<float>(c.m_a) / 255.0f);
                }
            } else {
                const int cols = e->width;
                const std::span<const std::uint8_t> buf(e->buf);
                for (int y = 0; y < e->rows; ++y) {
                    const int py = dy0 + y;
                    const std::size_t off = static_cast<std::size_t>(y) * static_cast<std::size_t>(cols) * 3u;
                    // 批处理整行（LCD 三通道）：裁剪只判一次，内联 gamma 混合。
                    p.blend_subpixel_span(dx0, py, c, buf.subspan(off, static_cast<std::size_t>(cols) * 3u).data(),
                                          cols, true);
                }
            }

            // 推进：hb 的 x_adv 已在物理 px 空间（已含 hinting/kerning/OT 特性），
            // 叠加 letter/word_spacing（dp 间距须乘 scale 换算到物理像素），与 metric 同源。
            // letter_spacing 仅加在相邻字形之间（整串共 (n-1) 次），末字形后不加，与 line_prefix 一致。
            pen_x += sg.x_adv;
            if (sg.cp == ' ' && opts.word_spacing != 0.0f) {
                pen_x += opts.word_spacing * p.scale();
            }
            if (j + 1 < sl.glyphs.size()) {
                pen_x += opts.letter_spacing * p.scale();
            }
        }
        pen_y += line_h;
        pen_y = std::floor(pen_y + 0.5f); // 下一行同样 snap 到整数像素
    }
}

} // namespace

// ============================ 公共 API ============================

auto FontEngine::instance() -> FontEngine & {
    static FontEngine s;
    return s;
}

FontEngine::~FontEngine() = default;

auto FontEngine::shape_cache_stats() -> ShapeCacheStats { return shape_cache().stats(); }

auto FontEngine::shape_cache_clear() -> void { shape_cache().clear(); }

auto FontEngine::set_default_font(const std::string &ttf_path) -> void {
    shape_cache_clear();
    set_default_font_file(ttf_path);
}

auto FontEngine::register_font(const std::string &family, const std::string &ttf_path) -> void {
    shape_cache_clear();
    register_font_file(family, ttf_path);
}

auto FontEngine::register_font_from_memory(const std::string &family, const std::vector<std::uint8_t> &ttf_bytes)
    -> void {
    shape_cache_clear();
    register_font_memory(family, std::vector<std::uint8_t>(ttf_bytes));
}

auto FontEngine::set_text_aa_mode(TextAAMode mode) -> void { aa_mode() = mode; }

auto FontEngine::text_aa_mode() -> TextAAMode { return aa_mode(); }

auto FontEngine::measure_width(const std::string &text, const Font &f) -> float {
    return measure_width(text, f, TextLayoutOpts{});
}

auto FontEngine::measure_width(const std::string &text, const Font &f, const TextLayoutOpts &opts) -> float {
    const auto &faces = resolve_faces(f.family);
    if (faces.empty()) {
        return BitmapFont::measure_width(text, f.size_pt);
    }
    // 自然度量：96 DPI 逻辑像素尺寸，dp 间距不需换算（spacing_scale=1）。
    return max_line_width_px(text, faces, px_measure(f), opts, 1.0f);
}

auto FontEngine::display_width(const std::string &text, const Font &f, const TextLayoutOpts &opts, float scale)
    -> float {
    // 实显宽度：按绘制同源的物理像素尺寸真算后折回 dp。FT hinting 把 advance 取整到
    // 整像素，同一字形在两个像素尺寸下的 advance 不成 scale 比例，故不能用自然度量
    // 线性近似（行尾可差数 dp，行内累计误差造成选区/命中与实绘错位）。
    const auto &faces = resolve_faces(f.family);
    if (faces.empty() || scale == 1.0f) {
        return measure_width(text, f, opts);
    }
    return max_line_width_px(text, faces, px_display(f, scale), opts, scale) / scale;
}

auto FontEngine::display_caret_x(const std::string &text, std::size_t char_index, const Font &f,
                                 const TextLayoutOpts &opts, float scale) -> float {
    // 实显 caret：物理像素尺寸下的前缀推进折回 dp，与 draw_text_impl 的 pen_x 逐字符对齐；
    // scale=1 退化为 caret_x（两者同源，结果逐位相等）。
    const auto &faces = resolve_faces(f.family);
    if (faces.empty() || scale == 1.0f) {
        return caret_x(text, char_index, f, opts);
    }
    return prefix_advance_px(text, char_index, faces, px_display(f, scale), opts, scale) / scale;
}

auto FontEngine::measure_height(const Font &f) -> float {
    const auto &faces = resolve_faces(f.family);
    if (faces.empty()) {
        return BitmapFont::measure_height(f.size_pt);
    }
    return line_height_px(faces, px_measure(f));
}

auto FontEngine::caret_x(const std::string &text, std::size_t char_index, const Font &f) -> float {
    return caret_x(text, char_index, f, TextLayoutOpts{});
}

auto FontEngine::caret_x(const std::string &text, std::size_t char_index, const Font &f, const TextLayoutOpts &opts)
    -> float {
    const auto &faces = resolve_faces(f.family);
    if (faces.empty()) {
        return BitmapFont::measure_width(cp_substr(text, char_index), f.size_pt);
    }
    // 自然 caret：96 DPI 逻辑像素尺寸下的前缀推进（spacing_scale=1）。
    return prefix_advance_px(text, char_index, faces, px_measure(f), opts, 1.0f);
}

auto FontEngine::hit_test_char(const std::string &text, float x, const Font &f) -> std::size_t {
    return hit_test_char(text, x, f, TextLayoutOpts{});
}

auto FontEngine::hit_test_char(const std::string &text, float x, const Font &f, const TextLayoutOpts &opts)
    -> std::size_t {
    const std::size_t total = cp_count(text);
    if (total == 0 || x <= 0.0f) {
        return 0;
    }
    const auto &faces = resolve_faces(f.family);
    if (faces.empty()) {
        // BitmapFont 兜底：逐边界走 caret_x（位图字体恒定宽，成本低，无需单趟优化）。
        float prev = 0.0f;
        for (std::size_t i = 1; i <= total; ++i) {
            const float boundary = caret_x(text, i, f, opts);
            const float mid = (prev + boundary) * 0.5f;
            if (x < mid) {
                return i - 1;
            }
            prev = boundary;
        }
        return total;
    }
    // 单趟扫描：边界值与逐次 caret_x 逐位一致，避免 O(n²)（长行拖选卡顿主因）。
    return hit_test_single_pass(text, x, faces, px_measure(f), opts, 1.0f, 1.0f, false);
}

auto FontEngine::hit_test_char_inclusive(const std::string &text, float x, const Font &f) -> std::size_t {
    return hit_test_char_inclusive(text, x, f, TextLayoutOpts{});
}

auto FontEngine::hit_test_char_inclusive(const std::string &text, float x, const Font &f, const TextLayoutOpts &opts)
    -> std::size_t {
    const std::size_t total = cp_count(text);
    if (total == 0 || x <= 0.0f) {
        return 0;
    }
    const auto &faces = resolve_faces(f.family);
    if (faces.empty()) {
        // BitmapFont 兜底：逐边界走 caret_x。
        for (std::size_t i = 0; i < total; ++i) {
            if (x <= caret_x(text, i + 1, f, opts)) {
                return i;
            }
        }
        // 含入语义：落在整行最右（超出末字符右缘）时，命中末字符本身（而非 caret 越界），
        // 使拖拽到行尾也能选中末字符（消除行尾漏选）。
        return total - 1u;
    }
    return hit_test_single_pass(text, x, faces, px_measure(f), opts, 1.0f, 1.0f, true);
}

auto FontEngine::display_hit_test_char(const std::string &text, float x, const Font &f, const TextLayoutOpts &opts,
                                       float scale) -> std::size_t {
    const auto &faces = resolve_faces(f.family);
    if (faces.empty() || scale == 1.0f) {
        return hit_test_char(text, x, f, opts);
    }
    // 实显命中（caret 语义）：单趟扫描，边界与逐次 display_caret_x 逐位一致，
    // 与实绘字形逐字符对齐且避免 O(n²)。
    return hit_test_single_pass(text, x, faces, px_display(f, scale), opts, scale, scale, false);
}

auto FontEngine::display_hit_test_char_inclusive(const std::string &text, float x, const Font &f,
                                                 const TextLayoutOpts &opts, float scale) -> std::size_t {
    const auto &faces = resolve_faces(f.family);
    if (faces.empty() || scale == 1.0f) {
        return hit_test_char_inclusive(text, x, f, opts);
    }
    // 实显命中（含头含尾）：单趟扫描；行尾右侧命中末字符（消除行尾漏选）。
    return hit_test_single_pass(text, x, faces, px_display(f, scale), opts, scale, scale, true);
}

auto FontEngine::draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c) -> void {
    draw_text_impl(p, r, text, f, c, aa_mode(), TextLayoutOpts{});
}

auto FontEngine::draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                           const TextLayoutOpts &opts) -> void {
    draw_text_impl(p, r, text, f, c, aa_mode(), opts);
}

auto FontEngine::draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                           TextAAMode aa_mode) -> void {
    draw_text_impl(p, r, text, f, c, aa_mode, TextLayoutOpts{});
}

auto FontEngine::draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                           TextAAMode aa_mode, const TextLayoutOpts &opts) -> void {
    draw_text_impl(p, r, text, f, c, aa_mode, opts);
}

} // namespace aurora::render

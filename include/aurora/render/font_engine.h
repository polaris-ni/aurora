#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/render/text_aa_mode.h"

namespace aurora::render {

/**
 * @brief 文本抗锯齿策略（FreeType 驱动，跨平台一致）。
 *
 * - `Supersample`：灰度 AA——`FT_RENDER_MODE_NORMAL` 输出 A8 覆盖度，盒式合成。
 *   颜色安全、背景无关，对半透明文本与任意背景均正确。
 * - `ClearType`：屏幕最佳——`FT_RENDER_MODE_LCD` 输出 3× 水平 RGB 子像素覆盖度，
 *   由 `Painter::blend_subpixel` 逐通道合成，得到真·子像素锐利文本（非灰度降级）。
 *   仅当文本不透明（`c.a == 255`）时使用，否则自动回退 `Supersample`。
 *   跨机一致（字体由引擎内置打包），不依赖系统 ClearType 调谐。
 */
/**
 * @brief 字体引擎（单例）：提供「真实字体渲染」的度量与绘制（specification/03-layout-render.md §8.2）。
 *
 * 设计目标：widget/布局层只依赖本接口的抽象语义，不关心字形解码来源。
 *
 * 实现策略：以 FreeType 作为唯一字体内核（经 CMake FetchContent 编入静态库），
 * 跨平台一致、确定性。内置 Noto Sans（OFL）作为全平台默认字体（引擎首次使用时自动
 * 注册，含 Headless），确保跨机文本渲染逐位确定；缺字按候选 FT_Face 链回退（含系统
 * CJK 字体），避免豆腐块。`set_default_font` / `register_font` / `register_font_from_memory`
 * 可注入私有字体。无任何可用字体文件时回退到内置 `BitmapFont`（零依赖位图字体），
 * 保证 headless 渲染始终可输出文本。
 *
 * 文本选中相关原语 `caret_x` / `hit_test_char` 以「码点」为索引单位（UTF-8 安全），
 * 供 Text/TextInput 精确落光标与命中测试，无需 widget 自行计算布局。
 */

/**
 * @brief 文本布局附加选项：在「字体度量」之外影响测量与绘制的排版属性。
 *
 * 由 `Text` 的 `letter_spacing` / `word_spacing` / `font_style=Italic` 提供，使
 * `measure_width` / `caret_x` / `hit_test_char` / `draw_text` 在「有间距 / 斜体」时
 * 保持完全一致（度量、光标、命中、像素一一对应），避免布局与绘制错位。
 */
struct TextLayoutOpts {
    float letter_spacing = 0.0f; ///< 字形间额外间距（逻辑 dp），加在每对相邻字形之间
    float word_spacing = 0.0f;   ///< 词间额外间距（逻辑 dp），加在每个空格之后
    bool italic = false;         ///< 是否斜体（FreeType 经 FT_Set_Transform 施加 shear 变换实现）

    auto operator==(const TextLayoutOpts &o) const -> bool {
        return letter_spacing == o.letter_spacing && word_spacing == o.word_spacing && italic == o.italic;
    }
};

/**
 * @brief 文本 shaping 缓存统计快照。
 *
 * 缓存是真实优化（非 `AURORA_ENABLE_PROFILING` 门控），故统计始终可用。`命中率 = hits / (hits + misses)`。
 */
struct ShapeCacheStats {
    std::uint64_t hits = 0;   ///< 命中次数（跳过 hb_shape）
    std::uint64_t misses = 0; ///< 未命中次数（触发 hb_shape）
    std::size_t entries = 0;  ///< 当前缓存条目数
    std::size_t bytes = 0;    ///< 当前估算占用字节数
};

class FontEngine {
  public:
    /// @brief 进程级单例。
    static auto instance() -> FontEngine &;

    FontEngine(const FontEngine &) = delete;
    FontEngine(FontEngine &&) = delete;
    auto operator=(const FontEngine &) -> FontEngine & = delete;
    auto operator=(FontEngine &&) -> FontEngine & = delete;

    /// @brief 加载默认字体文件（经 FreeType 加载并覆盖默认链；family 为空表示默认）。
    static auto set_default_font(const std::string &ttf_path) -> void;

    /// @brief 注册 family→ttf_path 的私有字体文件（family 为空表示默认 sans-serif）。
    static auto register_font(const std::string &family, const std::string &ttf_path) -> void;

    /// @brief 注册内存字体（family 为空表示默认 sans-serif）。用于打包内置/私有 TTF 字节，
    ///        例如引擎内置的 Noto Sans 即经此注册，避免运行时依赖字体文件路径。
    static auto register_font_from_memory(const std::string &family, const std::vector<std::uint8_t> &ttf_bytes)
        -> void;

    /// @brief 设置文本抗锯齿策略（影响 FreeType 渲染模式；默认 `TextAAMode::Supersample`）。
    static auto set_text_aa_mode(TextAAMode mode) -> void;

    /// @brief 取得当前文本抗锯齿策略。
    [[nodiscard]] static auto text_aa_mode() -> TextAAMode;

    /// @brief 测量字符串宽度（设备像素，含字距/kerning）。
    [[nodiscard]] static auto measure_width(const std::string &text, const Font &f) -> float;

    /// @brief 测量字符串宽度（含 `opts` 的间距/斜体）。无间距且非斜体时与上方等价。
    [[nodiscard]] static auto measure_width(const std::string &text, const Font &f, const TextLayoutOpts &opts)
        -> float;

    /// @brief 实显宽度（逻辑 dp）：按「绘制所用帧缓冲像素比 scale（dp→物理，Painter::scale）」
    ///        推导物理像素尺寸测量整串宽度并折算回 dp。FreeType hinting 把每个字形 advance
    ///        取整到整像素，同一字形在不同像素尺寸下的 advance 不成比例，96dp 测量
    ///        （measure_width）与物理光栅实绘宽度可差数 dp 且在行尾累计；选区高亮/命中需与
    ///        实绘像素对齐时用本函数。scale=1（96 DPI，含 Headless 测试）退化为 measure_width。
    [[nodiscard]] static auto display_width(const std::string &text, const Font &f, const TextLayoutOpts &opts,
                                            float scale) -> float;

    /// @brief 实显 caret x（逻辑 dp）：按 scale 推导物理像素尺寸测前 `char_index` 个码点的
    ///        前缀推进后折算回 dp，与 draw_text 的 pen 推进逐字符同源（同 px、同 hinting
    ///        advance、同 kerning/间距语义），逐字符精确（而非整行线性近似）；选区高亮/命中
    ///        需与实绘像素对齐时用本函数。scale=1 退化为 `caret_x`（两者同源逐位相等）。
    [[nodiscard]] static auto display_caret_x(const std::string &text, std::size_t char_index, const Font &f,
                                              const TextLayoutOpts &opts, float scale) -> float;

    /// @brief 测量单行高度（设备像素，ascent+descent）。
    [[nodiscard]] static auto measure_height(const Font &f) -> float;

    /// @brief 选中原语：第 `char_index` 个码点之前的基线 x（码点索引，UTF-8 安全）。
    [[nodiscard]] static auto caret_x(const std::string &text, std::size_t char_index, const Font &f) -> float;

    /// @brief 选中原语（含 `opts` 的间距/斜体）：第 `char_index` 个码点之前的基线 x。
    [[nodiscard]] static auto caret_x(const std::string &text, std::size_t char_index, const Font &f,
                                      const TextLayoutOpts &opts) -> float;

    /// @brief 选中原语：给定点击 x，返回最近的光标码点下标（0..码点数）。
    [[nodiscard]] static auto hit_test_char(const std::string &text, float x, const Font &f) -> std::size_t;

    /// @brief 选中原语（含 `opts` 的间距/斜体）：给定点击 x，返回最近的光标码点下标。
    [[nodiscard]] static auto hit_test_char(const std::string &text, float x, const Font &f, const TextLayoutOpts &opts)
        -> std::size_t;

    /// @brief 命中测试（含头含尾）：给定点击 x，返回「被点击字符」的码点下标——
    ///        点击落在某字符的任意位置（含右半）均计入该字符。消除 `hit_test_char`
    ///        （按中点返回下一光标）在选区端点造成的 off-by-one 漏选（行首/行尾字符未高亮）。
    [[nodiscard]] static auto hit_test_char_inclusive(const std::string &text, float x, const Font &f) -> std::size_t;

    /// @brief 命中测试（含头含尾，含 `opts` 的间距/斜体）。
    [[nodiscard]] static auto hit_test_char_inclusive(const std::string &text, float x, const Font &f,
                                                      const TextLayoutOpts &opts) -> std::size_t;

    /// @brief 实显命中测试（caret 语义，同 `hit_test_char`）：字符边界取 `display_caret_x`，
    ///        与缩放屏实绘字形位置逐字符对齐；scale=1 时与 `hit_test_char` 完全等价。
    [[nodiscard]] static auto display_hit_test_char(const std::string &text, float x, const Font &f,
                                                    const TextLayoutOpts &opts, float scale) -> std::size_t;

    /// @brief 实显命中测试（含头含尾语义，同 `hit_test_char_inclusive`）：字符边界取
    ///        `display_caret_x`；scale=1 时与 `hit_test_char_inclusive` 完全等价。
    [[nodiscard]] static auto display_hit_test_char_inclusive(const std::string &text, float x, const Font &f,
                                                              const TextLayoutOpts &opts, float scale) -> std::size_t;

    /// @brief 绘制文本：把字形以 `c` 着色、按覆盖度 alpha 混合进 Painter 帧缓冲。
    ///        具体抗锯齿策略由 `text_aa_mode()` 决定（见 `TextAAMode`）。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c) -> void;

    /// @brief 绘制文本（**显式覆盖抗锯齿策略**）：用于多变/非均匀/渐变背景下避免 ClearType 的
    ///        红/蓝子像素羽化（例：`examples/demos/common.h:GradientTitle` 在蓝→紫渐变上画白字）。
    ///        `Supersample` 灰度 AA 背景无关、颜色安全，无红/蓝羽化。
    ///        `ClearType` 等价于默认路径（屏幕最佳）。
    ///        其余失败兜底（半透明、字体不可用）回退 `Supersample`。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                          TextAAMode aa_mode) -> void;

    /// @brief 绘制文本（按进程级 AA 策略，含 `opts` 的间距/斜体）。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                          const TextLayoutOpts &opts) -> void;

    /// @brief 绘制文本（**显式覆盖 AA 策略**，含 `opts` 的间距/斜体）。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                          TextAAMode aa_mode, const TextLayoutOpts &opts) -> void;

    /// @brief 文本 shaping 缓存统计：命中率 = hits/(hits+misses) 。
    [[nodiscard]] static auto shape_cache_stats() -> ShapeCacheStats;

    /// @brief 清空 shaping 缓存（字体注册 / 变更后调用，避免陈旧字形序列）。
    static auto shape_cache_clear() -> void;

  private:
    FontEngine() = default;
    ~FontEngine();
};

} // namespace aurora::render
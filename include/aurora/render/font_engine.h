#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/enums.h"
#include "aurora/core/font.h"
#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/render/text_aa_mode.h"

/// @brief 渲染域命名空间：字体引擎门面与文本排版选项。
namespace aurora::render {

/// @brief 按族缺字回退链的容量上限（`TextLayoutOpts` 内定长数组的元素数）。
///
/// 取 8 的依据：回退链是「本族缺字时依次尝试的少量候选族」，等宽终端类消费方典型配置是
/// 「等宽主族 + 1~2 个 CJK 族」共 2~3 项；8 留足余量又不让 `TextLayoutOpts` 显著变大
/// （本结构按值 pervasive 传递，还是 shaping 缓存键的组成部分）。
/// 命名遵循全局常量口径（`AURORA_` 前缀 + UPPER_CASE）。
inline constexpr std::size_t AURORA_TEXT_FALLBACK_CHAIN_MAX = 8;

// 文本抗锯齿策略 `TextAAMode`（定义于 text_aa_mode.h，经 set_text_aa_mode 设为进程级默认）：
// - `Supersample`：灰度 AA——`FT_RENDER_MODE_NORMAL` 输出 A8 覆盖度，盒式合成。
//   颜色安全、背景无关，对半透明文本与任意背景均正确。
// - `ClearType`：屏幕最佳——`FT_RENDER_MODE_LCD` 输出 3× 水平 RGB 子像素覆盖度，
//   由 `Painter::blend_subpixel` 逐通道合成，得到真·子像素锐利文本（非灰度降级）。
//   仅当文本不透明（`c.a == 255`）时使用，否则自动回退 `Supersample`。
//   跨机一致（字体由引擎内置打包），不依赖系统 ClearType 调谐。

/// @brief 文本布局附加选项：在「字体度量」之外影响测量与绘制的排版属性。
///
/// 由 `Text` 的 `letter_spacing` / `word_spacing` / `font_style=Italic` 提供，使
/// `measure_width` / `caret_x` / `hit_test_char` / `draw_text` 在「有间距 / 斜体」时
/// 保持完全一致（度量、光标、命中、像素一一对应），避免布局与绘制错位。
struct TextLayoutOpts {
    float letter_spacing = 0.0F;  ///< 字形间额外间距（逻辑 dp），加在每对相邻字形之间
    float word_spacing = 0.0F;  ///< 词间额外间距（逻辑 dp），加在每个空格之后
    bool italic = false;  ///< 是否斜体（FreeType 经 FT_Set_Transform 施加 shear 变换实现）
    /// @brief 书写方向：nullopt = 按内容自动 guess（现状，golden 零影响）；
    ///        显式 RTL 时 HarfBuzz 把字形反转为**视觉序**（绘制按数组顺序左→右即为正确
    ///        视觉序），`caret_x`/`hit_test_*` 相应做逻辑↔视觉镜像映射（逻辑首字符在右缘）。
    ///        已知限制（alpha 范围）：按「1 码点 = 1 字形」近似映射（连字/簇未建模）；
    ///        混排 UBA 多 run 视觉重排、BitmapFont 兜底路径不支持 RTL。
    std::optional<TextDirection> direction = std::nullopt;

    /// @brief 按族缺字回退链：本族自身没有该字形时**先看哪一族**（顺序即语义）。
    ///
    /// 缺省（长度 0）= 不启用，与加本字段之前的行为逐字节一致。全局默认链恒在尾部兜底，
    /// 本字段只决定它之前插哪些族；族名的解析口径与 `list_font_families()` **完全同源**
    /// （故取自该函数结果的族名必定解析得到面），解析不到的族**跳过**并继续后续链段。
    /// 逐字段语义、顺序保证与不重排规则见 `resolve_faces` 的按族链重载。
    ///
    /// 承载形态是「定长数组 + 长度」而非 `std::vector` / `shared_ptr`：后者会让本结构**不再是
    /// 字面类型**，而既有调用点（本仓 `itest_text_selection` 等 8 处）用 `constexpr
    /// TextLayoutOpts o{}` 构造默认 opts，改承载形态会把它们全部编译失败——这是「不破坏既有
    /// 消费者」的一部分，不只是新功能自身的选择。定长数组还顺带免去拷贝时的分配与原子递增。
    ///
    /// 容量 `AURORA_TEXT_FALLBACK_CHAIN_MAX` 的取值依据：回退链是「本族缺字时依次尝试的少量
    /// 候选族」，终端/表格这类消费方通常 1~3 项（等宽族 + 一个 CJK 族）；超过上限的项**丢弃**
    /// （保留前 N 项，顺序语义不变），不静默扩容。
    std::array<std::string, AURORA_TEXT_FALLBACK_CHAIN_MAX> font_fallback_chain{};
    /// @brief 回退链的有效长度（<= `AURORA_TEXT_FALLBACK_CHAIN_MAX`）；0 = 未启用回退链。
    ///        只读前 N 项，尾部槽位恒为默认空串、不参与相等比较。
    std::size_t font_fallback_chain_size = 0;

    /// @brief 按固定格推进档位：每个字形一律按此步进，取代 HarfBuzz 给出的该 face 自身
    ///        physical px advance。nullopt = 不启用（默认），逐位沿用各自 face 的推进量。
    ///
    /// 供等宽网格类消费者（终端、表格）声明「一个字符占 N px」而无需自算字形位置：
    /// 回退面的双宽字形（如汉字 19 px）推进量本由**该 face** 说了算，于是同一 run 内其后的
    /// 字形被整体挪位并逐字累积；开启本档位后推进量与选面**解耦**——选面仍走缺字回退（缺字
    /// 照旧回退到 CJK 面），只有 pen 推进改为固定步进。
    ///
    /// 单位是**物理像素且已含 scale**：调用方直接传 `monospace_cell(f, scale).cell_width_px`
    /// 的整数倍即可，不需自己把 dp 折算成物理像素（该口径与 `monospace_cell` 的整格度量同源）。
    /// 传逻辑 dp 会导致缩放屏上格宽与实际推进不符。
    ///
    /// 与 `letter_spacing` / `word_spacing` 的关系：两者**叠加**在本档位之上（间距语义不变），
    /// 故网格消费方通常同时把它们留 0。此档下**不叠加**合成粗体的 `embolden_px`（格宽是约定值
    /// 不是量出来的推进，加粗字形溢出半格也不推挤邻格，与 `monospace_cell` 的既有口径一致）。
    std::optional<float> fixed_cell_advance_px = std::nullopt;

    /// @brief 逐字段相等比较：六个排版属性全同才相等。
    /// @param o 待比较的另一组布局选项。
    /// @return 两选项的 letter_spacing / word_spacing / italic / direction / 回退链 /
    ///         fixed_cell_advance_px 是否全部相等。
    /// @note 回退链按**内容**比较而非地址：录制/回放两侧会各自构造内容相同但地址不同的链
    ///       （共享句柄不跨录制边界），按地址比较会让回放路径永远命中不到度量缓存。
    auto operator==(const TextLayoutOpts &o) const -> bool {
        if (letter_spacing != o.letter_spacing || word_spacing != o.word_spacing || italic != o.italic ||
            direction != o.direction || fixed_cell_advance_px != o.fixed_cell_advance_px ||
            font_fallback_chain_size != o.font_fallback_chain_size) {
            return false;
        }
        // 只比前 N 项（size 已判等）：未启用的尾部槽位恒为默认空串，比它没有意义，
        // 且会让「同一条链的两种构造方式」被判不等。
        for (std::size_t i = 0; i < font_fallback_chain_size; ++i) {
            if (font_fallback_chain.at(i) != o.font_fallback_chain.at(i)) {
                return false;
            }
        }
        return true;
    }

    /// @brief 只读视图：当前生效的回退链（长度 = `font_fallback_chain_size`）。
    /// @return 指向链首元素的指针；链为空时返回空指针。
    [[nodiscard]] auto fallback_chain_view() const -> std::span<const std::string> {
        return {font_fallback_chain.data(), font_fallback_chain_size};
    }

    /// @brief 构造一条回退链的 opts（便捷入口；链长超上限时截断并保留前 N 项）。
    /// @param chain 按优先级排列的族名序列。
    /// @return 带该回退链的排版选项；其余排版属性取默认值。
    [[nodiscard]] static auto with_fallback_chain(std::span<const std::string> chain) -> TextLayoutOpts {
        TextLayoutOpts o;
        const std::size_t n = std::min(chain.size(), AURORA_TEXT_FALLBACK_CHAIN_MAX);
        for (std::size_t i = 0; i < n; ++i) {
            // span 用下标而非 at()：span 不提供 at()（只读视图，无越界检查成员）。
            o.font_fallback_chain.at(i) = chain[i];
        }
        o.font_fallback_chain_size = n;
        return o;
    }
};

/// @brief 文本 shaping 缓存统计快照。
///
/// 缓存是真实优化（非 `AURORA_ENABLE_PROFILING` 门控），故统计始终可用。`命中率 = hits / (hits + misses)`。
struct ShapeCacheStats {
    std::uint64_t hits = 0;  ///< 命中次数（跳过 hb_shape）
    std::uint64_t misses = 0;  ///< 未命中次数（触发 hb_shape）
    std::size_t entries = 0;  ///< 当前缓存条目数
    std::size_t bytes = 0;  ///< 当前估算占用字节数
};

/// @brief 等宽整像素网格的单格度量（单位：物理像素）。
///
/// 网格类消费者（终端、表格单元格）要的是「第 k 列/行落在哪个整像素」，而非整串排版宽度：
/// hinting 把每个字形的 advance 取整到整像素，整串宽度是这些整数之和，但把 dp 度量再乘
/// `scale` 折算会得到小数列宽，逐列累积成半格错位。三个字段的取整口径一律与绘制路径同源，
/// 故按 `x = col * cell_width_px`、`y = row * cell_height_px` 摆放的单行片段与实绘像素对齐。
struct CellMetrics {
    int cell_width_px = 0;  ///< 单格推进宽度：参考字形在该像素尺寸下的整像素 hinted advance
    int cell_height_px = 0;  ///< 单格行高：与绘制侧行推进同源的一次取整值
    int ascent_px = 0;  ///< 行盒顶 → 基线：与首行 pen_y 的 snap 口径逐位一致
};

/// @brief 字体引擎（单例）：提供「真实字体渲染」的度量与绘制（specification/03-layout-render.md §8.2）。
///
/// 设计目标：widget/布局层只依赖本接口的抽象语义，不关心字形解码来源。
///
/// 实现策略：以 FreeType 作为唯一字体内核（经 CMake FetchContent 编入静态库），
/// 跨平台一致、确定性。内置 Noto Sans（OFL）作为全平台默认字体（引擎首次使用时自动
/// 注册，含 Headless），确保跨机文本渲染逐位确定；缺字按候选 FT_Face 链回退（含系统
/// CJK 字体），避免豆腐块。`set_default_font` / `register_font` / `register_font_from_memory`
/// 可注入私有字体。无任何可用字体文件时回退到内置 `BitmapFont`（零依赖位图字体），
/// 保证 headless 渲染始终可输出文本。
///
/// 文本选中相关原语 `caret_x` / `hit_test_char` 以「码点」为索引单位（UTF-8 安全），
/// 供 Text/TextInput 精确落光标与命中测试，无需 widget 自行计算布局。
class FontEngine {
  public:
    /// @brief 进程级单例。
    /// @return 全进程唯一的 FontEngine 实例引用（首次调用时构造）。
    static auto instance() -> FontEngine &;

    /// @brief 单例类型禁止复制构造：实例仅经 `instance()` 获取，地址须稳定。
    FontEngine(const FontEngine &) = delete;
    /// @brief 单例类型禁止移动构造：实例地址须稳定。
    FontEngine(FontEngine &&) = delete;
    /// @brief 单例类型禁止复制赋值。
    /// @return 不存在：函数已 delete，不产生可求值的返回引用。
    auto operator=(const FontEngine &) -> FontEngine & = delete;
    /// @brief 单例类型禁止移动赋值。
    /// @return 不存在：函数已 delete，不产生可求值的返回引用。
    auto operator=(FontEngine &&) -> FontEngine & = delete;

    /// @brief 加载默认字体文件（经 FreeType 加载并覆盖默认链；family 为空表示默认）。
    /// @param ttf_path 字体文件路径。
    static auto set_default_font(const std::string &ttf_path) -> void;

    /// @brief 注册 family→ttf_path 的私有字体文件（family 为空表示默认 sans-serif）。
    /// @param family 字体族名；空串落入默认 sans-serif 槽。
    /// @param ttf_path 该族对应的字体文件路径。
    static auto register_font(const std::string &family, const std::string &ttf_path) -> void;

    /// @brief 注册内存字体（family 为空表示默认 sans-serif）。用于打包内置/私有 TTF 字节，
    ///        例如引擎内置的 Noto Sans 即经此注册，避免运行时依赖字体文件路径。
    /// @param family 字体族名；空串落入默认 sans-serif 槽。
    /// @param ttf_bytes 完整 TTF 文件字节；注册后由引擎持有其生命周期。
    static auto register_font_from_memory(const std::string &family, const std::vector<std::uint8_t> &ttf_bytes)
        -> void;

    /// @brief 设置文本抗锯齿策略（影响 FreeType 渲染模式；默认 `TextAAMode::Supersample`）。
    ///
    /// 会自增 `raster_generation()`：控件缓存（Display List / 离屏层）在录制时固化了光栅
    /// 结果，须以该世代失效，否则切换后仍回放旧光栅。
    /// @param mode 新的文本抗锯齿策略。
    static auto set_text_aa_mode(TextAAMode mode) -> void;

    /// @brief 取得当前文本抗锯齿策略。
    /// @return 进程级当前生效的 `TextAAMode`。
    [[nodiscard]] static auto text_aa_mode() -> TextAAMode;

    /// @brief 光栅状态世代：凡「全局影响字形光栅结果」的设置变更（`set_text_aa_mode`、
    ///        `set_default_font` / `register_font*`）均自增本计数。
    ///
    /// 必要性：控件的 Display List 与离屏层缓存（`Modifier::cache_layer`）在**录制时**就把
    /// 光栅结果（含 AA 模式、所选字面）固化进命令/位图，而 `Widget::mark_needs_paint()`
    /// 只沿父链向上传播失效、**不会**失效后代缓存——若不以本世代校验，切换 AA 模式后
    /// 后代仍回放旧光栅，表现为「切换瞬间无变化，过一会儿才随无关失效零星生效」。
    /// 控件把本世代纳入缓存命中条件即可 O(1) 感知全局光栅状态变化，无需整树遍历。
    /// @return 单调自增的光栅状态世代计数。
    [[nodiscard]] static auto raster_generation() -> std::uint64_t;

    /// @brief 测量字符串宽度（设备像素，含字距/kerning）。
    /// @param text 待测量的 UTF-8 文本。
    /// @param f 字体描述（族名/字号/字重）。
    /// @return 整串宽度，单位与绘制帧缓冲一致。
    [[nodiscard]] static auto measure_width(const std::string &text, const Font &f) -> float;

    /// @brief 测量字符串宽度（含 `opts` 的间距/斜体）。无间距且非斜体时与上方等价。
    /// @param text 待测量的 UTF-8 文本。
    /// @param f 字体描述。
    /// @param opts 排版附加选项（字距/词间距/斜体/方向）。
    /// @return 整串宽度（同 `measure_width` 口径）。
    [[nodiscard]] static auto measure_width(const std::string &text, const Font &f, const TextLayoutOpts &opts)
        -> float;

    /// @brief 实显宽度（逻辑 dp）：按「绘制所用帧缓冲像素比 scale（dp→物理，Painter::scale）」
    ///        推导物理像素尺寸测量整串宽度并折算回 dp。FreeType hinting 把每个字形 advance
    ///        取整到整像素，同一字形在不同像素尺寸下的 advance 不成比例，96dp 测量
    ///        （measure_width）与物理光栅实绘宽度可差数 dp 且在行尾累计；选区高亮/命中需与
    ///        实绘像素对齐时用本函数。scale=1（96 DPI，含 Headless 测试）退化为 measure_width。
    /// @param text 待测量的 UTF-8 文本。
    /// @param f 字体描述。
    /// @param opts 排版附加选项。
    /// @param scale 绘制所用帧缓冲像素比（dp→物理）。
    /// @return 折算回逻辑 dp 的实显宽度。
    [[nodiscard]] static auto display_width(const std::string &text, const Font &f, const TextLayoutOpts &opts,
                                            float scale) -> float;

    /// @brief 实显 caret x（逻辑 dp）：按 scale 推导物理像素尺寸测前 `char_index` 个码点的
    ///        前缀推进后折算回 dp，与 draw_text 的 pen 推进逐字符同源（同 px、同 hinting
    ///        advance、同 kerning/间距语义），逐字符精确（而非整行线性近似）；选区高亮/命中
    ///        需与实绘像素对齐时用本函数。scale=1 退化为 `caret_x`（两者同源逐位相等）。
    /// @param text 待定位的 UTF-8 文本。
    /// @param char_index 光标码点下标（0..码点数）。
    /// @param f 字体描述。
    /// @param opts 排版附加选项。
    /// @param scale 绘制所用帧缓冲像素比（dp→物理）。
    /// @return 前 `char_index` 个码点的前缀推进宽度（逻辑 dp）。
    [[nodiscard]] static auto display_caret_x(const std::string &text, std::size_t char_index, const Font &f,
                                              const TextLayoutOpts &opts, float scale) -> float;

    /// @brief 测量单行高度（设备像素，ascent+descent）。
    /// @param f 字体描述。
    /// @return 单行行盒高度。
    [[nodiscard]] static auto measure_height(const Font &f) -> float;

    /// @brief 测量单行基线上沿（单位与 `measure_height` 完全一致）：行盒顶 → 首行基线。
    ///
    /// 与 `draw_text` 计算首行 pen_y 的口径同源（同一 `px_measure` 尺寸下的主 face ascender，
    /// 回退 face 字形按主 face 基线对齐），唯一差别是本函数**不做**绘制侧的整像素 snap
    /// （`floor(origin_y + ascent + 0.5)`，见 emit_text_glyphs_core）——需要与实绘首行逐位
    /// 一致的调用方自行 snap。无可用字体面时回退 `BitmapFont::measure_ascent`（同一
    /// `pixel_size` 口径），恒有 `0 <= measure_ascent <= measure_height`。
    /// @param f 字体描述。
    /// @return 行盒顶到首行基线的距离（与 `measure_height` 完全同单位）。
    [[nodiscard]] static auto measure_ascent(const Font &f) -> float;

    /// @brief 等宽整像素网格的单格度量：终端/表格这类「按格排布」场景的列宽与行高来源。
    ///
    /// 口径与实绘同源：像素尺寸取 `lround(px_measure(f) * scale)`（与 `draw_text` 一致），
    /// `cell_width_px` 取该尺寸下参考字形 advance 的最大整数值。参考集刻意含制表符
    /// `U+2500`：它在部分字体里比数字宽，只按 ASCII 数字定格宽会让边框压到相邻格。
    /// 行高与基线取绘制侧同一次 `line_height_px` / `ascender_px` 结果并按同一口径取整。
    /// 无可用字体面时回退内置位图字体的整格网格（`AURORA_CELL` 的整数倍）。
    /// @param f 字体描述。同一网格应固定用一个参考 Font 取一次度量：字重切换可带来 1px advance 差。
    /// @param scale 绘制所用帧缓冲像素比（dp→物理），取与 `Painter::scale` 同值；<= 0 按 1 处理。
    /// @return 物理像素单位的单格度量。
    [[nodiscard]] static auto monospace_cell(const Font &f, float scale) -> CellMetrics;

    /// @brief 选中原语：第 `char_index` 个码点之前的基线 x（码点索引，UTF-8 安全）。
    /// @param text 待定位的 UTF-8 文本。
    /// @param char_index 光标码点下标（0..码点数）。
    /// @param f 字体描述。
    /// @return 该码点处的光标 x（与 `measure_width` 同口径）。
    [[nodiscard]] static auto caret_x(const std::string &text, std::size_t char_index, const Font &f) -> float;

    /// @brief 选中原语（含 `opts` 的间距/斜体）：第 `char_index` 个码点之前的基线 x。
    /// @param text 待定位的 UTF-8 文本。
    /// @param char_index 光标码点下标（0..码点数）。
    /// @param f 字体描述。
    /// @param opts 排版附加选项。
    /// @return 该码点处的光标 x（与带 opts 的 `measure_width` 同口径）。
    [[nodiscard]] static auto caret_x(const std::string &text, std::size_t char_index, const Font &f,
                                      const TextLayoutOpts &opts) -> float;

    /// @brief 选中原语：给定点击 x，返回最近的光标码点下标（0..码点数）。
    /// @param text 被点击的 UTF-8 文本。
    /// @param x 点击位置的水平坐标（与 `measure_width` 同口径）。
    /// @param f 字体描述。
    /// @return 距点击位置最近的光标码点下标。
    [[nodiscard]] static auto hit_test_char(const std::string &text, float x, const Font &f) -> std::size_t;

    /// @brief 选中原语（含 `opts` 的间距/斜体）：给定点击 x，返回最近的光标码点下标。
    /// @param text 被点击的 UTF-8 文本。
    /// @param x 点击位置的水平坐标。
    /// @param f 字体描述。
    /// @param opts 排版附加选项。
    /// @return 距点击位置最近的光标码点下标。
    [[nodiscard]] static auto hit_test_char(const std::string &text, float x, const Font &f, const TextLayoutOpts &opts)
        -> std::size_t;

    /// @brief 命中测试（含头含尾）：给定点击 x，返回「被点击字符」的码点下标——
    ///        点击落在某字符的任意位置（含右半）均计入该字符。消除 `hit_test_char`
    ///        （按中点返回下一光标）在选区端点造成的 off-by-one 漏选（行首/行尾字符未高亮）。
    /// @param text 被点击的 UTF-8 文本。
    /// @param x 点击位置的水平坐标（与 `measure_width` 同口径）。
    /// @param f 字体描述。
    /// @return 命中字符的码点下标（0..码点数-1；空文本返回 0；点击越过末字符右缘仍计末字符；
    ///         RTL 下点击视觉左缘之外命中逻辑末字符）。
    [[nodiscard]] static auto hit_test_char_inclusive(const std::string &text, float x, const Font &f) -> std::size_t;

    /// @brief 命中测试（含头含尾，含 `opts` 的间距/斜体）。
    /// @param text 被点击的 UTF-8 文本。
    /// @param x 点击位置的水平坐标。
    /// @param f 字体描述。
    /// @param opts 排版附加选项（字距/词间距/斜体/方向）。
    /// @return 命中字符的码点下标（0..码点数-1；空文本返回 0；越过末字符右缘仍计末字符）。
    [[nodiscard]] static auto hit_test_char_inclusive(const std::string &text, float x, const Font &f,
                                                      const TextLayoutOpts &opts) -> std::size_t;

    /// @brief 实显命中测试（caret 语义，同 `hit_test_char`）：字符边界取 `display_caret_x`，
    ///        与缩放屏实绘字形位置逐字符对齐；scale=1 时与 `hit_test_char` 完全等价。
    /// @param text 被点击的 UTF-8 文本。
    /// @param x 点击位置的水平坐标（逻辑 dp）。
    /// @param f 字体描述。
    /// @param opts 排版附加选项。
    /// @param scale 绘制所用帧缓冲像素比（dp→物理）；无字体面或 scale=1 时退化为 `hit_test_char`。
    /// @return 距点击位置最近的光标码点下标（0..码点数）。
    [[nodiscard]] static auto display_hit_test_char(const std::string &text, float x, const Font &f,
                                                    const TextLayoutOpts &opts, float scale) -> std::size_t;

    /// @brief 实显命中测试（含头含尾语义，同 `hit_test_char_inclusive`）：字符边界取
    ///        `display_caret_x`；scale=1 时与 `hit_test_char_inclusive` 完全等价。
    /// @param text 被点击的 UTF-8 文本。
    /// @param x 点击位置的水平坐标（逻辑 dp）。
    /// @param f 字体描述。
    /// @param opts 排版附加选项。
    /// @param scale 绘制所用帧缓冲像素比（dp→物理）；无字体面或 scale=1 时退化为 `hit_test_char_inclusive`。
    /// @return 命中字符的码点下标（0..码点数-1；行尾右侧命中末字符）。
    [[nodiscard]] static auto display_hit_test_char_inclusive(const std::string &text, float x, const Font &f,
                                                              const TextLayoutOpts &opts, float scale) -> std::size_t;

    /// @brief 绘制文本：把字形以 `c` 着色、按覆盖度 alpha 混合进 Painter 帧缓冲。
    ///        具体抗锯齿策略由 `text_aa_mode()` 决定（见 `TextAAMode`）。
    /// @param p 目标 Painter（字形逐像素混合进其帧缓冲，越界部分被裁剪）。
    /// @param r 摆放区域（仅读 `origin`：整串文本首行左上原点）。
    /// @param text 待绘制的 UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 着色颜色（alpha 参与混合）。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c) -> void;

    /// @brief 绘制文本（**显式覆盖抗锯齿策略**）：用于多变/非均匀/渐变背景下避免 ClearType 的
    ///        红/蓝子像素羽化（例：`examples/demos/common.h:GradientTitle` 在蓝→紫渐变上画白字）。
    ///        `Supersample` 灰度 AA 背景无关、颜色安全，无红/蓝羽化。
    ///        `ClearType` 等价于默认路径（屏幕最佳）。
    ///        其余失败兜底（半透明、字体不可用）回退 `Supersample`。
    /// @param p 目标 Painter。
    /// @param r 摆放区域（仅读 `origin`）。
    /// @param text 待绘制的 UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 着色颜色（半透明时 `ClearType` 自动回退灰度 AA）。
    /// @param aa_mode 显式抗锯齿策略。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                          TextAAMode aa_mode) -> void;

    /// @brief 绘制文本（按进程级 AA 策略，含 `opts` 的间距/斜体）。
    /// @param p 目标 Painter。
    /// @param r 摆放区域（仅读 `origin`）。
    /// @param text 待绘制的 UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 着色颜色。
    /// @param opts 排版附加选项（字距/词间距/斜体/方向）。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                          const TextLayoutOpts &opts) -> void;

    /// @brief 绘制文本（**显式覆盖 AA 策略**，含 `opts` 的间距/斜体）。
    /// @param p 目标 Painter。
    /// @param r 摆放区域（仅读 `origin`）。
    /// @param text 待绘制的 UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 着色颜色。
    /// @param aa_mode 显式抗锯齿策略。
    /// @param opts 排版附加选项。
    static auto draw_text(Painter &p, const Rect &r, const std::string &text, const Font &f, Color c,
                          TextAAMode aa_mode, const TextLayoutOpts &opts) -> void;

    /// @brief 批量绘制文本片段（`Painter::draw_text_runs` 的引擎侧实现）。
    ///
    /// 与「对每个片段各调一次 `draw_text`」逐位一致：落笔坐标、图集条目键、snap 口径全同，差别只在
    /// 相邻同 `Font` 的片段共用一次字体面解析、像素尺寸与行高度量——这三项各含字符串键构造与
    /// `FT_Set_Pixel_Sizes` 状态变更，是逐段调用时的每段固定开销。
    ///
    /// 单位与同类其余入口不同，刻意如此：`runs` 的区域原点是**逻辑 dp**，本函数内部按 `p.scale()`
    /// 折算到物理像素。`draw_text` 由调用方（`Painter`）预缩放，是因为它只传一个矩形；批量入参若同样
    /// 要求预缩放，调用方就得复制整段数组只为改写原点，连带复制每片段的 `Font::family`。
    /// @param p 目标 Painter。
    /// @param runs 片段数组（可为空，空即无操作）。
    /// @param aa_mode 整批共用的抗锯齿策略。
    /// @param opts 整批共用的排版附加选项。
    static auto draw_text_runs(Painter &p, std::span<const TextRun> runs, TextAAMode aa_mode,
                               const TextLayoutOpts &opts) -> void;

    /// @brief 文本 shaping 缓存统计：命中率 = hits/(hits+misses) 。
    /// @return 当前缓存的统计快照（命中/未命中次数、条目数、估算字节占用）。
    [[nodiscard]] static auto shape_cache_stats() -> ShapeCacheStats;

    /// @brief 清空 shaping 缓存（字体注册 / 变更后调用，避免陈旧字形序列）。
    static auto shape_cache_clear() -> void;

  private:
    FontEngine() = default;
    ~FontEngine() = default;
};

}  // namespace aurora::render
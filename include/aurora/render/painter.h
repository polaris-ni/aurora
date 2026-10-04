#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/transform.h"
#include "aurora/core/types.h"
#include "aurora/render/blend.h"

/// @brief Aurora 根命名空间：本头在其中提供软件栅格绘制器 Painter 与光栅耗时排查接口。
namespace aurora {

/// @brief 渲染子域命名空间：本头在此定义批量文本的片段类型，其余文本排版类型只前向声明
///        （完整定义见 font_engine.h / text_aa_mode.h），避免 painter.h ↔ font_engine.h 循环包含。
namespace render {
struct TextLayoutOpts;  // 前向声明（完整定义见 font_engine.h），避免 painter.h ↔ font_engine.h 循环包含
/// @brief 文本抗锯齿策略枚举（前向声明；完整定义见 text_aa_mode.h）。
enum class TextAAMode : std::uint8_t;  // 前向声明（完整定义见 text_aa_mode.h）

/// @brief 批量文本绘制的一个片段：一段同属性文本与其摆放区域。
///
/// 刻意与 `Painter::draw_text` 的单次入参一一对应（区域只读 `origin`，单位为逻辑 dp），
/// 因此「一批片段逐个 `draw_text`」与 `draw_text_runs` 的输出逐位相同——批量化省的是每次
/// 调用的字体面解析、像素尺寸与行高度量，不改变任何落笔坐标。
struct TextRun {
    std::string_view text;  ///< 片段的 UTF-8 文本（含 `\n` 时按多行排版，与 `draw_text` 同口径）。
    Rect box;  ///< 摆放区域（逻辑 dp，仅读 `origin`：片段首行左上角）。
    Font font;  ///< 字体描述（族/字号/字重）。
    Color color;  ///< 文字颜色（alpha 参与混合）。
};
}  // namespace render

class DisplayList;  // 前向声明（完整定义见 display_list.h）；录制/回放接口

/// @brief 软件栅格绘制器：在 RGBA8 像素缓冲上绘制矩形/文本/图像。
///
/// 纯软件实现，无 GPU 依赖（ARCHITECTURE.md §8.1）。后端可插拔（Surface 抽象）：
/// HeadlessSurface（内存 PNG）、Win32Surface（GDI）、GlfwSurface（OpenGL 1.1 可选）。
/// widget 层只依赖抽象绘制语义（fillRect/drawText/drawImage）。
class Painter {
  public:
    /// @brief 默认构造：空画布（宽高 0、scale 1、无裁剪、全不透明）。
    Painter() = default;

    /// @brief device pixel ratio（物理像素 / 逻辑 dp，通常 = dpi / 96）。
    ///        几何绘制把 dp 坐标乘以它映射到物理帧缓冲像素；像素级写入（含文本光栅）不加 scale。
    ///        scale == 1.0（96 DPI）时行为与旧版一致。
    /// @return 当前 device pixel ratio（set_scale 保证恒 > 0）。
    [[nodiscard]] auto scale() const -> float { return scale_; }
    /// @brief 设置 device pixel ratio（须在 `begin` 分配帧缓冲前调用）。
    /// @param s 目标比例；s <= 0 时回落 1.0。
    auto set_scale(float s) -> void { scale_ = s > 0.0F ? s : 1.0F; }

    /// @brief 分配画布。`width`/`height` 为**逻辑 dp** 尺寸；内部按 `scale()` 放大为物理像素缓冲。
    /// @param width 画布宽（逻辑 dp）。
    /// @param height 画布高（逻辑 dp）。
    auto begin(int width, int height) -> void;

    /// @brief `begin` 时的物理缓冲宽（= dp × scale 四舍五入）。
    /// @return 物理像素宽；未 `begin` 时为 0。
    [[nodiscard]] auto width() const -> int;
    /// @brief `begin` 时的物理缓冲高（= dp × scale 四舍五入）。
    /// @return 物理像素高；未 `begin` 时为 0。
    [[nodiscard]] auto height() const -> int;
    /// @brief RGBA8 像素缓冲基址。
    /// @return 帧缓冲首地址；未 `begin` 时为空指针。
    [[nodiscard]] auto data() const -> const std::uint8_t *;

    /// @brief 只读读取帧缓冲像素（ClearType 路径用于取得目标背景色）；越界返回透明色 `Color{0,0,0,0}`。
    /// @param x 物理像素列（不做 dp 换算）。
    /// @param y 物理像素行（不做 dp 换算）。
    /// @return 该像素 RGBA 色；缓冲外返回透明色。
    [[nodiscard]] auto get_pixel(int x, int y) const -> Color;

    /// @brief 设置全局绘制透明度（0..1），乘入后续所有绘制的源 alpha（转场淡入淡出用）。
    /// @param a 全局透明度因子（0..1）。
    auto set_alpha(double a) -> void;

    /// @brief 取得当前全局透明度。
    /// @return 最近一次 `set_alpha` 设定的因子（初始 1.0 全不透明）。
    [[nodiscard]] auto global_alpha() const -> double { return global_alpha_; }

    /// @brief 填充矩形（源覆盖混合，考虑 alpha）。
    /// @param r 目标矩形（逻辑 dp，内部乘 scale）。
    /// @param c 填充色（含 alpha）。
    auto fill_rect(const Rect &r, Color c) -> void;

    /// @brief 把矩形区重置为新帧零基底（RGBA 全零；不走混合、不受裁剪栈/全局透明度影响）。
    /// 像素边界与矩形裁剪快路径取整一致（保留 x ∈ [ceil(l), floor(r)]，含右/下边界），
    /// 供脏区裁剪重绘在保留上帧缓冲的前提下，先把裁剪区恢复到与 `begin` 后一致的
    /// 零基底，避免半透明内容与上帧像素双重混合（rect 为逻辑 dp，内部乘 scale）。
    /// @param r 清零矩形（逻辑 dp，内部乘 scale）。
    auto clear_rect(const Rect &r) -> void;

    /// @brief 用纯色描边矩形边框（1px，受裁剪影响）。
    /// @param r 边框矩形（逻辑 dp，内部乘 scale）。
    /// @param c 边框色。
    auto draw_rect(const Rect &r, Color c) -> void;

    /// @brief 绘制抗锯齿线段（圆帽）：从 a 到 b，线宽 `width`（逻辑 dp，内部乘 scale）。
    /// 基于点到线段距离 SDF 的 1px 羽化覆盖度，供勾号✓/斜线/简单矢量图形使用（受裁剪与全局透明度影响）。
    /// @param a 起点（逻辑 dp）。
    /// @param b 终点（逻辑 dp）。
    /// @param width 线宽（逻辑 dp）。
    /// @param c 线段色。
    auto draw_line(Point a, Point b, float width, Color c) -> void;

    /// @brief 填充圆角矩形（抗锯齿）：等价于 push_clip_rounded + fill_rect + pop_clip 的便捷组合；
    /// radius <= 0 退化为 fill_rect。控件绘制常用（Checkbox/Button 背景）。
    /// @param r 目标矩形（逻辑 dp，内部乘 scale）。
    /// @param radius 圆角半径（逻辑 dp）。
    /// @param c 填充色。
    auto fill_rounded_rect(const Rect &r, float radius, Color c) -> void;

    /// @brief 描边圆角矩形边框（抗锯齿，向内描边）：沿圆角矩形轮廓向内绘制 `thickness` dp 宽的边框带。
    /// radius = min(w,h)/2 时即圆环（RadioButton 外圈）；thickness <= 0 无操作。
    /// @param r 外框矩形（逻辑 dp）。
    /// @param radius 圆角半径（逻辑 dp）。
    /// @param thickness 边框带宽（逻辑 dp，向内描边）。
    /// @param c 边框色。
    auto draw_rounded_border(const Rect &r, float radius, float thickness, Color c) -> void;

    /// @brief 绘制抗锯齿多段线（圆角连接 + 圆帽）：逐像素取「到折线的最小距离」SDF，
    ///        1px 羽化（与 `draw_line` 同口径），join/cap 由距离场的 min 天然融合。
    /// 覆盖度只按**几何**计算一次，故半透明（如系列降透明）不会出现顶点处二次合成的串珠。
    /// 点集为逻辑 dp；宽度 `width` 为逻辑 dp。点数 < 2 / width <= 0 / 全透明时无操作。
    /// @param pts 折线顶点序列（逻辑 dp）。
    /// @param width 线宽（逻辑 dp）。
    /// @param c 线条色。
    auto stroke_polyline(const std::vector<Point> &pts, float width, Color c) -> void;

    /// @brief 填充抗锯齿扇形 / 环扇：圆心 `center`，内/外半径（`inner_r <= 0` 即实心扇形），
    ///        角度区间 [a0, a1) 弧度制（y 轴向下，0 = +x 方向）；`a1 - a0 >= 2π` 视为整圆 / 整环。
    /// 径向（内外弧）与角向（两侧半径边）四条边各 1px 羽化，覆盖度取「到最近边的距离」SDF。
    /// 半径为逻辑 dp。`outer_r <= 0` / `inner_r >= outer_r` / 角差 <= 0 / 全透明时无操作。
    /// @param center 圆心（逻辑 dp）。
    /// @param outer_r 外半径（逻辑 dp）。
    /// @param inner_r 内半径（逻辑 dp；<= 0 为实心扇形）。
    /// @param a0 起始角（弧度，区间左闭）。
    /// @param a1 终止角（弧度，区间右开；角差 >= 2π 视为整圆/整环）。
    /// @param c 填充色。
    auto fill_sector(Point center, float outer_r, float inner_r, float a0, float a1, Color c) -> void;

    /// @brief 描边弧线（语义糖）：等价于 `fill_sector` 的环带形式——
    ///        inner = radius - thickness/2，outer = radius + thickness/2（向内夹取到 0）。
    /// @param center 弧心（逻辑 dp）。
    /// @param radius 弧半径（逻辑 dp，环带中线）。
    /// @param thickness 环带宽度（逻辑 dp）。
    /// @param a0 起始角（弧度）。
    /// @param a1 终止角（弧度）。
    /// @param c 描边色。
    auto stroke_arc(Point center, float radius, float thickness, float a0, float a1, Color c) -> void;

    /// @brief 绘制文本：委托 FontEngine（真实字体渲染；无 GDI/字体时回退内置位图字体）。
    /// @param r 排版区域（逻辑 dp）。
    /// @param s UTF-8 文本。
    /// @param f 字体描述（族/字号/风格）。
    /// @param c 文字颜色。
    auto draw_text(const Rect &r, const std::string &s, const Font &f, Color c) -> void;

    /// @brief 绘制文本（含排版 opts：letter/word spacing & italic）；抗锯齿策略取 FontEngine 进程级 `text_aa_mode()`。
    ///        与 `Text` 的 `letter_spacing` / `word_spacing` / `font_style=Italic` 联动，度量/光标/命中一致。
    /// @param r 排版区域（逻辑 dp）。
    /// @param s UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 文字颜色。
    /// @param opts 排版选项（字距/词距/斜体等；见 font_engine.h 的 TextLayoutOpts）。
    auto draw_text(const Rect &r, const std::string &s, const Font &f, Color c, const render::TextLayoutOpts &opts)
        -> void;

    /// @brief 绘制文本（**显式覆盖抗锯齿策略**，含排版 opts）。
    /// @param r 排版区域（逻辑 dp）。
    /// @param s UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 文字颜色。
    /// @param aa_mode 抗锯齿策略（覆盖 FontEngine 进程级默认）。
    /// @param opts 排版选项。
    auto draw_text(const Rect &r, const std::string &s, const Font &f, Color c, render::TextAAMode aa_mode,
                   const render::TextLayoutOpts &opts) -> void;

    /// @brief 批量绘制文本片段：把「一行拆成若干同属性片段」的场景（终端网格、表格单元格）从每片段一次
    ///        调用降为整批一次调用。输出与逐个 `draw_text` 逐位一致，省的是每次调用的字体面解析、像素尺寸
    ///        与行高度量。录制态按片段各落一条 `DrawText` 命令（回放后端不需新增分支）。
    ///        抗锯齿策略取 FontEngine 进程级 `text_aa_mode()`，排版选项取默认值。
    /// @param runs 片段数组（可空；空数组即无操作）。片段区域内所有 `Font` 相同时收益最大。
    auto draw_text_runs(std::span<const render::TextRun> runs) -> void;

    /// @brief 批量绘制文本片段（**整批共用排版 opts**）：字距/词距/斜体按同一份 `opts` 作用于全部片段。
    ///        与逐个 `draw_text(..., opts)` 逐位一致；调用方若各片段排版属性不同，请按属性分组分批调用
    ///        （本入口不提供 per-run opts，避免 `TextRun` 承载排版字段后与 `Font` 语义重叠）。
    ///        抗锯齿策略取 FontEngine 进程级 `text_aa_mode()`。
    /// @param runs 片段数组（可空；空数组即无操作）。
    /// @param opts 整批共用的排版选项（字距/词距/斜体）。
    auto draw_text_runs(std::span<const render::TextRun> runs, const render::TextLayoutOpts &opts) -> void;

    /// @brief 批量绘制文本片段（**整批共用排版 opts + 显式覆盖抗锯齿策略**）。
    ///        与逐个 `draw_text(..., aa_mode, opts)` 逐位一致。
    /// @param runs 片段数组（可空；空数组即无操作）。
    /// @param aa_mode 抗锯齿策略（覆盖 FontEngine 进程级默认）。
    /// @param opts 整批共用的排版选项（字距/词距/斜体）。
    auto draw_text_runs(std::span<const render::TextRun> runs, render::TextAAMode aa_mode,
                        const render::TextLayoutOpts &opts) -> void;

    /// @brief 把带 alpha 的源色按源覆盖混合到单像素（受裁剪约束）；供字体/半透明使用。
    /// @param x 物理像素列（不做 dp 换算）。
    /// @param y 物理像素行（不做 dp 换算）。
    /// @param c 源色（含 alpha）。
    auto blend_pixel(int x, int y, Color c) -> void;

    /// @brief 真 ClearType 子像素合成：以 `c` 着色，按 R/G/B 三个子像素各自的覆盖度
    ///        `cr`/`cg`/`cb`（0..255）做逐通道源覆盖混合。用于 FreeType `FT_RENDER_MODE_LCD`
    ///        输出的 3× 水平 RGB 覆盖度位图，产生屏幕最佳的子像素锐利文本（非灰度降级）。
    /// @param x 目标物理像素列。
    /// @param y 目标物理像素行。
    /// @param c 文字着色（含 alpha）。
    /// @param cr R 子像素覆盖度（0..255）。
    /// @param cg G 子像素覆盖度（0..255）。
    /// @param cb B 子像素覆盖度（0..255）。
    auto blend_subpixel(int x, int y, Color c, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) -> void;

    /// @brief 批量子像素合成（性能优化）：把同一行的 N 个相邻像素一次性合成，裁剪判定只做一次、
    ///        内联 gamma-correct 混合，消除逐像素函数调用 + 裁剪栈遍历 + SIMD 分发开销。
    ///        `src` 为长度 N（Gray）或 3N（LCD 三通道 RGB 子像素）的覆盖度缓冲；某像素三通道
    ///        全为 0 时自动跳过。`src_alpha` 为源色 alpha 归一因子（Gray 路径须传入 c.a/255）。
    ///        圆角裁剪或录制态自动回退到逐像素 blend_subpixel。
    /// @param x0 行起始物理像素列。
    /// @param y 物理像素行。
    /// @param c 文字着色（含 alpha）。
    /// @param src 覆盖度缓冲（Gray 长 N；LCD 长 3N，R/G/B 交织）。
    /// @param n 像素个数。
    /// @param lcd true = LCD 三通道模式，false = Gray 单通道模式。
    /// @param src_alpha 源 alpha 归一因子（Gray 路径传 c.a/255）。
    auto blend_subpixel_span(int x0, int y, Color c, const std::uint8_t *src, int n, bool lcd, float src_alpha = 1.0F)
        -> void;

    /// @brief 把带 alpha 的源色按源覆盖混合到矩形；等价于 fill_rect（保留以清晰表达混合语义）。
    /// @param r 目标矩形（逻辑 dp）。
    /// @param c 源色（含 alpha）。
    auto blend_rect(const Rect &r, Color c) -> void;

    /// @brief 绘制解码后的图像到目标矩形（RGBA8，alpha 混合，受裁剪约束）。
    /// 图像按目标矩形尺寸做 premultiplied-alpha 空间的双线性采样（四点插值，避免暗边/锯齿）。
    /// @param img 已解码图像（RGBA8）。
    /// @param dest 目标矩形（逻辑 dp）。
    auto draw_image(const Image &img, const Rect &dest) -> void;

    /// @brief 线性渐变填充：在 area 内沿 (start→end) 方向插值 colors/stops 色标。
    /// stops 归一化 [0,1]，与 colors 等长；start/end 为 area 内逻辑 dp 坐标。
    /// @param area 填充矩形（逻辑 dp）。
    /// @param start 渐变起点（area 内逻辑 dp）。
    /// @param end 渐变终点（area 内逻辑 dp）。
    /// @param colors 色标颜色（与 stops 等长）。
    /// @param stops 归一化色标位置 [0,1]（与 colors 等长）。
    auto draw_linear_gradient(const Rect &area, Point start, Point end, const std::vector<Color> &colors,
                              const std::vector<float> &stops) -> void;

    /// @brief 径向渐变填充：在 area 内以 center 为圆心、radius 为半径插值 colors/stops。
    /// @param area 填充矩形（逻辑 dp）。
    /// @param center 圆心（逻辑 dp）。
    /// @param radius 半径（逻辑 dp；0 = 纯色填充）。
    /// @param colors 色标颜色（与 stops 等长）。
    /// @param stops 归一化色标位置 [0,1]（与 colors 等长）。
    auto draw_radial_gradient(const Rect &area, Point center, float radius, const std::vector<Color> &colors,
                              const std::vector<float> &stops) -> void;

    /// @brief 投影阴影：在 shape 偏移 (offset_x, offset_y) 处绘制模糊矩形阴影。
    /// blur_radius 控制模糊程度（0=硬边），color 为阴影色（含 alpha）。
    /// @param shape 源形状矩形（逻辑 dp，决定阴影外形）。
    /// @param offset_x 水平偏移（逻辑 dp）。
    /// @param offset_y 垂直偏移（逻辑 dp）。
    /// @param blur_radius 模糊半径（逻辑 dp；0 = 硬边）。
    /// @param color 阴影色（含 alpha）。
    auto draw_shadow(const Rect &shape, float offset_x, float offset_y, float blur_radius, Color color) -> void;

    /// @brief 就地模糊帧缓冲中的矩形区域（分离式两遍 box blur ≈ 高斯）。
    /// 用于 `Modifier::blur`（内容模糊）与 `Modifier::backdrop_filter`（毛玻璃：
    /// 绘内容前先模糊背后区域）。radius 为逻辑 dp（内部乘 scale）；<=0 无操作。
    /// @param region 模糊区域（逻辑 dp）。
    /// @param radius 模糊半径（逻辑 dp）。
    auto blur_region(const Rect &region, float radius) -> void;

    /// @brief 就地垂直平移帧缓冲像素（`dy` 为逻辑 dp：>0 内容下移，<0 内容上移）。
    ///
    /// 像素行在缓冲中连续存储，故垂直平移退化为**单次 `std::memmove`**，
    /// 成本 O(缓冲字节) 但常数极小。滚动容器重锚点（reanchor）时用它搬移仍可复用的
    /// 像素、只重绘让出的条带，替代整块 `composite` 的逐像素矩阵求逆
    /// （3 屏离屏缓冲实测 ~30ms → ~1ms，是 60Fps 滚动预算的关键）。
    ///
    /// 移出缓冲的像素直接丢弃；让出的条带重置为 `begin` 后的零基底（语义同 `clear_rect`），
    /// 调用方须负责重绘该条带。`|dy|` 超过缓冲高度时整块清零。
    /// **不经过裁剪栈与 `global_alpha`**（纯像素搬移，与 `clear_rect` 一致）；
    /// 录制模式下为 no-op（Display List 无对应命令，该原语仅服务直绘的离屏缓冲）。
    /// @param dy 垂直平移量（逻辑 dp）。
    auto shift_pixels(float dy) -> void;

    /// @brief 把 `region` 内已绘制像素与 `tint` 按 `mode` 混合（就地覆盖）。
    /// 用于 `Modifier::blend_mode`；应在内容绘制完成后调用。`strength`（0..1）控制强度，
    /// 0 不改变、1 完全按模式混合。`BlendMode` 定义见 `aurora/render/blend.h`。
    /// @param region 作用区域（逻辑 dp）。
    /// @param mode 混合模式。
    /// @param tint 混合色。
    /// @param strength 强度（0..1）；默认 1.0。
    auto blend_region(const Rect &region, BlendMode mode, Color tint, float strength = 1.0F) -> void;

    /// @brief 把 `region` 内像素 RGB 乘以渐变遮罩因子（0..1），形成淡出 / 聚焦。
    /// 用于 `Modifier::shader_mask`；`strength`（0..1）控制强度，0 不改变、1 完全遮罩。
    /// @param region 作用区域（逻辑 dp）。
    /// @param kind 遮罩形态（线性/径向等淡出方向）。
    /// @param strength 强度（0..1）；默认 1.0。
    auto mask_region(const Rect &region, ShaderMaskKind kind, float strength = 1.0F) -> void;

    /// @brief 把已渲染的离屏子树（源缓冲）按仿射矩阵合成回本缓冲。
    ///
    /// 用于修饰节点的旋转 / 缩放 / 任意仿射变换：子树先渲染到离屏 Painter，
    /// 再经 `matrix` 映射到本缓冲（矩阵为逻辑 dp 空间，内部乘 `scale_` 到物理像素）。
    /// 合成尊重本缓冲的裁剪栈与 `global_alpha`（透明度由此统一生效）。
    ///
    /// @param src 源 Painter（已 begin，逻辑尺寸 = 其 begin 尺寸，物理 = *scale_）。
    /// @param matrix 逻辑 dp 空间仿射矩阵（含平移/旋转/缩放，建议绕内容中心构造）。
    auto composite(const Painter &src, const Matrix2D &matrix) -> void;
    /// @brief 把离屏位图（Image）按矩阵合成到当前画布（回放 Composite 命令用，含源缩放）。
    /// @param src 源位图（RGBA8）。
    /// @param matrix 逻辑 dp 空间仿射矩阵。
    /// @param src_scale 源位图的 device pixel ratio（dp→物理像素换算）。
    auto composite(const Image &src, const Matrix2D &matrix, float src_scale) -> void;
    /// @brief 导出当前画布像素为 Image（离屏合成录制时捕获源缓冲）。
    /// @return 当前物理缓冲的 RGBA8 拷贝；未 begin 时为空图。
    [[nodiscard]] auto to_image() const -> Image;

    /// @brief 离屏合成核心：把源像素（设备分辨率 spix，尺寸 sw×sh，逻辑比例 sscale）按 matrix 贴回当前画布。
    /// @param spix 源像素基址（RGBA8）。
    /// @param sw 源物理宽（像素）。
    /// @param sh 源物理高（像素）。
    /// @param sscale 源的 device pixel ratio。
    /// @param matrix 逻辑 dp 空间仿射矩阵。
    auto composite_pixels(const std::uint8_t *spix, int sw, int sh, float sscale, const Matrix2D &matrix) -> void;

    /// @brief 压入裁剪矩形（与当前裁剪取交集），后续绘制仅保留交集内像素。
    /// @param r 裁剪矩形（逻辑 dp）。
    auto push_clip(const Rect &r) -> void;

    /// @brief 压入圆角矩形裁剪（默认抗锯齿；与当前裁剪取交集）。
    /// @param r 裁剪矩形（逻辑 dp）。
    /// @param radius 圆角半径（逻辑 dp）。
    /// @param anti_alias 圆角边界是否做 SDF 抗锯齿；默认 true。
    auto push_clip_rounded(const Rect &r, float radius, bool anti_alias = true) -> void;

    /// @brief 弹出最近一次 pushClip / pushClipRounded 设置的裁剪。
    auto pop_clip() -> void;

    /// @brief 当前是否存在裁剪（裁剪栈非空）。
    /// @return 裁剪栈非空时为 true。
    [[nodiscard]] auto has_clip() const -> bool;

    /// @brief 当前有效裁剪矩形（各层矩形裁剪交集）的逻辑 dp 全局坐标。
    ///        无裁剪时返回整块画布；圆角裁剪退化为其外接矩形（保守，保证不误剔除）。
    /// @return 有效裁剪矩形（逻辑 dp）。
    [[nodiscard]] auto clip_bounds() const -> Rect;

    // ---- Display List 录制 / 回放（AURORA_ENABLE_DISPLAY_LIST）----
    /// @brief 进入录制模式并将绘制命令写入给定 DisplayList（先清空）。绘制原语在录制模式下
    ///        仅记录命令、不直接上屏；嵌套调用以栈管理（子控件缓存 DL 压平并入父 DL）。
    /// @param dl 录制目标（进入时先 clear，压入录制栈）。
    auto record(DisplayList &dl) -> void;
    /// @brief 退出当前录制层级；栈空时回到 Direct（上屏）模式。
    auto stop() -> void;
    /// @brief 是否处于录制模式（录制栈非空）。
    /// @return 录制栈非空时为 true。
    [[nodiscard]] auto is_recording() const -> bool { return !recording_stack_.empty(); }

    /// @brief Window 脏区裁剪绘制（partial clip）期间抑制 Display List 录制/回放：
    ///        partial clip 下子树 paint 只画 clip 内子节点，若此时录制 DL 会丢失 clip 外子节点，
    ///        后续 full 帧 replay 该 DL 时会永久丢失 clip 外子节点（与整帧重绘逐位不一致）。
    /// @param skip true = 抑制录制/回放（脏区裁剪期间），false = 恢复。
    auto set_skip_dl_record(bool skip) -> void { skip_dl_record_ = skip; }
    /// @brief 当前是否处于脏区裁剪的 DL 抑制态。
    /// @return true 表示 Display List 录制/回放被抑制（partial clip 期间）。
    [[nodiscard]] auto skip_dl_record() const -> bool { return skip_dl_record_; }

    /// @brief 标记当前及所有外层录制层级为「含动态内容」：录制这些层级的祖先控件不应
    ///        缓存其 Display List（内容每帧变化或绘制含副作用）。由不可缓存控件在录制模式下调用。
    auto mark_recording_dynamic() -> void;
    /// @brief 当前录制层级是否含有动态内容（用于决定本控件 DL 是否可安全缓存）。
    /// @return 录制栈非空且栈顶层被标记动态时为 true。
    [[nodiscard]] auto recording_is_dynamic() const -> bool {
        return !rec_dynamic_.empty() && (rec_dynamic_.back() != 0);
    }

    // ---- GPU 层缓存命令（仅录制模式；Direct 模式为 no-op）----
    // 层语义见 `rhi::RhiBackend` 消费侧与 `specification/03` §8.7：`begin_layer` 后至
    // `end_layer` 前的命令重定向到常驻层纹理（`aux_key` 寻址，尺寸 = bounds.size 逻辑 dp），
    // `draw_layer` 把层纹理按矩阵合成回画布。仅软件直绘（非录制）不走层命令（走 paint_cache_）。
    /// @brief 开始层捕获（录制模式下记 BeginLayer 命令；Direct 模式 no-op）。
    /// @param key 层纹理缓存键（与 RhiBackend 流式槽同键寻址）。
    /// @param size 层尺寸（逻辑 dp）。
    auto begin_layer(std::uint64_t key, const Size &size) -> void;
    /// @brief 结束层捕获（录制模式下记 EndLayer 命令；Direct 模式 no-op）。
    auto end_layer() -> void;
    /// @brief 合成层纹理到当前画布（录制模式下记 DrawLayer 命令；Direct 模式 no-op）。
    /// @param key 层纹理缓存键。
    /// @param matrix 逻辑 dp 空间仿射矩阵。
    /// @param src_scale 层的 device pixel ratio。
    auto draw_layer(std::uint64_t key, const Matrix2D &matrix, float src_scale) -> void;

  private:
    struct ClipRegion {
        Rect rect;  ///< 裁剪矩形（物理像素）
        bool rounded = false;  ///< 是否圆角裁剪（叠加 SDF 覆盖度）
        float radius = 0.0F;  ///< 圆角半径（物理像素）
        bool anti_alias = true;  ///< 圆角是否抗锯齿（SDF 覆盖度）

        /// @brief 计算点 (x,y) 处裁剪覆盖度（0=完全裁剪，1=完全保留；圆角边界 0..1 抗锯齿）。
        /// @param x 物理像素列。
        /// @param y 物理像素行。
        /// @return 该点裁剪覆盖度（0..1）。
        [[nodiscard]] auto coverage(float x, float y) const -> float;
    };

    /// @brief 物理像素直写：先乘全局透明度与裁剪栈覆盖度（圆角走 SDF，0..1 抗锯齿），
    ///        再在线性光空间做 source-over 混合写入帧缓冲（目标 alpha 恒置 255）；
    ///        越界坐标静默丢弃。裁剪/混合语义由调用方保证。
    /// @param x 物理像素列。
    /// @param y 物理像素行。
    /// @param c 源色（含 alpha）。
    auto set_pixel(int x, int y, Color c) -> void;

    /// @brief 逐像素原语的迭代范围收缩：与裁剪栈各矩形求交（物理像素，含右/下边界语义
    ///        与 coverage 的 contains 像素级等价）；返回 false 表示交集为空（可直接早退）。
    ///        只剔除必被裁剪丢弃的迭代，结果逐位不变（部分脏区重绘性能关键）。
    /// @param x0 迭代左界（物理像素，原地收缩）。
    /// @param y0 迭代上界（物理像素，原地收缩）。
    /// @param x1 迭代右界（物理像素，原地收缩）。
    /// @param y1 迭代下界（物理像素，原地收缩）。
    /// @return 交集非空返回 true；为空返回 false（调用方应早退）。
    [[nodiscard]] auto shrink_to_clips(int &x0, int &y0, int &x1, int &y1) const -> bool;

    /// @brief 给定扫描线 y，计算圆角裁剪 SDF ≤ t 的全覆写 x 范围（半开区间）。
    /// @param cr 圆角裁剪区域。
    /// @param y 扫描线（物理像素行）。
    /// @param t SDF 全覆写阈值。
    /// @return 全覆写 x 半开区间 [左, 右)。
    [[nodiscard]] static auto rounded_full_x_range(const ClipRegion &cr, int y, float t) -> std::pair<int, int>;

    /// @brief fill_rect 的「行级快速路径」：全局不透明且裁剪栈全为非圆角矩形时，
    /// 把裁剪收缩进边界后整行写入（覆写/内联 source-over）。命中返回 true（已绘制），
    /// 否则返回 false 交由慢路径处理（x0..y1 保持不变）。
    /// @param x0 绘制左界（物理像素，收缩后原地生效）。
    /// @param y0 绘制上界（物理像素）。
    /// @param x1 绘制右界（物理像素）。
    /// @param y1 绘制下界（物理像素）。
    /// @param c 填充色。
    /// @return true = 快路径已绘制；false = 未命中，x0..y1 不变，须走慢路径。
    auto fill_rect_fast_path(int &x0, int &y0, int &x1, int &y1, Color c) -> bool;

    /// @brief fill_rect 的「慢路径」：圆角裁剪（SDF 覆盖度）/ 全局透明度 <1 时逐像素处理。
    /// @param x0 绘制左界（物理像素）。
    /// @param y0 绘制上界（物理像素）。
    /// @param x1 绘制右界（物理像素）。
    /// @param y1 绘制下界（物理像素）。
    /// @param c 填充色（含 alpha）。
    auto fill_rect_slow_path(int x0, int y0, int x1, int y1, Color c) -> void;

    int width_ = 0;  ///< 物理缓冲宽（begin 时按 dp × scale 取整）
    int height_ = 0;  ///< 物理缓冲高（dp × scale 取整）
    float scale_ = 1.0F;  ///< device pixel ratio（dp → 物理像素）
    std::vector<std::uint8_t> pixels_;  ///< RGBA8 帧缓冲（宽 × 高 × 4 字节）
    std::vector<ClipRegion> clip_stack_;  ///< 裁剪栈（矩形 + 圆角，滚动/圆角容器用）
    bool has_rounded_clip_ = false;  ///< 裁剪栈中是否存在圆角裁剪（blend_pixel 快速路径判定）
    double global_alpha_ = 1.0;  ///< 全局绘制透明度（set_alpha 设置）
    bool skip_dl_record_ =
        false;  ///< Window 脏区裁剪绘制期间设为 true，抑制 DL 录制/回放（partial clip 下录制会丢失 clip 外子节点）

    // ---- Display List 录制栈（AURORA_ENABLE_DISPLAY_LIST）----
    /// @brief 把一条文本绘制命令录入当前录制目标（变长字符串入池）。
    /// @param r 排版区域（逻辑 dp）。
    /// @param s UTF-8 文本。
    /// @param f 字体描述。
    /// @param c 文字颜色。
    /// @param aa 抗锯齿策略。
    /// @param opts 排版选项。
    auto record_text_cmd(const Rect &r, const std::string &s, const Font &f, Color c, render::TextAAMode aa,
                         const render::TextLayoutOpts &opts) -> void;
    std::vector<DisplayList *> recording_stack_;  ///< 录制目标栈；非空即录制模式
    std::vector<char> rec_dynamic_;  ///< 与录制栈平行的「含动态内容」标记（mark_recording_dynamic 置全部）
};

/// @brief [性能排查] 返回各光栅原语耗时累加器（毫秒）的 JSON 串；读取后清零。
/// `per_frame_divisor`（默认 1）用于把「累计窗口总量」折算为「每帧均值」（通常传 FPS）。
/// 仅供 DEBUG 排查，不用于生产。
/// @param per_frame_divisor 帧数折算除数（通常传 FPS）；默认 1 = 窗口总量原值。
/// @return 各原语耗时（毫秒）的 JSON 串。
auto paint_primitive_timing_json(double per_frame_divisor = 1.0) -> std::string;

/// @brief [性能排查] 返回累加器中「整段 widget 绘制」(scene) 的当前值（毫秒），不重置。
/// 用于让调用方判断上一帧是否真实绘制（scene>~1ms 即非 idle），以便保留有代表性的帧分解。
/// @return scene 累加当前值（毫秒）。
auto paint_timing_scene_last() -> double;

/// @brief [性能排查] 返回「上一完成绘制帧」逐原语耗时（毫秒）的 JSON 串，不重置。
/// divisor=1，即该帧真实耗时；用于每秒打印代表性绘制帧分解，避免 idle 帧零值污染与整秒/FPS 折算误差。
/// @return 上一完成帧的逐原语耗时 JSON 串。
auto paint_primitive_timing_last_json() -> std::string;

}  // namespace aurora

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/transform.h"
#include "aurora/core/types.h"
#include "aurora/render/blend.h"

namespace aurora {

namespace render {
struct TextLayoutOpts;  // 前向声明（完整定义见 font_engine.h），避免 painter.h ↔ font_engine.h 循环包含
enum class TextAAMode : std::uint8_t;  // 前向声明（完整定义见 text_aa_mode.h）
}  // namespace render

class DisplayList;  // 前向声明（完整定义见 display_list.h）；录制/回放接口

/**
 * @brief 软件栅格绘制器：在 RGBA8 像素缓冲上绘制矩形/文本/图像。
 *
 * 纯软件实现，无 GPU 依赖（ARCHITECTURE.md §8.1）。后端可插拔（Surface 抽象）：
 * HeadlessSurface（内存 PNG）、Win32Surface（GDI）、GlfwSurface（OpenGL 1.1 可选）。
 * widget 层只依赖抽象绘制语义（fillRect/drawText/drawImage）。
 */
class Painter {
  public:
    Painter() = default;

    /// @brief device pixel ratio（物理像素 / 逻辑 dp，通常 = dpi / 96）。
    ///        几何绘制把 dp 坐标乘以它映射到物理帧缓冲像素；像素级写入（含文本光栅）不加 scale。
    ///        scale == 1.0（96 DPI）时行为与旧版一致。
    [[nodiscard]] auto scale() const -> float { return scale_; }
    /// @brief 设置 device pixel ratio（须在 `begin` 分配帧缓冲前调用）。
    auto set_scale(float s) -> void { scale_ = s > 0.0F ? s : 1.0F; }

    /// @brief 分配画布。`width`/`height` 为**逻辑 dp** 尺寸；内部按 `scale()` 放大为物理像素缓冲。
    auto begin(int width, int height) -> void;

    [[nodiscard]] auto width() const -> int;
    [[nodiscard]] auto height() const -> int;
    [[nodiscard]] auto data() const -> const std::uint8_t *;

    /// @brief 只读读取帧缓冲像素（ClearType 路径用于取得目标背景色）；越界返回 Color{}。
    [[nodiscard]] auto get_pixel(int x, int y) const -> Color;

    /// @brief 设置全局绘制透明度（0..1），乘入后续所有绘制的源 alpha（转场淡入淡出用）。
    auto set_alpha(double a) -> void;

    /// @brief 取得当前全局透明度。
    [[nodiscard]] auto global_alpha() const -> double { return global_alpha_; }

    /// @brief 填充矩形（源覆盖混合，考虑 alpha）。
    auto fill_rect(const Rect &r, Color c) -> void;

    /// @brief 把矩形区重置为新帧零基底（RGBA 全零；不走混合、不受裁剪栈/全局透明度影响）。
    /// 像素边界与矩形裁剪快路径取整一致（保留 x ∈ [ceil(l), floor(r)]，含右/下边界），
    /// 供脏区裁剪重绘在保留上帧缓冲的前提下，先把裁剪区恢复到与 `begin` 后一致的
    /// 零基底，避免半透明内容与上帧像素双重混合（rect 为逻辑 dp，内部乘 scale）。
    auto clear_rect(const Rect &r) -> void;

    /// @brief 用纯色描边矩形边框（1px，受裁剪影响）。
    auto draw_rect(const Rect &r, Color c) -> void;

    /// @brief 绘制抗锯齿线段（圆帽）：从 a 到 b，线宽 `width`（逻辑 dp，内部乘 scale）。
    /// 基于点到线段距离 SDF 的 1px 羽化覆盖度，供勾号✓/斜线/简单矢量图形使用（受裁剪与全局透明度影响）。
    auto draw_line(Point a, Point b, float width, Color c) -> void;

    /// @brief 填充圆角矩形（抗锯齿）：等价于 push_clip_rounded + fill_rect + pop_clip 的便捷组合；
    /// radius <= 0 退化为 fill_rect。控件绘制常用（Checkbox/Button 背景）。
    auto fill_rounded_rect(const Rect &r, float radius, Color c) -> void;

    /// @brief 描边圆角矩形边框（抗锯齿，向内描边）：沿圆角矩形轮廓向内绘制 `thickness` dp 宽的边框带。
    /// radius = min(w,h)/2 时即圆环（RadioButton 外圈）；thickness <= 0 无操作。
    auto draw_rounded_border(const Rect &r, float radius, float thickness, Color c) -> void;

    /// @brief 绘制文本：委托 FontEngine（真实字体渲染；无 GDI/字体时回退内置位图字体）。
    auto draw_text(const Rect &r, const std::string &s, const Font &f, Color c) -> void;

    /// @brief 绘制文本（含排版 opts：letter/word spacing & italic）；抗锯齿策略取 FontEngine 进程级 `text_aa_mode()`。
    ///        与 `Text` 的 `letter_spacing` / `word_spacing` / `font_style=Italic` 联动，度量/光标/命中一致。
    auto draw_text(const Rect &r, const std::string &s, const Font &f, Color c, const render::TextLayoutOpts &opts)
        -> void;

    /// @brief 绘制文本（**显式覆盖抗锯齿策略**，含排版 opts）。
    auto draw_text(const Rect &r, const std::string &s, const Font &f, Color c, render::TextAAMode aa_mode,
                   const render::TextLayoutOpts &opts) -> void;

    /// @brief 把带 alpha 的源色按源覆盖混合到单像素（受裁剪约束）；供字体/半透明使用。
    auto blend_pixel(int x, int y, Color c) -> void;

    /// @brief 真 ClearType 子像素合成：以 `c` 着色，按 R/G/B 三个子像素各自的覆盖度
    ///        `cr`/`cg`/`cb`（0..255）做逐通道源覆盖混合。用于 FreeType `FT_RENDER_MODE_LCD`
    ///        输出的 3× 水平 RGB 覆盖度位图，产生屏幕最佳的子像素锐利文本（非灰度降级）。
    auto blend_subpixel(int x, int y, Color c, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) -> void;

    /// @brief 批量子像素合成（性能优化）：把同一行的 N 个相邻像素一次性合成，裁剪判定只做一次、
    ///        内联 gamma-correct 混合，消除逐像素函数调用 + 裁剪栈遍历 + SIMD 分发开销。
    ///        `src` 为长度 N（Gray）或 3N（LCD 三通道 RGB 子像素）的覆盖度缓冲；某像素三通道
    ///        全为 0 时自动跳过。`src_alpha` 为源色 alpha 归一因子（Gray 路径须传入 c.a/255）。
    ///        圆角裁剪或录制态自动回退到逐像素 blend_subpixel。
    auto blend_subpixel_span(int x0, int y, Color c, const std::uint8_t *src, int n, bool lcd, float src_alpha = 1.0F)
        -> void;

    /// @brief 把带 alpha 的源色按源覆盖混合到矩形；等价于 fill_rect（保留以清晰表达混合语义）。
    auto blend_rect(const Rect &r, Color c) -> void;

    /// @brief 绘制解码后的图像到目标矩形（RGBA8，alpha 混合，受裁剪约束）。
    /// 图像按目标矩形尺寸做最近邻缩放（保持像素准确性；不追求平滑）。
    auto draw_image(const Image &img, const Rect &dest) -> void;

    /// @brief 线性渐变填充：在 area 内沿 (start→end) 方向插值 colors/stops 色标。
    /// stops 归一化 [0,1]，与 colors 等长；start/end 为 area 内逻辑 dp 坐标。
    auto draw_linear_gradient(const Rect &area, Point start, Point end, const std::vector<Color> &colors,
                              const std::vector<float> &stops) -> void;

    /// @brief 径向渐变填充：在 area 内以 center 为圆心、radius 为半径插值 colors/stops。
    auto draw_radial_gradient(const Rect &area, Point center, float radius, const std::vector<Color> &colors,
                              const std::vector<float> &stops) -> void;

    /// @brief 投影阴影：在 shape 偏移 (offset_x, offset_y) 处绘制模糊矩形阴影。
    /// blur_radius 控制模糊程度（0=硬边），color 为阴影色（含 alpha）。
    auto draw_shadow(const Rect &shape, float offset_x, float offset_y, float blur_radius, Color color) -> void;

    /// @brief 就地模糊帧缓冲中的矩形区域（分离式两遍 box blur ≈ 高斯）。
    /// 用于 `Modifier::blur`（内容模糊）与 `Modifier::backdrop_filter`（毛玻璃：
    /// 绘内容前先模糊背后区域）。radius 为逻辑 dp（内部乘 scale）；<=0 无操作。
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
    auto shift_pixels(float dy) -> void;

    /// @brief 把 `region` 内已绘制像素与 `tint` 按 `mode` 混合（就地覆盖）。
    /// 用于 `Modifier::blend_mode`；应在内容绘制完成后调用。`strength`（0..1）控制强度，
    /// 0 不改变、1 完全按模式混合。`BlendMode` 定义见 `aurora/render/blend.h`。
    auto blend_region(const Rect &region, BlendMode mode, Color tint, float strength = 1.0F) -> void;

    /// @brief 把 `region` 内像素 RGB 乘以渐变遮罩因子（0..1），形成淡出 / 聚焦。
    /// 用于 `Modifier::shader_mask`；`strength`（0..1）控制强度，0 不改变、1 完全遮罩。
    auto mask_region(const Rect &region, ShaderMaskKind kind, float strength = 1.0F) -> void;

    /**
     * @brief 把已渲染的离屏子树（源缓冲）按仿射矩阵合成回本缓冲。
     *
     * 用于修饰节点的旋转 / 缩放 / 任意仿射变换：子树先渲染到离屏 Painter，
     * 再经 `matrix` 映射到本缓冲（矩阵为逻辑 dp 空间，内部乘 `m_scale` 到物理像素）。
     * 合成尊重本缓冲的裁剪栈与 `global_alpha`（透明度由此统一生效）。
     *
     * @param src   源 Painter（已 begin，逻辑尺寸 = 其 begin 尺寸，物理 = *m_scale）。
     * @param matrix 逻辑 dp 空间仿射矩阵（含平移/旋转/缩放，建议绕内容中心构造）。
     */
    auto composite(const Painter &src, const Matrix2D &matrix) -> void;
    /// @brief 把离屏位图（Image）按矩阵合成到当前画布（回放 Composite 命令用，含源缩放）。
    auto composite(const Image &src, const Matrix2D &matrix, float src_scale) -> void;
    /// @brief 导出当前画布像素为 Image（离屏合成录制时捕获源缓冲）。
    [[nodiscard]] auto to_image() const -> Image;

    /// @brief 离屏合成核心：把源像素（设备分辨率 spix，尺寸 sw×sh，逻辑比例 sscale）按 matrix 贴回当前画布。
    auto composite_pixels(const std::uint8_t *spix, int sw, int sh, float sscale, const Matrix2D &matrix) -> void;

    /// @brief 压入裁剪矩形（与当前裁剪取交集），后续绘制仅保留交集内像素。
    auto push_clip(const Rect &r) -> void;

    /// @brief 压入圆角矩形裁剪（默认抗锯齿；与当前裁剪取交集）。
    auto push_clip_rounded(const Rect &r, float radius, bool anti_alias = true) -> void;

    /// @brief 弹出最近一次 pushClip / pushClipRounded 设置的裁剪。
    auto pop_clip() -> void;

    /// @brief 当前是否存在裁剪（裁剪栈非空）。
    [[nodiscard]] auto has_clip() const -> bool;

    /// @brief 当前有效裁剪矩形（各层矩形裁剪交集）的逻辑 dp 全局坐标。
    ///        无裁剪时返回整块画布；圆角裁剪退化为其外接矩形（保守，保证不误剔除）。
    [[nodiscard]] auto clip_bounds() const -> Rect;

    // ---- Display List 录制 / 回放（AURORA_DISPLAY_LIST）----
    /// @brief 进入录制模式并将绘制命令写入给定 DisplayList（先清空）。绘制原语在录制模式下
    ///        仅记录命令、不直接上屏；嵌套调用以栈管理（子控件缓存 DL 压平并入父 DL）。
    auto record(DisplayList &dl) -> void;
    /// @brief 退出当前录制层级；栈空时回到 Direct（上屏）模式。
    auto stop() -> void;
    /// @brief 是否处于录制模式（录制栈非空）。
    [[nodiscard]] auto is_recording() const -> bool { return !recording_stack_.empty(); }

    /// @brief Window 脏区裁剪绘制（partial clip）期间抑制 Display List 录制/回放：
    ///        partial clip 下子树 paint 只画 clip 内子节点，若此时录制 DL 会丢失 clip 外子节点命令，
    ///        后续 full 帧 replay 该 DL 时会永久丢失 clip 外子节点（与整帧重绘逐位不一致）。
    auto set_skip_dl_record(bool skip) -> void { skip_dl_record_ = skip; }
    [[nodiscard]] auto skip_dl_record() const -> bool { return skip_dl_record_; }

    /// @brief 标记当前及所有外层录制层级为「含动态内容」：录制这些层级的祖先控件不应
    ///        缓存其 Display List（内容每帧变化或绘制含副作用）。由不可缓存控件在录制模式下调用。
    auto mark_recording_dynamic() -> void;
    /// @brief 当前录制层级是否含有动态内容（用于决定本控件 DL 是否可安全缓存）。
    [[nodiscard]] auto recording_is_dynamic() const -> bool {
        return !rec_dynamic_.empty() && (rec_dynamic_.back() != 0);
    }

  private:
    struct ClipRegion {
        Rect rect;
        bool rounded = false;
        float radius = 0.0F;
        bool anti_alias = true;  ///< 圆角是否抗锯齿（SDF 覆盖度）

        /// @brief 计算点 (x,y) 处裁剪覆盖度（0=完全裁剪，1=完全保留；圆角边界 0..1 抗锯齿）。
        [[nodiscard]] auto coverage(float x, float y) const -> float;
    };

    auto set_pixel(int x, int y, Color c) -> void;

    /// @brief 逐像素原语的迭代范围收缩：与裁剪栈各矩形求交（物理像素，含右/下边界语义
    ///        与 coverage 的 contains 像素级等价）；返回 false 表示交集为空（可直接早退）。
    ///        只剔除必被裁剪丢弃的迭代，结果逐位不变（部分脏区重绘性能关键）。
    [[nodiscard]] auto shrink_to_clips(int &x0, int &y0, int &x1, int &y1) const -> bool;

    /// @brief 给定扫描线 y，计算圆角裁剪 SDF ≤ t 的全覆写 x 范围（半开区间）。
    [[nodiscard]] static auto rounded_full_x_range(const ClipRegion &cr, int y, float t) -> std::pair<int, int>;

    /// @brief fill_rect 的「行级快速路径」：全局不透明且裁剪栈全为非圆角矩形时，
    /// 把裁剪收缩进边界后整行写入（覆写/内联 source-over）。命中返回 true（已绘制），
    /// 否则返回 false 交由慢路径处理（x0..y1 保持不变）。
    auto fill_rect_fast_path(int &x0, int &y0, int &x1, int &y1, Color c) -> bool;

    /// @brief fill_rect 的「慢路径」：圆角裁剪（SDF 覆盖度）/ 全局透明度 <1 时逐像素处理。
    auto fill_rect_slow_path(int x0, int y0, int x1, int y1, Color c) -> void;

    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0F;  ///< device pixel ratio（dp → 物理像素）
    std::vector<std::uint8_t> pixels_;
    std::vector<ClipRegion> clip_stack_;  ///< 裁剪栈（矩形 + 圆角，滚动/圆角容器用）
    bool has_rounded_clip_ = false;  ///< 裁剪栈中是否存在圆角裁剪（blend_pixel 快速路径判定）
    double global_alpha_ = 1.0;  ///< 全局绘制透明度（set_alpha 设置）
    bool skip_dl_record_ =
        false;  ///< Window 脏区裁剪绘制期间设为 true，抑制 DL 录制/回放（partial clip 下录制会丢失 clip 外子节点）

    // ---- Display List 录制栈（AURORA_DISPLAY_LIST）----
    /// @brief 把一条文本绘制命令录入当前录制目标（变长字符串入池）。
    auto record_text_cmd(const Rect &r, const std::string &s, const Font &f, Color c, render::TextAAMode aa,
                         const render::TextLayoutOpts &opts) -> void;
    std::vector<DisplayList *> recording_stack_;  ///< 录制目标栈；非空即录制模式
    std::vector<char> rec_dynamic_;  ///< 与录制栈平行的「含动态内容」标记（mark_recording_dynamic 置全部）
};

/// @brief [性能排查] 返回各光栅原语耗时累加器（毫秒）的 JSON 串；读取后清零。
/// `per_frame_divisor`（默认 1）用于把「累计窗口总量」折算为「每帧均值」（通常传 FPS）。
/// 仅供 DEBUG 排查，不用于生产。
auto paint_primitive_timing_json(double per_frame_divisor = 1.0) -> std::string;

/// @brief [性能排查] 返回累加器中「整段 widget 绘制」(scene) 的当前值（毫秒），不重置。
/// 用于让调用方判断上一帧是否真实绘制（scene>~1ms 即非 idle），以便保留有代表性的帧分解。
auto paint_timing_scene_last() -> double;

/// @brief [性能排查] 返回「上一完成绘制帧」逐原语耗时（毫秒）的 JSON 串，不重置。
/// divisor=1，即该帧真实耗时；用于每秒打印代表性绘制帧分解，避免 idle 帧零值污染与整秒/FPS 折算误差。
auto paint_primitive_timing_last_json() -> std::string;

}  // namespace aurora

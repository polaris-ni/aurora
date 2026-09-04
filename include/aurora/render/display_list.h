#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/transform.h"
#include "aurora/core/types.h"
#include "aurora/render/blend.h"
#include "aurora/render/text_aa_mode.h"

namespace aurora {

class Painter;  // 前置声明：replay 实现（display_list.cpp）依赖 Painter 完整类型

/// @brief 录制-回放绘制命令类型。覆盖 Painter 全部「上屏」原语（离屏合成 composite 不录制，
///        由 Widget 的 cache_layer / 非恒等 Transform 离屏路径独立处理）。
enum class CmdKind : std::uint8_t {
    FillRect,
    ClearRect,
    DrawRect,
    DrawLine,  ///< 抗锯齿线段（pt0→pt1，f0=线宽）
    RoundedBorder,  ///< 圆角矩形描边（f0=圆角半径，f1=边框宽）
    DrawText,
    DrawImage,
    LinearGradient,
    RadialGradient,
    Shadow,
    BlurRegion,
    BlendRegion,
    MaskRegion,
    PushClip,
    PushClipRounded,
    PopClip,
    Composite,  ///< 离屏合成（cache_layer / 非恒等 Transform）：录制时捕获离屏像素缓冲
    SetAlpha,
};

/// @brief 单条绘制命令。变长数据（文本 / 渐变色标 / 渐变停靠）经索引引用 DisplayList 的数据池，
///        避免每条命令内嵌大对象（图像拷贝共享像素缓冲句柄，开销可忽略）。
struct DrawCmd {
    CmdKind kind = CmdKind::FillRect;
    Rect bounds{};
    Color color;
    Point pt0{};  ///< 渐变起点 / 圆心 / 阴影形状（与 bounds 同义时忽略）
    Point pt1{};  ///< 渐变终点
    float f0 = 0, f1 = 0, f2 = 0;  ///< 半径 / 模糊 / 强度 / 偏移（按命令语义取用）
    bool rounded_aa = true;  ///< PushClipRounded 抗锯齿标志
    BlendMode blend_mode = BlendMode::Normal;
    ShaderMaskKind mask_kind = ShaderMaskKind::LinearFade;
    render::TextAAMode aa_mode = render::TextAAMode::Supersample;
    double alpha = 1.0;  ///< SetAlpha 目标不透明度
    int str_idx = -1;  ///< 文本字符串在 m_str_pool 的索引
    int col_idx = -1;  ///< 渐变颜色数组在 m_color_pool 的索引
    int flt_idx = -1;  ///< 渐变停靠数组在 m_float_pool 的索引
    // 文本排版标量（拆出存储以避免直接持有 TextLayoutOpts，减少头耦合）
    float text_ls = 0, text_ws = 0;
    bool text_italic = false;
    int font_idx = -1;  ///< 文本字体在 m_font_pool 的索引
    int image_idx = -1;  ///< 图像在 m_image_pool 的索引
    // Composite（离屏合成）专用
    int matrix_idx = -1;  ///< 离屏缓冲变换矩阵在 m_matrix_pool 的索引
    float composite_scale = 1.0F;  ///< 离屏缓冲的设备像素缩放（源 Painter 的 scale）
};

/// @brief Display List：绘制命令缓冲 + 变长数据池，支持录制（由 Painter 驱动）与回放。
///
/// 回放语义：Direct 模式下调用对应公共绘制原语执行到画布；Recording 模式下将命令追加到当前
/// 录制目标，从而把子控件缓存的 DL「压平」并入父控件 DL（命中时父级整树一次 replay 即可）。
class DisplayList {
  public:
    auto clear() -> void {
        cmds_.clear();
        str_pool_.clear();
        color_pool_.clear();
        float_pool_.clear();
        font_pool_.clear();
        image_pool_.clear();
        matrix_pool_.clear();
    }
    [[nodiscard]] auto empty() const -> bool { return cmds_.empty(); }

    /// @brief 已录制的命令条数（性能诊断 / 计数门槛用：反映录制规模，与回放开销正相关）。
    [[nodiscard]] auto cmd_count() const -> std::size_t { return cmds_.size(); }

    auto push_cmd(const DrawCmd &cmd) -> void { cmds_.push_back(cmd); }

    /// @brief 变长数据入池，返回索引（供 DrawCmd 引用）。
    auto add_string(const std::string &s) -> int {
        str_pool_.push_back(s);
        return static_cast<int>(str_pool_.size()) - 1;
    }
    auto add_colors(const std::vector<Color> &v) -> int {
        color_pool_.push_back(v);
        return static_cast<int>(color_pool_.size()) - 1;
    }
    auto add_floats(const std::vector<float> &v) -> int {
        float_pool_.push_back(v);
        return static_cast<int>(float_pool_.size()) - 1;
    }
    auto add_font(const Font &f) -> int {
        font_pool_.push_back(f);
        return static_cast<int>(font_pool_.size()) - 1;
    }
    auto add_image(const Image &img) -> int {
        image_pool_.push_back(img);
        return static_cast<int>(image_pool_.size()) - 1;
    }
    auto add_matrix(const Matrix2D &m) -> int {
        matrix_pool_.push_back(m);
        return static_cast<int>(matrix_pool_.size()) - 1;
    }

    /// @brief 回放整条命令流。
    auto replay(Painter &p) const -> void;

  private:
    std::vector<DrawCmd> cmds_;
    std::vector<std::string> str_pool_;
    std::vector<std::vector<Color>> color_pool_;
    std::vector<std::vector<float>> float_pool_;
    std::vector<Font> font_pool_;
    std::vector<Image> image_pool_;
    std::vector<Matrix2D> matrix_pool_;
};

}  // namespace aurora

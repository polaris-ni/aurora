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

namespace aurora::rhi {
class RhiBackend;  // 前置声明（完整定义见 render/rhi/rhi_backend.h）：回放目标抽象
}  // namespace aurora::rhi

namespace aurora {

class Painter;  // 前置声明：replay 实现（display_list.cpp）依赖 Painter 完整类型

/// @brief 录制-回放绘制命令类型。覆盖 Painter 全部「上屏」原语（离屏合成 composite 不录制，
///        由 Widget 的 cache_layer / 非恒等 Transform 离屏路径独立处理）。
enum class CmdKind : std::uint8_t {
    FillRect,  ///< 用当前颜色填充 bounds 实心矩形
    ClearRect,  ///< 清空 bounds 区域（写回透明）
    DrawRect,  ///< 以边框色描边 bounds 矩形
    DrawLine,  ///< 抗锯齿线段（pt0→pt1，f0=线宽）
    RoundedBorder,  ///< 圆角矩形描边（f0=圆角半径，f1=边框宽）
    DrawText,  ///< 绘制文本：str_idx 取字串、font_idx 取字体、按排版标量布局
    DrawImage,  ///< 绘制图像：image_idx 取像素，落在 bounds
    LinearGradient,  ///< 线性渐变：pt0→pt1 轴向，col_idx/flt_idx 取色标与停靠
    RadialGradient,  ///< 径向渐变：pt0 圆心、f0 半径，col_idx/flt_idx 取色标与停靠
    Shadow,  ///< 投影：按形状 + f0/f1/f2/f3（模糊/偏移/强度）绘制
    BlurRegion,  ///< 对 bounds 区域做高斯模糊（f0=半径）
    BlendRegion,  ///< 对 bounds 区域按 blend_mode 混合着色
    MaskRegion,  ///< 对 bounds 区域施加 mask_kind 着色器遮罩
    PushClip,  ///< 压入矩形裁剪区（bounds）
    PushClipRounded,  ///< 压入圆角矩形裁剪区（f0=圆角半径，rounded_aa 控抗锯齿）
    PopClip,  ///< 弹出最近裁剪区
    Composite,  ///< 离屏合成（cache_layer / 非恒等 Transform）：录制时捕获离屏像素缓冲
    SetAlpha,  ///< 设置后续命令的整体不透明度（alpha）
    Polyline,  ///< 抗锯齿多段线：点集经 `pt_idx` 引用 point_pool_，f0 = 线宽（逻辑 dp）
    Sector,  ///< 抗锯齿扇形 / 环扇：pt0 = 圆心，f0 = 外半径，f1 = 内半径，f2 = 起角，f3 = 止角
    BeginLayer,  ///< GPU 层缓存开始：后续命令重定向到常驻层纹理（aux_key = 层键，bounds = 层尺寸）
    EndLayer,  ///< GPU 层缓存结束：层纹理定稿，恢复重定向前的目标与状态
    DrawLayer,  ///< 层合成：aux_key = 层键，matrix_idx = 放置矩阵，composite_scale = 层录制缩放
};

/// @brief 单条绘制命令。变长数据（文本 / 渐变色标 / 渐变停靠）经索引引用 DisplayList 的数据池，
///        避免每条命令内嵌大对象（图像拷贝共享像素缓冲句柄，开销可忽略）。
struct DrawCmd {
    CmdKind kind = CmdKind::FillRect;  ///< 命令类型；决定其余字段如何取用
    Rect bounds{};  ///< 命令的作用包围盒（绘制区域 / 裁剪区）
    Color color;  ///< 主色（描边色 / 阴影色 / 清屏色，按 kind 取用）
    Point pt0{};  ///< 渐变起点 / 圆心 / 阴影形状（与 bounds 同义时忽略）
    Point pt1{};  ///< 渐变终点
    float f0 = 0;  ///< 通用数值槽 0：描边宽度 / 外半径 / 偏移 x / 强度等（按 kind 取用）
    float f1 = 0;  ///< 通用数值槽 1：内半径 / 描边厚度 / 偏移 y 等（按 kind 取用）
    float f2 = 0;  ///< 通用数值槽 2：起始角 a0 / 强度等（按 kind 取用）
    float f3 = 0;  ///< 通用数值槽 3：终止角 a1 等（按 kind 取用）
    bool rounded_aa = true;  ///< PushClipRounded 抗锯齿标志
    BlendMode blend_mode = BlendMode::Normal;  ///< BlendRegion 的混合模式
    ShaderMaskKind mask_kind = ShaderMaskKind::LinearFade;  ///< MaskRegion 的遮罩类型
    render::TextAAMode aa_mode = render::TextAAMode::Supersample;  ///< DrawText 的抗锯齿模式
    double alpha = 1.0;  ///< SetAlpha 目标不透明度
    int str_idx = -1;  ///< 文本字符串在 str_pool_ 的索引
    int col_idx = -1;  ///< 渐变颜色数组在 color_pool_ 的索引
    int flt_idx = -1;  ///< 渐变停靠数组在 float_pool_ 的索引
    float text_ls = 0;  ///< 文本字距（DrawText 排版标量，拆出存储以降低头耦合）
    float text_ws = 0;  ///< 文本词距（DrawText 排版标量，拆出存储以降低头耦合）
    bool text_italic = false;  ///< DrawText 是否以斜体绘制
    int font_idx = -1;  ///< 文本字体在 font_pool_ 的索引
    int image_idx = -1;  ///< 图像在 image_pool_ 的索引
    int pt_idx = -1;  ///< Polyline 点集在 point_pool_ 的索引
    int matrix_idx = -1;  ///< 离屏缓冲变换矩阵在 matrix_pool_ 的索引
    float composite_scale = 1.0F;  ///< 离屏缓冲的设备像素缩放（源 Painter 的 scale）
    std::uint64_t aux_key = 0;  ///< GPU 层缓存层键（进程内唯一；0 保留）
};

/// @brief Display List：绘制命令缓冲 + 变长数据池，支持录制（由 Painter 驱动）与回放。
///
/// 回放语义：Direct 模式下调用对应公共绘制原语执行到画布；Recording 模式下将命令追加到当前
/// 录制目标，从而把子控件缓存的 DL「压平」并入父控件 DL（命中时父级整树一次 replay 即可）。
class DisplayList {
  public:
    /// @brief 清空全部命令与数据池，回到空列表状态。
    auto clear() -> void {
        cmds_.clear();
        str_pool_.clear();
        color_pool_.clear();
        float_pool_.clear();
        font_pool_.clear();
        image_pool_.clear();
        matrix_pool_.clear();
        point_pool_.clear();
    }

    /// @brief 判断当前是否未录制任何命令。
    /// @return 无命令时为 true。
    [[nodiscard]] auto empty() const -> bool { return cmds_.empty(); }

    /// @brief 已录制的命令条数（性能诊断 / 计数门槛用：反映录制规模，与回放开销正相关）。
    /// @return 命令缓冲区中的条目数。
    [[nodiscard]] auto cmd_count() const -> std::size_t { return cmds_.size(); }

    /// @brief 追加一条已构造好的绘制命令。
    /// @param cmd 待入队的命令（按值拷贝进缓冲区）。
    auto push_cmd(const DrawCmd &cmd) -> void { cmds_.push_back(cmd); }

    /// @brief 变长数据入池，返回索引（供 DrawCmd 引用）。
    /// @param s 待入池的文本字符串。
    /// @return 该字符串在 str_pool_ 中的下标。
    auto add_string(const std::string &s) -> int {
        str_pool_.push_back(s);
        return static_cast<int>(str_pool_.size()) - 1;
    }

    /// @brief 渐变颜色数组入池。
    /// @param v 待入池的颜色数组。
    /// @return 该数组在 color_pool_ 中的下标。
    auto add_colors(const std::vector<Color> &v) -> int {
        color_pool_.push_back(v);
        return static_cast<int>(color_pool_.size()) - 1;
    }

    /// @brief 渐变停靠数组入池。
    /// @param v 待入池的浮点停靠数组。
    /// @return 该数组在 float_pool_ 中的下标。
    auto add_floats(const std::vector<float> &v) -> int {
        float_pool_.push_back(v);
        return static_cast<int>(float_pool_.size()) - 1;
    }

    /// @brief 文本字体入池。
    /// @param f 待入池的字体描述。
    /// @return 该字体在 font_pool_ 中的下标。
    auto add_font(const Font &f) -> int {
        font_pool_.push_back(f);
        return static_cast<int>(font_pool_.size()) - 1;
    }

    /// @brief 图像入池，返回索引（供 DrawCmd 引用）。
    /// @param img 待入池的图像（共享像素缓冲句柄）。
    /// @return 该图像在 image_pool_ 中的下标。
    auto add_image(const Image &img) -> int {
        // 预热源对象的内容摘要（const 下经 mutable 缓存落回源）：GPU 纹理缓存经
        // `Image::content_hash()` 寻址，预热后逐帧的池拷贝携带有效缓存，免除每帧全量哈希。
        // 流式图像（stream_key != 0）不走摘要寻址路径，跳过预热（视频逐帧更新免全帧哈希）。
        if (img.stream_key == 0) {
            (void)img.content_hash();
        }
        image_pool_.push_back(img);
        return static_cast<int>(image_pool_.size()) - 1;
    }

    /// @brief 离屏变换矩阵入池。
    /// @param m 待入池的二维矩阵。
    /// @return 该矩阵在 matrix_pool_ 中的下标。
    auto add_matrix(const Matrix2D &m) -> int {
        matrix_pool_.push_back(m);
        return static_cast<int>(matrix_pool_.size()) - 1;
    }

    /// @brief 多段线点集入池。
    /// @param v 待入池的点集。
    /// @return 该点集在 point_pool_ 中的下标。
    auto add_points(const std::vector<Point> &v) -> int {
        point_pool_.push_back(v);
        return static_cast<int>(point_pool_.size()) - 1;
    }

    /// @brief 按池下标只读取文本字符串。下标由录制时生成，回放侧据此解析（见 `rhi::CmdData`）；
    ///        调用方**不得**自行构造下标。
    /// @param idx str_pool_ 下标。
    /// @return 对应字符串的常量引用。
    [[nodiscard]] auto string_at(int idx) const -> const std::string & {
        return str_pool_[static_cast<std::size_t>(idx)];
    }

    /// @brief 按池下标只读取渐变颜色数组。下标由录制时生成，调用方不得自行构造。
    /// @param idx color_pool_ 下标。
    /// @return 对应颜色数组的常量引用。
    [[nodiscard]] auto colors_at(int idx) const -> const std::vector<Color> & {
        return color_pool_[static_cast<std::size_t>(idx)];
    }

    /// @brief 按池下标只读取渐变停靠数组。下标由录制时生成，调用方不得自行构造。
    /// @param idx float_pool_ 下标。
    /// @return 对应浮点数组的常量引用。
    [[nodiscard]] auto floats_at(int idx) const -> const std::vector<float> & {
        return float_pool_[static_cast<std::size_t>(idx)];
    }

    /// @brief 按池下标只读取字体。下标由录制时生成，调用方不得自行构造。
    /// @param idx font_pool_ 下标。
    /// @return 对应字体的常量引用。
    [[nodiscard]] auto font_at(int idx) const -> const Font & { return font_pool_[static_cast<std::size_t>(idx)]; }

    /// @brief 按池下标只读取图像。下标由录制时生成，调用方不得自行构造。
    /// @param idx image_pool_ 下标。
    /// @return 对应图像的常量引用。
    [[nodiscard]] auto image_at(int idx) const -> const Image & { return image_pool_[static_cast<std::size_t>(idx)]; }

    /// @brief 按池下标只读取离屏变换矩阵。下标由录制时生成，调用方不得自行构造。
    /// @param idx matrix_pool_ 下标。
    /// @return 对应矩阵的常量引用。
    [[nodiscard]] auto matrix_at(int idx) const -> const Matrix2D & {
        return matrix_pool_[static_cast<std::size_t>(idx)];
    }

    /// @brief 按池下标只读取多段线点集。下标由录制时生成，调用方不得自行构造。
    /// @param idx point_pool_ 下标。
    /// @return 对应点集的常量引用。
    [[nodiscard]] auto points_at(int idx) const -> const std::vector<Point> & {
        return point_pool_[static_cast<std::size_t>(idx)];
    }

    /// @brief 回放整条命令流到 RHI 后端（**唯一实现**；命令语义解释在各后端内，见
    ///        `rhi::SoftwareRhi::submit`）。本类只负责「遍历命令 + 把池下标解析为 `rhi::CmdData`」。
    /// @param backend 接收提交命令与数据的 RHI 后端引用。
    auto replay(rhi::RhiBackend &backend) const -> void;

    /// @brief 回放整条命令流到软件 `Painter`。等价于回放到一个临时包裹该 `Painter` 的
    ///        `rhi::SoftwareRhi`（逐条转发回同一批原语，像素输出逐位不变）。
    /// @param p 目标软件画布。
    auto replay(Painter &p) const -> void;

  private:
    std::vector<DrawCmd> cmds_;
    std::vector<std::string> str_pool_;
    std::vector<std::vector<Color>> color_pool_;
    std::vector<std::vector<float>> float_pool_;
    std::vector<Font> font_pool_;
    std::vector<Image> image_pool_;
    std::vector<Matrix2D> matrix_pool_;
    std::vector<std::vector<Point>> point_pool_;
};

}  // namespace aurora

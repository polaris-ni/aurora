#pragma once

/// @brief 绘制修饰节点（Paint 切片）：Background / GradientBackground / ShadowNode /
/// BlendNode / ShaderMaskNode / CacheLayerNode / Border / Clip / ClipRounded / OpacityNode / BlurNode。
/// 本文件为 modifier.h 的子切片；消费者通常直接 `#include` "aurora/modifier/modifier.h"。
/// @file modifier_paint.h

#include <algorithm>

#include "aurora/core/color.h"
#include "aurora/modifier/modifier_base.h"
#include "aurora/render/blend.h"

namespace aurora {

/// @brief 背景色修饰：不影响尺寸，绘制时填充矩形（在内容之下）。
/// 可选 `radius` 实现圆角背景（硬遮罩，配合 Painter 圆角裁剪）。
class Background : public ModifierNode {
  public:
    /// @brief 构造纯色背景修饰节点。
    /// @param color 填充颜色。
    /// @param radius 圆角半径（dp，负值钳为 0；0 = 直角背景）。
    Background(Color color, float radius = 0.0F) : color_(color), radius_(radius < 0.0F ? 0.0F : radius) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（背景只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::Background。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Background; }

    /// @brief 布局：背景不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加背景时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取填充颜色。
    /// @return 构造时传入的背景色。
    [[nodiscard]] auto color() const -> Color { return color_; }
    /// @brief 读取圆角半径。
    /// @return 圆角半径（dp，已钳为非负）；0 表示直角背景。
    [[nodiscard]] auto corner_radius() const -> float { return radius_; }

  private:
    Color color_;
    float radius_ = 0.0F;
};

/// @brief 渐变背景修饰（Paint 切片）：不影响尺寸，绘制时以线性/径向渐变填充矩形。
class GradientBackground : public ModifierNode {
  public:
    enum class Type : std::uint8_t { Linear, Radial };

    /// @brief 线性渐变背景构造：沿角度决定的渐变轴按色标插值填充矩形。
    /// @param colors 色标颜色（沿渐变方向排列，与 stops 等长）。
    /// @param stops 归一化色标位置 [0,1]（与 colors 一一对应）。
    /// @param angle_deg 渐变轴角度（度，0 = 从左到右，90 = 从上到下）。
    GradientBackground(std::vector<Color> colors, std::vector<float> stops, float angle_deg)
        : type_(Type::Linear), colors_(std::move(colors)), stops_(std::move(stops)), angle_(angle_deg) {}

    /// @brief 径向渐变背景构造：以盒中心为圆心、半对角线为半径按色标插值填充。
    /// @param colors 色标颜色（由内向外排列，与 stops 等长）。
    /// @param stops 归一化色标位置 [0,1]（与 colors 一一对应）。
    GradientBackground(std::vector<Color> colors, std::vector<float> stops)
        : type_(Type::Radial), colors_(std::move(colors)), stops_(std::move(stops)) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（渐变背景只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::GradientBackground。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::GradientBackground; }

    /// @brief 布局：渐变背景不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加渐变时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取渐变类型。
    /// @return Type::Linear（线性）或 Type::Radial（径向），由所选构造函数决定。
    [[nodiscard]] auto type() const -> Type { return type_; }
    /// @brief 读取色标颜色列表。
    /// @return 构造时传入的颜色（渐变方向序）。
    [[nodiscard]] auto colors() const -> const std::vector<Color> & { return colors_; }
    /// @brief 读取归一化色标位置列表。
    /// @return 与 colors() 等长的 [0,1] 位置序列。
    [[nodiscard]] auto stops() const -> const std::vector<float> & { return stops_; }
    /// @brief 读取线性渐变轴角度。
    /// @return 角度（度，0 = 从左到右）；径向节点未提供该参数，恒为默认值 0。
    [[nodiscard]] auto angle() const -> float { return angle_; }

  private:
    Type type_;
    std::vector<Color> colors_;
    std::vector<float> stops_;
    float angle_ = 0.0F;  ///< 线性渐变角度（度，0=从左到右）
};

/// @brief 阴影修饰（Paint 切片）：在内容之下绘制投影阴影。
class ShadowNode : public ModifierNode {
  public:
    /// @brief 构造阴影修饰节点。
    /// @param offset_x 投影水平偏移（dp，正值为向右）。
    /// @param offset_y 投影垂直偏移（dp，正值为向下）。
    /// @param blur 阴影模糊半径（dp，负值钳为 0）。
    /// @param color 阴影颜色。
    ShadowNode(float offset_x, float offset_y, float blur, Color color)
        : offset_x_(offset_x), offset_y_(offset_y), blur_(blur < 0.0F ? 0.0F : blur), color_(color) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（阴影只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::Shadow。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Shadow; }

    /// @brief 布局：阴影不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加阴影时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取投影水平偏移。
    /// @return 偏移量（dp）。
    [[nodiscard]] auto offset_x() const -> float { return offset_x_; }
    /// @brief 读取投影垂直偏移。
    /// @return 偏移量（dp）。
    [[nodiscard]] auto offset_y() const -> float { return offset_y_; }
    /// @brief 读取阴影模糊半径。
    /// @return 半径（dp，已钳为非负）。
    [[nodiscard]] auto blur() const -> float { return blur_; }
    /// @brief 读取阴影颜色。
    /// @return 构造时传入的投影色。
    [[nodiscard]] auto color() const -> Color { return color_; }

  private:
    float offset_x_ = 0.0F;
    float offset_y_ = 0.0F;
    float blur_ = 0.0F;
    Color color_;
};

/// @brief 混合修饰（Paint 切片）：内容绘制完成后，把内容盒像素与 `tint` 按 `mode` 混合。
class BlendNode : public ModifierNode {
  public:
    /// @brief 构造混合修饰节点。
    /// @param mode 逐通道混合模式（见 `BlendMode`）。
    /// @param tint 与内容盒像素混合的纯色。
    /// @param strength 混合强度 [0,1]（越界钳制）。
    BlendNode(BlendMode mode, Color tint, float strength)
        : mode_(mode), tint_(tint), strength_(std::clamp(strength, 0.0F, 1.0F)) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（混合只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::Blend。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Blend; }

    /// @brief 布局：混合不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加混合时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取混合模式。
    /// @return 构造时传入的模式。
    [[nodiscard]] auto mode() const -> BlendMode { return mode_; }
    /// @brief 读取混合纯色。
    /// @return 构造时传入的 tint。
    [[nodiscard]] auto tint() const -> Color { return tint_; }
    /// @brief 读取混合强度。
    /// @return 强度 [0,1]（构造时已钳制）。
    [[nodiscard]] auto strength() const -> float { return strength_; }

  private:
    BlendMode mode_;
    Color tint_;
    float strength_ = 0.0F;
};

/// @brief 着色器遮罩修饰（Paint 切片）：内容绘制完成后按 `kind` 渐变淡出内容盒像素。
class ShaderMaskNode : public ModifierNode {
  public:
    /// @brief 构造着色器遮罩修饰节点。
    /// @param kind 遮罩渐变类型（淡出 / 升起 / 径向淡出，见 `ShaderMaskKind`）。
    /// @param strength 淡出强度 [0,1]（越界钳制）。
    ShaderMaskNode(ShaderMaskKind kind, float strength) : kind_(kind), strength_(std::clamp(strength, 0.0F, 1.0F)) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（遮罩只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::ShaderMask。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::ShaderMask; }

    /// @brief 布局：着色器遮罩不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加遮罩时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取遮罩渐变类型。
    /// @return 构造时传入的类型。
    [[nodiscard]] auto mask_kind() const -> ShaderMaskKind { return kind_; }
    /// @brief 读取淡出强度。
    /// @return 强度 [0,1]（构造时已钳制）。
    [[nodiscard]] auto strength() const -> float { return strength_; }

  private:
    ShaderMaskKind kind_;
    float strength_ = 0.0F;
};

/// @brief 离屏缓存修饰（Paint 切片）：把子树渲染结果缓存到离屏位图，
/// 尺寸不变且未失效时直接复用（类比 Flutter `RepaintBoundary`）。
class CacheLayerNode : public ModifierNode {
  public:
    /// @brief 修饰切片种类。
    /// @return Kind::Paint（缓存层只影响绘制方式，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::CacheLayer。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::CacheLayer; }

    /// @brief 布局：缓存层不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加缓存时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }
};

/// @brief 边框修饰（Paint 切片）：不改变尺寸，绘制时在内容之上描边（支持任意线宽）。
class Border : public ModifierNode {
  public:
    /// @brief 构造边框修饰节点。
    /// @param width 描边线宽（dp，负值钳为 0）。
    /// @param color 描边颜色。
    explicit Border(float width, Color color) : width_(width < 0.0F ? 0.0F : width), color_(color) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（边框只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::Border。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Border; }

    /// @brief 布局：边框不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加边框时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取描边线宽。
    /// @return 线宽（dp，已钳为非负）。
    [[nodiscard]] auto border_width() const -> float { return width_; }
    /// @brief 读取描边颜色。
    /// @return 构造时传入的颜色。
    [[nodiscard]] auto border_color() const -> Color { return color_; }

  private:
    float width_ = 0.0F;
    Color color_;
};

/// @brief 矩形裁剪修饰（Paint 切片）：不改变尺寸，绘制时把内容裁剪到本控件盒子内
/// （与既有裁剪栈取交集）。用于 overflow 隐藏。
class Clip : public ModifierNode {
  public:
    /// @brief 修饰切片种类。
    /// @return Kind::Paint（裁剪只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::Clip。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Clip; }

    /// @brief 布局：矩形裁剪不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不裁剪时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }
};

/// @brief 圆角裁剪修饰（Paint 切片）：绘制时把内容裁剪到圆角矩形（硬遮罩，无抗锯齿）。
/// 常用于头像圆形裁剪、卡片圆角 overflow 隐藏。
class ClipRounded : public ModifierNode {
  public:
    /// @brief 构造圆角裁剪修饰节点。
    /// @param radius 圆角半径（dp，负值钳为 0）。
    explicit ClipRounded(float radius) : radius_(radius < 0.0F ? 0.0F : radius) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（圆角裁剪只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::ClipRounded。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::ClipRounded; }

    /// @brief 布局：圆角裁剪不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不裁剪时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取圆角半径。
    /// @return 半径（dp，已钳为非负）。
    [[nodiscard]] auto radius() const -> float { return radius_; }

  private:
    float radius_ = 0.0F;
};

/// @brief 不透明度修饰（Paint 切片）：不改变尺寸，绘制时整体乘以透明度（alpha）。
class OpacityNode : public ModifierNode {
  public:
    /// @brief 构造不透明度修饰节点。
    /// @param alpha 不透明度 [0,1]（越界钳制；0 = 完全透明，1 = 完全不透明）。
    explicit OpacityNode(float alpha) : alpha_(std::clamp(alpha, 0.0F, 1.0F)) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（不透明度只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }

    /// @brief 布局：不透明度不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不加透明度时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取不透明度系数。
    /// @return 系数 [0,1]（构造时已钳制），由 `Modifier::transform` 累乘进整体 alpha。
    [[nodiscard]] auto alpha() const -> float { return alpha_; }

  private:
    float alpha_ = 0.0F;
};

/// @brief 模糊修饰（Paint 切片）：高斯近似模糊（分离式 box blur）。
/// - `backdrop=false`（`Modifier::blur`）：内容绘制后模糊整个内容盒（内容模糊）。
/// - `backdrop=true`（`Modifier::backdrop_filter`）：内容绘制前先模糊背后区域（毛玻璃）。
class BlurNode : public ModifierNode {
  public:
    /// @brief 构造模糊修饰节点。
    /// @param radius 模糊半径（dp，负值钳为 0）。
    /// @param backdrop true = 背景毛玻璃（内容绘制前先模糊该盒背后像素）；
    ///        false = 内容模糊（内容绘制后模糊该盒像素）。
    explicit BlurNode(float radius, bool backdrop = false)
        : radius_(radius < 0.0F ? 0.0F : radius), backdrop_(backdrop) {}

    /// @brief 修饰切片种类。
    /// @return Kind::Paint（模糊只影响绘制，不影响测量与输入）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    /// @brief Paint 子类型鉴别。
    /// @return PaintKind::Blur。
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Blur; }

    /// @brief 布局：模糊不改变尺寸，原约束直接交给子节点测量。
    /// @param c 外层传入的布局约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点的测量结果（尺寸与不模糊时一致）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    /// @brief 读取模糊半径。
    /// @return 半径（dp，已钳为非负）。
    [[nodiscard]] auto radius() const -> float { return radius_; }
    /// @brief 读取模糊作用目标。
    /// @return true = 模糊背景区域（毛玻璃），false = 模糊内容盒自身。
    [[nodiscard]] auto is_backdrop() const -> bool { return backdrop_; }

  private:
    float radius_ = 0.0F;
    bool backdrop_ = false;
};

}  // namespace aurora

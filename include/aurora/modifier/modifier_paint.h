#pragma once

/// @file modifier_paint.h
/// @brief 绘制修饰节点（Paint 切片）：Background / GradientBackground / ShadowNode /
/// BlendNode / ShaderMaskNode / CacheLayerNode / Border / Clip / ClipRounded / OpacityNode / BlurNode。
/// 本文件为 modifier.h 的子切片；消费者通常直接 #include "aurora/modifier/modifier.h"。

#include <algorithm>

#include "aurora/modifier/modifier_base.h"

namespace aurora {

/// @brief 背景色修饰：不影响尺寸，绘制时填充矩形（在内容之下）。
/// 可选 `radius` 实现圆角背景（硬遮罩，配合 Painter 圆角裁剪）。
class Background : public ModifierNode {
  public:
    Background(Color color, float radius = 0.0f) : m_color(color), m_radius(radius < 0.0f ? 0.0f : radius) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Background; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto color() const -> Color { return m_color; }
    [[nodiscard]] auto corner_radius() const -> float { return m_radius; }

  private:
    Color m_color;
    float m_radius = 0.0f;
};

/// @brief 渐变背景修饰（Paint 切片）：不影响尺寸，绘制时以线性/径向渐变填充矩形。
class GradientBackground : public ModifierNode {
  public:
    enum class Type : std::uint8_t { Linear, Radial };

    /// 线性渐变构造
    GradientBackground(std::vector<Color> colors, std::vector<float> stops, float angle_deg)
        : m_type(Type::Linear), m_colors(std::move(colors)), m_stops(std::move(stops)), m_angle(angle_deg) {}

    /// 径向渐变构造
    GradientBackground(std::vector<Color> colors, std::vector<float> stops)
        : m_type(Type::Radial), m_colors(std::move(colors)), m_stops(std::move(stops)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::GradientBackground; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto type() const -> Type { return m_type; }
    [[nodiscard]] auto colors() const -> const std::vector<Color> & { return m_colors; }
    [[nodiscard]] auto stops() const -> const std::vector<float> & { return m_stops; }
    [[nodiscard]] auto angle() const -> float { return m_angle; }

  private:
    Type m_type;
    std::vector<Color> m_colors;
    std::vector<float> m_stops;
    float m_angle = 0.0f; ///< 线性渐变角度（度，0=从左到右）
};

/// @brief 阴影修饰（Paint 切片）：在内容之下绘制投影阴影。
class ShadowNode : public ModifierNode {
  public:
    ShadowNode(float offset_x, float offset_y, float blur, Color color)
        : m_offset_x(offset_x), m_offset_y(offset_y), m_blur(blur < 0.0f ? 0.0f : blur), m_color(color) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Shadow; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto offset_x() const -> float { return m_offset_x; }
    [[nodiscard]] auto offset_y() const -> float { return m_offset_y; }
    [[nodiscard]] auto blur() const -> float { return m_blur; }
    [[nodiscard]] auto color() const -> Color { return m_color; }

  private:
    float m_offset_x = 0.0f;
    float m_offset_y = 0.0f;
    float m_blur = 0.0f;
    Color m_color;
};

/// @brief 混合修饰（Paint 切片）：内容绘制完成后，把内容盒像素与 `tint` 按 `mode` 混合。
class BlendNode : public ModifierNode {
  public:
    BlendNode(BlendMode mode, Color tint, float strength)
        : m_mode(mode), m_tint(tint), m_strength(std::clamp(strength, 0.0f, 1.0f)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Blend; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto mode() const -> BlendMode { return m_mode; }
    [[nodiscard]] auto tint() const -> Color { return m_tint; }
    [[nodiscard]] auto strength() const -> float { return m_strength; }

  private:
    BlendMode m_mode;
    Color m_tint;
    float m_strength = 0.0f;
};

/// @brief 着色器遮罩修饰（Paint 切片）：内容绘制完成后按 `kind` 渐变淡出内容盒像素。
class ShaderMaskNode : public ModifierNode {
  public:
    ShaderMaskNode(ShaderMaskKind kind, float strength) : m_kind(kind), m_strength(std::clamp(strength, 0.0f, 1.0f)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::ShaderMask; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto mask_kind() const -> ShaderMaskKind { return m_kind; }
    [[nodiscard]] auto strength() const -> float { return m_strength; }

  private:
    ShaderMaskKind m_kind;
    float m_strength = 0.0f;
};

/// @brief 离屏缓存修饰（Paint 切片）：把子树渲染结果缓存到离屏位图，
/// 尺寸不变且未失效时直接复用（类比 Flutter `RepaintBoundary`）。
class CacheLayerNode : public ModifierNode {
  public:
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::CacheLayer; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }
};

/// @brief 边框修饰（Paint 切片）：不改变尺寸，绘制时在内容之上描边（支持任意线宽）。
class Border : public ModifierNode {
  public:
    explicit Border(float width, Color color) : m_width(width < 0.0f ? 0.0f : width), m_color(color) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Border; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto border_width() const -> float { return m_width; }
    [[nodiscard]] auto border_color() const -> Color { return m_color; }

  private:
    float m_width = 0.0f;
    Color m_color;
};

/// @brief 矩形裁剪修饰（Paint 切片）：不改变尺寸，绘制时把内容裁剪到本控件盒子内
/// （与既有裁剪栈取交集）。用于 overflow 隐藏。
class Clip : public ModifierNode {
  public:
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Clip; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }
};

/// @brief 圆角裁剪修饰（Paint 切片）：绘制时把内容裁剪到圆角矩形（硬遮罩，无抗锯齿）。
/// 常用于头像圆形裁剪、卡片圆角 overflow 隐藏。
class ClipRounded : public ModifierNode {
  public:
    explicit ClipRounded(float radius) : m_radius(radius < 0.0f ? 0.0f : radius) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::ClipRounded; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto radius() const -> float { return m_radius; }

  private:
    float m_radius = 0.0f;
};

/// @brief 不透明度修饰（Paint 切片）：不改变尺寸，绘制时整体乘以透明度（alpha）。
class OpacityNode : public ModifierNode {
  public:
    explicit OpacityNode(float alpha) : m_alpha(std::clamp(alpha, 0.0f, 1.0f)) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto alpha() const -> float { return m_alpha; }

  private:
    float m_alpha = 0.0f;
};

/// @brief 模糊修饰（Paint 切片）：高斯近似模糊（分离式 box blur）。
/// - `backdrop=false`（`Modifier::blur`）：内容绘制后模糊整个内容盒（内容模糊）。
/// - `backdrop=true`（`Modifier::backdrop_filter`）：内容绘制前先模糊背后区域（毛玻璃）。
class BlurNode : public ModifierNode {
  public:
    explicit BlurNode(float radius, bool backdrop = false)
        : m_radius(radius < 0.0f ? 0.0f : radius), m_backdrop(backdrop) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Paint; }
    [[nodiscard]] auto paint_kind() const -> PaintKind override { return PaintKind::Blur; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);
    }

    [[nodiscard]] auto radius() const -> float { return m_radius; }
    [[nodiscard]] auto is_backdrop() const -> bool { return m_backdrop; }

  private:
    float m_radius = 0.0f;
    bool m_backdrop = false;
};

} // namespace aurora

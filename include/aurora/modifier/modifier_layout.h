#pragma once

/// @file modifier_layout.h
/// @brief 布局修饰节点（Layout 切片）：Padding / PaddingEdges / FlexWeight / SizeModifier。
/// 本文件为 modifier.h 的子切片；消费者通常直接 #include "aurora/modifier/modifier.h"。

#include "aurora/modifier/modifier_base.h"

namespace aurora {

/// @brief 内边距修饰：收缩子节点约束并加回内边距尺寸。
class Padding : public ModifierNode {
  public:
    explicit Padding(float pad)
        : m_pad(pad < 0.0f ? (Diagnostics::degraded("layout", "Padding 负值已降级为 0"), 0.0f) : pad) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const float h = m_pad * 2.0f;
        Constraints inner;
        inner.min = Size{ .width = std::max(0.0f, c.min.width - h), .height = std::max(0.0f, c.min.height - h) };
        inner.max = Size{ .width = std::max(0.0f, c.max.width - h), .height = std::max(0.0f, c.max.height - h) };
        const Size s = measure_child(inner);
        return Size{ .width = s.width + h, .height = s.height + h };
    }

    [[nodiscard]] auto padding() const -> float { return m_pad; }

  private:
    float m_pad = 0.0f;
};

/// @brief 非对称内边距修饰：支持 left/top/right/bottom 独立设置（对应 Flutter EdgeInsets）。
class PaddingEdges : public ModifierNode {
  public:
    explicit PaddingEdges(EdgeInsets insets) : m_insets(insets) {
        // 负值降级为 0
        if (m_insets.left < 0.0f || m_insets.top < 0.0f || m_insets.right < 0.0f || m_insets.bottom < 0.0f) {
            Diagnostics::degraded("layout", "PaddingEdges 负值已降级为 0");
            m_insets.left = std::max(0.0f, m_insets.left);
            m_insets.top = std::max(0.0f, m_insets.top);
            m_insets.right = std::max(0.0f, m_insets.right);
            m_insets.bottom = std::max(0.0f, m_insets.bottom);
        }
    }

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const float h = m_insets.horizontal();
        const float v = m_insets.vertical();
        Constraints inner;
        inner.min = Size{ .width = std::max(0.0f, c.min.width - h), .height = std::max(0.0f, c.min.height - v) };
        inner.max = Size{ .width = std::max(0.0f, c.max.width - h), .height = std::max(0.0f, c.max.height - v) };
        const Size s = measure_child(inner);
        return Size{ .width = s.width + h, .height = s.height + v };
    }

    [[nodiscard]] auto insets() const -> EdgeInsets { return m_insets; }

  private:
    EdgeInsets m_insets;
};

/// @brief Flex 权重修饰：在 Row/Column 中按权重瓜分主轴剩余空间（对应 Expand / Flutter `Expanded`）。
/// 权重 0 表示不扩展（仅占内容尺寸）。组合在 widget 的 `modifier` 上，与 flex 布局正交。
/// 自身不改变子节点尺寸，仅作为父级 flex 分配的依据（由 `Modifier::flexWeight()` 读取）。
class FlexWeight : public ModifierNode {
  public:
    explicit FlexWeight(float weight) : m_weight(weight) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c); // 不改变尺寸，透明透传
    }

    [[nodiscard]] auto flex_weight() const -> float override { return m_weight; }

  private:
    float m_weight = 0.0f;
};

/// @brief 固定/填充尺寸修饰（Layout 切片）：把命中轴约束夹成目标尺寸，强制子节点按该尺寸测量。
/// 与 `Widget::width/height` 强类型意图正交：本修饰可组合、可随状态变化（Reactive<Modifier>）。
/// 语义对齐 Compose `Modifier.size/fillMaxWidth` 与 Flutter `SizedBox`。
class SizeModifier : public ModifierNode {
  public:
    /// @brief 设置固定宽度（-1 表示不约束，沿用子节点尺寸）。
    auto set_width(float w) -> void { m_w = w; }
    /// @brief 设置固定高度（-1 表示不约束，沿用子节点尺寸）。
    auto set_height(float h) -> void { m_h = h; }
    /// @brief 沿主轴填充父级可用宽度（min=max=约束上限）。
    auto set_fill_w(bool b) -> void { m_fill_w = b; }
    /// @brief 沿主轴填充父级可用高度（min=max=约束上限）。
    auto set_fill_h(bool b) -> void { m_fill_h = b; }

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        Constraints inner = c;
        if (m_fill_w) {
            inner.min.width = c.max.width;
            inner.max.width = c.max.width;
        } else if (m_w >= 0.0f) {
            inner.min.width = m_w;
            inner.max.width = m_w;
        }
        if (m_fill_h) {
            inner.min.height = c.max.height;
            inner.max.height = c.max.height;
        } else if (m_h >= 0.0f) {
            inner.min.height = m_h;
            inner.max.height = m_h;
        }
        const Size s = measure_child(inner);
        float w = s.width;
        if (m_fill_w) {
            w = c.max.width;
        } else if (m_w >= 0.0f) {
            w = m_w;
        }
        float h = s.height;
        if (m_fill_h) {
            h = c.max.height;
        } else if (m_h >= 0.0f) {
            h = m_h;
        }
        return Size{ .width = w, .height = h };
    }

  private:
    float m_w = -1.0f; ///< -1 = 不约束
    float m_h = -1.0f;
    bool m_fill_w = false;
    bool m_fill_h = false;
};

} // namespace aurora

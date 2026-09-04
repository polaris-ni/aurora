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
        : pad_(pad < 0.0F ? (Diagnostics::degraded("layout", "Padding degrade to 0"), 0.0F) : pad) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const float h = pad_ * 2.0F;
        Constraints inner;
        inner.min = Size{.width = std::max(0.0F, c.min.width - h), .height = std::max(0.0F, c.min.height - h)};
        inner.max = Size{.width = std::max(0.0F, c.max.width - h), .height = std::max(0.0F, c.max.height - h)};
        const Size s = measure_child(inner);
        return Size{.width = s.width + h, .height = s.height + h};
    }

    [[nodiscard]] auto padding() const -> float { return pad_; }

  private:
    float pad_ = 0.0F;
};

/// @brief 非对称内边距修饰：支持 left/top/right/bottom 独立设置（对应 Flutter EdgeInsets）。
class PaddingEdges : public ModifierNode {
  public:
    explicit PaddingEdges(EdgeInsets insets) : insets_(insets) {
        // 负值降级为 0
        if (insets_.left < 0.0F || insets_.top < 0.0F || insets_.right < 0.0F || insets_.bottom < 0.0F) {
            Diagnostics::degraded("layout", "PaddingEdges 负值已降级为 0");
            insets_.left = std::max(0.0F, insets_.left);
            insets_.top = std::max(0.0F, insets_.top);
            insets_.right = std::max(0.0F, insets_.right);
            insets_.bottom = std::max(0.0F, insets_.bottom);
        }
    }

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const float h = insets_.horizontal();
        const float v = insets_.vertical();
        Constraints inner;
        inner.min = Size{.width = std::max(0.0F, c.min.width - h), .height = std::max(0.0F, c.min.height - v)};
        inner.max = Size{.width = std::max(0.0F, c.max.width - h), .height = std::max(0.0F, c.max.height - v)};
        const Size s = measure_child(inner);
        return Size{.width = s.width + h, .height = s.height + v};
    }

    [[nodiscard]] auto insets() const -> EdgeInsets { return insets_; }

  private:
    EdgeInsets insets_;
};

/// @brief Flex 权重修饰：在 Row/Column 中按权重瓜分主轴剩余空间（对应 Expand / Flutter `Expanded`）。
/// 权重 0 表示不扩展（仅占内容尺寸）。组合在 widget 的 `modifier` 上，与 flex 布局正交。
/// 自身不改变子节点尺寸，仅作为父级 flex 分配的依据（由 `Modifier::flexWeight()` 读取）。
class FlexWeight : public ModifierNode {
  public:
    explicit FlexWeight(float weight) : weight_(weight) {}

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);  // 不改变尺寸，透明透传
    }

    [[nodiscard]] auto flex_weight() const -> float override { return weight_; }

  private:
    float weight_ = 0.0F;
};

/// @brief 固定/填充尺寸修饰（Layout 切片）：把命中轴约束夹成目标尺寸，强制子节点按该尺寸测量。
/// 与 `Widget::width/height` 强类型意图正交：本修饰可组合、可随状态变化（Reactive<Modifier>）。
/// 语义对齐 Compose `Modifier.size/fillMaxWidth` 与 Flutter `SizedBox`。
class SizeModifier : public ModifierNode {
  public:
    /// @brief 设置固定宽度（-1 表示不约束，沿用子节点尺寸）。
    auto set_width(float w) -> void { w_ = w; }
    /// @brief 设置固定高度（-1 表示不约束，沿用子节点尺寸）。
    auto set_height(float h) -> void { h_ = h; }
    /// @brief 沿主轴填充父级可用宽度（min=max=约束上限）。
    auto set_fill_w(bool b) -> void { fill_w_ = b; }
    /// @brief 沿主轴填充父级可用高度（min=max=约束上限）。
    auto set_fill_h(bool b) -> void { fill_h_ = b; }

    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        Constraints inner = c;
        if (fill_w_) {
            inner.min.width = c.max.width;
            inner.max.width = c.max.width;
        } else if (w_ >= 0.0F) {
            inner.min.width = w_;
            inner.max.width = w_;
        }
        if (fill_h_) {
            inner.min.height = c.max.height;
            inner.max.height = c.max.height;
        } else if (h_ >= 0.0F) {
            inner.min.height = h_;
            inner.max.height = h_;
        }
        const Size s = measure_child(inner);
        float w = s.width;
        if (fill_w_) {
            w = c.max.width;
        } else if (w_ >= 0.0F) {
            w = w_;
        }
        float h = s.height;
        if (fill_h_) {
            h = c.max.height;
        } else if (h_ >= 0.0F) {
            h = h_;
        }
        return Size{.width = w, .height = h};
    }

  private:
    float w_ = -1.0F;  ///< -1 = 不约束
    float h_ = -1.0F;
    bool fill_w_ = false;
    bool fill_h_ = false;
};

}  // namespace aurora

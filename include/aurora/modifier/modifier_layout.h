#pragma once

/// @brief 布局修饰节点（Layout 切片）：Padding / PaddingEdges / FlexWeight / SizeModifier。
/// 本文件为 modifier.h 的子切片；消费者通常直接 `#include` "aurora/modifier/modifier.h"。
/// @file modifier_layout.h

#include "aurora/core/diagnostics.h"
#include "aurora/modifier/modifier_base.h"

namespace aurora {

/// @brief 内边距修饰：收缩子节点约束并加回内边距尺寸。
class Padding : public ModifierNode {
  public:
    /// @brief 构造均匀内边距修饰。
    /// @param pad 四边相同的内边距宽度（dp）；负值降级为 0 并经 Diagnostics::degraded 上报。
    explicit Padding(float pad)
        : pad_(pad < 0.0F ? (Diagnostics::degraded("layout", "Padding degrade to 0"), 0.0F) : pad) {}

    /// @brief 节点类别鉴别。
    /// @return 恒为 Kind::Layout（本节点参与测量）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    /// @brief 测量：把约束两维各收缩 2×pad 后交给子节点，再在子结果上加回 2×pad。
    /// @param c 本节点收到的外部约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点尺寸加均匀内边距（宽高各 +2×pad）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        const float h = pad_ * 2.0F;
        Constraints inner;
        inner.min = Size{.width = std::max(0.0F, c.min.width - h), .height = std::max(0.0F, c.min.height - h)};
        inner.max = Size{.width = std::max(0.0F, c.max.width - h), .height = std::max(0.0F, c.max.height - h)};
        const Size s = measure_child(inner);
        return Size{.width = s.width + h, .height = s.height + h};
    }

    /// @brief 读取当前内边距宽度。
    /// @return 构造时传入并已夹为非负的 pad（dp）。
    [[nodiscard]] auto padding() const -> float { return pad_; }

  private:
    float pad_ = 0.0F;
};

/// @brief 非对称内边距修饰：支持 left/top/right/bottom 独立设置（对应 Flutter EdgeInsets）。
class PaddingEdges : public ModifierNode {
  public:
    /// @brief 构造非对称内边距修饰。
    /// @param insets 四边内边距；含负值时各边降级为 0 并经 Diagnostics::degraded 上报。
    explicit PaddingEdges(const EdgeInsets &insets) : insets_(insets) {
        // 负值降级为 0
        if (insets_.left < 0.0F || insets_.top < 0.0F || insets_.right < 0.0F || insets_.bottom < 0.0F) {
            Diagnostics::degraded("layout", "PaddingEdges negative value degraded to 0");
            insets_.left = std::max(0.0F, insets_.left);
            insets_.top = std::max(0.0F, insets_.top);
            insets_.right = std::max(0.0F, insets_.right);
            insets_.bottom = std::max(0.0F, insets_.bottom);
        }
    }

    /// @brief 节点类别鉴别。
    /// @return 恒为 Kind::Layout（本节点参与测量）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    /// @brief 测量：把约束按 insets 的水平/垂直总量收缩后交给子节点，再在子结果上加回。
    /// @param c 本节点收到的外部约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点尺寸加内边距（宽 +insets.horizontal()，高 +insets.vertical()）。
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

    /// @brief 读取当前四边内边距。
    /// @return 构造时传入并已夹为非负的 EdgeInsets。
    [[nodiscard]] auto insets() const -> EdgeInsets { return insets_; }

  private:
    EdgeInsets insets_;
};

/// @brief Flex 权重修饰：在 Row/Column 中按权重瓜分主轴剩余空间（对应 Expand / Flutter `Expanded`）。
/// 权重 0 表示不扩展（仅占内容尺寸）。组合在 widget 的 `modifier` 上，与 flex 布局正交。
/// 自身不改变子节点尺寸，仅作为父级 flex 分配的依据（由 `Modifier::flex_weight()` 读取）。
class FlexWeight : public ModifierNode {
  public:
    /// @brief 构造 Flex 权重修饰。
    /// @param weight flex 权重；0 表示不扩展（仅占内容尺寸）。
    explicit FlexWeight(float weight) : weight_(weight) {}

    /// @brief 节点类别鉴别。
    /// @return 恒为 Kind::Layout（供父级 flex 分配读取）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    /// @brief 测量：原样透传约束给子节点（本节点不改变尺寸，仅作父级 flex 分配依据）。
    /// @param c 本节点收到的外部约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 子节点尺寸（透明透传）。
    auto layout(const Constraints &c, const std::function<Size(const Constraints &)> &measure_child) const
        -> Size override {
        return measure_child(c);  // 不改变尺寸，透明透传
    }

    /// @brief 读取 flex 权重（由 Row/Column 在 flex 布局时经基类钩子取用，瓜分主轴剩余空间）。
    /// @return 构造时传入的权重。
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
    /// @param w 目标宽度（dp）；>= 0 时把子节点宽度约束 min/max 都钉为该值。
    auto set_width(float w) -> void { w_ = w; }
    /// @brief 设置固定高度（-1 表示不约束，沿用子节点尺寸）。
    /// @param h 目标高度（dp）；>= 0 时把子节点高度约束 min/max 都钉为该值。
    auto set_height(float h) -> void { h_ = h; }
    /// @brief 沿对应轴（宽）填充父级可用宽度（min=max=约束上限）。
    /// @param b true = 宽度填充父级上限（优先于 set_width）；false = 不填充。
    auto set_fill_w(bool b) -> void { fill_w_ = b; }
    /// @brief 沿对应轴（高）填充父级可用高度（min=max=约束上限）。
    /// @param b true = 高度填充父级上限（优先于 set_height）；false = 不填充。
    auto set_fill_h(bool b) -> void { fill_h_ = b; }

    /// @brief 节点类别鉴别。
    /// @return 恒为 Kind::Layout（本节点参与测量）。
    [[nodiscard]] auto kind() const -> Kind override { return Kind::Layout; }

    /// @brief 测量：命中轴把约束钉成固定尺寸（w_/h_ >= 0）或填充上限（fill 标志，优先）后交给子节点。
    /// @param c 本节点收到的外部约束。
    /// @param measure_child 以给定约束测量子节点的回调。
    /// @return 钉过尺寸的轴取目标值（固定值或约束上限），未钉的轴取子节点测量结果。
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

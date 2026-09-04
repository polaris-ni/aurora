#pragma once

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

#include "aurora/core/enums.h"
#include "aurora/core/types.h"
#include "aurora/widget/alignment.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 层叠布局：所有子节点堆叠在同一原点，
 * 尺寸取子节点最大包围盒。适合叠加徽章、浮层、绝对定位组合。
 *
 * 与 `Column`/`Row` 不同，`Stack` 不做主轴排布，所有子节点从 (0,0) 开始绘制。
 * 对齐通过 `Alignment` 控制（默认 top-left）。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */

class Stack : public Container {
  public:
    Stack() = default;
    explicit Stack(std::vector<Node> children, Alignment align = Alignment::TopLeft) : align_(align) {
        children_ = std::move(children);
    }
    /// @brief 便捷构造：扁平罗列子项（Stack{ a, b }），对齐取默认 top-left。
    Stack(std::initializer_list<Node> kids, Alignment align = Alignment::TopLeft) : align_(align) {
        set_children(kids);
    }

    /// @brief 设置子节点适配策略（链式）；默认 Passthrough（沿用父约束，等同原行为）。
    auto set_fit(StackFit fit) -> Stack & {
        fit_ = fit;
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Stack"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Stack",
            .properties =
                {
                    {.name = "alignment",
                     .type = "Alignment",
                     .default_value = "TopLeft",
                     .required = false,
                     .note = "子节点对齐"},
                    {.name = "fit",
                     .type = "StackFit",
                     .default_value = "Passthrough",
                     .required = false,
                     .note = "子节点适配策略"},
                    {.name = "width", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "height", .type = "Length", .default_value = "auto", .required = false},
                    {.name = "show", .type = "bool", .default_value = "true", .required = false},
                },
            .events = {},
            .children_policy = "multiple",
            .examples = {R"(au::Stack{ au::Text("bg"), au::Text("fg") })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["alignment"] = align_;
        props["fit"] = fit_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("alignment")) {
            align_ = static_cast<Alignment>(props["alignment"].get<int>());
        }
        if (props.contains("fit")) {
            fit_ = static_cast<StackFit>(props["fit"].get<int>());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const Constraints base = (fit_ == StackFit::Loose)
                                     ? Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                                                   .max = Size{.width = c.max.width, .height = c.max.height}}
                                     : c;  // Passthrough / Expand 首轮均按父约束测尺寸
        float max_w = 0.0F;
        float max_h = 0.0F;
        std::vector<Size> sizes;
        sizes.reserve(children_.size());
        for (Node &child : children_) {
            const Size s = child.widget().layout(base, ctx);
            sizes.push_back(s);
            max_w = std::max(max_w, s.width);
            max_h = std::max(max_h, s.height);
        }
        max_w = std::max(c.min.width, std::min(max_w, c.max.width));
        max_h = std::max(c.min.height, std::min(max_h, c.max.height));
        // Expand：子节点强制撑满 Stack 包围盒（二次布局）。
        if (fit_ == StackFit::Expand) {
            const Constraints tight{.min = Size{.width = max_w, .height = max_h},
                                    .max = Size{.width = max_w, .height = max_h}};
            for (std::size_t i = 0; i < children_.size(); ++i) {
                sizes[i] = children_[i].widget().layout(tight, ctx);
            }
        }
        // 记录各子对齐原点，供绘制使用
        origins_.clear();
        for (std::size_t i = 0; i < children_.size(); ++i) {
            const Point off = align_origin(align_, sizes[i], Size{.width = max_w, .height = max_h});
            origins_.push_back(off);
            children_[i].set_bounds(Rect{.origin = off, .size = sizes[i]});  // 落定子节点盒，供命中测试几何权威
        }
        return c.constrain(Size{.width = max_w, .height = max_h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        for (std::size_t i = 0; i < children_.size(); ++i) {
            const Point off = (i < origins_.size()) ? origins_[i] : Point{.x = 0, .y = 0};
            Rect child_bounds{.origin = Point{.x = bounds.origin.x + off.x, .y = bounds.origin.y + off.y},
                              .size = children_[i].widget().size()};
            children_[i].widget().paint(p, child_bounds, ctx);
        }
    }

  private:
    Alignment align_ = Alignment::TopLeft;
    StackFit fit_ = StackFit::Passthrough;  ///< 子节点适配策略（默认沿用原行为）
    std::vector<Point> origins_;
};

/// @brief 便捷别名：默认居中的 Stack（浮层叠加常用）。
using Overlay = Stack;

}  // namespace aurora

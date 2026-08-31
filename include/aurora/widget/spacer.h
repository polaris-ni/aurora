#pragma once

#include "aurora/core/types.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 弹性空间（REQUIREMENTS §4.1 / 规格 §4.1.1）。
 *
 * 在 `Column`/`Row` 中吸收主轴方向的全部剩余自由空间，用于把相邻 widget 推到两端。
 * 自身无绘制。自由空间为 0 时退化为 0 尺寸（与 Compose/Flutter Spacer 一致）。
 *
 * 注意：Spacer 仅在父容器存在主轴剩余空间（如父约束强制更大尺寸）时可见生效；
 * 若父容器按内容裁剪，自由空间为 0，Spacer 不占空间。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Spacer : public Widget {
  public:
    explicit Spacer(bool expand = true) : m_expand(expand) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Spacer"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Spacer",
            .properties = {
                { .name = "expand", .type = "bool", .default_value = "true", .required = false, .note = "是否吸收剩余空间", .json_type = "boolean" },
                { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
                { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
            },
            .events = {},
            .children_policy = "none",
            .examples = { "au::Spacer()" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["expand"] = m_expand;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("expand")) {
            m_expand = props["expand"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        // 占据父约束给出的全部可用空间（由父 FlexLayouter 决定自由空间分配）。
        if (m_expand) {
            return c.constrain(Size{ .width = c.max.width, .height = c.max.height });
        }
        return c.constrain(Size{ .width = 0.0f, .height = 0.0f });
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    bool m_expand = false;
};

} // namespace aurora

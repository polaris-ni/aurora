#pragma once

#include <cstdint>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 分隔线方向。
enum class Orientation : std::uint8_t {
    Horizontal,
    Vertical,
};

/// @brief 分隔线属性（聚合，AI 用指定初始化器填写）。
struct DividerProps {
    Orientation orientation = Orientation::Horizontal;
    float thickness = 1.0F;
    Color color = Color{200, 200, 200, 255};
    float indent = 0.0F;  ///< 起点缩进 dp（横向为左缩进，纵向为上缩进）
    float end_indent = 0.0F;  ///< 终点缩进 dp（横向为右缩进，纵向为下缩进）
};

/**
 * @brief 分隔线（叶控件）：绘制一条视觉分隔线（headless 占位渲染）。
 *
 * 横向填满父宽度、纵向填满父高度，厚度由 `thickness` 决定，颜色由 `color` 决定。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Divider : public LeafWidget, public DividerProps {
  public:
    Divider() = default;
    /// @brief 配置块构造（specification/04-widget.md §2.5）。
    explicit Divider(const DividerProps &props) : DividerProps(props) {}

    /// @brief 设置起点缩进 dp（链式）。
    auto set_indent(float v) -> Divider & {
        indent = v;
        return *this;
    }
    /// @brief 设置终点缩进 dp（链式）。
    auto set_end_indent(float v) -> Divider & {
        end_indent = v;
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Divider"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Divider",
            .properties =
                {
                    {.name = "orientation",
                     .type = "Orientation",
                     .default_value = "horizontal",
                     .required = false,
                     .note = "方向",
                     .json_type = "string",
                     .enum_values = {"Horizontal", "Vertical"}},
                    {.name = "thickness",
                     .type = "float",
                     .default_value = "1.0",
                     .required = false,
                     .note = "线粗(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "{200,200,200,255}",
                     .required = false,
                     .note = "颜色",
                     .json_type = "array"},
                    {.name = "indent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "起点缩进(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "end_indent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "终点缩进(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "width",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "height",
                     .type = "Length",
                     .default_value = "auto",
                     .required = false,
                     .note = "",
                     .json_type = "array"},
                    {.name = "show",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "",
                     .json_type = "boolean"},
                },
            .events = {},
            .children_policy = "none",
            .examples = {"au::Divider()", "au::Divider(au::DividerProps{ .orientation = Orientation::Vertical })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["orientation"] = orientation == Orientation::Vertical ? "vertical" : "horizontal";
        props["thickness"] = thickness;
        props["color"] = color_to_json(color);
        props["indent"] = indent;
        props["end_indent"] = end_indent;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("orientation")) {
            const std::string o = props["orientation"].get<std::string>();
            orientation = o == "vertical" ? Orientation::Vertical : Orientation::Horizontal;
        }
        if (props.contains("thickness")) {
            thickness = props["thickness"].get<float>();
        }
        if (props.contains("color")) {
            color = json_to_color(props["color"]);
        }
        if (props.contains("indent")) {
            indent = props["indent"].get<float>();
        }
        if (props.contains("end_indent")) {
            end_indent = props["end_indent"].get<float>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        if (orientation == Orientation::Vertical) {
            return c.constrain(Size{.width = thickness, .height = c.max.height});
        }
        return c.constrain(Size{.width = c.max.width, .height = thickness});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        if (orientation == Orientation::Vertical) {
            const float y0 = bounds.origin.y + indent;
            const float h = bounds.size.height - indent - end_indent;
            if (h > 0.0F) {
                const Rect r{.origin = Point{.x = bounds.origin.x, .y = y0},
                             .size = Size{.width = thickness, .height = h}};
                p.fill_rect(r, color);
            }
        } else {
            const float x0 = bounds.origin.x + indent;
            const float w = bounds.size.width - indent - end_indent;
            if (w > 0.0F) {
                const Rect r{.origin = Point{.x = x0, .y = bounds.origin.y},
                             .size = Size{.width = w, .height = thickness}};
                p.fill_rect(r, color);
            }
        }
    }
};

}  // namespace aurora

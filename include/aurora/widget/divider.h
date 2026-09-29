#pragma once

#include <cstdint>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 分隔线方向。
enum class Orientation : std::uint8_t {
    Horizontal,  ///< 横向：线沿父宽度水平铺满，厚度为竖直方向线宽。
    Vertical,  ///< 纵向：线沿父高度竖直铺满，厚度为水平方向线宽。
};

/// @brief 分隔线属性（聚合，AI 用指定初始化器填写）。
struct DividerProps {
    Orientation orientation = Orientation::Horizontal;  ///< 分隔线方向，默认横向。
    float thickness = 1.0F;  ///< 线厚（dp），默认 1.0。
    Color color = Color{200, 200, 200, 255};  ///< 线颜色，默认不透明浅灰。
    float indent = 0.0F;  ///< 起点缩进 dp（横向为左缩进，纵向为上缩进）
    float end_indent = 0.0F;  ///< 终点缩进 dp（横向为右缩进，纵向为下缩进）
};

/// @brief 分隔线（叶控件）：绘制一条视觉分隔线（headless 占位渲染）。
///
/// 横向填满父宽度、纵向填满父高度，厚度由 `thickness` 决定，颜色由 `color` 决定。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Divider : public LeafWidget, public DividerProps {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 默认构造：横向、1.0 dp 厚、浅灰色、无缩进。
    Divider() = default;
    /// @brief 配置块构造（specification/04-widget.md §2.5）。
    /// @param props 分隔线属性块，整份拷入 DividerProps 基类字段。
    explicit Divider(const DividerProps &props) : DividerProps(props) {}

    /// @brief 设置起点缩进 dp（链式）。
    /// @param v 新起点缩进（横向为左缩进，纵向为上缩进）。
    /// @return 自身引用，便于链式调用。
    auto set_indent(float v) -> Divider & {
        indent = v;
        return *this;
    }
    /// @brief 设置终点缩进 dp（链式）。
    /// @param v 新终点缩进（横向为右缩进，纵向为下缩进）。
    /// @return 自身引用，便于链式调用。
    auto set_end_indent(float v) -> Divider & {
        end_indent = v;
        return *this;
    }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Divider"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Divider"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Divider 的控件描述符（方向/线粗/颜色/缩进/尺寸属性）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Divider",
            .properties =
                {
                    {.name = "orientation",
                     .type = "Orientation",
                     .default_value = "horizontal",
                     .required = false,
                     .note = "Direction",
                     .json_type = "string",
                     .enum_values = {"Horizontal", "Vertical"}},
                    {.name = "thickness",
                     .type = "float",
                     .default_value = "1.0",
                     .required = false,
                     .note = "Line thickness (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "color",
                     .type = "Color",
                     .default_value = "{200,200,200,255}",
                     .required = false,
                     .note = "Color",
                     .json_type = "array"},
                    {.name = "indent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "Start indent (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "end_indent",
                     .type = "float",
                     .default_value = "0.0",
                     .required = false,
                     .note = "End indent (dp)",
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
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Divider 的控件描述符（方向/线粗/颜色/缩进/尺寸属性，无事件、无子项）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集 SignalViewBase 指针；Divider 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 序列化分隔线属性：方向以 "vertical"/"horizontal" 字符串写入，另含线粗、颜色与两端缩进。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("orientation", orientation == Orientation::Vertical ? "vertical" : "horizontal");
        props.set("thickness", thickness);
        props.set("color", color_to_json(color));
        props.set("indent", indent);
        props.set("end_indent", end_indent);
    }

    /// @brief 反序列化分隔线属性：按存在的键逐一还原方向/线粗/颜色/缩进，缺键保持当前值。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("orientation")) {
            const std::string o = props.at("orientation")->as_or<std::string>("");
            orientation = o == "vertical" ? Orientation::Vertical : Orientation::Horizontal;
        }
        if (props.contains("thickness")) {
            thickness = props.at("thickness")->as_or<float>(0.0F);
        }
        if (props.contains("color")) {
            color = json_to_color(*props.at("color"));
        }
        if (props.contains("indent")) {
            indent = props.at("indent")->as_or<float>(0.0F);
        }
        if (props.contains("end_indent")) {
            end_indent = props.at("end_indent")->as_or<float>(0.0F);
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

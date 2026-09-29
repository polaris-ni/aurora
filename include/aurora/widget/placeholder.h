#pragma once

#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 降级视觉占位控件（需求 #18 / 降级视觉语言）。
///
/// 当某个控件无法构建/反序列化、或需标注「此处缺失/错误」时，用本控件渲染为一个
/// 灰底、警示色边框、显示说明文字的盒子，使局部错误不致拖垮整棵 UI。
///
/// @code
/// auto fallback = au::Placeholder("Button does not support serialization yet");
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Placeholder : public Widget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 默认构造：说明文字为空（渲染时显示 "(placeholder)"），三色取灰底/警示红框/深灰字默认。
    Placeholder() = default;
    /// @brief 以说明文字构造占位控件。
    /// @param msg 占位说明文字，移动存入 message_。
    explicit Placeholder(std::string msg) : message_(std::move(msg)) {}

    /// @brief 设置说明文字（链式）。
    /// @param msg 新说明文字，移动存入 message_。
    /// @return 自身引用，便于链式调用。
    auto set_message(std::string msg) -> Placeholder & {
        message_ = std::move(msg);
        return *this;
    }

    /// @brief 设置背景色（链式）。
    /// @param c 新背景色。
    /// @return 自身引用，便于链式调用。
    auto set_background_color(Color c) -> Placeholder & {
        background_ = c;
        return *this;
    }

    /// @brief 设置边框（警示）色（链式）。
    /// @param c 新边框颜色。
    /// @return 自身引用，便于链式调用。
    auto set_border_color(Color c) -> Placeholder & {
        border_ = c;
        return *this;
    }

    /// @brief 设置文字色（链式）。
    /// @param c 新文字颜色。
    /// @return 自身引用，便于链式调用。
    auto set_text_color(Color c) -> Placeholder & {
        text_ = c;
        return *this;
    }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Placeholder"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Placeholder"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Placeholder 的控件描述符（文字/三色/尺寸属性）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Placeholder",
            .properties =
                {
                    {.name = "message",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = false,
                     .note = "Placeholder text",
                     .json_type = "string"},
                    {.name = "background_color",
                     .type = "Color",
                     .default_value = "{242,242,242,255}",
                     .required = false,
                     .note = "Background color",
                     .json_type = "array"},
                    {.name = "border_color",
                     .type = "Color",
                     .default_value = "{192,57,43,255}",
                     .required = false,
                     .note = "Border color",
                     .json_type = "array"},
                    {.name = "text_color",
                     .type = "Color",
                     .default_value = "{85,85,85,255}",
                     .required = false,
                     .note = "Text color",
                     .json_type = "array"},
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
            .examples = {"au::Placeholder(\"something went wrong\")"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Placeholder 的控件描述符（文字/三色/尺寸属性，无事件、无子项）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集 SignalViewBase 指针；Placeholder 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 序列化占位属性：文字与背景/边框/文字色（颜色经 color_to_json）。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("message", message_);
        props.set("background_color", color_to_json(background_));
        props.set("border_color", color_to_json(border_));
        props.set("text_color", color_to_json(text_));
    }

    /// @brief 反序列化占位属性：按存在的键逐一还原文字与三色，缺键保持当前值。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("message")) {
            message_ = props.at("message")->as_or<std::string>("");
        }
        if (props.contains("background_color")) {
            background_ = json_to_color(*props.at("background_color"));
        }
        if (props.contains("border_color")) {
            border_ = json_to_color(*props.at("border_color"));
        }
        if (props.contains("text_color")) {
            text_ = json_to_color(*props.at("text_color"));
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const Font f{.size_pt = 14.0F};
        const std::string s = message_.empty() ? "(placeholder)" : message_;
        const float w = render::FontEngine::measure_width(s, f) + 16.0F;
        const float h = render::FontEngine::measure_height(f) + 12.0F;
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, background_);
        p.draw_rect(bounds, border_);
        const Rect inner{.origin = Point{.x = bounds.origin.x + 6.0F, .y = bounds.origin.y + 6.0F},
                         .size = Size{.width = bounds.size.width - 12.0F, .height = bounds.size.height - 12.0F}};
        p.draw_text(inner, message_.empty() ? "(placeholder)" : message_, Font{.size_pt = 14.0F}, text_);
    }

  private:
    std::string message_;
    Color background_ = Color{0xF2U, 0xF2U, 0xF2U, 0xFFU};  // 浅灰底
    Color border_ = Color{0xC0U, 0x39U, 0x2BU, 0xFFU};  // 警示红
    Color text_ = Color{0x55U, 0x55U, 0x55U, 0xFFU};  // 深灰字
};

}  // namespace aurora

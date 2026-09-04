#pragma once

#include <string>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 降级视觉占位控件（需求 #18 / 降级视觉语言）。
 *
 * 当某个控件无法构建/反序列化、或需标注「此处缺失/错误」时，用本控件渲染为一个
 * 灰底、警示色边框、显示说明文字的盒子，使局部错误不致拖垮整棵 UI。
 *
 * @code
 *   auto fallback = au::Placeholder{ .message = "Button does not support serialization yet" };
 * @endcode
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Placeholder : public Widget {
  public:
    Placeholder() = default;
    explicit Placeholder(std::string msg) : message_(std::move(msg)) {}

    /// @brief 设置说明文字（链式）。
    auto set_message(std::string msg) -> Placeholder & {
        message_ = std::move(msg);
        return *this;
    }

    /// @brief 设置背景色（链式）。
    auto set_background_color(Color c) -> Placeholder & {
        background_ = c;
        return *this;
    }

    /// @brief 设置边框（警示）色（链式）。
    auto set_border_color(Color c) -> Placeholder & {
        border_ = c;
        return *this;
    }

    /// @brief 设置文字色（链式）。
    auto set_text_color(Color c) -> Placeholder & {
        text_ = c;
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Placeholder"; }

    /// @brief 运行时自描述（规格附录 B）。
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
                     .note = "文字色",
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
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["message"] = message_;
        props["background_color"] = color_to_json(background_);
        props["border_color"] = color_to_json(border_);
        props["text_color"] = color_to_json(text_);
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("message")) {
            message_ = props["message"].get<std::string>();
        }
        if (props.contains("background_color")) {
            background_ = json_to_color(props["background_color"]);
        }
        if (props.contains("border_color")) {
            border_ = json_to_color(props["border_color"]);
        }
        if (props.contains("text_color")) {
            text_ = json_to_color(props["text_color"]);
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

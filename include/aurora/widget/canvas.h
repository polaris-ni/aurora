#pragma once

#include <functional>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 自定义绘制画布（specification/04-widget.md §3.6）。
 *
 * 接受一个绘制回调 `onPaint(painter, bounds)`，在布局给定的矩形内自由绘制
 * （图形图表、自定义图形、原型验证等）。尺寸由 `width`/`height`（或默认 100x100）决定。
 *
 * @code
 *   au::Canvas(200, 100, [](Painter& p, Rect b){ p.fillRect(b, au::colors::Blue); });
 * @endcode
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Canvas : public Widget {
  public:
    using PaintFn = std::function<void(Painter &, const Rect &)>;

    Canvas() = default;
    Canvas(float w, float h, PaintFn on_paint) : width_(px(w)), height_(px(h)), on_paint_(std::move(on_paint)) {}

    Canvas(PaintFn on_paint) : on_paint_(std::move(on_paint)) {}

    auto width(Length v) -> Canvas & override {
        width_ = v;
        return *this;
    }
    auto height(Length v) -> Canvas & override {
        height_ = v;
        return *this;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Canvas"; }

    /// @brief 运行时自描述（规格附录 B）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Canvas",
            .properties =
                {
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
            .events = {"on_paint"},
            .children_policy = "none",
            .examples = {"au::Canvas(200, 100, [](Painter& p, Rect b){ p.fill_rect(b, au::colors::Blue); })"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["note"] = "Canvas paint callback is not serializable";
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        const float w = width_.kind == LengthKind::Fixed ? std::max(c.min.width, std::min(width_.value, c.max.width))
                                                         : std::max(c.min.width, std::min(100.0F, c.max.width));
        const float h = height_.kind == LengthKind::Fixed
                            ? std::max(c.min.height, std::min(height_.value, c.max.height))
                            : std::max(c.min.height, std::min(100.0F, c.max.height));
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        if (on_paint_) {
            on_paint_(p, bounds);
        }
    }

  private:
    Length width_ = auto_length();
    Length height_ = auto_length();
    PaintFn on_paint_;
};

}  // namespace aurora

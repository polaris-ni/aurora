#pragma once

#include <functional>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 自定义绘制画布（specification/04-widget.md §3.6）。
///
/// 接受一个绘制回调 `onPaint(painter, bounds)`，在布局给定的矩形内自由绘制
/// （图形图表、自定义图形、原型验证等）。尺寸由 `width`/`height`（或默认 100x100）决定。
///
/// @code
/// au::Canvas(200, 100, [](Painter& p, Rect b){ p.fillRect(b, au::colors::Blue); });
/// @endcode
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Canvas : public Widget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 绘制回调类型：签名 void(Painter &, const Rect &)，布局矩形内自由绘制。
    /// @param Painter 回调入参类型：画布绘制器引用。
    /// @param Rect 回调入参类型：本控件布局矩形引用。
    using PaintFn = std::function<void(Painter &, const Rect &)>;

    /// @brief 默认构造：宽高均为 auto（布局时按约束回退 100x100），无绘制回调。
    Canvas() = default;

    /// @brief 定宽高清布构造：w/h 经 px() 转定长，回调移动存入 on_paint_。
    /// @param w 画布宽度，经 px() 转为定长 Length。
    /// @param h 画布高度，经 px() 转为定长 Length。
    /// @param on_paint 绘制回调，移动存入 on_paint_；布局矩形内自由绘制。
    Canvas(float w, float h, PaintFn on_paint) : width_(px(w)), height_(px(h)), on_paint_(std::move(on_paint)) {}

    /// @brief 仅指定绘制回调构造：宽高保持 auto，布局时按约束回退 100x100。
    /// @param on_paint 绘制回调，移动存入 on_paint_。
    Canvas(PaintFn on_paint) : on_paint_(std::move(on_paint)) {}

    /// @brief 设置画布宽度约束。
    /// @param v 新的宽度 Length（Fixed 值直接生效；auto 时布局按约束回退 100）。
    /// @return 自身引用，便于链式调用。
    auto width(Length v) -> Canvas & override {
        width_ = v;
        return *this;
    }
    /// @brief 设置画布高度约束。
    /// @param v 新的高度 Length（Fixed 值直接生效；auto 时布局按约束回退 100）。
    /// @return 自身引用，便于链式调用。
    auto height(Length v) -> Canvas & override {
        height_ = v;
        return *this;
    }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Canvas"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Canvas"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Canvas 的控件描述符（尺寸属性、on_paint 事件与示例）。
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
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Canvas 的控件描述符（width/height/show 属性、on_paint 事件、单例示例）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集 SignalViewBase 指针；Canvas 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 序列化画布属性：基类通用属性之外附加一条不可序列化说明。
    /// @param props 目标 JSON 对象，写入 width/height/show 及 note 提示。
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

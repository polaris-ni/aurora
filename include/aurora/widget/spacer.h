#pragma once

#include "aurora/core/types.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 弹性空间。
///
/// 在 `Column`/`Row` 中吸收主轴方向的全部剩余自由空间，用于把相邻 widget 推到两端。
/// 自身无绘制。需要「无剩余空间时退化为 0 尺寸」时用 `Spacer(false)`。
///
/// 剩余空间按 flex 权重在布局阶段二分配，因此会先扣除 Spacer 之后各兄弟的基准尺寸——
/// `Top / Spacer / Bottom` 三段里 Bottom 仍占住自己的高度，不会被推出容器。
/// 父级主轴为无限（如 `Scroll` 内容轴）时无「剩余」可言，Spacer 退化为 0。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Spacer : public Widget {
  public:
    /// @brief 纯展示件默认不是 Tab 停点：只有挂上点击 / 手势 / 菜单 / 滚动 / 键盘认领时
    ///        才可聚焦（覆写基类 public virtual；分级默认见 specification/05 §4.2）。
    /// @return 具备输入语义时为 true，否则 false。
    [[nodiscard]] auto wants_focus() const -> bool override { return has_input_semantics(); }
    /// @brief 构造弹性空间并按 expand 同步 flex 权重修饰。
    /// @param expand 是否吸收主轴剩余空间；true 时挂 expand(1.0) 权重。
    explicit Spacer(bool expand = true) : expand_(expand) { apply_expand(); }

    /// @brief 类型名，供序列化与运行时自描述使用。
    /// @return C 字符串 "Spacer"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Spacer"; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return Spacer 的控件描述符（expand/尺寸属性，无事件、无子项）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Spacer",
            .properties =
                {
                    {.name = "expand",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "是否吸收剩余空间",
                     .json_type = "boolean"},
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
            .examples = {"au::Spacer()"},
        };
    }
    /// @brief 运行时自描述：转发静态描述符。
    /// @return Spacer 的控件描述符（expand/尺寸/show 属性，无事件、无子项）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集本控件的可订阅信号视图。
    /// @param out 输出参数，收集 SignalViewBase 指针；Spacer 无信号，恒不写入。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 序列化扩展属性：在基类通用属性之外写入 expand。
    /// @param props 目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["expand"] = expand_;
    }

    /// @brief 反序列化扩展属性：含 expand 键时还原并同步 flex 权重修饰。
    /// @param props 源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("expand")) {
            expand_ = props["expand"].get<bool>();
            apply_expand();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        // 占据父约束给出的全部可用空间（由父 FlexLayouter 决定自由空间分配）。
        if (expand_) {
            return c.constrain(Size{.width = c.max.width, .height = c.max.height});
        }
        return c.constrain(Size{.width = 0.0F, .height = 0.0F});
    }

    auto on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) -> void override {}

  private:
    /// @brief 把 expand 落到自身修饰链：FlexLayouter 按子项 `Modifier::flex_weight` 在阶段二分配剩余空间，
    /// 从而先扣除 Spacer 之后兄弟的基准尺寸；权重不进 props 序列化，故须随 expand 同步维护。
    auto apply_expand() -> void { modifier.set(expand_ ? Modifier().expand(1.0F) : Modifier()); }

    bool expand_ = false;
};

}  // namespace aurora

#pragma once

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 水平工具栏：子控件水平排列 + 背景/底部分隔线。
/// 子控件从左到右排列（垂直居中）；超出宽度的子项被裁剪（溢出菜单为后续增强）。
/// 对标 Qt `QToolBar`、WPF `ToolBar`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class ToolBar : public Container {
  public:
    ToolBar() = default;
    /// @brief 以子节点列表构造工具栏。
    /// @param children 初始子节点集合。
    explicit ToolBar(std::vector<Node> children) { children_ = std::move(children); }
    /// @brief 以初始化列表构造工具栏。
    /// @param kids 初始子节点集合。
    ToolBar(std::initializer_list<Node> kids) { set_children(kids); }

    /// @brief 控件类型名，供 Inspector 与序列化辨识。
    /// @return 字符串字面量 "ToolBar"。
    [[nodiscard]] auto type_name() const -> const char * override { return "ToolBar"; }

    /// @brief 静态自描述：属性键与示例子节点。
    /// @return 本控件的 WidgetDescriptor 描述表。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ToolBar",
            .properties =
                {
                    {.name = "bar_height",
                     .type = "float",
                     .default_value = "40.0",
                     .required = false,
                     .note = "Toolbar height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "4.0",
                     .required = false,
                     .note = "Child spacing (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "padding",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "Horizontal padding (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .examples = {"au::ToolBar{ btn1, btn2, au::Divider{} }"},
        };
    }
    /// @brief 实例自描述：委托 describe_static()。
    /// @return 本控件的 WidgetDescriptor 描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 本控件无对外可订阅信号，不登记任何项。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 设置栏高（链式）。
    /// @param h 栏高(dp)；非正值回退默认 40.0。
    /// @return *this，便于链式调用。
    auto set_bar_height(float h) -> ToolBar & {
        bar_height_ = h > 0.0F ? h : 40.0F;
        return *this;
    }
    /// @brief 当前栏高。
    /// @return bar_height_ 的即时值(dp)。
    [[nodiscard]] auto bar_height() const -> float { return bar_height_; }

    /// @brief 设置子项间距（链式）。
    /// @param g 间距(dp)；负值按 0 处理。
    /// @return *this，便于链式调用。
    auto set_gap(float g) -> ToolBar & {
        gap_ = g < 0.0F ? 0.0F : g;
        return *this;
    }

    /// @brief 序列化栏高、间距与内边距到 props。
    /// @param props 输出 JSON 对象，先写入基类属性再补充本控件字段。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("bar_height", bar_height_);
        props.set("gap", gap_);
        props.set("padding", padding_);
    }

    /// @brief 从 JSON 恢复可序列化字段；缺省键保留现值。
    /// @param props 序列化时写入的属性对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("bar_height")) {
            bar_height_ = props.at("bar_height")->as_or<float>(0.0F);
        }
        if (props.contains("gap")) {
            gap_ = props.at("gap")->as_or<float>(0.0F);
        }
        if (props.contains("padding")) {
            padding_ = props.at("padding")->as_or<float>(0.0F);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float w = c.max.is_finite() ? c.max.width : 640.0F;
        float x = padding_;
        for (Node &child : children_) {
            Constraints inner;
            inner.min = Size{.width = 0.0F, .height = 0.0F};
            // 子项按内容宽度测量（无界宽避免 Text 等控件填满整栏）
            inner.max = Size{.width = std::numeric_limits<float>::infinity(), .height = bar_height_ - 8.0F};
            const Size s = child.widget().layout(inner, ctx);
            const float y = (bar_height_ - s.height) * 0.5F;  // 垂直居中
            child.set_bounds(Rect{.origin = Point{.x = x, .y = y}, .size = s});
            x += s.width + gap_;
        }
        return c.constrain(Size{.width = w, .height = bar_height_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 背景 + 底部分隔线
        p.fill_rect(bounds, Color{250, 250, 252, 255});
        p.fill_rect(Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + bounds.size.height - 1.0F},
                         .size = Size{.width = bounds.size.width, .height = 1.0F}},
                    Color{225, 225, 229, 255});
        Container::on_paint(p, bounds, ctx);
    }

  private:
    float bar_height_ = 40.0F;
    float gap_ = 4.0F;
    float padding_ = 6.0F;
};

/// @brief 底部状态栏：多区域水平排列（左对齐 + 尾项右对齐）。
/// 常规子项从左向右排列；最后一个子项右对齐（常放版本号/坐标等）。
/// 对标 Qt `QStatusBar`、WPF `StatusBar`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
class StatusBar : public Container {
  public:
    StatusBar() = default;
    /// @brief 以子节点列表构造状态栏。
    /// @param children 初始子节点集合。
    explicit StatusBar(std::vector<Node> children) { children_ = std::move(children); }
    /// @brief 以初始化列表构造状态栏。
    /// @param kids 初始子节点集合。
    StatusBar(std::initializer_list<Node> kids) { set_children(kids); }

    /// @brief 控件类型名，供 Inspector 与序列化辨识。
    /// @return 字符串字面量 "StatusBar"。
    [[nodiscard]] auto type_name() const -> const char * override { return "StatusBar"; }

    /// @brief 静态自描述：属性键与示例子节点。
    /// @return 本控件的 WidgetDescriptor 描述表。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "StatusBar",
            .properties =
                {
                    {.name = "bar_height",
                     .type = "float",
                     .default_value = "24.0",
                     .required = false,
                     .note = "Status bar height (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "12.0",
                     .required = false,
                     .note = "Region spacing (dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .examples = {R"(au::StatusBar{ au::Text("Ready"), au::Text("Ln 1, Col 1") })"},
        };
    }
    /// @brief 实例自描述：委托 describe_static()。
    /// @return 本控件的 WidgetDescriptor 描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 本控件无对外可订阅信号，不登记任何项。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 设置栏高（链式）。
    /// @param h 栏高(dp)；非正值回退默认 24.0。
    /// @return *this，便于链式调用。
    auto set_bar_height(float h) -> StatusBar & {
        bar_height_ = h > 0.0F ? h : 24.0F;
        return *this;
    }
    /// @brief 当前栏高。
    /// @return bar_height_ 的即时值(dp)。
    [[nodiscard]] auto bar_height() const -> float { return bar_height_; }

    /// @brief 序列化栏高与区域间距到 props。
    /// @param props 输出 JSON 对象，先写入基类属性再补充本控件字段。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("bar_height", bar_height_);
        props.set("gap", gap_);
    }

    /// @brief 从 JSON 恢复可序列化字段；缺省键保留现值。
    /// @param props 序列化时写入的属性对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("bar_height")) {
            bar_height_ = props.at("bar_height")->as_or<float>(0.0F);
        }
        if (props.contains("gap")) {
            gap_ = props.at("gap")->as_or<float>(0.0F);
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float w = c.max.is_finite() ? c.max.width : 640.0F;
        Constraints inner;
        inner.min = Size{.width = 0.0F, .height = 0.0F};
        // 子项按内容宽度测量（无界宽避免 Text 等控件填满整栏）
        inner.max = Size{.width = std::numeric_limits<float>::infinity(), .height = bar_height_ - 4.0F};

        float x = padding_;
        for (std::size_t i = 0; i < children_.size(); ++i) {
            Node &child = children_[i];
            const Size s = child.widget().layout(inner, ctx);
            const float y = (bar_height_ - s.height) * 0.5F;
            if (i + 1 == children_.size() && children_.size() > 1) {
                // 尾项右对齐
                child.set_bounds(Rect{.origin = Point{.x = std::max(x, w - padding_ - s.width), .y = y}, .size = s});
            } else {
                child.set_bounds(Rect{.origin = Point{.x = x, .y = y}, .size = s});
                x += s.width + gap_;
            }
        }
        return c.constrain(Size{.width = w, .height = bar_height_});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 背景 + 顶部分隔线
        p.fill_rect(bounds, Color{248, 248, 250, 255});
        p.fill_rect(Rect{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = 1.0F}},
                    Color{225, 225, 229, 255});
        Container::on_paint(p, bounds, ctx);
    }

  private:
    float bar_height_ = 24.0F;
    float gap_ = 12.0F;
    float padding_ = 8.0F;
};

}  // namespace aurora

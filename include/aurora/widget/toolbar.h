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

/**
 * @brief 水平工具栏：子控件水平排列 + 背景/底部分隔线。
 *
 * 子控件从左到右排列（垂直居中）；超出宽度的子项被裁剪（溢出菜单为后续增强）。
 * 对标 Qt `QToolBar`、WPF `ToolBar`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ToolBar : public Container {
  public:
    ToolBar() = default;
    explicit ToolBar(std::vector<Node> children) { children_ = std::move(children); }
    ToolBar(std::initializer_list<Node> kids) { set_children(kids); }

    [[nodiscard]] auto type_name() const -> const char * override { return "ToolBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ToolBar",
            .properties =
                {
                    {.name = "bar_height",
                     .type = "float",
                     .default_value = "40.0",
                     .required = false,
                     .note = "工具栏高度(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "4.0",
                     .required = false,
                     .note = "子项间距(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "padding",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "左右内边距(dp)",
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
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 设置栏高（链式）。
    auto set_bar_height(float h) -> ToolBar & {
        bar_height_ = h > 0.0F ? h : 40.0F;
        return *this;
    }
    [[nodiscard]] auto bar_height() const -> float { return bar_height_; }

    /// @brief 设置子项间距（链式）。
    auto set_gap(float g) -> ToolBar & {
        gap_ = g < 0.0F ? 0.0F : g;
        return *this;
    }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["bar_height"] = bar_height_;
        props["gap"] = gap_;
        props["padding"] = padding_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("bar_height")) {
            bar_height_ = props["bar_height"].get<float>();
        }
        if (props.contains("gap")) {
            gap_ = props["gap"].get<float>();
        }
        if (props.contains("padding")) {
            padding_ = props["padding"].get<float>();
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

/**
 * @brief 底部状态栏：多区域水平排列（左对齐 + 尾项右对齐）。
 *
 * 常规子项从左向右排列；最后一个子项右对齐（常放版本号/坐标等）。
 * 对标 Qt `QStatusBar`、WPF `StatusBar`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class StatusBar : public Container {
  public:
    StatusBar() = default;
    explicit StatusBar(std::vector<Node> children) { children_ = std::move(children); }
    StatusBar(std::initializer_list<Node> kids) { set_children(kids); }

    [[nodiscard]] auto type_name() const -> const char * override { return "StatusBar"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "StatusBar",
            .properties =
                {
                    {.name = "bar_height",
                     .type = "float",
                     .default_value = "24.0",
                     .required = false,
                     .note = "状态栏高度(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                    {.name = "gap",
                     .type = "float",
                     .default_value = "12.0",
                     .required = false,
                     .note = "区域间距(dp)",
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
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 设置栏高（链式）。
    auto set_bar_height(float h) -> StatusBar & {
        bar_height_ = h > 0.0F ? h : 24.0F;
        return *this;
    }
    [[nodiscard]] auto bar_height() const -> float { return bar_height_; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["bar_height"] = bar_height_;
        props["gap"] = gap_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("bar_height")) {
            bar_height_ = props["bar_height"].get<float>();
        }
        if (props.contains("gap")) {
            gap_ = props["gap"].get<float>();
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

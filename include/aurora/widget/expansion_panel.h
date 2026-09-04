#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 折叠面板：标题头 + 可折叠内容区。
 *
 * 点击标题头切换展开/收起；`expanded()` 为响应式状态可订阅。
 * 收起时内容不参与布局（高度仅头部）；展开时头部下方显示内容。
 *
 * 对标 Flutter `ExpansionTile`、WPF `Expander`、SwiftUI `DisclosureGroup`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class ExpansionPanel : public SingleChild {
  public:
    ExpansionPanel() = default;
    ExpansionPanel(std::string header, Node content, bool initially_expanded = false)
        : SingleChild(std::move(content)), header_(std::move(header)) {
        expanded_.set(initially_expanded);
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "ExpansionPanel"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ExpansionPanel",
            .properties =
                {
                    {.name = "header",
                     .type = "string",
                     .default_value = "\"\"",
                     .required = true,
                     .note = "标题文本",
                     .json_type = "string"},
                    {.name = "expanded",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "是否展开",
                     .json_type = "boolean"},
                    {.name = "header_height",
                     .type = "float",
                     .default_value = "36.0",
                     .required = false,
                     .note = "标题头高度(dp)",
                     .json_type = "number",
                     .enum_values = {},
                     .min_value = "0"},
                },
            .events = {"on_toggle"},
            .children_policy = "single",
            .examples = {R"(au::ExpansionPanel("Details", au::Text("content"), false))"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&expanded_); }

    [[nodiscard]] auto expanded() -> State<bool> & { return expanded_; }
    [[nodiscard]] auto is_expanded() const -> bool { return expanded_.get(); }
    [[nodiscard]] auto header() const -> const std::string & { return header_; }

    /// @brief 展开/收起（触发 on_toggle）。
    auto set_expanded(bool v) -> void {
        if (v != expanded_.get()) {
            expanded_.set(v);
            mark_needs_layout();
            mark_needs_paint();
            if (on_toggle_) {
                on_toggle_(v);
            }
        }
    }

    /// @brief 切换状态。
    auto toggle() -> void { set_expanded(!expanded_.get()); }

    /// @brief 设置切换回调（链式）。
    auto set_on_toggle(std::function<void(bool)> cb) -> ExpansionPanel & {
        on_toggle_ = std::move(cb);
        return *this;
    }

    /// @brief 点击标题头切换展开。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && e.local_position.y < header_height_) {
            toggle();
            e.is_handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["header"] = header_;
        props["expanded"] = expanded_.get();
        props["header_height"] = header_height_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("header")) {
            header_ = props["header"].get<std::string>();
        }
        if (props.contains("expanded")) {
            expanded_.set(props["expanded"].get<bool>());
        }
        if (props.contains("header_height")) {
            header_height_ = props["header_height"].get<float>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float w = c.max.is_finite() ? c.max.width : 320.0F;
        float h = header_height_;
        if (expanded_.get() && child_) {
            Constraints inner;
            inner.min = Size{.width = 0.0F, .height = 0.0F};
            inner.max =
                Size{.width = w, .height = c.max.is_finite() ? std::max(0.0F, c.max.height - header_height_) : 1e9f};
            const Size cs = child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = header_height_}, .size = cs});
            h += cs.height;
        }
        return c.constrain(Size{.width = w, .height = h});
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = 13.0F;
        // 标题头
        const Rect head{.origin = bounds.origin, .size = Size{.width = bounds.size.width, .height = header_height_}};
        p.fill_rect(head, Color(246, 246, 248, 255));
        p.draw_rect(head, Color(228, 228, 232, 255));
        // 展开箭头 + 标题
        const Rect arrow_box{.origin = Point{.x = head.origin.x + 10.0F, .y = head.origin.y + 10.0F},
                             .size = Size{.width = 16.0F, .height = header_height_ - 20.0F}};
        p.draw_text(arrow_box, expanded_.get() ? "v" : ">", f, Color(100, 100, 105, 255));
        const Rect title_box{.origin = Point{.x = head.origin.x + 30.0F, .y = head.origin.y + 10.0F},
                             .size = Size{.width = head.size.width - 40.0F, .height = header_height_ - 20.0F}};
        p.draw_text(title_box, header_, f, Color(30, 30, 30, 255));
        // 内容
        if (expanded_.get() && child_) {
            const Rect cb = child_.bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            child_.widget().paint(p, global, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (local.y < header_height_) {
            return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
        }
        if (expanded_.get() && child_) {
            const Rect cb = child_.bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                return child_.widget().hit_test(local - cb.origin, global, ctx);
            }
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (local.y >= header_height_ && expanded_.get() && child_) {
            const Rect cb = child_.bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                return child_.widget().hit_test_chain(local - cb.origin, global, ctx);
            }
        }
        return {};
    }

  private:
    std::string header_;
    State<bool> expanded_{false};
    float header_height_ = 36.0F;
    std::function<void(bool)> on_toggle_;
};

}  // namespace aurora

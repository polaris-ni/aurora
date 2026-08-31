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
 * @brief 折叠面板（规格 §3.7）：标题头 + 可折叠内容区。
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
        : SingleChild(std::move(content)), m_header(std::move(header)) {
        m_expanded.set(initially_expanded);
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "ExpansionPanel"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "ExpansionPanel",
            .properties = {
                { .name = "header", .type = "string", .default_value = "\"\"", .required = true, .note = "标题文本", .json_type = "string" },
                { .name = "expanded", .type = "bool", .default_value = "false", .required = false, .note = "是否展开", .json_type = "boolean" },
                { .name = "header_height", .type = "float", .default_value = "36.0", .required = false, .note = "标题头高度(dp)", .json_type = "number", .enum_values = {}, .min_value = "0" },
            },
            .events = { "on_toggle" },
            .children_policy = "single",
            .examples = { R"(au::ExpansionPanel("Details", au::Text("content"), false))" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_expanded); }

    [[nodiscard]] auto expanded() -> State<bool> & { return m_expanded; }
    [[nodiscard]] auto is_expanded() const -> bool { return m_expanded.get(); }
    [[nodiscard]] auto header() const -> const std::string & { return m_header; }

    /// @brief 展开/收起（触发 on_toggle）。
    auto set_expanded(bool v) -> void {
        if (v != m_expanded.get()) {
            m_expanded.set(v);
            mark_needs_layout();
            mark_needs_paint();
            if (m_on_toggle) {
                m_on_toggle(v);
            }
        }
    }

    /// @brief 切换状态。
    auto toggle() -> void { set_expanded(!m_expanded.get()); }

    /// @brief 设置切换回调（链式）。
    auto set_on_toggle(std::function<void(bool)> cb) -> ExpansionPanel & {
        m_on_toggle = std::move(cb);
        return *this;
    }

    /// @brief 点击标题头切换展开。
    auto on_pointer_event(MouseEvent &e) -> void override {
        if (e.action == MouseAction::Press && e.local_position.y < m_header_height) {
            toggle();
            e.handled = true;
            return;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["header"] = m_header;
        props["expanded"] = m_expanded.get();
        props["header_height"] = m_header_height;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("header")) {
            m_header = props["header"].get<std::string>();
        }
        if (props.contains("expanded")) {
            m_expanded.set(props["expanded"].get<bool>());
        }
        if (props.contains("header_height")) {
            m_header_height = props["header_height"].get<float>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        const float w = c.max.is_finite() ? c.max.width : 320.0f;
        float h = m_header_height;
        if (m_expanded.get() && m_child) {
            Constraints inner;
            inner.min = Size{ .width = 0.0f, .height = 0.0f };
            inner.max =
                Size{ .width = w, .height = c.max.is_finite() ? std::max(0.0f, c.max.height - m_header_height) : 1e9f };
            const Size cs = m_child.widget().layout(inner, ctx);
            m_child.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = m_header_height }, .size = cs });
            h += cs.height;
        }
        return c.constrain(Size{ .width = w, .height = h });
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        Font f;
        f.size_pt = 13.0f;
        // 标题头
        const Rect head{ .origin = bounds.origin,
                         .size = Size{ .width = bounds.size.width, .height = m_header_height } };
        p.fill_rect(head, Color(246, 246, 248, 255));
        p.draw_rect(head, Color(228, 228, 232, 255));
        // 展开箭头 + 标题
        const Rect arrow_box{ .origin = Point{ .x = head.origin.x + 10.0f, .y = head.origin.y + 10.0f },
                              .size = Size{ .width = 16.0f, .height = m_header_height - 20.0f } };
        p.draw_text(arrow_box, m_expanded.get() ? "v" : ">", f, Color(100, 100, 105, 255));
        const Rect title_box{ .origin = Point{ .x = head.origin.x + 30.0f, .y = head.origin.y + 10.0f },
                              .size = Size{ .width = head.size.width - 40.0f, .height = m_header_height - 20.0f } };
        p.draw_text(title_box, m_header, f, Color(30, 30, 30, 255));
        // 内容
        if (m_expanded.get() && m_child) {
            const Rect cb = m_child.bounds();
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            m_child.widget().paint(p, global, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (local.y < m_header_height) {
            return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this
                                                                                                        : nullptr;
        }
        if (m_expanded.get() && m_child) {
            const Rect cb = m_child.bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
                return m_child.widget().hit_test(local - cb.origin, global, ctx);
            }
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (local.y >= m_header_height && m_expanded.get() && m_child) {
            const Rect cb = m_child.bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
                return m_child.widget().hit_test_chain(local - cb.origin, global, ctx);
            }
        }
        return {};
    }

  private:
    std::string m_header;
    State<bool> m_expanded{ false };
    float m_header_height = 36.0f;
    std::function<void(bool)> m_on_toggle;
};

} // namespace aurora

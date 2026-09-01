#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 锚定弹出层：非模态浮层，锚定在指定位置弹出。
 *
 * 与 `Dialog` 区分：`Dialog` 是模态阻塞（遮罩+居中），`Popup` 是非模态锚定浮层
 * （下拉菜单、自动补全、上下文菜单渲染的基础）。
 *
 * 布局语义：Popup 在常规流中占据零尺寸；打开时其内容以覆盖层形式绘制在锚点处，
 * 命中测试优先命中弹出内容；点击弹出内容之外时若 `dismiss_on_outside_click` 为
 * true 则自动关闭（经 OverlayHost 或外层派发逻辑调用 `handle_outside_click`）。
 *
 * 对标 Qt `QMenu` 弹出、WPF `Popup`、Flutter `showMenu`/`OverlayEntry`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Popup : public SingleChild {
  public:
    Popup() = default;
    explicit Popup(Node content) : SingleChild(std::move(content)) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "Popup"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Popup",
            .properties = {
                { .name="open", .type="bool", .default_value="false", .required=false, .note="是否打开", .json_type="boolean" },
                { .name="anchor_x", .type="float", .default_value="0", .required=false, .note="锚点 X（全局坐标）", .json_type="number" },
                { .name="anchor_y", .type="float", .default_value="0", .required=false, .note="锚点 Y（全局坐标）", .json_type="number" },
                { .name="dismiss_on_outside_click", .type="bool", .default_value="true", .required=false, .note="点击外部自动关闭", .json_type="boolean" },
            },
            .events = { "on_close" },
            .children_policy = "single",
            .examples = { "au::Popup(au::Text(\"menu\")).open_at(au::Point{100, 50})" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 在指定全局坐标打开弹出层（链式）。
    auto open_at(Point anchor) -> Popup & {
        m_anchor = anchor;
        m_open = true;
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }

    /// @brief 关闭弹出层。
    auto close() -> void {
        if (m_open) {
            m_open = false;
            mark_needs_paint();
            if (m_on_close) {
                m_on_close();
            }
        }
    }

    [[nodiscard]] auto is_open() const -> bool { return m_open; }
    [[nodiscard]] auto anchor() const -> Point { return m_anchor; }

    /// @brief 设置关闭回调（链式）。
    auto set_on_close(std::function<void()> cb) -> Popup & {
        m_on_close = std::move(cb);
        return *this;
    }

    /// @brief 设置点击外部是否自动关闭（默认 true，链式）。
    auto set_dismiss_on_outside_click(bool v) -> Popup & {
        m_dismiss_outside = v;
        return *this;
    }
    [[nodiscard]] auto dismiss_on_outside_click() const -> bool { return m_dismiss_outside; }

    /// @brief 设置弹出内容。
    auto set_content(Node content) -> void { m_child = std::move(content); }

    /// @brief 处理一次「全局点击」：命中弹出内容返回 false（不关闭）；
    /// 点击外部且允许 dismiss 则关闭并返回 true（已消费该点击）。
    auto handle_outside_click(Point global_pos) -> bool {
        if (!m_open) {
            return false;
        }
        const Rect content_box{ .origin = m_anchor, .size = m_content_size };
        if (content_box.contains(global_pos)) {
            return false;
        }
        if (m_dismiss_outside) {
            close();
            return true;
        }
        return false;
    }

    /// @brief 弹出内容的全局盒（打开时有效）。
    [[nodiscard]] auto content_bounds() const -> Rect { return Rect{ .origin = m_anchor, .size = m_content_size }; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["open"] = m_open;
        props["anchor_x"] = m_anchor.x;
        props["anchor_y"] = m_anchor.y;
        props["dismiss_on_outside_click"] = m_dismiss_outside;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("open")) {
            m_open = props["open"].get<bool>();
        }
        if (props.contains("anchor_x")) {
            m_anchor.x = props["anchor_x"].get<float>();
        }
        if (props.contains("anchor_y")) {
            m_anchor.y = props["anchor_y"].get<float>();
        }
        if (props.contains("dismiss_on_outside_click")) {
            m_dismiss_outside = props["dismiss_on_outside_click"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (m_open && m_child) {
            // 弹出内容按无界约束测量（浮层不受常规流约束限制）
            Constraints free;
            free.min = Size{ .width = 0.0f, .height = 0.0f };
            free.max = Size{ .width = c.max.is_finite() ? c.max.width : 4096.0f,
                             .height = c.max.is_finite() ? c.max.height : 4096.0f };
            m_content_size = m_child.widget().layout(free, ctx);
            m_child.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = m_content_size });
        } else {
            m_content_size = Size{ .width = 0.0f, .height = 0.0f };
        }
        // 常规流中占零尺寸（浮层不参与父布局）
        return c.constrain(Size{ .width = 0.0f, .height = 0.0f });
    }

    auto on_paint(Painter &p, const Rect & /*bounds*/, const BuildContext &ctx) -> void override {
        if (!m_open || !m_child) {
            return;
        }
        // 内容绘制在锚点处（全局坐标），叠加轻微投影提升层次感
        const Rect content_box{ .origin = m_anchor, .size = m_content_size };
        p.draw_shadow(content_box, 0.0f, 2.0f, 8.0f, Color(0, 0, 0, 48));
        m_child.widget().paint(p, content_box, ctx);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (!m_open || !m_child) {
            return nullptr;
        }
        // local 是相对本 Popup 布局盒的坐标；弹出内容在全局 m_anchor 处。
        // 将 local 换算为全局坐标后再映射到内容局部坐标。
        const Point global{ .x = bounds.origin.x + local.x, .y = bounds.origin.y + local.y };
        const Rect content_box{ .origin = m_anchor, .size = m_content_size };
        if (!content_box.contains(global)) {
            return nullptr;
        }
        const Point content_local{ .x = global.x - m_anchor.x, .y = global.y - m_anchor.y };
        return m_child.widget().hit_test(content_local, content_box, ctx);
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!m_open || !m_child) {
            return {};
        }
        const Point global{ .x = bounds.origin.x + local.x, .y = bounds.origin.y + local.y };
        const Rect content_box{ .origin = m_anchor, .size = m_content_size };
        if (!content_box.contains(global)) {
            return {};
        }
        const Point content_local{ .x = global.x - m_anchor.x, .y = global.y - m_anchor.y };
        return m_child.widget().hit_test_chain(content_local, content_box, ctx);
    }

  private:
    bool m_open = false;
    bool m_dismiss_outside = true;
    Point m_anchor{ .x = 0.0f, .y = 0.0f };
    Size m_content_size{ .width = 0.0f, .height = 0.0f };
    std::function<void()> m_on_close;
};

/**
 * @brief 覆盖层宿主：管理基础内容 + 多个浮层的 z-order。
 *
 * 子节点 [0] 为基础内容（占满可用空间）；[1..N] 为浮层（Popup 等），
 * 按序号从低到高绘制（后加的在上层）。命中测试自顶层向下：
 * 顶层浮层先命中；点击落空的浮层若允许 dismiss 则自动关闭。
 *
 * 对标 Flutter `Overlay`/`OverlayEntry`、WPF `AdornerLayer`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class OverlayHost : public Container {
  public:
    OverlayHost() = default;
    explicit OverlayHost(Node base) { m_children.push_back(std::move(base)); }

    [[nodiscard]] auto type_name() const -> const char * override { return "OverlayHost"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "OverlayHost",
            .properties = {},
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .examples = { "au::OverlayHost(au::Column{...})" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 追加一个浮层（返回浮层序号）。
    auto add_overlay(Node overlay) -> std::size_t {
        m_children.push_back(std::move(overlay));
        mark_needs_layout();
        return m_children.size() - 1;
    }

    /// @brief 移除指定序号的浮层（0 = 基础内容，不可移除）。
    auto remove_overlay(std::size_t index) -> void {
        if (index >= 1 && index < m_children.size()) {
            m_children.erase(m_children.begin() + static_cast<std::ptrdiff_t>(index));
            mark_needs_layout();
        }
    }

    /// @brief 浮层数量（不含基础内容）。
    [[nodiscard]] auto overlay_count() const -> std::size_t { return m_children.empty() ? 0 : m_children.size() - 1; }

    /// @brief 处理一次全局点击：自顶层向下询问各 Popup 浮层是否因外部点击而关闭。
    /// 返回 true 表示有浮层因此关闭（已消费该点击）。
    auto handle_outside_click(Point global_pos) -> bool {
        for (std::size_t i = m_children.size(); i > 1; --i) {
            if (auto *popup = dynamic_cast<Popup *>(&m_children[i - 1].widget())) {
                if (popup->handle_outside_click(global_pos)) {
                    return true;
                }
            }
        }
        return false;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{ .width = 0.0f, .height = 0.0f };
        }
        // 基础内容占满可用空间
        if (!m_children.empty()) {
            const Constraints base{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = self };
            const Size bs = m_children[0].widget().layout(base, ctx);
            if (!c.max.is_finite()) {
                self = bs;
            }
            m_children[0].set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bs });
        }
        // 浮层按自身需求测量（覆盖绘制，不参与流布局）
        for (std::size_t i = 1; i < m_children.size(); ++i) {
            const Constraints free{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = self };
            const Size os = m_children[i].widget().layout(free, ctx);
            m_children[i].set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = os });
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 低序号先绘制（基础内容在下、浮层在上）
        for (Node &child : m_children) {
            const Rect cb = child.bounds();
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            child.widget().paint(p, global, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 自顶层向下命中（浮层优先）
        for (std::size_t i = m_children.size(); i > 0; --i) {
            Node &child = m_children[i - 1];
            const Rect cb = child.bounds();
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            Widget *r = child.widget().hit_test(local - cb.origin, global, ctx);
            if (r != nullptr) {
                return r;
            }
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        for (std::size_t i = m_children.size(); i > 0; --i) {
            Node &child = m_children[i - 1];
            const Rect cb = child.bounds();
            const Rect global{ .origin =
                                   Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                               .size = cb.size };
            std::vector<HitNode> r = child.widget().hit_test_chain(local - cb.origin, global, ctx);
            if (!r.empty()) {
                return r;
            }
        }
        return {};
    }
};

} // namespace aurora

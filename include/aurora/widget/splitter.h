#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 分割器方向。
 */
enum class SplitterOrientation {
    Horizontal, ///< 左右两栏（分隔条垂直，可水平拖动）
    Vertical,   ///< 上下两栏（分隔条水平，可垂直拖动）
};

/**
 * @brief 可拖拽分割器：两区域按比例分配空间，拖动分隔条调整。
 *
 * `HSplitter{first, second}` 左右布局；`VSplitter{first, second}` 上下布局。
 * 比例存于响应式 `ratio()`，可订阅联动；`min_first`/`min_second` 限制两侧最小尺寸。
 *
 * 对标 Qt `QSplitter`、WPF `GridSplitter`、SwiftUI `HSplitView`/`VSplitView`。
 * @note Thread: main-thread only
 * @note Rebuildable: yes, via from_json
 */
class Splitter : public Widget {
  public:
    Splitter() = default;
    Splitter(SplitterOrientation orient, Node first, Node second, float initial_ratio = 0.5f)
        : m_orient(orient), m_first(std::move(first)), m_second(std::move(second)) {
        m_ratio.set(std::clamp(initial_ratio, 0.0f, 1.0f));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Splitter"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Splitter",
            .properties = {
                { .name = "orientation", .type = "SplitterOrientation", .default_value = "Horizontal", .required = false, .note = "分割方向" },
                { .name = "ratio", .type = "float", .default_value = "0.5", .required = false, .note = "第一区域占比(0~1)" },
                { .name = "min_first", .type = "float", .default_value = "50.0", .required = false, .note = "第一区域最小尺寸(dp)" },
                { .name = "min_second", .type = "float", .default_value = "50.0", .required = false, .note = "第二区域最小尺寸(dp)" },
                { .name = "handle_size", .type = "float", .default_value = "6.0", .required = false, .note = "分隔条厚度(dp)" },
            },
            .events = { "on_ratio_change" },
            .children_policy = "multiple",
            .examples = { "au::HSplitter(sidebar, content, 0.3f)" },
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&m_ratio); }

    /// @brief 当前比例（响应式）。
    [[nodiscard]] auto ratio() -> State<float> & { return m_ratio; }
    [[nodiscard]] auto ratio_value() const -> float { return m_ratio.get(); }

    /// @brief 设置比例（钳制到 min_first/min_second 允许的范围）。
    auto set_ratio(float r) -> void {
        const float clamped = clamp_ratio(r);
        if (clamped != m_ratio.get()) {
            m_ratio.set(clamped);
            mark_needs_layout();
            mark_needs_paint();
            if (m_on_ratio_change) {
                m_on_ratio_change(clamped);
            }
        }
    }

    /// @brief 设置两侧最小尺寸（链式）。
    auto set_min_sizes(float min_first, float min_second) -> Splitter & {
        m_min_first = std::max(0.0f, min_first);
        m_min_second = std::max(0.0f, min_second);
        return *this;
    }

    /// @brief 设置分隔条厚度（链式）。
    auto set_handle_size(float s) -> Splitter & {
        m_handle_size = s > 0.0f ? s : 6.0f;
        return *this;
    }

    /// @brief 设置比例变化回调（链式）。
    auto set_on_ratio_change(std::function<void(float)> cb) -> Splitter & {
        m_on_ratio_change = std::move(cb);
        return *this;
    }

    /// @brief 拖拽分隔条：Press 命中分隔条开始，Move 更新比例，Release 结束。
    auto on_pointer_event(MouseEvent &e) -> void override {
        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float pos_on_axis = horizontal ? e.local_position.x : e.local_position.y;
        const float total = horizontal ? m_total_size.width : m_total_size.height;

        switch (e.action) {
        case MouseAction::Press: {
            const float handle_start = first_extent();
            if (pos_on_axis >= handle_start && pos_on_axis <= handle_start + m_handle_size) {
                m_dragging = true;
                e.handled = true;
                return;
            }
            break;
        }
        case MouseAction::Move:
            if (m_dragging && total > 0.0f) {
                set_ratio((pos_on_axis - (m_handle_size * 0.5f)) / std::max(1.0f, total - m_handle_size));
                e.handled = true;
                return;
            }
            break;
        case MouseAction::Release:
            if (m_dragging) {
                m_dragging = false;
                e.handled = true;
                return;
            }
            break;
        default: break;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto is_dragging() const -> bool { return m_dragging; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["orientation"] = m_orient == SplitterOrientation::Horizontal ? "horizontal" : "vertical";
        props["ratio"] = m_ratio.get();
        props["min_first"] = m_min_first;
        props["min_second"] = m_min_second;
        props["handle_size"] = m_handle_size;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("orientation")) {
            m_orient = props["orientation"].get<std::string>() == "vertical" ? SplitterOrientation::Vertical
                                                                             : SplitterOrientation::Horizontal;
        }
        if (props.contains("ratio")) {
            m_ratio.set(props["ratio"].get<float>());
        }
        if (props.contains("min_first")) {
            m_min_first = props["min_first"].get<float>();
        }
        if (props.contains("min_second")) {
            m_min_second = props["min_second"].get<float>();
        }
        if (props.contains("handle_size")) {
            m_handle_size = props["handle_size"].get<float>();
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (m_first) {
            fn(m_first.widget());
        }
        if (m_second) {
            fn(m_second.widget());
        }
    }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        m_child_view.clear();
        if (m_first) {
            m_child_view.push_back(m_first);
        }
        if (m_second) {
            m_child_view.push_back(m_second);
        }
        return m_child_view;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{ .width = 400.0f, .height = 300.0f };
        }
        m_total_size = self;

        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float total_axis = horizontal ? self.width : self.height;
        const float avail = std::max(0.0f, total_axis - m_handle_size);
        const float first_len = clamp_ratio(m_ratio.get()) * avail;
        const float second_len = avail - first_len;

        if (m_first) {
            Constraints fc;
            if (horizontal) {
                fc.min = Size{ .width = first_len, .height = self.height };
                fc.max = Size{ .width = first_len, .height = self.height };
            } else {
                fc.min = Size{ .width = self.width, .height = first_len };
                fc.max = Size{ .width = self.width, .height = first_len };
            }
            m_first.widget().set_layout_parent(this);
            m_first.widget().layout(fc, ctx);
            m_first.set_bounds(horizontal ? Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                                  .size = Size{ .width = first_len, .height = self.height } }
                                          : Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                                                  .size = Size{ .width = self.width, .height = first_len } });
        }
        if (m_second) {
            Constraints sc;
            if (horizontal) {
                sc.min = Size{ .width = second_len, .height = self.height };
                sc.max = Size{ .width = second_len, .height = self.height };
            } else {
                sc.min = Size{ .width = self.width, .height = second_len };
                sc.max = Size{ .width = self.width, .height = second_len };
            }
            m_second.widget().set_layout_parent(this);
            m_second.widget().layout(sc, ctx);
            const float off = first_len + m_handle_size;
            m_second.set_bounds(horizontal ? Rect{ .origin = Point{ .x = off, .y = 0.0f },
                                                   .size = Size{ .width = second_len, .height = self.height } }
                                           : Rect{ .origin = Point{ .x = 0.0f, .y = off },
                                                   .size = Size{ .width = self.width, .height = second_len } });
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 两区域
        if (m_first) {
            const Rect cb = m_first.bounds();
            m_first.widget().paint(
                p,
                Rect{ .origin = Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                      .size = cb.size },
                ctx);
        }
        if (m_second) {
            const Rect cb = m_second.bounds();
            m_second.widget().paint(
                p,
                Rect{ .origin = Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                      .size = cb.size },
                ctx);
        }
        // 分隔条
        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float start = first_extent();
        const Rect handle = horizontal ? Rect{ .origin = Point{ .x = bounds.origin.x + start, .y = bounds.origin.y },
                                               .size = Size{ .width = m_handle_size, .height = bounds.size.height } }
                                       : Rect{ .origin = Point{ .x = bounds.origin.x, .y = bounds.origin.y + start },
                                               .size = Size{ .width = bounds.size.width, .height = m_handle_size } };
        p.fill_rect(handle, m_dragging ? Color{ 0, 122, 255, 120 } : Color{ 0, 0, 0, 24 });
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 分隔条区域命中自身（供拖拽）
        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float pos_on_axis = horizontal ? local.x : local.y;
        const float start = first_extent();
        if (pos_on_axis >= start && pos_on_axis <= start + m_handle_size) {
            return this;
        }
        // 两区域递归命中
        for (Node *child : { &m_first, &m_second }) {
            if (!*child) {
                continue;
            }
            const Rect cb = child->bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
                Widget *r = child->widget().hit_test(local - cb.origin, global, ctx);
                if (r != nullptr) {
                    return r;
                }
            }
        }
        return Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = bounds.size }.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float pos_on_axis = horizontal ? local.x : local.y;
        const float start = first_extent();
        if (pos_on_axis >= start && pos_on_axis <= start + m_handle_size) {
            return {}; // 分隔条即自身（基类组装时前置 this）
        }
        for (Node *child : { &m_first, &m_second }) {
            if (!*child) {
                continue;
            }
            const Rect cb = child->bounds();
            if (cb.contains(local)) {
                const Rect global{ .origin =
                                       Point{ .x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y },
                                   .size = cb.size };
                std::vector<HitNode> r = child->widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        if (m_first) {
            m_first.widget().mount(ctx);
        }
        if (m_second) {
            m_second.widget().mount(ctx);
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (m_first) {
            m_first.widget().tick(now);
        }
        if (m_second) {
            m_second.widget().tick(now);
        }
    }

  private:
    /// @brief 第一区域轴向长度（含比例钳制）。
    [[nodiscard]] auto first_extent() const -> float {
        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float total_axis = horizontal ? m_total_size.width : m_total_size.height;
        const float avail = std::max(0.0f, total_axis - m_handle_size);
        return clamp_ratio(m_ratio.get()) * avail;
    }

    /// @brief 把比例钳制到 min_first/min_second 允许的范围。
    [[nodiscard]] auto clamp_ratio(float r) const -> float {
        const bool horizontal = m_orient == SplitterOrientation::Horizontal;
        const float total_axis = horizontal ? m_total_size.width : m_total_size.height;
        const float avail = std::max(1.0f, total_axis - m_handle_size);
        float lo = m_min_first / avail;
        float hi = 1.0f - (m_min_second / avail);
        if (lo > hi) { // 空间不足以满足双方最小值时对半
            lo = hi = 0.5f;
        }
        return std::clamp(r, lo, hi);
    }

    SplitterOrientation m_orient = SplitterOrientation::Horizontal;
    Node m_first;
    Node m_second;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> m_child_view;
    State<float> m_ratio{ 0.5f };
    float m_min_first = 50.0f;
    float m_min_second = 50.0f;
    float m_handle_size = 6.0f;
    bool m_dragging = false;
    Size m_total_size{ .width = 0.0f, .height = 0.0f };
    std::function<void(float)> m_on_ratio_change;
};

/// @brief 便捷工厂：左右分栏。
[[nodiscard]] inline auto HSplitter(Node first, Node second, float initial_ratio = 0.5f) -> Splitter {
    return Splitter{ SplitterOrientation::Horizontal, std::move(first), std::move(second), initial_ratio };
}

/// @brief 便捷工厂：上下分栏。
[[nodiscard]] inline auto VSplitter(Node first, Node second, float initial_ratio = 0.5f) -> Splitter {
    return Splitter{ SplitterOrientation::Vertical, std::move(first), std::move(second), initial_ratio };
}

} // namespace aurora

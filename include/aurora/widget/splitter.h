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
enum class SplitterOrientation : std::uint8_t {
    Horizontal,  ///< 左右两栏（分隔条垂直，可水平拖动）
    Vertical,  ///< 上下两栏（分隔条水平，可垂直拖动）
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
    Splitter(SplitterOrientation orient, Node first, Node second, float initial_ratio = 0.5F)
        : orient_(orient), first_(std::move(first)), second_(std::move(second)) {
        ratio_.set(std::clamp(initial_ratio, 0.0F, 1.0F));
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Splitter"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Splitter",
            .properties =
                {
                    {.name = "orientation",
                     .type = "SplitterOrientation",
                     .default_value = "Horizontal",
                     .required = false,
                     .note = "分割方向"},
                    {.name = "ratio",
                     .type = "float",
                     .default_value = "0.5",
                     .required = false,
                     .note = "第一区域占比(0~1)"},
                    {.name = "min_first",
                     .type = "float",
                     .default_value = "50.0",
                     .required = false,
                     .note = "第一区域最小尺寸(dp)"},
                    {.name = "min_second",
                     .type = "float",
                     .default_value = "50.0",
                     .required = false,
                     .note = "第二区域最小尺寸(dp)"},
                    {.name = "handle_size",
                     .type = "float",
                     .default_value = "6.0",
                     .required = false,
                     .note = "分隔条厚度(dp)"},
                },
            .events = {"on_ratio_change"},
            .children_policy = "multiple",
            .examples = {"au::HSplitter(sidebar, content, 0.3f)"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&ratio_); }

    /// @brief 当前比例（响应式）。
    [[nodiscard]] auto ratio() -> State<float> & { return ratio_; }
    [[nodiscard]] auto ratio_value() const -> float { return ratio_.get(); }

    /// @brief 设置比例（钳制到 min_first/min_second 允许的范围）。
    auto set_ratio(float r) -> void {
        const float clamped = clamp_ratio(r);
        if (clamped != ratio_.get()) {
            ratio_.set(clamped);
            mark_needs_layout();
            mark_needs_paint();
            if (on_ratio_change_) {
                on_ratio_change_(clamped);
            }
        }
    }

    /// @brief 设置两侧最小尺寸（链式）。
    auto set_min_sizes(float min_first, float min_second) -> Splitter & {
        min_first_ = std::max(0.0F, min_first);
        min_second_ = std::max(0.0F, min_second);
        return *this;
    }

    /// @brief 设置分隔条厚度（链式）。
    auto set_handle_size(float s) -> Splitter & {
        handle_size_ = s > 0.0F ? s : 6.0F;
        return *this;
    }

    /// @brief 设置比例变化回调（链式）。
    auto set_on_ratio_change(std::function<void(float)> cb) -> Splitter & {
        on_ratio_change_ = std::move(cb);
        return *this;
    }

    /// @brief 拖拽分隔条：Press 命中分隔条开始，Move 更新比例，Release 结束。
    auto on_pointer_event(MouseEvent &e) -> void override {
        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float pos_on_axis = horizontal ? e.local_position.x : e.local_position.y;
        const float total = horizontal ? total_size_.width : total_size_.height;

        switch (e.action) {
            case MouseAction::Press: {
                const float handle_start = first_extent();
                if (pos_on_axis >= handle_start && pos_on_axis <= handle_start + handle_size_) {
                    dragging_ = true;
                    e.is_handled = true;
                    return;
                }
                break;
            }
            case MouseAction::Move:
                if (dragging_ && total > 0.0F) {
                    set_ratio((pos_on_axis - (handle_size_ * 0.5F)) / std::max(1.0F, total - handle_size_));
                    e.is_handled = true;
                    return;
                }
                break;
            case MouseAction::Release:
                if (dragging_) {
                    dragging_ = false;
                    e.is_handled = true;
                    return;
                }
                break;
            default:
                break;
        }
        Widget::on_pointer_event(e);
    }

    [[nodiscard]] auto is_dragging() const -> bool { return dragging_; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["orientation"] = orient_ == SplitterOrientation::Horizontal ? "horizontal" : "vertical";
        props["ratio"] = ratio_.get();
        props["min_first"] = min_first_;
        props["min_second"] = min_second_;
        props["handle_size"] = handle_size_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("orientation")) {
            orient_ = props["orientation"].get<std::string>() == "vertical" ? SplitterOrientation::Vertical
                                                                            : SplitterOrientation::Horizontal;
        }
        if (props.contains("ratio")) {
            ratio_.set(props["ratio"].get<float>());
        }
        if (props.contains("min_first")) {
            min_first_ = props["min_first"].get<float>();
        }
        if (props.contains("min_second")) {
            min_second_ = props["min_second"].get<float>();
        }
        if (props.contains("handle_size")) {
            handle_size_ = props["handle_size"].get<float>();
        }
    }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (first_) {
            fn(first_.widget());
        }
        if (second_) {
            fn(second_.widget());
        }
    }

    [[nodiscard]] auto child_nodes() const -> const std::vector<Node> & override {
        child_view_.clear();
        if (first_) {
            child_view_.push_back(first_);
        }
        if (second_) {
            child_view_.push_back(second_);
        }
        return child_view_;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 400.0F, .height = 300.0F};
        }
        total_size_ = self;

        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float total_axis = horizontal ? self.width : self.height;
        const float avail = std::max(0.0F, total_axis - handle_size_);
        const float first_len = clamp_ratio(ratio_.get()) * avail;
        const float second_len = avail - first_len;

        if (first_) {
            Constraints fc;
            if (horizontal) {
                fc.min = Size{.width = first_len, .height = self.height};
                fc.max = Size{.width = first_len, .height = self.height};
            } else {
                fc.min = Size{.width = self.width, .height = first_len};
                fc.max = Size{.width = self.width, .height = first_len};
            }
            first_.widget().set_layout_parent(this);
            first_.widget().layout(fc, ctx);
            first_.set_bounds(horizontal ? Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                                                .size = Size{.width = first_len, .height = self.height}}
                                         : Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                                                .size = Size{.width = self.width, .height = first_len}});
        }
        if (second_) {
            Constraints sc;
            if (horizontal) {
                sc.min = Size{.width = second_len, .height = self.height};
                sc.max = Size{.width = second_len, .height = self.height};
            } else {
                sc.min = Size{.width = self.width, .height = second_len};
                sc.max = Size{.width = self.width, .height = second_len};
            }
            second_.widget().set_layout_parent(this);
            second_.widget().layout(sc, ctx);
            const float off = first_len + handle_size_;
            second_.set_bounds(horizontal ? Rect{.origin = Point{.x = off, .y = 0.0F},
                                                 .size = Size{.width = second_len, .height = self.height}}
                                          : Rect{.origin = Point{.x = 0.0F, .y = off},
                                                 .size = Size{.width = self.width, .height = second_len}});
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 两区域
        if (first_) {
            const Rect cb = first_.bounds();
            first_.widget().paint(
                p,
                Rect{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                     .size = cb.size},
                ctx);
        }
        if (second_) {
            const Rect cb = second_.bounds();
            second_.widget().paint(
                p,
                Rect{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                     .size = cb.size},
                ctx);
        }
        // 分隔条
        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float start = first_extent();
        const Rect handle = horizontal ? Rect{.origin = Point{.x = bounds.origin.x + start, .y = bounds.origin.y},
                                              .size = Size{.width = handle_size_, .height = bounds.size.height}}
                                       : Rect{.origin = Point{.x = bounds.origin.x, .y = bounds.origin.y + start},
                                              .size = Size{.width = bounds.size.width, .height = handle_size_}};
        p.fill_rect(handle, dragging_ ? Color{0, 122, 255, 120} : Color{0, 0, 0, 24});
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 分隔条区域命中自身（供拖拽）
        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float pos_on_axis = horizontal ? local.x : local.y;
        const float start = first_extent();
        if (pos_on_axis >= start && pos_on_axis <= start + handle_size_) {
            return this;
        }
        // 两区域递归命中
        for (Node *child : {&first_, &second_}) {
            if (!*child) {
                continue;
            }
            const Rect cb = child->bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                Widget *r = child->widget().hit_test(local - cb.origin, global, ctx);
                if (r != nullptr) {
                    return r;
                }
            }
        }
        return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bounds.size}.contains(local) ? this : nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float pos_on_axis = horizontal ? local.x : local.y;
        const float start = first_extent();
        if (pos_on_axis >= start && pos_on_axis <= start + handle_size_) {
            return {};  // 分隔条即自身（基类组装时前置 this）
        }
        for (Node *child : {&first_, &second_}) {
            if (!*child) {
                continue;
            }
            const Rect cb = child->bounds();
            if (cb.contains(local)) {
                const Rect global{
                    .origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                    .size = cb.size};
                std::vector<HitNode> r = child->widget().hit_test_chain(local - cb.origin, global, ctx);
                if (!r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        if (first_) {
            first_.widget().mount(ctx);
        }
        if (second_) {
            second_.widget().mount(ctx);
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (first_) {
            first_.widget().tick(now);
        }
        if (second_) {
            second_.widget().tick(now);
        }
    }

  private:
    /// @brief 第一区域轴向长度（含比例钳制）。
    [[nodiscard]] auto first_extent() const -> float {
        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float total_axis = horizontal ? total_size_.width : total_size_.height;
        const float avail = std::max(0.0F, total_axis - handle_size_);
        return clamp_ratio(ratio_.get()) * avail;
    }

    /// @brief 把比例钳制到 min_first/min_second 允许的范围。
    [[nodiscard]] auto clamp_ratio(float r) const -> float {
        const bool horizontal = orient_ == SplitterOrientation::Horizontal;
        const float total_axis = horizontal ? total_size_.width : total_size_.height;
        const float avail = std::max(1.0F, total_axis - handle_size_);
        float lo = min_first_ / avail;
        float hi = 1.0F - (min_second_ / avail);
        if (lo > hi) {  // 空间不足以满足双方最小值时对半
            lo = hi = 0.5F;
        }
        return std::clamp(r, lo, hi);
    }

    SplitterOrientation orient_ = SplitterOrientation::Horizontal;
    Node first_;
    Node second_;
    /// @brief child_nodes() 视图缓存（const 方法返回引用需持久存储）。
    mutable std::vector<Node> child_view_;
    State<float> ratio_{0.5F};
    float min_first_ = 50.0F;
    float min_second_ = 50.0F;
    float handle_size_ = 6.0F;
    bool dragging_ = false;
    Size total_size_{.width = 0.0F, .height = 0.0F};
    std::function<void(float)> on_ratio_change_;
};

/// @brief 便捷工厂：左右分栏。
// NOLINTNEXTLINE(*-identifier-naming)
[[nodiscard]] inline auto HSplitter(Node first, Node second, float initial_ratio = 0.5F) -> Splitter {
    return Splitter{SplitterOrientation::Horizontal, std::move(first), std::move(second), initial_ratio};
}

/// @brief 便捷工厂：上下分栏。
/// // NOLINTNEXTLINE(*-identifier-naming)
[[nodiscard]] inline auto VSplitter(Node first, Node second, float initial_ratio = 0.5F) -> Splitter {
    return Splitter{SplitterOrientation::Vertical, std::move(first), std::move(second), initial_ratio};
}

}  // namespace aurora

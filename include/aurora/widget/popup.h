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
            .properties =
                {
                    {.name = "open",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "是否打开",
                     .json_type = "boolean"},
                    {.name = "anchor_x",
                     .type = "float",
                     .default_value = "0",
                     .required = false,
                     .note = "锚点 X（全局坐标）",
                     .json_type = "number"},
                    {.name = "anchor_y",
                     .type = "float",
                     .default_value = "0",
                     .required = false,
                     .note = "锚点 Y（全局坐标）",
                     .json_type = "number"},
                    {.name = "dismiss_on_outside_click",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "点击外部自动关闭",
                     .json_type = "boolean"},
                },
            .events = {"on_close"},
            .children_policy = "single",
            .examples = {"au::Popup(au::Text(\"menu\")).open_at(au::Point{100, 50})"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 在指定全局坐标打开弹出层（链式）。
    auto open_at(Point anchor) -> Popup & {
        anchor_ = anchor;
        open_ = true;
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }

    /// @brief 关闭弹出层。
    auto close() -> void {
        if (open_) {
            open_ = false;
            mark_needs_paint();
            if (on_close_) {
                on_close_();
            }
        }
    }

    [[nodiscard]] auto is_open() const -> bool { return open_; }
    [[nodiscard]] auto anchor() const -> Point { return anchor_; }

    /// @brief 设置关闭回调（链式）。
    auto set_on_close(std::function<void()> cb) -> Popup & {
        on_close_ = std::move(cb);
        return *this;
    }

    /// @brief 设置点击外部是否自动关闭（默认 true，链式）。
    auto set_dismiss_on_outside_click(bool v) -> Popup & {
        dismiss_outside_ = v;
        return *this;
    }
    [[nodiscard]] auto dismiss_on_outside_click() const -> bool { return dismiss_outside_; }

    /// @brief 设置弹出内容。
    auto set_content(Node content) -> void { child_ = std::move(content); }

    /// @brief 处理一次「全局点击」：命中弹出内容返回 false（不关闭）；
    /// 点击外部且允许 dismiss 则关闭并返回 true（已消费该点击）。
    auto handle_outside_click(Point global_pos) -> bool {
        if (!open_) {
            return false;
        }
        const Rect content_box{.origin = anchor_, .size = content_size_};
        if (content_box.contains(global_pos)) {
            return false;
        }
        if (dismiss_outside_) {
            close();
            return true;
        }
        return false;
    }

    /// @brief 弹出内容的全局盒（打开时有效）。
    [[nodiscard]] auto content_bounds() const -> Rect { return Rect{.origin = anchor_, .size = content_size_}; }

    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props["open"] = open_;
        props["anchor_x"] = anchor_.x;
        props["anchor_y"] = anchor_.y;
        props["dismiss_on_outside_click"] = dismiss_outside_;
    }

    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("open")) {
            open_ = props["open"].get<bool>();
        }
        if (props.contains("anchor_x")) {
            anchor_.x = props["anchor_x"].get<float>();
        }
        if (props.contains("anchor_y")) {
            anchor_.y = props["anchor_y"].get<float>();
        }
        if (props.contains("dismiss_on_outside_click")) {
            dismiss_outside_ = props["dismiss_on_outside_click"].get<bool>();
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (open_ && child_) {
            // 弹出内容按无界约束测量（浮层不受常规流约束限制）
            Constraints free;
            free.min = Size{.width = 0.0F, .height = 0.0F};
            free.max = Size{.width = c.max.is_finite() ? c.max.width : 4096.0F,
                            .height = c.max.is_finite() ? c.max.height : 4096.0F};
            content_size_ = child_.widget().layout(free, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = content_size_});
        } else {
            content_size_ = Size{.width = 0.0F, .height = 0.0F};
        }
        // 常规流中占零尺寸（浮层不参与父布局）
        return c.constrain(Size{.width = 0.0F, .height = 0.0F});
    }

    auto on_paint(Painter &p, const Rect & /*bounds*/, const BuildContext &ctx) -> void override {
        if (!open_ || !child_) {
            return;
        }
        // 内容绘制在锚点处（全局坐标），叠加轻微投影提升层次感
        const Rect content_box{.origin = anchor_, .size = content_size_};
        p.draw_shadow(content_box, 0.0F, 2.0F, 8.0F, Color(0, 0, 0, 48));
        child_.widget().paint(p, content_box, ctx);
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (!open_ || !child_) {
            return nullptr;
        }
        // local 是相对本 Popup 布局盒的坐标；弹出内容在全局 m_anchor 处。
        // 将 local 换算为全局坐标后再映射到内容局部坐标。
        const Point global{.x = bounds.origin.x + local.x, .y = bounds.origin.y + local.y};
        const Rect content_box{.origin = anchor_, .size = content_size_};
        if (!content_box.contains(global)) {
            return nullptr;
        }
        const Point content_local{.x = global.x - anchor_.x, .y = global.y - anchor_.y};
        return child_.widget().hit_test(content_local, content_box, ctx);
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        if (!open_ || !child_) {
            return {};
        }
        const Point global{.x = bounds.origin.x + local.x, .y = bounds.origin.y + local.y};
        const Rect content_box{.origin = anchor_, .size = content_size_};
        if (!content_box.contains(global)) {
            return {};
        }
        const Point content_local{.x = global.x - anchor_.x, .y = global.y - anchor_.y};
        return child_.widget().hit_test_chain(content_local, content_box, ctx);
    }

  private:
    bool open_ = false;
    bool dismiss_outside_ = true;
    Point anchor_{.x = 0.0F, .y = 0.0F};
    Size content_size_{.width = 0.0F, .height = 0.0F};
    std::function<void()> on_close_;
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
    explicit OverlayHost(Node base) { children_.push_back(std::move(base)); }

    [[nodiscard]] auto type_name() const -> const char * override { return "OverlayHost"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "OverlayHost",
            .properties = {},
            .events = {},
            .children_policy = "multiple",
            .allowed_child_types = {},
            .examples = {"au::OverlayHost(au::Column{...})"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 追加一个浮层（返回浮层序号）。
    auto add_overlay(Node overlay) -> std::size_t {
        children_.push_back(std::move(overlay));
        mark_needs_layout();
        return children_.size() - 1;
    }

    /// @brief 移除指定序号的浮层（0 = 基础内容，不可移除）。
    auto remove_overlay(std::size_t index) -> void {
        if (index >= 1 && index < children_.size()) {
            children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(index));
            mark_needs_layout();
        }
    }

    /// @brief 浮层数量（不含基础内容）。
    [[nodiscard]] auto overlay_count() const -> std::size_t { return children_.empty() ? 0 : children_.size() - 1; }

    /// @brief 处理一次全局点击：自顶层向下询问各 Popup 浮层是否因外部点击而关闭。
    /// 返回 true 表示有浮层因此关闭（已消费该点击）。
    auto handle_outside_click(Point global_pos) -> bool {
        for (std::size_t i = children_.size(); i > 1; --i) {
            if (auto *popup = dynamic_cast<Popup *>(&children_[i - 1].widget())) {
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
            self = Size{.width = 0.0F, .height = 0.0F};
        }
        // 基础内容占满可用空间
        if (!children_.empty()) {
            const Constraints base{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            const Size bs = children_[0].widget().layout(base, ctx);
            if (!c.max.is_finite()) {
                self = bs;
            }
            children_[0].set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = bs});
        }
        // 浮层按自身需求测量（覆盖绘制，不参与流布局）
        for (std::size_t i = 1; i < children_.size(); ++i) {
            const Constraints free{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            const Size os = children_[i].widget().layout(free, ctx);
            children_[i].set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = os});
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        // 低序号先绘制（基础内容在下、浮层在上）
        for (Node &child : children_) {
            const Rect cb = child.bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            child.widget().paint(p, global, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 自顶层向下命中（浮层优先）
        for (std::size_t i = children_.size(); i > 0; --i) {
            Node &child = children_[i - 1];
            const Rect cb = child.bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            Widget *r = child.widget().hit_test(local - cb.origin, global, ctx);
            if (r != nullptr) {
                return r;
            }
        }
        return nullptr;
    }

    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        for (std::size_t i = children_.size(); i > 0; --i) {
            Node &child = children_[i - 1];
            const Rect cb = child.bounds();
            const Rect global{.origin = Point{.x = bounds.origin.x + cb.origin.x, .y = bounds.origin.y + cb.origin.y},
                              .size = cb.size};
            std::vector<HitNode> r = child.widget().hit_test_chain(local - cb.origin, global, ctx);
            if (!r.empty()) {
                return r;
            }
        }
        return {};
    }
};

}  // namespace aurora

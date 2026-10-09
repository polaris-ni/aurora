#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "aurora/core/types.h"
#include "aurora/event/focus.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 锚定弹出层：非模态浮层，锚定在指定位置弹出。
///
/// 与 `Dialog` 区分：`Dialog` 是模态阻塞（遮罩+居中），`Popup` 是非模态锚定浮层
/// （下拉菜单、自动补全、上下文菜单渲染的基础）。
///
/// 布局语义：Popup 在常规流中占据零尺寸；打开时其内容以覆盖层形式绘制在锚点处，
/// 命中测试优先命中弹出内容；点击弹出内容之外时若 `dismiss_on_outside_click` 为
/// true 则自动关闭（经 OverlayHost 或外层派发逻辑调用 `handle_outside_click`）。
///
/// 对标 Qt `QMenu` 弹出、WPF `Popup`、Flutter `showMenu`/`OverlayEntry`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class Popup : public SingleChild {
  public:
    /// @brief 默认构造：内容为空、初始为关闭状态。
    Popup() = default;
    /// @brief 以弹出内容构造（初始为关闭状态）。
    /// @param content 弹出内容节点。
    explicit Popup(Node content) : SingleChild(std::move(content)) {}

    /// @brief 类型标识。
    /// @return 类型名字符串 "Popup"。
    [[nodiscard]] auto type_name() const -> const char * override { return "Popup"; }

    /// @brief 静态描述符：open/anchor_x/anchor_y/dismiss_on_outside_click 属性与 on_close 事件。
    /// @return Popup 的组件描述符。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Popup",
            .properties =
                {
                    {.name = "open",
                     .type = "bool",
                     .default_value = "false",
                     .required = false,
                     .note = "Open",
                     .json_type = "boolean"},
                    {.name = "anchor_x",
                     .type = "float",
                     .default_value = "0",
                     .required = false,
                     .note = "Anchor X (global coordinates)",
                     .json_type = "number"},
                    {.name = "anchor_y",
                     .type = "float",
                     .default_value = "0",
                     .required = false,
                     .note = "Anchor Y (global coordinates)",
                     .json_type = "number"},
                    {.name = "dismiss_on_outside_click",
                     .type = "bool",
                     .default_value = "true",
                     .required = false,
                     .note = "Close automatically when clicking outside",
                     .json_type = "boolean"},
                },
            .events = {"on_close"},
            .children_policy = "single",
            .examples = {"au::Popup(au::Text(\"menu\")).open_at(au::Point{100, 50})"},
        };
    }
    /// @brief 实例描述符。
    /// @return 转发 describe_static()。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集信号视图；Popup 无外露信号。
    /// @param out 信号视图累加表（恒不写入）。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 在指定全局坐标打开弹出层（链式）：压入焦点作用域，Tab 焦点关在本层内。
    /// @param anchor 弹出锚点（全局坐标）。
    /// @return *this（链式调用）。
    auto open_at(Point anchor) -> Popup & {
        if (!open_ && current_focus_manager() != nullptr) {
            current_focus_manager()->push_scope(this);
        }
        anchor_ = anchor;
        open_ = true;
        mark_needs_layout();
        mark_needs_paint();
        return *this;
    }

    /// @brief 关闭弹出层：弹出焦点作用域并恢复打开前焦点。
    auto close() -> void {
        if (open_) {
            if (current_focus_manager() != nullptr) {
                current_focus_manager()->pop_scope();
            }
            open_ = false;
            mark_needs_paint();
            if (on_close_) {
                on_close_();
            }
        }
    }

    /// @brief 是否处于打开状态。
    /// @return 打开为 true。
    [[nodiscard]] auto is_open() const -> bool { return open_; }
    /// @brief 当前弹出锚点。
    /// @return 最近一次 open_at 设置的全局坐标。
    [[nodiscard]] auto anchor() const -> Point { return anchor_; }

    /// @brief 设置关闭回调（链式）。
    /// @param cb 关闭时触发的回调。
    /// @return *this（链式调用）。
    auto set_on_close(std::function<void()> cb) -> Popup & {
        on_close_ = std::move(cb);
        return *this;
    }

    /// @brief 设置点击外部是否自动关闭（默认 true，链式）。
    /// @param v 为 true 时点击弹出内容之外自动关闭。
    /// @return *this（链式调用）。
    auto set_dismiss_on_outside_click(bool v) -> Popup & {
        dismiss_outside_ = v;
        return *this;
    }
    /// @brief 是否启用点击外部自动关闭。
    /// @return 启用为 true（默认）。
    [[nodiscard]] auto dismiss_on_outside_click() const -> bool { return dismiss_outside_; }

    /// @brief 设置弹出内容。
    /// @param content 新的内容节点。
    auto set_content(Node content) -> void { child_ = std::move(content); }

    /// @brief 处理一次「全局点击」：命中弹出内容返回 false（不关闭）；
    /// 点击外部且允许 dismiss 则关闭并返回 true（已消费该点击）。
    /// @param global_pos 点击位置（全局坐标）。
    /// @return 已消费该点击（外部点击触发关闭）为 true。
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
    /// @return 以 anchor_ 为原点、实测内容尺寸为大小的矩形。
    [[nodiscard]] auto content_bounds() const -> Rect { return Rect{.origin = anchor_, .size = content_size_}; }

    /// @brief 序列化弹出属性（open/anchor_x/anchor_y/dismiss_on_outside_click），先链入基类通用属性。
    /// @param props 输出 JSON 对象。
    auto serialize_props(Json &props) const -> void override {
        Widget::serialize_props(props);
        props.set("open", Json{open_});
        props.set("anchor_x", anchor_.x);
        props.set("anchor_y", anchor_.y);
        props.set("dismiss_on_outside_click", Json{dismiss_outside_});
    }

    /// @brief 从 JSON 恢复弹出属性，缺失键保持当前值；先链入基类。
    /// @param props 输入 JSON 对象。
    auto deserialize_props(const Json &props) -> void override {
        Widget::deserialize_props(props);
        if (props.contains("open")) {
            open_ = props.at("open")->as_or<bool>(false);
        }
        if (props.contains("anchor_x")) {
            anchor_.x = props.at("anchor_x")->as_or<float>(0.0F);
        }
        if (props.contains("anchor_y")) {
            anchor_.y = props.at("anchor_y")->as_or<float>(0.0F);
        }
        if (props.contains("dismiss_on_outside_click")) {
            dismiss_outside_ = props.at("dismiss_on_outside_click")->as_or<bool>(false);
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
        // local 是相对本 Popup 布局盒的坐标；弹出内容在全局 anchor_ 处。
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

    /// @brief 恒 false：本控件是「正面范式」，**不申报追加命中盒**。
    ///
    /// 弹出内容按 `anchor_`（**全局**坐标）绘制，与布局盒坐标系不一致，故基类的
    /// 「按 `bounds().origin` 折算逐层下探」在这里算不出正确结果。命中由本控件自己的
    /// `on_hit_test_chain` 在**自己的入口**里重映射并下降，无需祖先把它当追加盒折算。
    /// 覆写为 false 以免基类折算产出一个与派发链分叉的假申报。
    ///
    /// ⚠️ 这与「祖先**不需要**为本控件开闸」是两件事：祖先仍须问一次
    /// `covers_remapped_descendant`（见下）才能放行——本控件在常规流中占**零尺寸**盒，
    /// 基类按布局盒折算的两道闸对它恒判假。
    ///
    /// @return 恒 false。
    /// @note Side-effects: pure
    [[nodiscard]] auto covers_descendant_extra_hit_box(const Point & /*local*/, const BuildContext & /*ctx*/,
                                                       const Point & /*ancestor_offset*/) const -> bool override {
        return false;
    }

    /// @brief 命中侧可达区申报：把局部点换算到全局后判 `content_box` 是否含该点。
    ///
    /// 与本类 `on_hit_test_chain` 的下降口径**逐字同构**：同一个 `content_box`、同一套换算，故
    /// 「祖先闸认」与「本控件认」不会分叉。关闭态与无子节点返回 `false`（此时既不绘制也不命中）。
    ///
    /// ⚠️ `ancestor_offset` 在本入口是**完整全局原点**（x 与 y 都有），不是「视口坐标系 y」——
    /// 换算需要 x。这与 `covers_extra_hit_box` 家族的形参语义**不同**（那个只累加、供翻转判据
    /// 判「离视口多远」，不需要 x），故本入口不复用那个形参的语义，改由调用方直接给全局原点。
    ///
    /// @param local 待测点（本控件本地坐标）。
    /// @param ctx 构建上下文。
    /// @param self_origin 本控件的**全局原点**（调用方从 `bounds.origin + cb.origin` 取得）。
    /// @return 该点落在弹出内容盒内为 true。
    /// @note Side-effects: pure
    [[nodiscard]] auto covers_remapped_descendant_at(const Point &local, const BuildContext &ctx,
                                                     const Point &self_origin) const -> bool override {
        (void)ctx;
        if (!open_ || !child_) {
            return false;
        }
        const Rect content_box{.origin = anchor_, .size = content_size_};
        return content_box.contains(Point{.x = self_origin.x + local.x, .y = self_origin.y + local.y});
    }

    /// @brief 绘制侧放行：打开态且有内容时返回 true。
    ///
    /// 本控件在常规流中占**零尺寸**盒，而 `Container::on_paint` 的遮挡剔除闸按
    /// `global.intersects(clip)` 判定（`Rect::intersects` 是严格比较）⇒ 零尺寸盒恒假、整棵被跳过，
    /// 表现为「浮层没画出来且无任何报错」。与 `covers_remapped_descendant` **必须同改**。
    ///
    /// @return 打开态且有内容为 true。
    /// @note Side-effects: pure
    [[nodiscard]] auto paints_outside_layout_box_at() const -> bool override { return open_ && child_; }

    /// @brief 窗口盒基准重映射：打开态返回 `anchor_`，其上祖先链一概不参与。
    ///
    /// 与本类 `on_paint` / `on_hit_test_chain` 同源：两者下传给内容的盒原点都是 `anchor_`
    /// （**全局**坐标，`on_paint` 直接 `content_box{origin = anchor_}`、命中链用
    /// `content_box.contains(global)` 判），全程不参与 `Popup` 自身在树中的位置——
    /// 故 `window_bounds()` 沿父链递推到本层时必须以 `anchor_` **替换**已累加量并终止上溯，
    /// 而不是叠加（叠加会读成「`Popup` 的树上位置 + `anchor_`」，与派发链分叉）。
    ///
    /// 关闭态（`!open_`）与无子节点时返回 `std::nullopt`：此时内容未被布局、`content_size_`
    /// 为零盒，基准没有被重映射，维持 `Widget` 的缺省递推。
    ///
    /// @return `anchor_`（打开且有内容）；否则 `std::nullopt`。
    [[nodiscard]] auto child_content_origin() const -> std::optional<Point> override {
        if (!open_ || !child_) {
            return std::nullopt;
        }
        return anchor_;
    }

  private:
    bool open_ = false;
    bool dismiss_outside_ = true;
    Point anchor_{.x = 0.0F, .y = 0.0F};
    Size content_size_{.width = 0.0F, .height = 0.0F};
    std::function<void()> on_close_;
};

/// @brief 覆盖层宿主：管理基础内容 + 多个浮层的 z-order。
///
/// 子节点 [0] 为基础内容（占满可用空间）；[1..N] 为浮层（Popup 等），
/// 按序号从低到高绘制（后加的在上层）。命中测试自顶层向下：
/// 顶层浮层先命中；点击落空的浮层若允许 dismiss 则自动关闭。
///
/// 对标 Flutter `Overlay`/`OverlayEntry`、WPF `AdornerLayer`。
/// @note Thread: main-thread only
/// @note Rebuildable: yes, via from_json
///
class OverlayHost : public Container {
  public:
    /// @brief 默认构造：无基础内容与浮层。
    OverlayHost() = default;
    /// @brief 以基础内容构造覆盖层宿主。
    /// @param base 基础内容节点（子节点 [0]）。
    explicit OverlayHost(Node base) { children_.push_back(std::move(base)); }

    /// @brief 类型标识。
    /// @return 类型名字符串 "OverlayHost"。
    [[nodiscard]] auto type_name() const -> const char * override { return "OverlayHost"; }

    /// @brief 静态描述符：无属性与事件，子节点策略为 multiple。
    /// @return OverlayHost 的组件描述符。
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
    /// @brief 实例描述符。
    /// @return 转发 describe_static()。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    /// @brief 收集信号视图；OverlayHost 无外露信号。
    /// @param out 信号视图累加表（恒不写入）。
    auto collect_signals([[maybe_unused]] std::vector<SignalViewBase *> &out) -> void override {}

    /// @brief 追加一个浮层（返回可移除的浮层序号）。
    ///
    /// 宿主尚无基础内容时（`children_` 为空）新节点落在序号 0，而 0 恒被解释为**基础内容**、
    /// `remove_overlay` 拒收 ⇒ 返回 0 会让调用方持有一个永远删不掉的序号。此处以 `std::nullopt`
    /// 显式表达「本次追加未产生可移除浮层」，与 `remove_overlay` 的口径保持一致。
    ///
    /// 挂载时机：新浮层由 `Container` 的补挂机制在**本宿主下一次布局**时以父侧 ctx 挂上
    /// （`Widget::layout` 入口消费登记），调用方**无须**自备 `BuildContext` 或自行 `mount`。
    /// @param overlay 浮层节点（如 Popup）。
    /// @return 新浮层的序号（≥ 1）；宿主尚无基础内容时为 `std::nullopt`。
    [[nodiscard]] auto add_overlay(Node overlay) -> std::optional<std::size_t> {
        children_.push_back(std::move(overlay));
        note_pending_mount();
        mark_needs_layout();
        const std::size_t index = children_.size() - 1;
        return index == 0 ? std::nullopt : std::optional<std::size_t>{index};
    }

    /// @brief 移除指定序号的浮层（0 = 基础内容，不可移除）。
    /// @param index 浮层序号；越界或为 0 时不做任何事。
    auto remove_overlay(std::size_t index) -> void {
        if (index >= 1 && index < children_.size()) {
            children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(index));
            mark_needs_layout();
        }
    }

    /// @brief 浮层数量（不含基础内容）。
    /// @return 子节点数减一；无子节点时为 0。
    [[nodiscard]] auto overlay_count() const -> std::size_t { return children_.empty() ? 0 : children_.size() - 1; }

    /// @brief 处理一次全局点击：自顶层向下询问各 Popup 浮层是否因外部点击而关闭。
    /// 返回 true 表示有浮层因此关闭（已消费该点击）。
    /// @param global_pos 点击位置（全局坐标）。
    /// @return 有浮层因外部点击关闭为 true。
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

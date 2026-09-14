#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <utility>

#include "aurora/animation/spring.h"
#include "aurora/event/gesture.h"
#include "aurora/render/painter.h"
#include "aurora/widget/descriptor.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 拖动消除容器：child 沿主轴拖出（位移 + 透明度联动），飞出后从树上摘除。
 *
 * 内部持有 `DragToDismiss`（跟手 1:1 + 松手 spring 裁决）；`progress` 映射为
 * 主轴位移（`progress × travel`，travel 默认取布局主轴向尺寸）与透明度
 * （`1 − 0.5 × progress`，飞出途中渐隐；完全消失由位移出屏承担）。
 *
 * 摘除策略：spring 飞出完成后默认从最近 `Container` 祖先的 children 移除自身
 * （触发重排）；`on_dismissed(cb)` 注册自定义回调时可覆盖默认摘除（如列表数据删除
 * 后由数据层重建子树）。位移经 paint 平移实现，**不动布局盒**（动画不改布局，
 * spec §6.5 约束；摘除是离散操作，发生在动画完成后的下一帧）。
 *
 * 对标 Flutter `Dismissible`。
 *
 * @note Thread: main-thread only
 * @note Rebuildable: no（手势进度与消除回调为运行时态，工厂注册仅收录自描述元数据）
 */
class Dismissible : public SingleChild {
  public:
    explicit Dismissible(Node child, DragAxis axis = DragAxis::Horizontal,
                          SpringDescription spring = SpringDescription{})
        : SingleChild(std::move(child)), dtd_(axis, 200.0, spring) {
        // spring 阶段由每帧 tick 驱动，须开启 gesture-tick（同 ToastHost 模式）。
        needs_gesture_tick_ = true;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "Dismissible"; }

    [[nodiscard]] static auto describe_static() -> WidgetDescriptor {
        return WidgetDescriptor{
            .name = "Dismissible",
            .properties =
                {
                    {.name = "axis",
                     .type = "DragAxis",
                     .default_value = "Horizontal",
                     .required = false,
                     .note = "拖动主轴（Horizontal/Vertical）"},
                },
            .events = {"on_dismissed"},
            .children_policy = "single",
            .examples = {"au::Dismissible(card) /* 水平拖出消除 */"},
        };
    }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }

    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 注册消除回调（替代默认摘除：回调存在时不再自动从父容器移除）。
    auto on_dismissed(std::function<void()> cb) -> void { on_dismissed_ = std::move(cb); }

    /// @brief 跟手进度（0..1，诊断/联动绑定用）。
    [[nodiscard]] auto progress() const -> double { return dtd_.progress().get(); }

    /// @brief spring 阶段是否仍在推进（跟手不算动画）。
    [[nodiscard]] auto is_animating() const -> bool { return dtd_.is_animating(); }

    /// @brief 消除行程（逻辑 dp；默认 200，构造后可调）。
    double travel_distance = 200.0;

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        Size self = c.max;
        if (!c.max.is_finite()) {
            self = Size{.width = 200.0F, .height = 64.0F};
        }
        if (child_) {
            const Constraints inner{.min = Size{.width = 0.0F, .height = 0.0F}, .max = self};
            child_.widget().layout(inner, ctx);
            child_.set_bounds(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = self});
        }
        // 首次布局后按主轴向尺寸校准行程（同步给 DragToDismiss，跟手 1:1 与 paint 平移共用同值）。
        if (!travel_calibrated_) {
            travel_calibrated_ = true;
            const float axis_size =
                (dtd_axis_ == DragAxis::Horizontal) ? self.width : self.height;
            if (axis_size > 0.0F) {
                travel_distance = static_cast<double>(axis_size);
                dtd_.set_travel(travel_distance);
            }
        }
        return c.constrain(self);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (!child_) {
            return;
        }
        const double progress = dtd_.progress().get();
        if (progress <= 0.0) {
            child_.widget().paint(p, bounds, ctx);
            return;
        }
        // 位移 + 渐隐（progress 1 → 0.5 透明度）：paint 期平移，不动布局盒。
        const float shift = static_cast<float>(progress * travel_distance);
        const Rect shifted{.origin = Point{.x = bounds.origin.x + (dtd_axis_ == DragAxis::Horizontal ? shift : 0.0F),
                                           .y = bounds.origin.y + (dtd_axis_ == DragAxis::Vertical ? shift : 0.0F)},
                           .size = bounds.size};
        const float alpha = static_cast<float>(1.0 - (0.5 * progress));
        const double prev_alpha = p.global_alpha();
        p.set_alpha(prev_alpha * alpha);
        child_.widget().paint(p, shifted, ctx);
        p.set_alpha(prev_alpha);  // Painter 无栈：手动还原（见 set_alpha 契约）
    }

    auto on_pointer_event(MouseEvent &e) -> void {
        const bool was_dragging = dtd_.is_dragging();  // Release 会先结束识别，须先采样
        dtd_.on_mouse(e);
        if (e.action == MouseAction::Release && was_dragging) {
            dtd_.on_release();  // 松手裁决：阈值判飞出/回位，spring 接管
        }
        if (dtd_.is_dragging() || dtd_.is_animating()) {
            e.is_handled = true;  // 拖动/飞出中：消费事件，父级不响应点击。
        } else {
            SingleChild::on_pointer_event(e);
        }
    }

    auto on_pointer_event(TouchEvent &e) -> void {
        dtd_.on_touch(e);
        SingleChild::on_pointer_event(e);  // 修饰链照常
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        SingleChild::tick_gestures(now);
        // spring 推进：以墙钟差为 dt（tick 由 Application::tick 每帧驱动）。
        const double dt = last_tick_.has_value()
                              ? std::chrono::duration<double>(now - *last_tick_).count()
                              : (1.0 / 60.0);
        last_tick_ = now;
        const bool was_animating = dtd_.is_animating();
        dtd_.tick(dt);
        if (was_animating || dtd_.is_dragging()) {
            mark_needs_paint();
        }
        // spring 结束 + progress 已到 1 → 摘除。
        if (was_animating && !dtd_.is_animating() && dtd_.progress().get() >= 1.0) {
            fire_dismissed();
        }
    }

  private:
    auto fire_dismissed() -> void {
        if (on_dismissed_) {
            on_dismissed_();
            return;  // 自定义回调接管摘除语义。
        }
        // 默认：从最近 Container 祖先移除本节点。
        remove_from_parent_container();
    }

    auto remove_from_parent_container() -> void {
        Widget *w = layout_parent();
        while (w != nullptr) {
            if (auto *container = dynamic_cast<Container *>(w); container != nullptr) {
                container->remove_child(this);  // 公开接口；Node 随之析构，remove_child 内标脏重排
                return;
            }
            w = w->layout_parent();
        }
        // 无 Container 祖先（如直接作为根）：静默保持终态。
    }

    DragAxis dtd_axis_ = DragAxis::Horizontal;
    DragToDismiss dtd_;
    std::function<void()> on_dismissed_;
    std::optional<std::chrono::steady_clock::time_point> last_tick_;
    bool travel_calibrated_ = false;
};

}  // namespace aurora

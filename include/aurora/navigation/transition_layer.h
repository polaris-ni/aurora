#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

#include "aurora/animation/timeline.h"
#include "aurora/core/types.h"
#include "aurora/navigation/hero.h"
#include "aurora/navigation/route.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 转场合成层（specification/05-event-navigation.md §7.4）：在导航转场进行中叠加渲染「旧页」与「新页」两棵子树，
/// 按 `progress`（0→1）合成淡入淡出或水平滑动，进度到 1 后由 `NavigatorHost` 丢弃旧页。
///
/// 自身不持有动画：外部 `Animator` 通过 `AnimationController` 绑定 `progress` 状态驱动本层。
/// 两页均按本层满尺寸布局；`Fade` 用 `Painter::set_alpha` 做交叉淡变，`Slide` 用水平偏移矩形
/// 并在本层范围内裁剪。绘制顺序：旧页在下、新页在上。
///
class TransitionLayer : public Widget {
  public:
    /// @brief 构造转场合成层：装载新旧两页并绑定进度源与转场类型。
    /// @param old_root 旧页子树根（可空；转场结束由 `NavigatorHost` 丢弃）。
    /// @param new_root 新页子树根（可空）。
    /// @param progress 转场进度状态（0→1，外部 `Animator` 驱动；非空且生命周期须覆盖本层）。
    /// @param kind 转场类型（Fade 交叉淡变 / Slide 水平滑动）。
    TransitionLayer(Node old_root, Node new_root, State<double> *progress, TransitionKind kind)
        : old_(std::move(old_root)), new_(std::move(new_root)), progress_(progress), kind_(kind) {}

    /// @brief 接入 `NavigatorHost` 的 Hero 注册表：捕获共享元素几何并在覆盖层做插值绘制。
    /// @param reg Hero 注册表（持有捕获态，move 进本层）。
    /// @param morphing 本帧 morphing tag 集合（宿主持有、覆盖层填充；可空 = 不记录）。
    auto set_hero_registry(std::shared_ptr<HeroRegistry> reg, std::unordered_set<std::string> *morphing) -> void {
        hero_reg_ = std::move(reg);
        morphing_ = morphing;
    }

    /// @brief 运行时类型名。
    /// @return 固定字符串 "TransitionLayer"。
    [[nodiscard]] auto type_name() const -> const char * override { return "TransitionLayer"; }

    /// @brief 禁用 Display List 缓存：按 progress 每帧合成淡入淡出 / 滑动且绘制 Hero 覆盖层，
    ///        内容每帧变化，缓存回放会冻结转场。
    /// @return 恒为 false。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    /// @brief 运行时自描述。
    /// @return 描述符：名称 TransitionLayer、多孩子策略。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "TransitionLayer", .children_policy = "multiple"};
    }

    /// @brief 不自行订阅 progress_：由宿主（NavigatorHost）统一订阅，避免重复绑定同一信号。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    /// @brief 枚举两棵子页：旧页在前、新页在后（与绘制层序一致；空页跳过）。
    /// @param fn 对每个在场子页以 const 引用回调一次。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (old_) {
            fn(old_.widget());
        }
        if (new_) {
            fn(new_.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 两页均按本层满尺寸布局。
        if (old_) {
            old_.widget().layout(c, ctx);
        }
        if (new_) {
            new_.widget().layout(c, ctx);
        }
        return c.constrain(c.max);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const double t = clamp01(progress_->get());
        p.push_clip(bounds);  // 滑动时限制在新页/旧页可视范围内

        // 旧页/新页绘制矩形（Slide 时整体水平偏移）。
        Rect gb_old = bounds;
        Rect gb_new = bounds;
        if (kind_ != TransitionKind::Fade) {
            const float w = bounds.size.width;
            if (old_) {
                const auto ox = static_cast<float>(bounds.origin.x - (t * w));
                gb_old = Rect{.origin = Point{.x = ox, .y = bounds.origin.y}, .size = bounds.size};
            }
            const auto nx = static_cast<float>(bounds.origin.x + (old_ ? (1.0 - t) * w : 0.0F));
            gb_new = Rect{.origin = Point{.x = nx, .y = bounds.origin.y}, .size = bounds.size};
        }

        // ---- Hero 共享元素：绘制前重置捕获态 ----
        if (hero_reg_) {
            hero_reg_->source.clear();
            hero_reg_->target.clear();
            hero_reg_->capture_mode = HeroRegistry::CaptureMode::Source;
            if (morphing_ != nullptr) {
                morphing_->clear();
            }
        }

        // 绘制旧页（捕获 source）。
        if (old_) {
            p.set_alpha(kind_ == TransitionKind::Fade ? (1.0 - t) : 1.0);
            old_.widget().paint(p, gb_old, ctx);
        }

        // 绘制新页（捕获 target）。
        if (hero_reg_) {
            hero_reg_->capture_mode = HeroRegistry::CaptureMode::Target;
        }
        if (new_) {
            double alpha_new = 1.0;
            if (old_ && kind_ == TransitionKind::Fade) {
                alpha_new = t;
            }
            p.set_alpha(alpha_new);
            new_.widget().paint(p, gb_new, ctx);
        }
        p.set_alpha(1.0);

        // ---- Hero 共享元素：覆盖层做矩形 lerp + 交叉淡变 ----
        if (hero_reg_) {
            hero_reg_->capture_mode = HeroRegistry::CaptureMode::None;
            draw_hero_morph(p, ctx, t);
        }

        p.pop_clip();
        p.set_alpha(1.0);  // 复位全局透明度，避免污染后续绘制
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 新页在最上层，优先命中；未命中再回退旧页（转场中旧页仍可见）。
        if (new_) {
            Widget *hit = new_.widget().hit_test(local, bounds, ctx);
            if (hit != nullptr) {
                return hit;
            }
        }
        if (old_) {
            return old_.widget().hit_test(local, bounds, ctx);
        }
        return nullptr;
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        if (old_) {
            old_.widget().mount(ctx);
        }
        if (new_) {
            new_.widget().mount(ctx);
        }
    }

    /// @brief 递归卸载旧页与新页（与 `on_mount` 逐字对称）。
    ///
    /// 转场中两页同处一棵树、分别挂载，故任一页被换宿主重挂时另一页不受影响。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        if (old_) {
            old_.widget().unmount();
        }
        if (new_) {
            new_.widget().unmount();
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);  // 本节点修饰链（LongPress 等）
        if (old_) {
            old_.widget().tick(now);
        }
        if (new_) {
            new_.widget().tick(now);
        }
    }

  private:
    /// @brief 把新旧页中同 tag 的 Hero 配对，在插值矩形上交叉淡变绘制。
    auto draw_hero_morph(Painter &p, const BuildContext &ctx, double t) const -> void {
        for (auto &[tag, src] : hero_reg_->source) {
            auto it = hero_reg_->target.find(tag);
            if (it == hero_reg_->target.end()) {
                continue;  // 仅旧页有该 tag：退化为普通淡出，不崩溃。
            }
            HeroEntry &dst = it->second;
            const Rect r = lerp(src.bounds, dst.bounds, t);
            if (t < 1.0) {
                p.set_alpha(1.0 - t);
                src.child.widget().paint(p, r, ctx);
                p.set_alpha(1.0);
            }
            if (t > 0.0) {
                p.set_alpha(t);
                dst.child.widget().paint(p, r, ctx);
                p.set_alpha(1.0);
            }
            if (morphing_ != nullptr) {
                morphing_->insert(tag);
            }
        }
    }

    static auto clamp01(double v) -> double {
        if (v < 0.0) {
            return 0.0;
        }
        if (v > 1.0) {
            return 1.0;
        }
        return v;
    }

    Node old_;  ///< 旧页子树（转场结束由 NavigatorHost 丢弃）
    Node new_;  ///< 新页子树
    State<double> *progress_;  ///< 转场进度（外部 Animator 绑定驱动，非空）
    TransitionKind kind_;
    std::shared_ptr<HeroRegistry> hero_reg_;  ///< Hero 注册表（由 NavigatorHost 注入）。
    std::unordered_set<std::string> *morphing_ = nullptr;  ///< 本帧 morphing 的 tag 集合（宿主持有，覆盖层填充）。
};

}  // namespace aurora

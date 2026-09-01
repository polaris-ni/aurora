#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

#include "aurora/animation/timeline.h" // lerp(Rect)
#include "aurora/core/types.h"
#include "aurora/navigation/hero.h"  // HeroRegistry / HeroEntry
#include "aurora/navigation/route.h" // TransitionKind
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 转场合成层（specification/05-event-navigation.md §7.4）：在导航转场进行中叠加渲染「旧页」与「新页」两棵子树，
 * 按 `progress`（0→1）合成淡入淡出或水平滑动，进度到 1 后由 `NavigatorHost` 丢弃旧页。
 *
 * 自身不持有动画：外部 `Animator` 通过 `AnimationController` 绑定 `progress` 状态驱动本层。
 * 两页均按本层满尺寸布局；`Fade` 用 `Painter::set_alpha` 做交叉淡变，`Slide` 用水平偏移矩形
 * 并在本层范围内裁剪。绘制顺序：旧页在下、新页在上。
 */
class TransitionLayer : public Widget {
  public:
    TransitionLayer(Node old_root, Node new_root, State<double> *progress, TransitionKind kind)
        : m_old(std::move(old_root)), m_new(std::move(new_root)), m_progress(progress), m_kind(kind) {}

    /// @brief 接入 `NavigatorHost` 的 Hero 注册表：捕获共享元素几何并在覆盖层做插值绘制。
    auto set_hero_registry(std::shared_ptr<HeroRegistry> reg, std::unordered_set<std::string> *morphing) -> void {
        m_hero_reg = std::move(reg);
        m_morphing = morphing;
    }

    [[nodiscard]] auto type_name() const -> const char * override { return "TransitionLayer"; }

    // 按 progress 每帧合成淡入淡出 / 滑动且绘制 Hero 覆盖层：内容每帧变化，缓存回放会冻结转场，
    // 故不可缓存 Display List。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{ .name = "TransitionLayer", .children_policy = "multiple" };
    }

    // 不自行订阅 m_progress：由宿主（NavigatorHost）统一订阅，避免重复绑定同一信号。
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (m_old) {
            fn(m_old.widget());
        }
        if (m_new) {
            fn(m_new.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        // 两页均按本层满尺寸布局。
        if (m_old) {
            m_old.widget().layout(c, ctx);
        }
        if (m_new) {
            m_new.widget().layout(c, ctx);
        }
        return c.constrain(c.max);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        const double t = clamp01(m_progress->get());
        p.push_clip(bounds); // 滑动时限制在新页/旧页可视范围内

        // 旧页/新页绘制矩形（Slide 时整体水平偏移）。
        Rect gb_old = bounds;
        Rect gb_new = bounds;
        if (m_kind != TransitionKind::Fade) {
            const float w = bounds.size.width;
            if (m_old) {
                const auto ox = static_cast<float>(bounds.origin.x - (t * w));
                gb_old = Rect{ .origin = Point{ .x = ox, .y = bounds.origin.y }, .size = bounds.size };
            }
            const auto nx = static_cast<float>(bounds.origin.x + (m_old ? (1.0 - t) * w : 0.0f));
            gb_new = Rect{ .origin = Point{ .x = nx, .y = bounds.origin.y }, .size = bounds.size };
        }

        // ---- Hero 共享元素：绘制前重置捕获态 ----
        if (m_hero_reg) {
            m_hero_reg->source.clear();
            m_hero_reg->target.clear();
            m_hero_reg->capture_mode = HeroRegistry::CaptureMode::Source;
            if (m_morphing != nullptr) {
                m_morphing->clear();
            }
        }

        // 绘制旧页（捕获 source）。
        if (m_old) {
            p.set_alpha(m_kind == TransitionKind::Fade ? (1.0 - t) : 1.0);
            m_old.widget().paint(p, gb_old, ctx);
        }

        // 绘制新页（捕获 target）。
        if (m_hero_reg) {
            m_hero_reg->capture_mode = HeroRegistry::CaptureMode::Target;
        }
        if (m_new) {
            double alpha_new = 1.0;
            if (m_old && m_kind == TransitionKind::Fade) {
                alpha_new = t;
            }
            p.set_alpha(alpha_new);
            m_new.widget().paint(p, gb_new, ctx);
        }
        p.set_alpha(1.0);

        // ---- Hero 共享元素：覆盖层做矩形 lerp + 交叉淡变 ----
        if (m_hero_reg) {
            m_hero_reg->capture_mode = HeroRegistry::CaptureMode::None;
            draw_hero_morph(p, ctx, t);
        }

        p.pop_clip();
        p.set_alpha(1.0); // 复位全局透明度，避免污染后续绘制
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        // 新页在最上层，优先命中；未命中再回退旧页（转场中旧页仍可见）。
        if (m_new) {
            Widget *hit = m_new.widget().hit_test(local, bounds, ctx);
            if (hit != nullptr) {
                return hit;
            }
        }
        if (m_old) {
            return m_old.widget().hit_test(local, bounds, ctx);
        }
        return nullptr;
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        if (m_old) {
            m_old.widget().mount(ctx);
        }
        if (m_new) {
            m_new.widget().mount(ctx);
        }
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now); // 本节点修饰链（LongPress 等）
        if (m_old) {
            m_old.widget().tick(now);
        }
        if (m_new) {
            m_new.widget().tick(now);
        }
    }

  private:
    /// @brief 把新旧页中同 tag 的 Hero 配对，在插值矩形上交叉淡变绘制。
    auto draw_hero_morph(Painter &p, const BuildContext &ctx, double t) const -> void {
        for (auto &[tag, src] : m_hero_reg->source) {
            auto it = m_hero_reg->target.find(tag);
            if (it == m_hero_reg->target.end()) {
                continue; // 仅旧页有该 tag：退化为普通淡出，不崩溃。
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
            if (m_morphing != nullptr) {
                m_morphing->insert(tag);
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

    Node m_old;                ///< 旧页子树（转场结束由 NavigatorHost 丢弃）
    Node m_new;                ///< 新页子树
    State<double> *m_progress; ///< 转场进度（外部 Animator 绑定驱动，非空）
    TransitionKind m_kind;
    std::shared_ptr<HeroRegistry> m_hero_reg;              ///< Hero 注册表（由 NavigatorHost 注入）。
    std::unordered_set<std::string> *m_morphing = nullptr; ///< 本帧 morphing 的 tag 集合（宿主持有，覆盖层填充）。
};

} // namespace aurora

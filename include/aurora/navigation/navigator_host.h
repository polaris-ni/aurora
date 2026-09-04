#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/animation/easing.h"
#include "aurora/animation/timeline.h"
#include "aurora/core/types.h"
#include "aurora/navigation/hero.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/route.h"
#include "aurora/navigation/transition_layer.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/provider.h"
#include "aurora/widget/widget.h"

namespace aurora {

/**
 * @brief 导航宿主（specification/05-event-navigation.md §7.4）：包裹 `Navigator` 并在切换路由时驱动 `TransitionLayer`
 * 做淡入淡出/滑动转场。自身不拥有动画驱动，复用 `Application::animator()` 的帧循环。
 *
 * 用法：把 `NavigatorHost` 作为渲染根（present_root 的目标），调用 `push/pop` 切换页面。
 * `push` 带 `RouteTransition{ .animated = true }` 时自动合成转场；`progress` 由绑定的
 * `AnimationController` 经 `Animator` 每帧推进，到 1 后丢弃旧页。
 *
 * deep linking：通过 `Navigator::path()` / `restore()` 导出与恢复栈名序列。
 */
class NavigatorHost : public Widget {
  public:
    explicit NavigatorHost(Animator &anim) : anim_(anim) {}

    /// @brief 从 `Animator` 摘除本host注册的控制器与绑定。
    ///
    /// `begin_transition` 会把成员 `m_ctrl` / `m_progress` 注册进 `m_anim`（通常是
    /// `Application` 的长生命周期 Animator）。本 host 是 shared_ptr 持有的 widget，
    /// 可能因 `present_root` 换根或父子树重建而先于 Animator 销毁；若不摘除，
    /// 下一帧 `Animator::tick` 就会 tick 已释放的 `m_ctrl` 并写入已释放的 `m_progress`。
    ~NavigatorHost() override {
        if (bound_) {
            anim_.remove(ctrl_);
        }
    }

    NavigatorHost(const NavigatorHost &) = delete;
    auto operator=(const NavigatorHost &) -> NavigatorHost & = delete;
    NavigatorHost(NavigatorHost &&) = delete;
    auto operator=(NavigatorHost &&) -> NavigatorHost & = delete;

    /// @brief 压入新页面（成为当前页）。animated 时启动转场。
    auto push(Route route) -> void {
        const RouteTransition &tr = route.transition();
        if (tr.animated && nav_.current_root()) {
            old_ = nav_.current_root();
            kind_ = tr.kind;
            begin_transition(tr.duration_seconds);
        } else {
            old_ = Node{};
            transitioning_ = false;
        }
        nav_.push(std::move(route));
        rebuild_display();
    }

    /// @brief 替换栈顶（原地换页）。animated 时启动转场。
    auto push_replacement(Route route) -> void {
        const RouteTransition &tr = route.transition();
        if (tr.animated && nav_.current_root()) {
            old_ = nav_.current_root();
            kind_ = tr.kind;
            begin_transition(tr.duration_seconds);
        } else {
            old_ = Node{};
            transitioning_ = false;
        }
        nav_.push_replacement(std::move(route));
        rebuild_display();
    }

    /// @brief 弹栈；仅剩根路由时拒绝（返回 false）。转场默认淡出。
    [[nodiscard]] auto pop() -> bool {
        if (!nav_.can_pop()) {
            return false;
        }
        if (nav_.current_root()) {
            old_ = nav_.current_root();
            kind_ = TransitionKind::Fade;
            begin_transition(0.3);
        }
        const bool ok = nav_.pop();
        rebuild_display();
        return ok;
    }

    /// @brief 回到根路由（清空到仅剩首个）。转场默认淡出。
    auto pop_to_root() -> void {
        if (nav_.current_root()) {
            old_ = nav_.current_root();
            kind_ = TransitionKind::Fade;
            begin_transition(0.3);
        }
        nav_.pop_to_root();
        rebuild_display();
    }

    /// @brief 按 URI 字符串重建路由栈（deep linking）：直接替换整栈，无转场动画。
    auto open_uri(const std::string &uri, const std::function<Route(const std::string &)> &build) -> void {
        nav_.open_uri(uri, build);
        transitioning_ = false;
        old_ = Node{};
        morphing_tags_.clear();
        rebuild_display();
    }

    /// @brief 按 URI 字符串 + 路由表重建路由栈；表中缺失的名称段被跳过。
    auto open_uri(const std::string &uri, const RouteRegistry &registry) -> void {
        nav_.open_uri(uri, registry);
        transitioning_ = false;
        old_ = Node{};
        morphing_tags_.clear();
        rebuild_display();
    }

    [[nodiscard]] auto navigator() -> Navigator & { return nav_; }
    [[nodiscard]] auto navigator() const -> const Navigator & { return nav_; }

    /// @brief 读取 Hero 注册表（测试 / 调试用；常态由内部持有）。
    [[nodiscard]] auto hero_registry() const -> const std::shared_ptr<HeroRegistry> & { return hero_reg_; }

    /// @brief 栈变化回调（请求下一帧重绘，ARCHITECTURE.md §5.2）。
    auto set_on_route_changed(std::function<void()> cb) -> void { nav_.set_on_route_changed(std::move(cb)); }

    [[nodiscard]] auto type_name() const -> const char * override { return "NavigatorHost"; }

    // 转场宿主：每帧写入 morphing 标记并驱动 TransitionLayer 按 progress 合成，绘制含副作用且
    // 内容每帧变化；嵌套时亦须阻止祖先缓存其易变输出，故整体不可缓存 Display List。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "NavigatorHost", .children_policy = "single"};
    }

    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&progress_); }

    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (display_) {
            fn(display_.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (display_) {
            // 登记布局父节点：缓存失效沿布局父链向上传播依赖此链完整。
            // 此前遗漏 → Provider/AppShell 的 m_layout_parent 为 null → 后代 mark_needs_layout
            // 的失效传播到不了 NavigatorHost，其布局缓存永不失效 → 第二次整树重排命中缓存
            // 直接 return，AppShell/BodyView 等动态子控件永不重建（骨架→真实内容切换、banner
            // 出场等依赖重排的逻辑全部失效，表现为内容空白/淡灰）。
            display_.widget().set_layout_parent(this);
            display_.widget().layout(c, ctx);
        }
        return c.constrain(c.max);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        if (transitioning_ && progress_.get() >= 1.0) {
            // 转场完成：丢弃旧页并清空 morphing 标记，Hero 恢复正常自绘。
            transitioning_ = false;
            old_ = Node{};
            morphing_tags_.clear();
            rebuild_display();
        }
        hero_reg_->morphing = morphing_tags_;  // 写入本帧 morphing 标记（上一帧计算）。
        if (display_) {
            display_.widget().paint(p, bounds, ctx);
        }
    }

    auto on_hit_test(const Point &local, const Rect &bounds, const BuildContext &ctx) -> Widget * override {
        if (display_) {
            return display_.widget().hit_test(local, bounds, ctx);
        }
        return nullptr;
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        host_ctx_ = ctx;
        host_mounted_ = true;
        rebuild_display();
    }

    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override {
        Widget::tick_gestures(now);
        if (display_) {
            display_.widget().tick(now);
        }
    }

  private:
    auto begin_transition(double duration_seconds) -> void {
        transitioning_ = true;
        progress_.set(0.0);
        if (!bound_) {
            // 绑定一次：首条转场的时长/曲线作为全局配置（MVP 不逐路由重建控制器，避免重复注册）。
            ctrl_ = AnimationController{std::max(duration_seconds, 1e-6)};
            anim_.bind(ctrl_, Tween<double>{0.0, 1.0, Curves::ease_in_out()}, progress_);
            bound_ = true;
        }
        ctrl_.forward(0.0);
        mark_needs_layout();
    }

    auto rebuild_display() -> void {
        Node page = nav_.current_root();
        // 注入 Hero 注册表：把每个页用 Provider 包裹（旧页在上一轮已是包裹页），
        // 页内 Hero 经 Provider 环境读取注册表，常态零开销。
        auto wrap = [&](Node n) -> Node {
            if (!n) {
                return n;
            }
            return Node{Provider<std::shared_ptr<HeroRegistry>>(hero_reg_, std::move(n))};
        };
        if (transitioning_ && old_) {
            auto tl =
                std::make_shared<TransitionLayer>(wrap(std::move(old_)), wrap(std::move(page)), &progress_, kind_);
            tl->set_hero_registry(hero_reg_, &morphing_tags_);
            display_ = Node{std::move(tl)};
        } else {
            display_ = wrap(std::move(page));
        }
        if (host_mounted_ && display_) {
            display_.widget().mount(host_ctx_);  // 幂等：已挂载的页不会重复订阅信号
        }
    }

    Animator &anim_;
    Navigator nav_;
    AnimationController ctrl_{0.3};
    State<double> progress_{0.0};
    Node old_;
    Node display_;
    bool transitioning_ = false;
    TransitionKind kind_ = TransitionKind::Fade;
    bool bound_ = false;
    bool host_mounted_ = false;
    BuildContext host_ctx_;
    std::shared_ptr<HeroRegistry> hero_reg_ = std::make_shared<HeroRegistry>();  ///< Hero 注册表（注入子树环境）。
    std::unordered_set<std::string> morphing_tags_;  ///< 上一帧计算出的 morphing tag 集合（覆盖层填充）。
};

}  // namespace aurora

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

/// @brief 导航宿主（specification/05-event-navigation.md §7.4）：包裹 `Navigator` 并在切换路由时驱动 `TransitionLayer`
/// 做淡入淡出/滑动转场。自身不拥有动画驱动，复用 `Application::animator()` 的帧循环。
///
/// 用法：把 `NavigatorHost` 作为渲染根（present_root 的目标），调用 `push/pop` 切换页面。
/// `push` 带 `RouteTransition{ .animated = true }` 时自动合成转场；`progress` 由绑定的
/// `AnimationController` 经 `Animator` 每帧推进，到 1 后丢弃旧页。
///
/// deep linking：通过 `Navigator::path()` / `restore()` 导出与恢复栈名序列。
class NavigatorHost : public Widget {
  public:
    /// @brief 以外部动画驱动构造宿主（仅借引用，不拥有 `Animator`）。
    /// @param anim 帧循环来源：`begin_transition` 把控制器与进度信号绑定到它上（通常传 `Application::animator()`，
    ///             其寿命须覆盖本 host，否则析构时的摘除会悬空）。
    explicit NavigatorHost(Animator &anim) : anim_(anim) {}

    /// @brief 从 `Animator` 摘除本host注册的控制器与绑定。
    /// `begin_transition` 会把成员 `ctrl_` / `progress_` 注册进 `anim_`（通常是
    /// `Application` 的长生命周期 Animator）。本 host 是 shared_ptr 持有的 widget，
    /// 可能因 `present_root` 换根或父子树重建而先于 Animator 销毁；若不摘除，
    /// 下一帧 `Animator::tick` 就会 tick 已释放的 `ctrl_` 并写入已释放的 `progress_`。
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
    /// @param route 待压入的路由（转场形态与时长取自其 `transition()`）；栈空时同样入栈但无转场。
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
    /// @param route 替换后的路由（转场配置同 `push`）；栈空时等价于压入且不起转场。
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
    /// @return 弹出成功返回 true（此时以 0.3 秒淡出转场切到上一层）；栈深 ≤1 返回 false 且不改栈、不起转场。
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
    /// @param uri 以 '/' 分隔的路由名序列（空段丢弃），委托 `Navigator::open_uri` 解析。
    /// @param build 名称到 `Route` 的构造回调。
    auto open_uri(const std::string &uri, const std::function<Route(const std::string &)> &build) -> void {
        nav_.open_uri(uri, build);
        transitioning_ = false;
        old_ = Node{};
        morphing_tags_.clear();
        rebuild_display();
    }

    /// @brief 按 URI 字符串 + 路由表重建路由栈；表中缺失的名称段被跳过。
    /// @param uri 以 '/' 分隔的路由名序列（空段丢弃）。
    /// @param registry 名称 → 路由构造器的查表（`RouteRegistry`），委托给 `Navigator::open_uri`。
    auto open_uri(const std::string &uri, const RouteRegistry &registry) -> void {
        nav_.open_uri(uri, registry);
        transitioning_ = false;
        old_ = Node{};
        morphing_tags_.clear();
        rebuild_display();
    }

    /// @brief 取出内部导航器（调用方可直接读栈快照或调栈深上限）。
    /// @return 内部 `Navigator` 的可写引用（生命周期随本 host）。
    [[nodiscard]] auto navigator() -> Navigator & { return nav_; }

    /// @brief 取出内部导航器（只读重载，语义同可写版）。
    /// @return 内部 `Navigator` 的常量引用。
    [[nodiscard]] auto navigator() const -> const Navigator & { return nav_; }

    /// @brief 读取 Hero 注册表（测试 / 调试用；常态由内部持有）。
    /// @return 注入到页面环境的那份注册表 shared_ptr 的常量引用（恒非空）。
    [[nodiscard]] auto hero_registry() const -> const std::shared_ptr<HeroRegistry> & { return hero_reg_; }

    /// @brief 栈变化回调（请求下一帧重绘，ARCHITECTURE.md §5.2）。
    /// @param cb 每次栈内容变化后由 `Navigator` 同步调用的闭包；传空即解除挂接。
    auto set_on_route_changed(std::function<void()> cb) -> void { nav_.set_on_route_changed(std::move(cb)); }

    /// @brief 控件类型名（结构快照 JSON 用）。
    /// @return 字面量 `"NavigatorHost"`。
    [[nodiscard]] auto type_name() const -> const char * override { return "NavigatorHost"; }

    /// @brief 转场宿主整体不可缓存 Display List。
    /// 每帧写入 morphing 标记并驱动 `TransitionLayer` 按 `progress` 合成，绘制含副作用且内容每帧
    /// 变化；嵌套时亦须阻止祖先缓存其易变输出。
    /// @return 恒为 false。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    /// @brief 运行时自描述（规格附录 B）。
    /// @return 名为 "NavigatorHost"、子节点策略 "single" 的描述表（无自有属性）。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "NavigatorHost", .children_policy = "single"};
    }

    /// @brief 登记需订阅的信号视图：转场进度 `progress_`。
    /// @param out 输出向量：追加 `&progress_`（裸指针，非拥有），基类据此订阅转场进度变化并重绘。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override { out.push_back(&progress_); }

    /// @brief 遍历展示子树（换页/转场期间为 `TransitionLayer`，否则为当前页的 Provider 包装）。
    /// @param fn 对每个可见子控件调用一次；展示节点为空（未挂载或空栈）时不调用。
    auto for_each_child(const std::function<void(const Widget &)> &fn) const -> void override {
        if (display_) {
            fn(display_.widget());
        }
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        if (display_) {
            // 登记布局父节点：缓存失效沿布局父链向上传播依赖此链完整。
            // 此前遗漏 → Provider/AppShell 的 layout_parent_ 为 null → 后代 mark_needs_layout
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

    /// @brief 命中链同样委派给展示子树（同 `SingleChild` 的写法）。
    /// 事件派发走的是**命中链**而非 `on_hit_test`：`EventDispatcher::dispatch_mouse`
    /// 取 `Widget::hit_test_chain`，而链的后代部分只经 `on_hit_test_chain` 收集，基类默认
    /// 返回空。若只覆写 `on_hit_test`（那不是这条路径的入口），链恒为空 ⇒ 每次按下都被判为
    /// 「点击空白」（清焦点并 return false），页面内的点击/悬停/拖拽/滚轮全部到不了，导航点击失效。
    /// @param local 命中点（宿主局部坐标）。
    /// @param bounds 本控件绘制区域。
    /// @param ctx 构建上下文。
    /// @return 展示子树的命中链；无展示节点时为空向量。
    auto on_hit_test_chain(const Point &local, const Rect &bounds, const BuildContext &ctx)
        -> std::vector<HitNode> override {
        return display_ ? display_.widget().hit_test_chain(local, bounds, ctx) : std::vector<HitNode>{};
    }

    auto on_mount(const BuildContext &ctx) -> void override {
        host_ctx_ = ctx;
        host_mounted_ = true;
        rebuild_display();
    }

    /// @brief 卸载当前展示页（与 `on_mount` 对称）。
    ///
    /// 先落 `host_mounted_ = false`：它同时是「宿主已挂载」的判位与 `rebuild_display` 里「新页要不要补挂」
    /// 的闸，卸载后若还有换页发生，新页不得再挂到这份已失效的 `host_ctx_` 上。
    /// @param ctx 本控件挂载时记录的那份上下文。
    auto on_unmount(const BuildContext &ctx) -> void override {
        (void)ctx;
        host_mounted_ = false;
        host_ctx_ = BuildContext{};
        if (display_) {
            display_.widget().unmount();
        }
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
            // 绑定一次：时长取首条转场；曲线固定 Curves::ease_in_out()，逐路由 RouteTransition::curve 当前未接线（MVP
            // 不逐路由重建控制器，避免重复注册）。
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
        // 换页自带布局级失效：动画路径由 begin_transition 标脏，而 open_uri 与未开转场的 push 只走到
        // 这里——不标脏则新页要等下一次无关失效（窗口 resize、别的控件标脏）才上屏，表现为「点了没反应」。
        mark_needs_layout();
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
    // Hero 注册表（注入子树环境）。
    std::shared_ptr<HeroRegistry> hero_reg_ = std::make_shared<HeroRegistry>();
    std::unordered_set<std::string> morphing_tags_;  ///< 上一帧计算出的 morphing tag 集合（覆盖层填充）。
};

}  // namespace aurora

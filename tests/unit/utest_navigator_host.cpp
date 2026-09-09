/// 测试类型: unit
/// 目标单元: include/aurora/navigation/navigator_host.h
/// 测试说明: 覆盖 NavigatorHost 与 Navigator 的挂接——页面栈透传、非动画 push 的直绘展示、
/// 动画 push/pop/replace 的 TransitionLayer 合成与 Animator tick 进度推进、绘制完成丢弃旧页、
/// open_uri 无转场重建、自描述/信号收集/Hero 注册表、命中代理与析构时从 Animator 摘除

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/environment/build_context.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_navigator_host {

namespace {

/// 纯色填充探针叶控件：布局取满约束、绘制整块填充（像素断言无字体依赖）。
class SolidBox final : public LeafWidget {
  public:
    explicit SolidBox(Color fill = Color{255, 0, 0}) : fill_(fill) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "SolidBox"; }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(c.max);
    }

    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, fill_);
    }

  private:
    Color fill_;
};

auto solid_page(Color fill) -> Node {
    return Node{std::make_shared<SolidBox>(fill)};
}

auto bounded(float w, float h) -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = w, .height = h}};
}

auto full_rect(float w, float h) -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = w, .height = h}};
}

auto animated_transition() -> RouteTransition {
    return RouteTransition{.animated = true, .kind = TransitionKind::Fade, .duration_seconds = 0.3};
}

/// 收集 host 当前展示子树的类型名序列。
auto display_types(const NavigatorHost &host) -> std::vector<std::string> {
    std::vector<std::string> names;
    host.for_each_child([&names](const Widget &w) { names.emplace_back(w.type_name()); });
    return names;
}

}  // namespace

AURORA_TEST_CASE(host_push_builds_stack) {
    Animator anim;
    NavigatorHost host(anim);

    host.push(Route{solid_page(Color{255, 0, 0}), "home"});
    host.push(Route{solid_page(Color{0, 0, 255}), "detail"});

    AURORA_TEST_CHECK_EQ(host.navigator().depth(), 2U);
    AURORA_TEST_CHECK_EQ(host.navigator().current().name(), std::string{"detail"});
    AURORA_TEST_CHECK_TRUE(host.navigator().can_pop());
}

AURORA_TEST_CASE(host_unanimated_push_keeps_page_display) {
    Animator anim;
    NavigatorHost host(anim);
    host.push(Route{solid_page(Color{255, 0, 0}), "home"});
    host.push(Route{solid_page(Color{0, 160, 0}), "settings"});  // 未开转场

    // 无转场：展示层直接是 Provider 包裹的当前页，动画器保持空闲。
    const std::vector<std::string> types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"Provider"});
    AURORA_TEST_CHECK_FALSE(anim.has_active());
}

AURORA_TEST_CASE(host_animated_push_transition_lifecycle) {
    Animator anim;
    NavigatorHost host(anim);
    host.push(Route{solid_page(Color{255, 0, 0}), "home"});

    BuildContext ctx;
    host.mount(ctx);
    host.push(Route{solid_page(Color{0, 0, 255}), "detail", animated_transition()});

    // 转场开始：展示层切换为 TransitionLayer，控制器进入 Forward。
    std::vector<std::string> types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"TransitionLayer"});
    AURORA_TEST_CHECK_TRUE(anim.has_active());

    // 一帧推进跨过时长：进度到 1，动画器转闲置。
    anim.tick(0.4);
    std::vector<SignalViewBase *> sigs;
    host.collect_signals(sigs);
    AURORA_TEST_REQUIRE_EQ(sigs.size(), 1U);
    const auto *progress = dynamic_cast<const State<double> *>(sigs[0]);
    AURORA_TEST_REQUIRE_NOT_NULL(progress);
    AURORA_TEST_CHECK_NEAR(progress->get(), 1.0, 1e-4F);
    AURORA_TEST_CHECK_FALSE(anim.has_active());

    // 完成后绘制一帧：on_paint 丢弃旧页，展示层回到 Provider 包裹的新页。
    host.layout(bounded(80.0F, 60.0F), ctx);
    Painter p;
    p.begin(80, 60);
    host.paint(p, full_rect(80.0F, 60.0F), ctx);
    types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"Provider"});
    AURORA_TEST_CHECK_EQ(p.get_pixel(40, 30), Color{0, 0, 255});
}

AURORA_TEST_CASE(host_pop_refused_at_root) {
    Animator anim;
    NavigatorHost host(anim);
    host.push(Route{solid_page(Color{255, 0, 0}), "home"});

    AURORA_TEST_CHECK_FALSE(host.pop());
    AURORA_TEST_CHECK_EQ(host.navigator().depth(), 1U);
    AURORA_TEST_CHECK_FALSE(anim.has_active());  // 拒绝时不启动转场
    const std::vector<std::string> types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"Provider"});
}

AURORA_TEST_CASE(host_pop_transitions_and_completes) {
    Animator anim;
    NavigatorHost host(anim);
    host.push(Route{solid_page(Color{255, 0, 0}), "home"});
    host.push(Route{solid_page(Color{0, 0, 255}), "detail"});

    BuildContext ctx;
    host.mount(ctx);
    AURORA_TEST_CHECK_TRUE(host.pop());  // pop 默认 Fade 转场

    AURORA_TEST_CHECK_EQ(host.navigator().depth(), 1U);
    std::vector<std::string> types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"TransitionLayer"});
    AURORA_TEST_CHECK_TRUE(anim.has_active());

    anim.tick(0.4);
    host.layout(bounded(80.0F, 60.0F), ctx);
    Painter p;
    p.begin(80, 60);
    host.paint(p, full_rect(80.0F, 60.0F), ctx);
    types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"Provider"});
    AURORA_TEST_CHECK_EQ(p.get_pixel(40, 30), Color{255, 0, 0});  // 应回到 home 页
}

AURORA_TEST_CASE(host_push_replacement_transitions_in_place) {
    Animator anim;
    NavigatorHost host(anim);
    host.push(Route{solid_page(Color{255, 0, 0}), "home"});
    BuildContext ctx;
    host.mount(ctx);

    host.push_replacement(Route{solid_page(Color{0, 160, 0}), "edited", animated_transition()});

    // 原地换页：深度不变，走转场合成。
    AURORA_TEST_CHECK_EQ(host.navigator().depth(), 1U);
    AURORA_TEST_CHECK_EQ(host.navigator().current().name(), std::string{"edited"});
    std::vector<std::string> types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"TransitionLayer"});
    AURORA_TEST_CHECK_TRUE(anim.has_active());

    anim.tick(0.4);
    Painter p;
    p.begin(60, 60);
    host.layout(bounded(60.0F, 60.0F), ctx);
    host.paint(p, full_rect(60.0F, 60.0F), ctx);
    types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"Provider"});
    AURORA_TEST_CHECK_EQ(p.get_pixel(30, 30), Color{0, 160, 0});
}

AURORA_TEST_CASE(host_open_uri_replaces_stack_without_transition) {
    Animator anim;
    NavigatorHost host(anim);
    host.push(Route{solid_page(Color{255, 0, 0}), "home"});
    BuildContext ctx;
    host.mount(ctx);
    host.push(Route{solid_page(Color{0, 0, 255}), "detail"});

    const std::function<Route(const std::string &)> build = [](const std::string &name) -> Route {
        return Route{solid_page(Color{0, 160, 0}), name};
    };
    host.open_uri("alpha/beta", build);

    // 直接替换整栈、无转场动画：展示层立即回到普通页面。
    AURORA_TEST_CHECK_EQ(host.navigator().depth(), 2U);
    const std::vector<std::string> path = host.navigator().path();
    AURORA_TEST_REQUIRE_EQ(path.size(), 2U);
    AURORA_TEST_CHECK_STREQ(path[0], "alpha");
    AURORA_TEST_CHECK_STREQ(path[1], "beta");
    const std::vector<std::string> types = display_types(host);
    AURORA_TEST_REQUIRE_EQ(types.size(), 1U);
    AURORA_TEST_CHECK_EQ(types[0], std::string{"Provider"});
}

AURORA_TEST_CASE(host_self_description_and_signals) {
    Animator anim;
    NavigatorHost host(anim);

    AURORA_TEST_CHECK_STREQ(host.type_name(), "NavigatorHost");
    AURORA_TEST_CHECK_FALSE(host.can_cache_display_list());  // 转场宿主不可缓存 Display List

    const auto desc = host.describe();
    AURORA_TEST_CHECK_EQ(desc.name, std::string{"NavigatorHost"});
    AURORA_TEST_CHECK_EQ(desc.children_policy, std::string{"single"});

    std::vector<SignalViewBase *> sigs;
    host.collect_signals(sigs);
    AURORA_TEST_REQUIRE_EQ(sigs.size(), 1U);
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const State<double> *>(sigs[0]));  // progress 信号

    // Hero 注册表内部持有且实例稳定。
    AURORA_TEST_CHECK_NOT_NULL(host.hero_registry().get());
    AURORA_TEST_CHECK_EQ(host.hero_registry().get(), host.hero_registry().get());
}

AURORA_TEST_CASE(host_hit_test_delegates_to_current_page) {
    Animator anim;
    NavigatorHost host(anim);
    auto page = std::make_shared<SolidBox>(Color{255, 0, 0});
    host.push(Route{Node{page}, "home"});

    BuildContext ctx;
    Widget *hit = host.hit_test(Point{.x = 50.0F, .y = 25.0F}, full_rect(100.0F, 50.0F), ctx);
    AURORA_TEST_CHECK_EQ(hit, page.get());

    const Widget *miss = host.hit_test(Point{.x = 150.0F, .y = 25.0F}, full_rect(100.0F, 50.0F), ctx);
    AURORA_TEST_CHECK_NULL(miss);
}

AURORA_TEST_CASE(host_destructor_detaches_from_animator) {
    Animator anim;
    {
        NavigatorHost host(anim);
        host.push(Route{solid_page(Color{255, 0, 0}), "home"});
        host.push(Route{solid_page(Color{0, 0, 255}), "detail", animated_transition()});
        AURORA_TEST_CHECK_TRUE(anim.has_active());
    }

    // host 持有的控制器先于 Animator 析构：析构须摘除登记，帧推进不得触挂垂。
    AURORA_TEST_CHECK_FALSE(anim.has_active());
    anim.tick(1.0);
    AURORA_TEST_CHECK_FALSE(anim.has_active());
}

}  // namespace aurora::test_cases::utest_navigator_host

// 目标源单元：navigation/navigator.h + src/aurora/navigation/navigator.cpp

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/app/application.h"
#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"
#include "google_play_data.h"
#include "google_play_ui.h"
#include "test_harness.h"

using aurora::Animator;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Navigator;
using aurora::NavigatorHost;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Route;
using aurora::RouteRegistry;
using aurora::RouteTransition;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::TransitionKind;
using aurora::Widget;
using aurora::WidgetDescriptor;

namespace aurora::tests::sec_deeplink {

static void run() {
    Navigator nav;
    nav.push(Route{Node{}, "root"});

    auto build = [](const std::string &name) -> Route { return Route{Node{}, name}; };

    // 切分 + 重建栈
    nav.open_uri("home/detail/42", build);
    const auto p = nav.path();
    AURORA_TEST_CHECK(p.size() == 3);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p[0] == "home");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p[1] == "detail");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p[2] == "42");
    AURORA_TEST_CHECK(nav.current().name() == "42");

    // 丢弃空段（连续斜杠 / 尾斜杠）
    nav.open_uri("a//b/", build);
    const auto p2 = nav.path();
    AURORA_TEST_CHECK(p2.size() == 2);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p2[0] == "a");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p2[1] == "b");

    // RouteRegistry 触发，并跳过缺失名称段
    RouteRegistry reg;
    reg["x"] = [](const std::string &) -> Route { return Route{Node{}, "x"}; };
    reg["z"] = [](const std::string &) -> Route { return Route{Node{}, "z"}; };
    nav.open_uri("x/missing/z", reg);
    const auto p3 = nav.path();
    AURORA_TEST_CHECK(p3.size() == 2);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p3[0] == "x");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p3[1] == "z");

    // 空 URI 不破坏栈（restore 空名序列时直接返回）
    nav.open_uri("", reg);
    AURORA_TEST_CHECK(nav.path().size() == 2);

    AURORA_LOG_INFO("test", "deeplink_test: ALL PASS");
}
}  // namespace aurora::tests::sec_deeplink

namespace aurora::tests::sec_navigation_transition {

namespace {
/// @brief 整屏纯色页：用于转场时按像素断言旧页淡出 / 新页淡入。
struct SolidPage : Widget {
    Color bg_;
    explicit SolidPage(Color c) : bg_(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    void on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) override { p.fill_rect(bounds, bg_); }
};
}  // namespace

static auto make_page(Color bg) -> Node { return Node{SolidPage{bg}}; }

static auto pixel(const Painter &p, const int x, const int y) -> std::array<uint8_t, 4> {
    constexpr int w = 200;
    const uint8_t *d = p.data();
    const int i = ((y * w) + x) * 4;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
    return {d[i], d[i + 1], d[i + 2], d[i + 3]};
}

static void run() {
    constexpr int w = 200;
    constexpr int h = 140;
    Animator anim;
    NavigatorHost host{anim};

    RouteTransition fade;
    fade.animated = true;
    fade.kind = TransitionKind::Fade;
    fade.duration_seconds = 0.4;

    Painter painter;
    painter.begin(w, h);
    constexpr BuildContext ctx;
    host.mount(ctx);

    constexpr auto full{Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                             .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}}};

    // ---- 首屏（蓝），无转场 ----
    host.push(Route{make_page(Color::blue()), "a", RouteTransition{}});
    host.paint(painter, full, ctx);
    const auto p0 = pixel(painter, w / 2, h / 2);
    (void)p0;  // 在 NDEBUG 下 assert 可能为 no-op
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p0[2] > 150 && p0[1] < 80);  // 纯蓝

    // ---- 切入绿页（带淡入淡出转场） ----
    host.push(Route{make_page(Color::green()), "b", fade});
    for (int i = 0; i < 2; ++i) {
        anim.tick(0.1);
        host.paint(painter, full, ctx);  // 推进约 0.2s → progress≈0.5
    }
    const auto p_mid = pixel(painter, w / 2, h / 2);
    (void)p_mid;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p_mid[1] > 60 && p_mid[2] > 40);  // 蓝绿混合（旧蓝淡出 + 新绿淡入）

    for (int i = 0; i < 4; ++i) {
        anim.tick(0.1);
        host.paint(painter, full, ctx);  // 推进至 progress=1
    }
    const auto p_end = pixel(painter, w / 2, h / 2);
    (void)p_end;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(p_end[1] > 120 && p_end[2] < 80);  // 纯绿（转场完成，旧页已丢弃）

    // ---- deep linking ----
    const auto path = host.navigator().path();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(path.size() == 2 && path[0] == "a" && path[1] == "b");

    host.navigator().restore({"x", "y"}, [](const std::string &n) -> Route {
        return Route{make_page(n == "x" ? Color::red() : Color::yellow()), n};
    });
    const auto path2 = host.navigator().path();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(path2.size() == 2 && path2[0] == "x" && path2[1] == "y");
    AURORA_TEST_CHECK(host.navigator().current().name() == "y");

    AURORA_LOG_INFO("test", "navigation_transition_test: ALL PASS");
}
}  // namespace aurora::tests::sec_navigation_transition

namespace aurora::tests::sec_navigator_host_ops {

namespace {
/// @brief 纯色页：用于导航栈断言。
struct SolidPage : Widget {
    Color bg_;
    explicit SolidPage(Color c) : bg_(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SolidPage"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "SolidPage", .children_policy = "none"};
    }
    auto collect_signals(std::vector<SignalViewBase *> & /*out*/) -> void override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(c.max); }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void override {
        p.fill_rect(bounds, bg_);
    }
};
}  // namespace

static auto make_page(Color bg, const std::string &name) -> Route { return Route{Node{SolidPage{bg}}, name}; }

static void run() {
    AURORA_TEST_PRINTF("=== test_navigator_host_ops ===\n");

    // ---- 1. push + pop 基本栈操作 ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        // 初始页
        host.push(make_page(Color::red(), "root"));
        AURORA_TEST_CHECK(host.navigator().depth() == 1);
        AURORA_TEST_CHECK(host.navigator().current().name() == "root");

        // push 第二页（无动画）
        host.push(make_page(Color::green(), "page2"));
        AURORA_TEST_CHECK(host.navigator().depth() == 2);
        AURORA_TEST_CHECK(host.navigator().current().name() == "page2");

        // push 第三页
        host.push(make_page(Color::blue(), "page3"));
        AURORA_TEST_CHECK(host.navigator().depth() == 3);
        AURORA_TEST_CHECK(host.navigator().current().name() == "page3");

        // pop 回到 page2
        bool popped = host.pop();
        AURORA_TEST_CHECK(popped);
        AURORA_TEST_CHECK(host.navigator().depth() == 2);
        AURORA_TEST_CHECK(host.navigator().current().name() == "page2");
    }

    // ---- 2. pop_to_root 清空到根 ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        host.push(make_page(Color::red(), "root"));
        host.push(make_page(Color::green(), "a"));
        host.push(make_page(Color::blue(), "b"));
        host.push(make_page(Color::yellow(), "c"));
        AURORA_TEST_CHECK(host.navigator().depth() == 4);

        host.pop_to_root();
        AURORA_TEST_CHECK(host.navigator().depth() == 1);
        AURORA_TEST_CHECK(host.navigator().current().name() == "root");
    }

    // ---- 3. pop 到仅剩根路由时拒绝 ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        host.push(make_page(Color::red(), "root"));
        AURORA_TEST_CHECK(host.navigator().depth() == 1);

        bool refused = host.pop();
        AURORA_TEST_CHECK(!refused);
        AURORA_TEST_CHECK(host.navigator().depth() == 1);
    }

    // ---- 4. push_replacement 替换栈顶 ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        host.push(make_page(Color::red(), "root"));
        host.push(make_page(Color::green(), "old_page"));
        AURORA_TEST_CHECK(host.navigator().depth() == 2);
        AURORA_TEST_CHECK(host.navigator().current().name() == "old_page");

        // 替换栈顶
        host.push_replacement(make_page(Color::blue(), "new_page"));
        AURORA_TEST_CHECK(host.navigator().depth() == 2);  // 深度不变
        AURORA_TEST_CHECK(host.navigator().current().name() == "new_page");
    }

    // ---- 5. set_on_route_changed 回调 ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        int changes = 0;
        host.set_on_route_changed([&]() -> void { ++changes; });

        host.push(make_page(Color::red(), "root"));
        AURORA_TEST_CHECK(changes == 1);

        host.push(make_page(Color::green(), "page2"));
        AURORA_TEST_CHECK(changes == 2);

        bool popped = host.pop();
        AURORA_TEST_CHECK(popped);
        AURORA_TEST_CHECK(changes == 3);

        host.pop_to_root();  // 仅剩根，仍触发回调
        AURORA_TEST_CHECK(changes == 4);
    }

    // ---- 6. open_uri（builder 回调版） ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        host.push(make_page(Color::red(), "root"));

        // open_uri 重建栈
        host.open_uri("settings/profile", [](const std::string &seg) -> Route {
            const Color c = seg == "settings" ? Color::green() : Color::blue();
            return Route{Node{SolidPage{c}}, seg};
        });

        AURORA_TEST_CHECK(host.navigator().depth() == 2);
        AURORA_TEST_CHECK(host.navigator().current().name() == "profile");

        auto path = host.navigator().path();
        AURORA_TEST_CHECK(path.size() == 2);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(path[0] == "settings");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK(path[1] == "profile");
    }

    // ---- 7. open_uri（RouteRegistry 版） ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        host.push(make_page(Color::red(), "root"));

        RouteRegistry registry;
        registry["home"] = [](const std::string &) -> Route { return Route{Node{SolidPage{Color::green()}}, "home"}; };
        registry["detail"] = [](const std::string &) -> Route {
            return Route{Node{SolidPage{Color::blue()}}, "detail"};
        };

        host.open_uri("home/detail", registry);
        AURORA_TEST_CHECK(host.navigator().depth() == 2);
        AURORA_TEST_CHECK(host.navigator().current().name() == "detail");
    }

    // ---- 8. navigator() 访问器 ----
    {
        Animator anim;
        NavigatorHost host{anim};
        BuildContext ctx;
        host.mount(ctx);

        host.push(make_page(Color::red(), "root"));
        host.push(make_page(Color::green(), "page2"));

        const Navigator &nav = host.navigator();
        AURORA_TEST_CHECK(nav.depth() == 2);
        AURORA_TEST_CHECK(nav.can_pop());
        AURORA_TEST_CHECK(nav.current().name() == "page2");
    }
}
}  // namespace aurora::tests::sec_navigator_host_ops

namespace aurora::tests::sec_navigator_layout_cache {

// 统计 [y0,y1) 行带内「彩色」像素数（alpha>0 且 RGB 极差>20），用于判断 banner 渐变是否绘制。
static auto count_colorful_band(const Surface &s, int y0, int y1) -> long {
    const int w = static_cast<int>(s.size().width);
    const int h = static_cast<int>(s.size().height);
    const std::uint8_t *buf = s.data();
    long cnt = 0;
    for (int y = y0; y < y1 && y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = ((y * w) + x) * 4;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int r = buf[i];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int g = buf[i + 1];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int b = buf[i + 2];
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
            const int a = buf[i + 3];
            const int mx = std::max({r, g, b});
            const int mn = std::min({r, g, b});
            if (a > 0 && (mx - mn) > 20) {
                ++cnt;
            }
        }
    }
    return cnt;
}

static void run() {
    auto &repo = gp::repository();
    Animator anim;
    Reactive dark{false};
    auto host = std::make_shared<NavigatorHost>(anim);
    auto on_open = [&](const std::string &) -> void {};
    host->push(Route{Node{std::make_shared<gp::ui::AppShell>(&repo, on_open, &dark)}, "home", {}});

    Scene scene{Node{host}};
    WindowOptions opts;
    opts.size = Size{.width = 1100.0F, .height = 760.0F};
    auto win_res = create_window(HeadlessOptions{opts});
    AURORA_TEST_CHECK(static_cast<bool>(win_res));
    Application app{std::move(scene), std::move(win_res.value()), opts};
    app.set_on_frame([&anim]() -> void { anim.tick(1.0 / 60.0); });

    auto *win = app.window();
    auto root = app.scene().root_node();

    (void)win->present_root(root);
    const long f1 = count_colorful_band(win->surface(), 56, 252);
    AURORA_TEST_PRINTF("frame1 banner band colorful = %ld (first frame should be nearly invisible)\n", f1);
    AURORA_TEST_CHECK_MSG(f1 < 1000, "entrance-animation first-frame banner should be nearly invisible (alpha≈0)");

    for (int i = 0; i < 60; ++i) {
        (void)win->present_root(root);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const long f_n = count_colorful_band(win->surface(), 56, 252);
    AURORA_TEST_PRINTF("frame60 banner band colorful = %ld (should be colorful after entrance)\n", f_n);

    // 整帧扫描：body 区（y∈[48,696)）应有彩色内容，证明骨架→真实内容切换已发生。
    const auto &s = win->surface();
    const int h = static_cast<int>(s.size().height);
    long body = 0;
    for (int y = 48; y < 696 && y < h; y += 8) {
        body += count_colorful_band(s, y, y + 8);
    }
    AURORA_TEST_PRINTF("frame60 body(48,696) colorful = %ld\n", body);

    AURORA_TEST_CHECK_MSG(f_n > 1000,
                          "under real AppShell, banner after entrance animation should show color gradient "
                          "(no longer stuck on first frame)");
    AURORA_TEST_CHECK_MSG(body > 20000, "body region (skeleton->real content) should have rendered colorful content");
}
}  // namespace aurora::tests::sec_navigator_layout_cache

AURORA_TEST() {
    aurora::tests::sec_deeplink::run();
    aurora::tests::sec_navigation_transition::run();
    aurora::tests::sec_navigator_host_ops::run();
    aurora::tests::sec_navigator_layout_cache::run();
}
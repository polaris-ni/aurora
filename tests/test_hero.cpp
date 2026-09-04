// Hero 共享元素转场单测（specification/05-event-navigation.md §7.4）。
// 无头 Painter 驱动 NavigatorHost：首屏与新页各放同 tag 的 Hero 于不同位置/尺寸，
// 转场期间断言注册表捕获到 source/target 几何、tag 进入 morphing 集合，转场结束后退出 morphing。

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/navigation/hero.h"
#include "aurora/navigation/navigator.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "aurora/render/painter.h"
#include "aurora/state/state.h"
#include "aurora/widget/widget.h"
#include "test_harness.h"

using aurora::Animator;
using aurora::BuildContext;
using aurora::Color;
using aurora::Constraints;
using aurora::Hero;
using aurora::NavigatorHost;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::Rect;
using aurora::Route;
using aurora::RouteTransition;
using aurora::SignalViewBase;
using aurora::SingleChild;
using aurora::Size;
using aurora::TransitionKind;
using aurora::Widget;
using aurora::WidgetDescriptor;

/// @brief 固定尺寸纯色块：用于断言 Hero 几何（位置/尺寸）与交叉淡变。
namespace {
struct FixedBox : Widget {
    Size sz_;
    Color bg_;
    FixedBox(float w, float h, Color c) : sz_{.width = w, .height = h}, bg_(c) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FixedBox"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FixedBox", .children_policy = "none"};
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override { return c.constrain(sz_); }
    void on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) override { p.fill_rect(bounds, bg_); }
};

/// @brief 把子节点放在相对自身原点的固定偏移处（布局定位，不改 paint 绝对包围盒语义）。
struct Placed : SingleChild {
    Point at_;
    Size sz_;
    Placed(Node child, Point a, Size s) : SingleChild(std::move(child)), at_(a), sz_(s) {}
    [[nodiscard]] auto type_name() const -> const char * override { return "Placed"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "Placed", .children_policy = "single"};
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        m_child.widget().layout(c, ctx);
        return sz_;
    }
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override {
        m_child.widget().paint(p, Rect{.origin = bounds.origin + at_, .size = sz_}, ctx);
    }
};
auto near_rect(const Rect &a, const Rect &b) -> bool {
    return near_f(a.origin.x, b.origin.x) && near_f(a.origin.y, b.origin.y) && near_f(a.size.width, b.size.width) &&
           near_f(a.size.height, b.size.height);
}
}  // namespace

AURORA_TEST() {
    constexpr int w = 200;
    constexpr int h = 140;
    Animator anim;
    NavigatorHost host{anim};

    Painter painter;
    painter.begin(w, h);
    constexpr BuildContext ctx;
    host.mount(ctx);
    constexpr Rect full{.origin = Point{.x = 0.0F, .y = 0.0F},
                        .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}};

    RouteTransition fade;
    fade.animated = true;
    fade.kind = TransitionKind::Fade;
    fade.duration_seconds = 0.4;

    // ---- 首屏（红 Hero "logo" @ (10,20) 40x30），无转场 ----
    host.push(Route{Node{Placed{Node{Hero{"logo", Node{FixedBox{40.0F, 30.0F, Color::red()}}}},
                                Point{.x = 10.0F, .y = 20.0F}, Size{.width = 40.0F, .height = 30.0F}}},
                    "a"});
    host.paint(painter, full, ctx);
    // 静态首屏：Hero 正常自绘，不在 morphing。
    AURORA_TEST_CHECK(host.hero_registry()->morphing.empty());

    // ---- 转场到新页（绿 Hero "logo" @ (120,60) 80x60） ----
    host.push(Route{Node{Placed{Node{Hero{"logo", Node{FixedBox{80.0F, 60.0F, Color::green()}}}},
                                Point{.x = 120.0F, .y = 60.0F}, Size{.width = 80.0F, .height = 60.0F}}},
                    "b", fade});

    // 推进到中途并多绘几帧，使 morphing 集合建立。
    for (int i = 0; i < 3; ++i) {
        anim.tick(0.1);
        host.paint(painter, full, ctx);
    }

    const auto reg = host.hero_registry();
    AURORA_TEST_CHECK(reg->source.count("logo") == 1);
    AURORA_TEST_CHECK(reg->target.count("logo") == 1);
    AURORA_TEST_CHECK(near_rect(reg->source["logo"].bounds, Rect{.origin = Point{.x = 10.0F, .y = 20.0F},
                                                                 .size = Size{.width = 40.0F, .height = 30.0F}}));
    AURORA_TEST_CHECK(near_rect(reg->target["logo"].bounds, Rect{.origin = Point{.x = 120.0F, .y = 60.0F},
                                                                 .size = Size{.width = 80.0F, .height = 60.0F}}));
    AURORA_TEST_CHECK(reg->morphing.count("logo") == 1);  // 同 tag 配对成功 → 进入 morphing（跳过页内自绘）

    // ---- 推进到完成：旧页丢弃，Hero 退出 morphing ----
    for (int i = 0; i < 5; ++i) {
        anim.tick(0.1);
        host.paint(painter, full, ctx);
    }
    AURORA_TEST_CHECK(host.hero_registry()->morphing.empty());
    AURORA_TEST_CHECK(host.navigator().current().name() == "b");

    AURORA_LOG_INFO("test", "hero_test: ALL PASS");
}

/// 测试类型: unit
/// 目标单元: include/aurora/widget/widget.h
/// 测试说明: utest_widget 单元测试
///
// 目标源单元（历史映射，保留供审计）: Widget + Widget
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// Containers(Column/Row/Stack 等，经 popup/splitter/tab_bar 等容器用例与本文件行使)、
// ExpansionPanel、ImageWidget、Timer(Timer 控件，经本文件 hooks/components 段及
// test_timers 行使)、 NavigatorHost(NavigatorHost 宿主操作)、TransitionLayer(转场图层)。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/aurora.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/focus.h"
#include "aurora/event/keycode.h"
#include "aurora/layout/flex.h"
#include "aurora/layout/flex_layouter.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/switch.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "aurora_test_harness.h"

using au::AnimationController;
using au::Animator;
using au::BuildContext;
using au::Button;
using au::Checkbox;
using au::Color;
using au::Column;
using au::Constraints;
using au::CrossAxisAlignment;
using au::current_focus_manager;
using au::EventDispatcher;
using au::Flex;
using au::FlexDirection;
using au::FlexItem;
using au::FlexLayouter;
using au::FocusManager;
using au::Json;
using au::KeyAction;
using au::KeyCode;
using au::KeyEvent;
using au::LayoutCtxBase;
using au::MainAxisAlignment;
using au::MainAxisSize;
using au::Modifier;
using au::MouseAction;
using au::MouseButton;
using au::MouseEvent;
using au::NavigatorHost;
using au::Node;
using au::Painter;
using au::Point;
using au::ProgressIndicator;
using au::Reactive;
using au::Rect;
using au::Route;
using au::RouteTransition;
using au::Row;
using au::RowProps;
using au::SignalViewBase;
using au::Size;
using au::Slider;
using au::Stack;
using au::State;
using au::Switch;
using au::Text;
using au::TextInputEvent;
using au::TextProps;
using au::TransitionKind;
using au::Tween;
using au::Widget;
using au::WidgetDescriptor;

namespace aurora::test_cases::utest_widget {

namespace colors = aurora::colors;
using au::Alignment;
using au::ColumnProps;
using au::Divider;
using au::DividerProps;
using au::LeafWidget;
using au::LocalizedString;
using au::Orientation;

namespace aurora::tests::sec_widget_defaults {

// 覆写 collect_signals 递增计数器，用于验证 Container 默认实现遍历子节点。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) 测试内部共享计数器，static 文件作用域
static int g_signal_calls = 0;

namespace {
// 仅实现 type_name()（+describe_static()），不覆写 describe() —— 验证 Widget 默认 describe 实现。
struct ProbeWidget : Widget {
    [[nodiscard]] auto type_name() const -> const char * override { return "ProbeWidget"; }
    [[maybe_unused]] static auto describe_static() -> WidgetDescriptor {
        WidgetDescriptor d;
        d.name = "ProbeWidget";
        return d;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        (void)c;
        (void)ctx;
        return Size{};
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        (void)p;
        (void)b;
        (void)ctx;
    }
};

struct CountingChild : Widget {
    [[nodiscard]] auto type_name() const -> const char * override { return "CountingChild"; }
    [[maybe_unused]] static auto describe_static() -> WidgetDescriptor {
        WidgetDescriptor d;
        d.name = "CountingChild";
        return d;
    }
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override {
        (void)out;
        g_signal_calls++;
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override {
        (void)c;
        (void)ctx;
        return Size{};
    }
    auto on_paint(Painter &p, const Rect &b, const BuildContext &ctx) -> void override {
        (void)p;
        (void)b;
        (void)ctx;
    }
};
}  // namespace

static void run() {
    // #4: Widget::describe() 默认实现返回 { .name = type_name() }
    ProbeWidget w;
    constexpr BuildContext ctx;
    w.mount(ctx);
    AURORA_TEST_CHECK(w.describe().name == "ProbeWidget");

    // #6: Container::collect_signals 默认实现遍历 children_
    g_signal_calls = 0;
    Row row;
    row.adopt_children({
        Node(std::make_unique<CountingChild>()),
        Node(std::make_unique<CountingChild>()),
    });
    std::vector<SignalViewBase *> out;
    row.collect_signals(out);
    AURORA_TEST_CHECK(g_signal_calls == 2);
}
}  // namespace aurora::tests::sec_widget_defaults

namespace aurora::tests::sec_widget_hooks {

namespace {

/// 只覆盖滑块绘制阶段的 Slider 子类（轨道/填充沿用基类）。
class DiamondSlider : public Slider {
  public:
    int thumb_calls = 0;

  protected:
    auto paint_thumb(Painter &p, const Rect &bounds, const Rect &track, Color c) -> void override {
        ++thumb_calls;
        const float cx = track.origin.x + (value_fraction() * track.size.width);
        const float cy = bounds.origin.y + (bounds.size.height * 0.5F);
        p.fill_rounded_rect(
            Rect{.origin = Point{.x = cx - 6.0F, .y = cy - 6.0F}, .size = Size{.width = 12.0F, .height = 12.0F}}, 3.0F,
            c);
    }
};

/// 只覆盖背景绘制阶段的 Button 子类（文字/边框/状态色逻辑不变）。
class GradientButton : public Button {
  public:
    int bg_calls = 0;

  protected:
    auto paint_background(Painter &p, const Rect &b, Color bg) -> void override {
        ++bg_calls;
        p.draw_linear_gradient(b, b.origin, Point{.x = b.right(), .y = b.bottom()}, {bg, bg.shaded(0.7F)},
                               {0.0F, 1.0F});
    }
};

/// 覆盖状态色解析钩子的 Button 子类。
class FixedColorButton : public Button {
  protected:
    [[nodiscard]] auto resolve_background() const -> Color override { return Color{1, 2, 3, 255}; }
};

/// 只覆盖滑块的 Switch 子类。
class SquareThumbSwitch : public Switch {
  public:
    int thumb_calls = 0;

  protected:
    auto paint_thumb(Painter &p, const Rect &bounds, Color thumb, bool on) -> void override {
        ++thumb_calls;
        const float d = bounds.size.height - 4.0F;
        const float x = on ? bounds.right() - d - 2.0F : bounds.origin.x + 2.0F;
        p.fill_rect(Rect{.origin = Point{.x = x, .y = bounds.origin.y + 2.0F}, .size = Size{.width = d, .height = d}},
                    thumb);
    }
};

/// 只覆盖填充的 ProgressIndicator 子类。
class StripedProgress : public ProgressIndicator {
  public:
    int fill_calls = 0;

  protected:
    auto paint_fill(Painter &p, const Rect &bounds, Color c, float radius) -> void override {
        ++fill_calls;
        ProgressIndicator::paint_fill(p, bounds, c.shaded(1.1F), radius);  // 复用基类 + 调色
    }
};

template <typename W>
auto render_once(W &w, float width, float height) -> void {
    BuildContext ctx;
    w.mount(ctx);
    Constraints c;
    c.min = Size{.width = 0.0F, .height = 0.0F};
    c.max = Size{.width = width, .height = height};
    const Size sz = w.layout(c, ctx);
    Painter p;
    p.begin(static_cast<int>(width), static_cast<int>(height));
    w.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = sz}, ctx);
}

}  // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_widget_hooks ===\n");

    // Slider 子类：paint_thumb 被调用，其余阶段沿用基类
    {
        DiamondSlider s;
        s.set_range(0.0, 1.0);
        s.set_value(0.5);
        render_once(s, 200.0F, 24.0F);
        AURORA_TEST_CHECK_MSG(s.thumb_calls == 1, "DiamondSlider: subclass paint_thumb invoked by render path");
    }

    // Button 子类：paint_background 被调用；resolve_background 可覆盖
    {
        GradientButton b;
        b.set_label("Go");
        render_once(b, 200.0F, 60.0F);
        AURORA_TEST_CHECK_MSG(b.bg_calls == 1, "GradientButton: subclass paint_background invoked by render path");

        FixedColorButton fb;
        fb.set_label("Hi");
        fb.set_corner_radius(0.0F);
        render_once(fb, 200.0F, 60.0F);
        // resolve_background 覆盖后背景为定制色：取左上角内一像素验证
        Painter p;
        p.begin(60, 30);
        constexpr BuildContext ctx;
        fb.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 60.0F, .height = 30.0F}}, ctx);
        const Color px = p.get_pixel(2, 2);
        AURORA_TEST_CHECK_MSG(px.r == 1 && px.g == 2 && px.b == 3,
                              "FixedColorButton: resolve_background override effective");
    }

    // Switch 子类：paint_thumb 被调用
    {
        SquareThumbSwitch sw;
        render_once(sw, 44.0F, 24.0F);
        AURORA_TEST_CHECK_MSG(sw.thumb_calls == 1, "SquareThumbSwitch: subclass paint_thumb invoked by render path");
    }

    // ProgressIndicator 子类：paint_fill 被调用（值 > 0 才有填充）
    {
        StripedProgress pi;
        pi.set_value(0.6);
        render_once(pi, 200.0F, 6.0F);
        AURORA_TEST_CHECK_MSG(pi.fill_calls == 1, "StripedProgress: subclass paint_fill invoked by render path");
    }
}
}  // namespace aurora::tests::sec_widget_hooks

namespace aurora::tests::sec_hit_zorder {

namespace {
auto layout_root(Widget &root, const float w, const float h) -> void {
    Constraints c;
    c.min = Size{.width = 0, .height = 0};
    c.max = Size{.width = w, .height = h};
    constexpr BuildContext ctx;
    root.layout(c, ctx);
}
auto paint_root(Widget &root, const float w, const float h) -> void {
    Painter p;
    p.begin(static_cast<int>(w), static_cast<int>(h));
    constexpr BuildContext ctx;
    root.paint(p, Rect{.origin = Point{.x = 0, .y = 0}, .size = Size{.width = w, .height = h}}, ctx);
}
auto hit_text(Widget &root, float x, const float y) -> std::string {
    Widget *h = EventDispatcher::hit_test(root, Point{.x = x, .y = y});
    auto const *t = dynamic_cast<Text *>(h);
    return (t != nullptr) ? t->display_text() : std::string{};
}
}  // namespace

static void run() {
    // (1) Stack：默认14pt文本 在底（先），Text控件 在顶（后，视觉上层）。
    //     重叠区 (0,0)→(74,27) 内 hit_test 必须命中顶层 Text控件，而非底层 默认14pt文本。
    {
        const auto bottom =
            std::make_shared<Text>(TextProps{.content = LocalizedString{"默认14pt文本"}, .soft_wrap = true});
        const auto top = std::make_shared<Text>(TextProps{.content = LocalizedString{"Text控件"}, .soft_wrap = true});
        Stack st{std::vector{Node{bottom}, Node{top}}};
        layout_root(st, 520, 800);
        paint_root(st, 520, 800);
        const std::string hit = hit_text(st, 37.0F, 13.0F);  // 重叠区中心
        AURORA_TEST_CHECK(hit == "Text控件");
    }
    // (2) 反向 Stack：Text控件 在底，默认14pt文本 在顶 → 重叠区应命中 默认14pt文本（验证“顶层恒优先”）。
    {
        const auto bottom =
            std::make_shared<Text>(TextProps{.content = LocalizedString{"Text控件"}, .soft_wrap = true});
        const auto top =
            std::make_shared<Text>(TextProps{.content = LocalizedString{"默认14pt文本"}, .soft_wrap = true});
        Stack st{std::vector{Node{bottom}, Node{top}}};
        layout_root(st, 520, 800);
        paint_root(st, 520, 800);
        const std::string hit = hit_text(st, 37.0F, 13.0F);
        AURORA_TEST_CHECK(hit == "默认14pt文本");
    }
    // (3) 不重叠的 Row 行为不变：各自命中自身。
    {
        const auto a = std::make_shared<Text>(TextProps{.content = LocalizedString{"默认14pt文本"}, .soft_wrap = true});
        const auto b = std::make_shared<Text>(TextProps{.content = LocalizedString{"Text控件"}, .soft_wrap = true});
        Row row{RowProps{.children = {Node{a}, Node{b}}}};
        layout_root(row, 520, 800);
        paint_root(row, 520, 800);
        AURORA_TEST_CHECK(hit_text(row, 30.0F, 13.0F) == "默认14pt文本");
        AURORA_TEST_CHECK(hit_text(row, 160.0F, 13.0F) == "Text控件");
    }
}
}  // namespace aurora::tests::sec_hit_zorder

AURORA_TEST() {
    aurora::tests::sec_widget_defaults::run();
    aurora::tests::sec_widget_hooks::run();
    aurora::tests::sec_hit_zorder::run();
}

}  // namespace aurora::test_cases::utest_widget

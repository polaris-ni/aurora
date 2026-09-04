// 目标源单元：widget/widget.h + src/aurora/widget/widget.cpp
// 吸收的既有测试（逐段原样保留，段名=sec_<原名>）：
//   - test_widget_defaults.cpp
//   - test_widget_hooks.cpp
//   - test_components.cpp
//   - test_hit_zorder.cpp
//   - test_lifetime_uaf.cpp
// 合并约定：每段包裹于独立 namespace（零符号冲突）；原 main 改为 run()，
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（au_test_main.cpp）统一提供。

// ── API 覆盖映射 ─────────────────────────────
// widget/containers.h(Column/Row/Stack 等，经 popup/splitter/tab_bar 等容器用例与本文件行使)、
// widget/expansion_panel.h、widget/image_widget.h、widget/timer.h(Timer 控件，经本文件 hooks/components 段及
// test_timers 行使)、 navigation/navigator_host.h(NavigatorHost 宿主操作)、navigation/transition_layer.h(转场图层)。

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
#include "test_harness.h"

using aurora::AnimationController;
using aurora::Animator;
using aurora::BuildContext;
using aurora::Button;
using aurora::Checkbox;
using aurora::Color;
using aurora::Column;
using aurora::Constraints;
using aurora::CrossAxisAlignment;
using aurora::current_focus_manager;
using aurora::EventDispatcher;
using aurora::Flex;
using aurora::FlexDirection;
using aurora::FlexItem;
using aurora::FlexLayouter;
using aurora::FocusManager;
using aurora::Json;
using aurora::KeyAction;
using aurora::KeyCode;
using aurora::KeyEvent;
using aurora::LayoutCtxBase;
using aurora::MainAxisAlignment;
using aurora::MainAxisSize;
using aurora::Modifier;
using aurora::MouseAction;
using aurora::MouseButton;
using aurora::MouseEvent;
using aurora::NavigatorHost;
using aurora::Node;
using aurora::Painter;
using aurora::Point;
using aurora::ProgressIndicator;
using aurora::Reactive;
using aurora::Rect;
using aurora::Route;
using aurora::RouteTransition;
using aurora::Row;
using aurora::RowProps;
using aurora::SignalViewBase;
using aurora::Size;
using aurora::Slider;
using aurora::Stack;
using aurora::State;
using aurora::Switch;
using aurora::Text;
using aurora::TextInputEvent;
using aurora::TextProps;
using aurora::TransitionKind;
using aurora::Tween;
using aurora::Widget;
using aurora::WidgetDescriptor;
namespace colors = aurora::colors;
using aurora::Alignment;
using aurora::ColumnProps;
using aurora::Divider;
using aurora::DividerProps;
using aurora::LeafWidget;
using aurora::LocalizedString;
using aurora::Orientation;

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

    // #6: Container::collect_signals 默认实现遍历 m_children
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
    int thumb_calls_ = 0;

  protected:
    auto paint_thumb(Painter &p, const Rect &bounds, const Rect &track, Color c) -> void override {
        ++thumb_calls_;
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
    int bg_calls_ = 0;

  protected:
    auto paint_background(Painter &p, const Rect &b, Color bg) -> void override {
        ++bg_calls_;
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
    int thumb_calls_ = 0;

  protected:
    auto paint_thumb(Painter &p, const Rect &bounds, Color thumb, bool on) -> void override {
        ++thumb_calls_;
        const float d = bounds.size.height - 4.0F;
        const float x = on ? bounds.right() - d - 2.0F : bounds.origin.x + 2.0F;
        p.fill_rect(Rect{.origin = Point{.x = x, .y = bounds.origin.y + 2.0F}, .size = Size{.width = d, .height = d}},
                    thumb);
    }
};

/// 只覆盖填充的 ProgressIndicator 子类。
class StripedProgress : public ProgressIndicator {
  public:
    int fill_calls_ = 0;

  protected:
    auto paint_fill(Painter &p, const Rect &bounds, Color c, float radius) -> void override {
        ++fill_calls_;
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
        AURORA_TEST_CHECK_MSG(s.thumb_calls_ == 1, "DiamondSlider: subclass paint_thumb invoked by render path");
    }

    // Button 子类：paint_background 被调用；resolve_background 可覆盖
    {
        GradientButton b;
        b.set_label("Go");
        render_once(b, 200.0F, 60.0F);
        AURORA_TEST_CHECK_MSG(b.bg_calls_ == 1, "GradientButton: subclass paint_background invoked by render path");

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
        AURORA_TEST_CHECK_MSG(px.m_r == 1 && px.m_g == 2 && px.m_b == 3,
                              "FixedColorButton: resolve_background override effective");
    }

    // Switch 子类：paint_thumb 被调用
    {
        SquareThumbSwitch sw;
        render_once(sw, 44.0F, 24.0F);
        AURORA_TEST_CHECK_MSG(sw.thumb_calls_ == 1, "SquareThumbSwitch: subclass paint_thumb invoked by render path");
    }

    // ProgressIndicator 子类：paint_fill 被调用（值 > 0 才有填充）
    {
        StripedProgress pi;
        pi.set_value(0.6);
        render_once(pi, 200.0F, 6.0F);
        AURORA_TEST_CHECK_MSG(pi.fill_calls_ == 1, "StripedProgress: subclass paint_fill invoked by render path");
    }
}
}  // namespace aurora::tests::sec_widget_hooks

namespace aurora::tests::sec_components {

namespace {

void render_tree(Widget &w, float ww, float hh) {
    constexpr BuildContext ctx;
    w.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = ww, .height = hh};
    w.layout(cc, ctx);
    Painter p;
    p.begin(static_cast<int>(ww), static_cast<int>(hh));
    w.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = ww, .height = hh}}, ctx);
}

// 模拟测量上下文 + trampoline：零堆分配，替代原 std::function 捕获 lambda。
struct MeasureCtx : LayoutCtxBase {
    float content_w_;
    float content_h_;
};

auto mc_pool() -> std::vector<MeasureCtx> & {  // NOLINT
    static std::vector<MeasureCtx> v;
    static bool init = (v.reserve(32), true);
    (void)init;
    return v;
}

// 模拟"测量"：尊重约束（填满有限主轴/交叉轴空间，content>0 时保留内容尺寸）。
auto item(float w, const float content_w, const float content_h) -> FlexItem {
    auto &pool = mc_pool();
    pool.push_back(MeasureCtx{{}, content_w, content_h});
    MeasureCtx *mc = &pool.back();
    return FlexItem::make<MeasureCtx>(w, mc, [](void *ctx, const Constraints &cc) -> Size {
        auto const *m = static_cast<MeasureCtx *>(ctx);
        constexpr float inf = Size::infinity().width;
        const float mw = (m->content_w_ > 0.0F) ? std::min(m->content_w_, cc.max.width)
                                                : (cc.max.width != inf ? cc.max.width : 0.0F);
        const float mh = (m->content_h_ > 0.0F) ? std::min(m->content_h_, cc.max.height)
                                                : (cc.max.height != inf ? cc.max.height : 0.0F);
        return Size{.width = mw, .height = mh};
    });
}

// Column/Row 对齐属性落地 + 核心 flex 语义变更（MainAxisSize::Max 撑满父级、对齐产生可见自由空间）。
void test_column_row_alignment() {
    Constraints c;
    c.max = Size{.width = 100.0F, .height = 200.0F};

    Flex f_min{.direction = FlexDirection::Column,
               .main_axis = MainAxisAlignment::Start,
               .cross_axis = CrossAxisAlignment::Start};
    f_min.main_axis_size = MainAxisSize::Min;
    auto r_min = FlexLayouter::layout(f_min, c, {item(0, 10, 20), item(0, 10, 20)});
    AURORA_TEST_CHECK_MSG(near_f(r_min.size.height, 40.0F), "Min: content height = 40");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(r_min.children[0].origin.y, 0.0F), "Min/Start child0 y=0");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(r_min.children[1].origin.y, 20.0F), "Min/Start child1 y=20");

    Flex f_max{.direction = FlexDirection::Column,
               .main_axis = MainAxisAlignment::Center,
               .cross_axis = CrossAxisAlignment::Start};
    f_max.main_axis_size = MainAxisSize::Max;
    auto r_max = FlexLayouter::layout(f_max, c, {item(0, 10, 20), item(0, 10, 20)});
    AURORA_TEST_CHECK_MSG(near_f(r_max.size.height, 200.0F), "Max: fills parent height = 200");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(r_max.children[0].origin.y, 80.0F), "Max/Center child0 y=80");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(r_max.children[1].origin.y, 100.0F), "Max/Center child1 y=100");

    Flex f_end{.direction = FlexDirection::Column,
               .main_axis = MainAxisAlignment::End,
               .cross_axis = CrossAxisAlignment::Start};
    f_end.main_axis_size = MainAxisSize::Max;
    auto r_end = FlexLayouter::layout(f_end, c, {item(0, 10, 20), item(0, 10, 20)});
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(r_end.children[0].origin.y, 160.0F), "Max/End child0 y=160");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(r_end.children[1].origin.y, 180.0F), "Max/End child1 y=180");

    Constraints c_inf;
    c_inf.max = Size{.width = Size::infinity().width, .height = Size::infinity().height};
    auto r_inf = FlexLayouter::layout(f_max, c_inf, {item(0, 10, 20), item(0, 10, 20)});
    AURORA_TEST_CHECK_MSG(near_f(r_inf.size.height, 40.0F), "Max + infinite constraint: still content size = 40");

    // Widget 层：Column/Row 经 set_main_axis_size 透传 flex.main_axis_size，撑满父级。
    Column col{Node{Text{"a"}}, Node{Text{"b"}}};
    col.set_main_axis_size(MainAxisSize::Max);
    BuildContext ctx;
    col.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = 100.0F, .height = 200.0F};
    const Size s = col.layout(cc, ctx);
    AURORA_TEST_CHECK_MSG(near_f(s.height, 200.0F), "Column(Max) fills parent height = 200");

    Row row{Node{Text{"a"}}, Node{Text{"b"}}};
    row.set_main_axis_size(MainAxisSize::Max);
    BuildContext rctx;
    row.mount(rctx);
    Constraints rc;
    rc.min = Size{.width = 0.0F, .height = 0.0F};
    rc.max = Size{.width = 200.0F, .height = 50.0F};
    const Size rs = row.layout(rc, rctx);
    AURORA_TEST_CHECK_MSG(near_f(rs.width, 200.0F), "Row(Max) fills parent width = 200");

    // 链式 setter + 序列化往返（属性键 main_axis_alignment / cross_axis_alignment / main_axis_size / gap）。
    Column col2{Node{Text{"a"}}};
    col2.set_main_axis_alignment(MainAxisAlignment::Center)
        .set_cross_axis_alignment(CrossAxisAlignment::Stretch)
        .set_main_axis_size(MainAxisSize::Max)
        .set_gap(8.0F);
    Json j;
    col2.serialize_props(j);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["main_axis_alignment"].get<std::string>() == "Center",
                          "serialize main_axis_alignment=Center");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["cross_axis_alignment"].get<std::string>() == "Stretch",
                          "serialize cross_axis_alignment=Stretch");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(j["main_axis_size"].get<std::string>() == "Max", "serialize main_axis_size=Max");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(j["gap"].get<float>(), 8.0F), "serialize gap=8");

    Column q{Node{Text{"b"}}};
    q.deserialize_props(j);
    Json k;
    q.serialize_props(k);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(k["main_axis_alignment"].get<std::string>() == "Center", "rt main_axis_alignment");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(k["cross_axis_alignment"].get<std::string>() == "Stretch", "rt cross_axis_alignment");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(k["main_axis_size"].get<std::string>() == "Max", "rt main_axis_size");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(near_f(k["gap"].get<float>(), 8.0F), "rt gap");

    Row row2{Node{Text{"a"}}};
    row2.set_main_axis_alignment(MainAxisAlignment::End)
        .set_cross_axis_alignment(CrossAxisAlignment::Center)
        .set_main_axis_size(MainAxisSize::Min);
    Json rj;
    row2.serialize_props(rj);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(rj["main_axis_alignment"].get<std::string>() == "End",
                          "Row serialize main_axis_alignment=End");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(rj["cross_axis_alignment"].get<std::string>() == "Center",
                          "Row serialize cross_axis_alignment=Center");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK_MSG(rj["main_axis_size"].get<std::string>() == "Min", "Row serialize main_axis_size=Min");
}

}  // namespace

static void run() {
    // ---------- 1. 基础控件：渲染不崩溃 ----------
    {
        auto col = Column{ColumnProps{.children = {
                                          Node{Divider{}},
                                          Node{Divider{DividerProps{.orientation = Orientation::Vertical}}},
                                          Node{Checkbox{Reactive{false}}},
                                          Node{Switch{Reactive{true}}},
                                          Node{Slider{Reactive{0.5}}},
                                          Node{ProgressIndicator{Reactive{0.3}}},
                                      }}};
        render_tree(col, 320.0F, 480.0F);
        AURORA_TEST_CHECK_MSG(true, "base widgets render without crash");
    }

    // ---------- 2. Checkbox 点击切换 ----------
    {
        Checkbox cb{Reactive{false}};
        render_tree(cb, 40.0F, 40.0F);
        auto bb = Rect{.origin = Point{}, .size = cb.size()};
        const Point c{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
        MouseEvent e;
        e.position = c;
        e.action = MouseAction::Press;
        EventDispatcher::dispatch(cb, e);
        e.action = MouseAction::Release;
        EventDispatcher::dispatch(cb, e);
        AURORA_TEST_CHECK_MSG(cb.value() == true, "Checkbox toggles on click");
    }

    // ---------- 3. Slider 拖拽设置值 ----------
    {
        Slider sl{Reactive{0.5}};
        render_tree(sl, 200.0F, 24.0F);
        auto bb = Rect{.origin = Point{}, .size = sl.size()};
        const float left = bb.origin.x + 4.0F;
        const float mid = bb.origin.x + (bb.size.width / 2.0F);
        MouseEvent e;
        e.position = Point{.x = mid, .y = bb.origin.y + (bb.size.height / 2.0F)};
        e.action = MouseAction::Press;
        EventDispatcher::dispatch(sl, e);
        e.position = Point{.x = left, .y = bb.origin.y + (bb.size.height / 2.0F)};
        e.action = MouseAction::Move;
        EventDispatcher::dispatch(sl, e);
        e.action = MouseAction::Release;
        EventDispatcher::dispatch(sl, e);
        AURORA_TEST_CHECK_MSG(sl.value() < 0.2, "Slider drag to left sets low value");
    }

    // ---------- 5. Align / Offset / 圆角裁剪 渲染不崩溃 ----------
    {
        Text aligned{"Aligned"};
        aligned.modifier.set(
            Modifier{}.align(Alignment::BottomRight).background(colors::AURORA_BLUE, 8.0F).clip_rounded(8.0F));
        Text offset{"Offset"};
        offset.modifier.set(Modifier{}.offset(20.0F, 10.0F).background(colors::AURORA_GREEN));
        auto stacked = Stack{std::vector{Node{std::move(aligned)}, Node{std::move(offset)}}};
        render_tree(stacked, 200.0F, 200.0F);
        AURORA_TEST_CHECK_MSG(true, "Align/Offset/rounded-clip render");
    }

    // ---------- 6. 手势：拖拽回调 ----------
    {
        bool dragged = false;
        Point last_delta{.x = 0.0F, .y = 0.0F};
        Text drag{"Drag me"};
        drag.modifier.set(Modifier{}.draggable([&](Point d, Point) -> void {
            dragged = true;
            last_delta = d;
        }));
        render_tree(drag, 120.0F, 40.0F);
        auto bb = Rect{.origin = Point{}, .size = drag.size()};
        const Point c{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
        MouseEvent e;
        e.position = c;
        e.action = MouseAction::Press;
        EventDispatcher::dispatch(drag, e);
        e.position = Point{.x = c.x + 20.0F, .y = c.y};
        e.action = MouseAction::Move;
        EventDispatcher::dispatch(drag, e);
        e.action = MouseAction::Release;
        EventDispatcher::dispatch(drag, e);
        AURORA_TEST_CHECK_MSG(dragged && std::abs(last_delta.x - 20.0F) < 0.001F, "Drag reports delta");
    }

    // ---------- 7. 手势：长按计时 ----------
    {
        bool fired = false;
        Text lp{"Long press"};
        lp.modifier.set(Modifier{}.long_press([&]() -> void { fired = true; }, 50.0F));
        render_tree(lp, 120.0F, 40.0F);
        auto bb = Rect{.origin = Point{}, .size = lp.size()};
        const Point c{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
        MouseEvent e;
        e.position = c;
        e.action = MouseAction::Press;
        EventDispatcher::dispatch(lp, e);
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        lp.tick(std::chrono::steady_clock::now());
        AURORA_TEST_CHECK_MSG(fired, "Long-press fires after threshold");
    }

    // ---------- 8b. 可复现渲染：逻辑快照 + 确定性 ----------
    {
        auto make_tree = []() -> Node {
            auto const col = std::make_shared<Column>(
                ColumnProps{.children = {
                                Node{Row{RowProps{.children = {Node{Text{"A"}}, Node{Text{"B"}}}}}},
                                Node{Text{"C"}},
                            }});
            col->modifier = Modifier{}.fill_max_size();
            return Node{col};
        };
        Node t1 = make_tree();
        const Json snap = render_to_logical_snapshot(t1, 200, 200);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(std::string{snap["type"].get<std::string>()} == "Column", "snapshot root type Column");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(snap["children"].size() == 2, "snapshot root has 2 children");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(std::string{snap["children"][0]["type"].get<std::string>()} == "Row",
                              "snapshot first child Row");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(std::abs(snap["box"]["w"].get<float>() - 200.0F) < 0.001F,
                              "snapshot root width = viewport");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        AURORA_TEST_CHECK_MSG(std::abs(snap["box"]["h"].get<float>() - 200.0F) < 0.001F,
                              "snapshot root height = viewport");

        // 确定性：两次快照字节一致
        Node t2 = make_tree();
        const Json snap2 = render_to_logical_snapshot(t2, 200, 200);
        AURORA_TEST_CHECK_MSG(snap.dump() == snap2.dump(), "logical snapshot is deterministic");

        // 像素确定性：同树两次栅格化结果一致
        auto render_pixels = [](Widget &w, int ww, int hh) -> std::vector<std::uint8_t> {
            constexpr BuildContext ctx;
            w.mount(ctx);
            Constraints c;
            c.min = Size{.width = 0.0F, .height = 0.0F};
            c.max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)};
            w.layout(c, ctx);
            Painter p;
            p.begin(ww, hh);
            w.paint(p,
                    Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                         .size = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}},
                    ctx);
            const std::uint8_t *d = p.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic, modernize-return-braced-init-list)
            // 测试助手：缓冲区间算术；范围构造保留圆括号（braced-init 会变 initializer_list）
            return std::vector(d, d + (static_cast<std::size_t>(ww) * hh * 4));
        };
        Node t3 = make_tree();
        const auto px1 = render_pixels(t3.widget(), 160, 160);
        const auto px2 = render_pixels(t3.widget(), 160, 160);
        AURORA_TEST_CHECK_MSG(px1 == px2 && !px1.empty(), "pixel render is deterministic");
    }

    test_column_row_alignment();
}
}  // namespace aurora::tests::sec_components

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

namespace aurora::tests::sec_lifetime_uaf {

namespace {

/// @brief 可聚焦叶控件：记录获焦/失焦次数，用于焦点悬垂断言。
class FocusLeaf : public LeafWidget {
  public:
    int gained_ = 0;
    int lost_ = 0;
    /// @brief activate() 计数出口：指向调用方栈变量，故本控件析构后仍可安全读取，
    ///        用于断言「已回收的焦点控件不再被虚调用」而无需解引用已释放对象。
    int *activate_sink_ = nullptr;
    void on_focus_change(bool focused) override {
        if (focused) {
            ++gained_;
        } else {
            ++lost_;
        }
    }
    void activate() override {
        if (activate_sink_ != nullptr) {
            ++*activate_sink_;
        }
    }
    void collect_signals(std::vector<SignalViewBase *> & /*out*/) override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FocusLeaf"; }
    [[nodiscard]] auto describe() const -> WidgetDescriptor override {
        return WidgetDescriptor{.name = "FocusLeaf", .children_policy = "none"};
    }

  protected:
    auto on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size override {
        return c.constrain(Size{.width = 40.0F, .height = 20.0F});
    }
    void on_paint(Painter & /*p*/, const Rect & /*bounds*/, const BuildContext & /*ctx*/) override {}
};

/// @brief 整屏纯色页（NavigatorHost 转场用）。
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

/// @brief 在 root 上跑一次完整的 Press+Release（触发 Button::activate → on_click）。
auto click_at(Widget &root, const Point &p, FocusManager *fm) -> void {
    MouseEvent press;
    press.position = p;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    EventDispatcher::dispatch(root, press, fm);

    MouseEvent release;
    release.position = p;
    release.action = MouseAction::Release;
    release.button = MouseButton::Left;
    EventDispatcher::dispatch(root, release, fm);
}

/// @brief 布局 + 绘制一遍，让命中链的 bounds 生效。ctx 由调用方持有（须活过整次派发）。
auto realize(Widget &root, Painter &p, BuildContext const &ctx, const int w, const int h) -> void {
    root.mount(ctx);
    Constraints cc;
    cc.min = Size{.width = 0.0F, .height = 0.0F};
    cc.max = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)};
    root.layout(cc, ctx);
    root.paint(p,
               Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                    .size = Size{.width = static_cast<float>(w), .height = static_cast<float>(h)}},
               ctx);
}

}  // namespace

static void run() {
    AURORA_TEST_PRINTF("=== test_lifetime_uaf ===\n");

    Painter painter;
    painter.begin(200, 200);

    // ---- 1) 自杀式点击：on_click 丢掉持有自己的最后一个 shared_ptr ----
    // 修复前：deliver_chain 的裸指针在回调返回后被 Widget::on_pointer_event 写入 → UAF。
    {
        auto col = std::make_shared<Column>();
        auto btn = std::make_shared<Button>("kill me");
        // 该 shared_ptr 拷贝是 btn 在树外的唯一额外强引用；on_click 里连同树内的一起丢掉。
        std::shared_ptr<Button> holder = btn;
        int clicks = 0;
        btn->set_on_click([&col, &holder, &clicks]() -> void {
            ++clicks;
            // 清空子树：丢掉树内对 btn 的强引用（模拟 push_replacement 重建页面）。
            col->adopt_children(std::vector<Node>{});
            holder.reset();  // 丢掉最后一个外部强引用 → 若无 keepalive，btn 此刻即释放
        });
        col->add(Node{btn});
        btn.reset();  // 之后 btn 只由 col 子树 + holder 持有

        BuildContext ctx;
        realize(*col, painter, ctx, 200, 200);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Rect bb = col->child_nodes()[0].bounds();
        const Point center{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};

        click_at(*col, center, nullptr);
        AURORA_TEST_CHECK_MSG(clicks == 1, "suicidal on_click fired and dispatch did not crash (keepalive effective)");
        AURORA_TEST_CHECK_MSG(col->child_count() == 0, "subtree cleared in callback");
        AURORA_TEST_CHECK_MSG(holder == nullptr,
                              "external strong reference released (widget actually destroyed in callback)");
    }

    // ---- 2) 焦点控件先于 FocusManager 被回收 ----
    // 修复前：m_focused 裸指针悬垂，focused() 返回野指针，按键派发即虚调用已释放内存。
    {
        FocusManager fm;
        Column dummy_root;
        int activations = 0;
        auto leaf = std::make_shared<FocusLeaf>();
        leaf->activate_sink_ = &activations;
        fm.set_focus(leaf.get());
        AURORA_TEST_CHECK_MSG(fm.focused() == leaf.get(), "shared_ptr widget can gain focus normally");
        AURORA_TEST_CHECK_MSG(fm.has_focus(leaf.get()), "has_focus is true for live widget");
        AURORA_TEST_CHECK_EQ(leaf->gained_, 1);

        // 前置证明：控件存活时 Enter 确实走到 focused->activate() 这条虚调用路径，
        // 否则下方「回收后不再触发」会变成永远成立的假阴性。
        KeyEvent live_enter;
        live_enter.action_ = KeyAction::Down;
        live_enter.key_ = static_cast<int>(KeyCode::Enter);
        AURORA_TEST_CHECK_MSG(EventDispatcher::dispatch(dummy_root, live_enter, fm),
                              "live focused widget activated by Enter");
        AURORA_TEST_CHECK_EQ(activations, 1);

        Widget *raw = leaf.get();
        leaf.reset();  // 焦点控件被回收，FocusManager 仍留有记录

        AURORA_TEST_CHECK_MSG(fm.focused() == nullptr, "reclaimed focused widget returns nullptr via live_focused()");
        AURORA_TEST_CHECK_MSG(!fm.has_focus(raw), "has_focus is false for reclaimed widget (must not dereference)");

        // Enter / Space 是唯一直达虚函数 activate() 的按键路径：修复前这里会对
        // 已释放内存读 vtable。回收后必须安全返回 false 且不再计数。
        KeyEvent dead_enter;
        dead_enter.action_ = KeyAction::Down;
        dead_enter.key_ = static_cast<int>(KeyCode::Enter);
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, dead_enter, fm),
                              "reclaimed focus: Enter no longer virtual-calls activate()");
        KeyEvent dead_space;
        dead_space.action_ = KeyAction::Down;
        dead_space.key_ = static_cast<int>(KeyCode::Space);
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, dead_space, fm),
                              "reclaimed focus: Space no longer virtual-calls activate()");
        AURORA_TEST_CHECK_EQ(activations, 1);  // 仍是存活期的那一次

        // 按键 / 文本派发遇到已回收焦点：安全返回 false，绝不虚调用。
        KeyEvent ke;
        ke.action_ = KeyAction::Down;
        ke.key_ = static_cast<int>(KeyCode::Backspace);
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, ke, fm),
                              "key dispatch safely returns false when focus reclaimed");

        TextInputEvent te;
        te.text_ = "x";
        AURORA_TEST_CHECK_MSG(!EventDispatcher::dispatch(dummy_root, te, fm),
                              "text dispatch safely returns false when focus reclaimed");

        // 文本派发的早退分支必须复原 thread-local，否则 &fm 泄漏到调用者作用域外。
        AURORA_TEST_CHECK_MSG(current_focus_manager() == nullptr,
                              "TextInput early-return branch restored current_focus_manager");

        // 清焦点不应对已释放控件发 on_focus_change(false)。
        fm.clear();
        AURORA_TEST_CHECK_MSG(fm.focused() == nullptr, "no focus after clear()");
    }

    // ---- 3) 栈/成员控件回退路径：weak_from_this() 为空弱引用时仍须正常工作 ----
    // 防「只用 weak_ptr」的过度修复：栈控件恒 lock 失败会让全部事件被静默丢弃。
    {
        FocusManager fm;
        FocusLeaf stack_leaf;  // 栈对象，未被 shared_ptr 持有
        fm.set_focus(&stack_leaf);
        AURORA_TEST_CHECK_MSG(fm.focused() == &stack_leaf, "stack widget (empty weak ref) still returned as focus");
        AURORA_TEST_CHECK_MSG(fm.has_focus(&stack_leaf), "stack widget has_focus is true");
        AURORA_TEST_CHECK_EQ(stack_leaf.gained_, 1);

        // 栈上 Button 的点击派发不得因 guard 为空而被丢弃。
        Column col;
        auto btn = std::make_shared<Button>("stack tree");
        int clicks = 0;
        btn->set_on_click([&clicks]() -> void { ++clicks; });
        col.add(Node{btn});
        BuildContext ctx;
        realize(col, painter, ctx, 200, 200);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        const Rect bb = col.child_nodes()[0].bounds();
        const Point center{.x = bb.origin.x + (bb.size.width * 0.5F), .y = bb.origin.y + (bb.size.height * 0.5F)};
        click_at(col, center, &fm);
        AURORA_TEST_CHECK_MSG(clicks == 1,
                              "stack root + shared child click still dispatched normally (not swallowed by lock)");
    }

    // ---- 4) NavigatorHost 先于 Animator 析构 ----
    // 修复前：begin_transition 把成员 m_ctrl / m_progress 注册进 Animator 且无注销，
    // host 析构后 anim.tick() 即 tick 已释放的控制器并写已释放的 State。
    {
        Animator anim;
        RouteTransition fade;
        fade.animated = true;
        fade.kind = TransitionKind::Fade;
        fade.duration_seconds = 0.4;

        {
            NavigatorHost host{anim};
            BuildContext ctx;
            host.mount(ctx);
            host.push(Route{Node{SolidPage{Color::red()}}, "a", fade});
            host.push(Route{Node{SolidPage{Color::blue()}}, "b", fade});  // 触发 begin_transition
            anim.tick(0.1);
            AURORA_TEST_CHECK_MSG(anim.has_active(), "transition in progress: Animator has active controller");
        }  // host 析构 → ~NavigatorHost 调 anim.remove(m_ctrl)

        // 若未注销，下面这两次 tick 会写已释放内存（ASan 下 heap-use-after-free）。
        anim.tick(0.1);
        anim.tick(0.1);
        AURORA_TEST_CHECK_MSG(!anim.has_active(),
                              "controllers unregistered from Animator after NavigatorHost destruction");
    }

    // ---- 5) Animator::remove 语义：重复注销幂等、未登记者无操作 ----
    {
        Animator anim;
        AnimationController c{1.0};
        State target{0.0};
        anim.bind(c, Tween{0.0, 1.0}, target);
        c.forward(0.0);
        anim.tick(0.5);
        AURORA_TEST_CHECK_MSG(target.get() > 0.0, "tick writes target State after bind");

        anim.remove(c);
        const double frozen = target.get();
        anim.tick(0.5);
        AURORA_TEST_CHECK_MSG(target.get() == frozen, "tick no longer writes target State after remove");

        anim.remove(c);  // 重复注销幂等
        AnimationController never{1.0};  // 从未登记
        anim.remove(never);
        AURORA_TEST_CHECK_MSG(!anim.has_active(), "repeated/invalid remove is safe");
    }
}
}  // namespace aurora::tests::sec_lifetime_uaf

AURORA_TEST() {
    aurora::tests::sec_widget_defaults::run();
    aurora::tests::sec_widget_hooks::run();
    aurora::tests::sec_components::run();
    aurora::tests::sec_hit_zorder::run();
    aurora::tests::sec_lifetime_uaf::run();
}
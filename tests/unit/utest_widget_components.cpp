// 目标源单元：widget/widget.h + src/aurora/widget/widget.cpp
// 用例经 AURORA_TEST() 注册，main 与汇总由 runner（aurora_test_main.cpp）统一提供。

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

// （自 utest_widget.cpp 拆分：组件示例组装段）

namespace aurora::test_cases::utest_widget_components {

namespace colors = aurora::colors;
using au::Alignment;
using au::ColumnProps;
using au::Divider;
using au::DividerProps;
using au::LeafWidget;
using au::LocalizedString;
using au::Orientation;

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
    float content_w;
    float content_h;
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
        const float mw =
            (m->content_w > 0.0F) ? std::min(m->content_w, cc.max.width) : (cc.max.width != inf ? cc.max.width : 0.0F);
        const float mh = (m->content_h > 0.0F) ? std::min(m->content_h, cc.max.height)
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
            return std::vector(d, d + (static_cast<std::size_t>(ww) * hh * 4));  // NOLINT
        };
        Node t3 = make_tree();
        const auto px1 = render_pixels(t3.widget(), 160, 160);
        const auto px2 = render_pixels(t3.widget(), 160, 160);
        AURORA_TEST_CHECK_MSG(px1 == px2 && !px1.empty(), "pixel render is deterministic");
    }

    test_column_row_alignment();
}
}  // namespace aurora::tests::sec_components

AURORA_TEST() {
    aurora::tests::sec_components::run();
}

}  // namespace aurora::test_cases::utest_widget_components

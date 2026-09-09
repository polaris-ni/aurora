/// 测试类型: integration
/// 目标单元: include/aurora/widget/{checkbox,slider,stack,divider,progress,switch,containers}.h
/// 测试说明: 基础控件组装与交互集成——Divider/Checkbox/Switch/Slider/ProgressIndicator
/// 渲染走通、Checkbox 完整点击切换、Slider 拖拽赋值、Stack 对齐/偏移/圆角裁剪渲染、
/// draggable/long_press 手势回调、逻辑快照与像素栅格确定性、Column/Row 对齐属性
/// 及序列化往返（低阶 FlexLayouter 语义由 utest_flex_layouter 覆盖）

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/modifier/modifier.h"
#include "aurora/render/offscreen.h"
#include "aurora/render/painter.h"
#include "aurora/widget/checkbox.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/divider.h"
#include "aurora/widget/progress.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/stack.h"
#include "aurora/widget/switch.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::itest_widget_components {

namespace {

/// 挂载 + 布局 + 绘制一遍（控件独立渲染的最小驱动）。
auto render_tree(Widget& w, float ww, float hh) -> void {
    const BuildContext ctx;
    w.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = ww, .height = hh}};
    w.layout(cc, ctx);
    Painter p;
    p.begin(static_cast<int>(ww), static_cast<int>(hh));
    w.paint(p, Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = ww, .height = hh}}, ctx);
}

/// 在控件中心合成「按下+抬起」（独立渲染坐标系：bounds 原点为 0,0）。
auto click_center(Widget& w) -> void {
    const Rect bb{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = w.size()};
    const Point center{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
    MouseEvent press;
    press.position = center;
    press.action = MouseAction::Press;
    press.button = MouseButton::Left;
    EventDispatcher::dispatch(w, press);
    MouseEvent release;
    release.position = center;
    release.action = MouseAction::Release;
    release.button = MouseButton::Left;
    EventDispatcher::dispatch(w, release);
}

}  // namespace

AURORA_TEST_CASE(base_widgets_compose_and_render) {
    Column col{Node{Divider{}},
               Node{Divider{DividerProps{.orientation = Orientation::Vertical}}},
               Node{Checkbox{Reactive{false}}},
               Node{Switch{Reactive{true}}},
               Node{Slider{Reactive{0.5}}},
               Node{ProgressIndicator{Reactive{0.3}}}};
    render_tree(col, 320.0F, 480.0F);
    // 渲染走通无崩溃即通过（像素精确值依赖字体渲染，不做像素断言）。
    AURORA_TEST_CHECK_TRUE(col.size().height > 0.0F && col.size().height <= 480.0F);
}

AURORA_TEST_CASE(checkbox_toggles_on_full_click) {
    Checkbox cb{Reactive{false}};
    render_tree(cb, 40.0F, 40.0F);
    click_center(cb);
    AURORA_TEST_CHECK_TRUE(cb.value());
}

AURORA_TEST_CASE(slider_drag_sets_low_value) {
    Slider sl{Reactive{0.5}};
    render_tree(sl, 200.0F, 24.0F);

    const Rect bb{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = sl.size()};
    const float left = bb.origin.x + 4.0F;
    const float mid_y = bb.origin.y + (bb.size.height / 2.0F);

    MouseEvent e;
    e.position = Point{.x = bb.origin.x + (bb.size.width / 2.0F), .y = mid_y};
    e.action = MouseAction::Press;
    EventDispatcher::dispatch(sl, e);
    e.position = Point{.x = left, .y = mid_y};
    e.action = MouseAction::Move;
    EventDispatcher::dispatch(sl, e);
    e.action = MouseAction::Release;
    EventDispatcher::dispatch(sl, e);
    AURORA_TEST_CHECK_TRUE(sl.value() < 0.2);
}

AURORA_TEST_CASE(stack_align_offset_rounded_clip_render) {
    Text aligned{"Aligned"};
    aligned.modifier.set(Modifier{}.align(Alignment::BottomRight).background(Color::blue(), 8.0F).clip_rounded(8.0F));
    Text offset{"Offset"};
    offset.modifier.set(Modifier{}.offset(20.0F, 10.0F).background(Color::green()));

    Stack stacked{Node{std::move(aligned)}, Node{std::move(offset)}};
    render_tree(stacked, 200.0F, 200.0F);
    // Align/Offset/圆角裁剪渲染走通（Stack 尺寸策略不在此断言，渲染无崩溃即通过）。
    AURORA_TEST_CHECK_MSG(true, "Align/Offset/rounded-clip render without crash");
}

AURORA_TEST_CASE(drag_gesture_reports_delta) {
    bool dragged = false;
    Point last_delta{.x = 0.0F, .y = 0.0F};
    Text drag{"Drag me"};
    drag.modifier.set(Modifier{}.draggable([&dragged, &last_delta](Point d, Point) -> void {
        dragged = true;
        last_delta = d;
    }));
    render_tree(drag, 120.0F, 40.0F);

    const Rect bb{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = drag.size()};
    const Point center{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};

    MouseEvent e;
    e.position = center;
    e.action = MouseAction::Press;
    EventDispatcher::dispatch(drag, e);
    e.position = Point{.x = center.x + 20.0F, .y = center.y};
    e.action = MouseAction::Move;
    EventDispatcher::dispatch(drag, e);
    e.action = MouseAction::Release;
    EventDispatcher::dispatch(drag, e);
    AURORA_TEST_CHECK_TRUE(dragged);
    AURORA_TEST_CHECK_NEAR(last_delta.x, 20.0F, 1e-3F);
}

AURORA_TEST_CASE(long_press_fires_after_threshold) {
    bool fired = false;
    Text lp{"Long press"};
    lp.modifier.set(Modifier{}.long_press([&fired]() -> void { fired = true; }, 50.0F));
    render_tree(lp, 120.0F, 40.0F);

    const Rect bb{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = lp.size()};
    MouseEvent e;
    e.position = Point{.x = bb.origin.x + (bb.size.width / 2.0F), .y = bb.origin.y + (bb.size.height / 2.0F)};
    e.action = MouseAction::Press;
    EventDispatcher::dispatch(lp, e);

    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    lp.tick(std::chrono::steady_clock::now());
    AURORA_TEST_CHECK_TRUE(fired);
}

AURORA_TEST_CASE(logical_snapshot_and_pixels_are_deterministic) {
    auto make_tree = []() -> Node {
        auto col = std::make_shared<Column>(ColumnProps{
            .children = {Node{Row{RowProps{.children = {Node{Text{"A"}}, Node{Text{"B"}}}}}}, Node{Text{"C"}}}});
        col->modifier = Modifier{}.fill_max_size();
        return Node{std::move(col)};
    };

    Node t1 = make_tree();
    const Json snap = render_to_logical_snapshot(t1, 200, 200);
    AURORA_TEST_CHECK_STREQ(snap["type"].get<std::string>(), "Column");
    AURORA_TEST_CHECK_EQ(snap["children"].size(), 2U);
    AURORA_TEST_CHECK_STREQ(snap["children"][0]["type"].get<std::string>(), "Row");
    AURORA_TEST_CHECK_NEAR(snap["box"]["w"].get<float>(), 200.0F, 1e-3F);
    AURORA_TEST_CHECK_NEAR(snap["box"]["h"].get<float>(), 200.0F, 1e-3F);

    // 快照确定性：两棵同构树 dump 一致。
    Node t2 = make_tree();
    const Json snap2 = render_to_logical_snapshot(t2, 200, 200);
    AURORA_TEST_CHECK_EQ(snap.dump(), snap2.dump());

    // 像素确定性：同树两次栅格化结果一致。
    auto render_pixels = [](Widget& w, int ww, int hh) -> std::vector<std::uint8_t> {
        const BuildContext ctx;
        w.mount(ctx);
        const Constraints c{.min = Size{.width = 0.0F, .height = 0.0F},
                            .max = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}};
        w.layout(c, ctx);
        Painter p;
        p.begin(ww, hh);
        w.paint(p,
                Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                     .size = Size{.width = static_cast<float>(ww), .height = static_cast<float>(hh)}},
                ctx);
        const std::uint8_t* d = p.data();
        // 像素缓冲首尾指针运算系 Painter::data() 裸指针接口的必要写法；且不改为花括号返回：
        // {d, d+n} 双迭代器初始化与 initializer_list 重载并存有解析陷阱，显式构造意图更清晰。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic,modernize-return-braced-init-list)
        return std::vector(d, d + (static_cast<std::size_t>(ww) * hh * 4));
    };
    Node t3 = make_tree();
    const auto px1 = render_pixels(t3.widget(), 160, 160);
    const auto px2 = render_pixels(t3.widget(), 160, 160);
    AURORA_TEST_CHECK_TRUE(px1 == px2 && !px1.empty());
}

AURORA_TEST_CASE(column_row_alignment_and_props_roundtrip) {
    // Widget 层：Column/Row 经 set_main_axis_size 透传 flex.main_axis_size，撑满父级。
    Column col{Node{Text{"a"}}, Node{Text{"b"}}};
    col.set_main_axis_size(MainAxisSize::Max);
    const BuildContext ctx;
    col.mount(ctx);
    const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 100.0F, .height = 200.0F}};
    const Size s = col.layout(cc, ctx);
    AURORA_TEST_CHECK_NEAR(s.height, 200.0F, 1e-3F);

    Row row{Node{Text{"a"}}, Node{Text{"b"}}};
    row.set_main_axis_size(MainAxisSize::Max);
    const BuildContext rctx;
    row.mount(rctx);
    const Constraints rc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = Size{.width = 200.0F, .height = 50.0F}};
    const Size rs = row.layout(rc, rctx);
    AURORA_TEST_CHECK_NEAR(rs.width, 200.0F, 1e-3F);

    // 链式 setter + 序列化往返（属性键 main_axis_alignment / cross_axis_alignment / main_axis_size / gap）。
    Column col2{Node{Text{"a"}}};
    col2.set_main_axis_alignment(MainAxisAlignment::Center)
        .set_cross_axis_alignment(CrossAxisAlignment::Stretch)
        .set_main_axis_size(MainAxisSize::Max)
        .set_gap(8.0F);
    Json j;
    col2.serialize_props(j);
    AURORA_TEST_CHECK_STREQ(j["main_axis_alignment"].get<std::string>(), "Center");
    AURORA_TEST_CHECK_STREQ(j["cross_axis_alignment"].get<std::string>(), "Stretch");
    AURORA_TEST_CHECK_STREQ(j["main_axis_size"].get<std::string>(), "Max");
    AURORA_TEST_CHECK_NEAR(j["gap"].get<float>(), 8.0F, 1e-4F);

    Column q{Node{Text{"b"}}};
    q.deserialize_props(j);
    Json k;
    q.serialize_props(k);
    AURORA_TEST_CHECK_STREQ(k["main_axis_alignment"].get<std::string>(), "Center");
    AURORA_TEST_CHECK_STREQ(k["cross_axis_alignment"].get<std::string>(), "Stretch");
    AURORA_TEST_CHECK_STREQ(k["main_axis_size"].get<std::string>(), "Max");
    AURORA_TEST_CHECK_NEAR(k["gap"].get<float>(), 8.0F, 1e-4F);
}

}  // namespace aurora::test_cases::itest_widget_components

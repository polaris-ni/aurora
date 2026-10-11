/// 测试类型: unit
/// 目标单元: include/aurora/widget/title_bar.h
/// 测试说明: 覆盖 TitleBar 的 API 面与其余交互分支（与 utest_title_bar.cpp 的 Snap 浮层命中链契约互补）。
/// 覆盖：全部 set_*/get_* 链式配置与取值、set_icon/set_title/set_subtitle/add_action/add_snap_action、
/// type_name/describe_static/describe/collect_signals、serialize/deserialize 往返、on_pointer_event 的
/// 控制钮分支（headless 下 chrome 缺失安全 no-op）、动作 chip 点击触发回调、空白区双击分支、
/// on_layout 动作矩形缓存、on_paint 的图标/标题/副标题/控制钮字形/Snap 浮层绘制分支、extra_hit_box。

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/layout/layout_engine.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/title_bar.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/title_bar_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_title_bar_api {

namespace {

constexpr float AURORA_HOST_W = 320.0F;
constexpr float AURORA_HOST_H = 260.0F;
constexpr float AURORA_BAR_H = 36.0F;
constexpr int AURORA_HOVER_MS = 400;

auto host_constraints() -> Constraints {
    return Constraints{.min = Size{.width = 0.0F, .height = 0.0F},
                       .max = Size{.width = AURORA_HOST_W, .height = AURORA_HOST_H}};
}

auto dummy_image() -> std::shared_ptr<Image> {
    return std::make_shared<Image>(Image{
        .width = 16, .height = 16, .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(16) * 16 * 4U, 255)});
}

auto press(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Press;
    e.button = MouseButton::Left;
    e.local_position = Point{.x = x, .y = y};
    return e;
}

auto move_to(float x, float y) -> MouseEvent {
    MouseEvent e;
    e.action = MouseAction::Move;
    e.button = MouseButton::Left;
    e.position = Point{.x = x, .y = y};  // 派发器按根坐标 position 换算 local_position
    return e;
}

}  // namespace

AURORA_TEST_CASE(api_surface_setters_and_getters) {
    auto bar = std::make_shared<TitleBar>();
    AURORA_TEST_CHECK_EQ(std::string(bar->type_name()), "TitleBar");
    AURORA_TEST_CHECK_EQ(std::string(bar->describe_static().name), "TitleBar");
    AURORA_TEST_CHECK_EQ(std::string(bar->describe().name), "TitleBar");

    bar->set_icon(dummy_image());
    bar->set_title("Document");
    bar->set_subtitle("Draft");
    bar->set_window_controls(true);
    bar->set_height(40.0F);
    bar->set_style(TitleBarStyle{});
    bar->add_action(TitleBarAction{.label = "Menu", .on_click = nullptr});
    bar->add_snap_action(TitleBarAction{.label = "Custom", .on_click = nullptr});

    AURORA_TEST_CHECK_EQ(bar->title(), "Document");
    AURORA_TEST_CHECK_EQ(bar->subtitle(), "Draft");
    AURORA_TEST_CHECK_EQ(bar->window_controls(), true);
    AURORA_TEST_CHECK_EQ(bar->action_count(), 1U);
    AURORA_TEST_CHECK_EQ(bar->snap_entry_count(), 5U);  // 内置 4 + 自定义 1
    AURORA_TEST_CHECK_EQ(bar->snap_open(), false);
    AURORA_TEST_CHECK_TRUE(bar->wants_click());

    // 无信号：collect_signals 空实现。
    std::vector<SignalViewBase *> sigs;
    bar->collect_signals(sigs);
    AURORA_TEST_CHECK_EQ(sigs.size(), 0U);
}

AURORA_TEST_CASE(serialize_and_deserialize_roundtrip) {
    auto bar = std::make_shared<TitleBar>();
    bar->set_title("Doc");
    bar->set_subtitle("Sub");
    bar->set_height(44.0F);
    bar->set_window_controls(false);
    bar->add_snap_action(TitleBarAction{.label = "X", .on_click = nullptr});

    Json props = Json::object();
    bar->serialize_props(props);
    AURORA_TEST_CHECK_TRUE(props.contains("height"));
    AURORA_TEST_CHECK_TRUE(props.contains("title"));
    AURORA_TEST_CHECK_TRUE(props.contains("subtitle"));
    AURORA_TEST_CHECK_TRUE(props.contains("window_controls"));
    AURORA_TEST_CHECK_TRUE(props.contains("snap_action_count"));
    AURORA_TEST_CHECK_EQ(props.at("snap_action_count")->as_or<std::size_t>(0), 1U);

    auto restored = std::make_shared<TitleBar>();
    restored->deserialize_props(props);
    AURORA_TEST_CHECK_EQ(restored->title(), "Doc");
    AURORA_TEST_CHECK_EQ(restored->subtitle(), "Sub");
    AURORA_TEST_CHECK_EQ(restored->window_controls(), false);

    // 缺失字段：反序列化不崩溃、保持现值。
    auto partial = std::make_shared<TitleBar>();
    partial->set_title("Keep");
    Json p2 = Json::object();
    p2.set("height", 50.0);  // 仅含 height
    partial->deserialize_props(p2);
    AURORA_TEST_CHECK_EQ(partial->title(), "Keep");  // 未提供，保持
}

AURORA_TEST_CASE(control_button_press_without_chrome_is_safe) {
    // headless 下 WindowChrome 缺失：控制钮命中分支执行但不调 chrome（is_handled 即可）。
    auto bar = std::make_shared<TitleBar>();
    bar->set_window_controls(true);
    BuildContext ctx;
    bar->mount(ctx);
    LayoutEngine::layout(*bar, host_constraints());

    const TitleBarGeometry geom = title_bar_geometry(AURORA_HOST_W, TitleBarStyle{}, false, true);
    auto center = [](const Rect &r) -> Point {
        return Point{.x = r.origin.x + (r.size.width * 0.5F), .y = r.origin.y + (r.size.height * 0.5F)};
    };
    MouseEvent close_ev = press(center(geom.close).x, center(geom.close).y);
    bar->on_pointer_event(close_ev);
    AURORA_TEST_CHECK_EQ(close_ev.is_handled, true);

    MouseEvent max_ev = press(center(geom.maximize).x, center(geom.maximize).y);
    bar->on_pointer_event(max_ev);
    AURORA_TEST_CHECK_EQ(max_ev.is_handled, true);

    MouseEvent min_ev = press(center(geom.minimize).x, center(geom.minimize).y);
    bar->on_pointer_event(min_ev);
    AURORA_TEST_CHECK_EQ(min_ev.is_handled, true);
}

AURORA_TEST_CASE(action_chip_press_fires_callback) {
    // 关闭控制钮以隔离：动作 chip 从右缘起排，命中即触发 on_click。
    auto bar = std::make_shared<TitleBar>();
    bar->set_window_controls(false);
    int hits = 0;
    bar->add_action(TitleBarAction{.label = "Foo", .on_click = [&hits]() { ++hits; }});
    BuildContext ctx;
    bar->mount(ctx);
    LayoutEngine::layout(*bar, host_constraints());

    // 动作矩形：cursor = w - 12，rect=[cursor-tw, cursor]，tw = measure("Foo", size13) + 16。
    Font f;
    f.size_pt = 13.0F;
    const float tw = render::FontEngine::measure_width("Foo", f) + 16.0F;
    const float cx = (AURORA_HOST_W - 12.0F) - (tw * 0.5F);
    MouseEvent e = press(cx, 5.0F);
    bar->on_pointer_event(e);
    AURORA_TEST_CHECK_EQ(hits, 1);
    AURORA_TEST_CHECK_EQ(e.is_handled, true);
}

AURORA_TEST_CASE(blank_area_double_click_branch_reaches_chrome_guard) {
    // 空白区两次 Press（<300ms、<5px）：dbl 为真，chrome 缺失跳过——覆盖 dbl 分支。
    auto bar = std::make_shared<TitleBar>();
    bar->set_window_controls(false);  // 无控制钮/动作，点落在空白
    BuildContext ctx;
    bar->mount(ctx);
    LayoutEngine::layout(*bar, host_constraints());

    MouseEvent first = press(5.0F, 5.0F);
    bar->on_pointer_event(first);
    AURORA_TEST_CHECK_EQ(first.is_handled, true);
    MouseEvent second = press(5.0F, 5.0F);
    bar->on_pointer_event(second);
    AURORA_TEST_CHECK_EQ(second.is_handled, true);
}

AURORA_TEST_CASE(paint_covers_icon_title_subtitle_glyphs_and_snap) {
    // 布局后绘制：覆盖图标/标题/副标题/控制钮字形分支；悬停开窗后绘制覆盖 Snap 浮层绘制分支。
    auto bar = std::make_shared<TitleBar>();
    bar->set_icon(dummy_image());
    bar->set_title("Doc");
    bar->set_subtitle("Sub");
    bar->set_window_controls(true);

    auto root = std::make_shared<Column>(ColumnProps{.children = {Node{bar}}, .gap = 0.0F});
    LayoutEngine::layout(*root, host_constraints());

    Painter painter;
    Rect bounds{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = AURORA_HOST_W, .height = AURORA_HOST_H}};
    bar->paint(painter, bounds, BuildContext{});  // 图标 + 标题 + 副标题 + 控制钮字形

    // 悬停最大化钮 ≥400ms → 真实路径展开 Snap 浮层。
    const TitleBarGeometry geom = title_bar_geometry(AURORA_HOST_W, TitleBarStyle{}, false, true);
    const Point mc{.x = geom.maximize.origin.x + (geom.maximize.size.width * 0.5F),
                   .y = geom.maximize.origin.y + (geom.maximize.size.height * 0.5F)};
    MouseEvent hover1 = move_to(mc.x, mc.y);
    EventDispatcher::dispatch(*root, hover1, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(AURORA_HOVER_MS + 60));
    MouseEvent hover2 = move_to(mc.x, mc.y);
    EventDispatcher::dispatch(*root, hover2, nullptr);
    AURORA_TEST_CHECK(bar->snap_open());

    LayoutEngine::layout(*root, host_constraints());
    bar->paint(painter, bounds, BuildContext{});  // Snap 浮层绘制分支

    // extra_hit_box 在展开态返回弹窗矩形。
    AURORA_TEST_CHECK(bar->extra_hit_box(BuildContext{}, Point{.x = 0.0F, .y = 0.0F}).has_value());
    AURORA_TEST_CHECK(bar->covers_extra_hit_box(Point{.x = geom.maximize.origin.x, .y = AURORA_BAR_H + 4.0F},
                                                BuildContext{}, Point{.x = 0.0F, .y = 0.0F}));
}

}  // namespace aurora::test_cases::utest_title_bar_api

// 目标源单元：event/gesture.h

// ── API 覆盖映射 ─────────────────────────────
// event/event.h(MouseEvent/KeyEvent/TouchEvent 等事件载荷，经派发与手势用例行使)。

#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/event/dispatcher.h"
#include "aurora/event/event.h"
#include "aurora/event/gesture.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/modifier/modifier.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/text.h"

#include "test_harness.h"

using aurora::BuildContext;
using aurora::Button;
using aurora::Column;
using aurora::ColumnProps;
using aurora::Constraints;
using aurora::EventDispatcher;
using aurora::HitNode;
using aurora::LocalizedString;
using aurora::Modifier;
using aurora::MouseAction;
using aurora::MouseEvent;
using aurora::Node;
using aurora::PinchRecognizer;
using aurora::Point;
using aurora::Rect;
using aurora::RotationRecognizer;
using aurora::Size;
using aurora::Text;
using aurora::TouchDispatcher;
using aurora::TouchEvent;
using aurora::TouchPoint;
using aurora::Widget;

namespace aurora::tests::sec_gesture {

namespace {
// 在根节点 layout 后补设绝对 bounds（dispatch 依赖根 bounds 做 Clickable 自命中）。
auto layout_root(Widget &root, const Constraints &c, const BuildContext &ctx) -> Size {
    const Size s = root.layout(c, ctx);
    return s;
}
} // namespace

static void run() {
    BuildContext ctx;
    Constraints c;
    c.max = Size{ .width = 400.0f, .height = 400.0f };

    // -------------------------------------------------------------------------
    // 场景 1：可点击父级（Column+Clickable）内嵌可拖拽子级（Button+Draggable）。
    // 拖拽子级时父级 click 不应触发（命中链自底向上冒泡，子级先消费）。
    // -------------------------------------------------------------------------
    {
        int parent_clicked = 0;
        int drag_fired = 0;

        Button child;
        child.label.set(LocalizedString{ "drag me" });
        child.modifier.set(
            Modifier{}
                .size(120.0f, 120.0f)
                .draggable([&](Point, Point) -> void { drag_fired++; }, []() -> void {}, []() -> void {}));

        Column parent{ ColumnProps{ .children = { Node{ std::move(child) } } } };
        parent.modifier.set(Modifier{}.size(200.0f, 200.0f).clickable([&]() -> void { parent_clicked++; }));

        layout_root(parent, c, ctx);

        // 命中链自底向上应含 [parent, child]（根→最深）。
        std::vector<HitNode> chain = parent.hit_test_chain(Point{ .x = 20.0f, .y = 20.0f },
                                                           Rect{ .origin = Point{}, .size = parent.size() }, ctx);
        AURORA_TEST_CHECK(chain.size() == 2); // 父 + 子

        // 拖拽序列：Press -> Move -> Release（在子级区域内）。
        MouseEvent press;
        press.position = Point{ .x = 20.0f, .y = 20.0f };
        press.action = MouseAction::Press;
        EventDispatcher::dispatch(parent, press);

        MouseEvent move;
        move.position = Point{ .x = 60.0f, .y = 60.0f };
        move.action = MouseAction::Move;
        EventDispatcher::dispatch(parent, move);

        MouseEvent release;
        release.position = Point{ .x = 60.0f, .y = 60.0f };
        release.action = MouseAction::Release;
        EventDispatcher::dispatch(parent, release);

        AURORA_TEST_CHECK(drag_fired > 0);      // 子级拖拽确实触发
        AURORA_TEST_CHECK(parent_clicked == 0); // 父级不应被误触发
        AURORA_LOG_INFO("test", "[1] drag on draggable child: parent click suppressed OK");

        // 仅轻点（Press+Release 无移动）：子级有手势亦应先消费，父级仍不触发。
        MouseEvent tap_press;
        tap_press.position = Point{ .x = 20.0f, .y = 20.0f };
        tap_press.action = MouseAction::Press;
        EventDispatcher::dispatch(parent, tap_press);
        MouseEvent tap_release;
        tap_release.position = Point{ .x = 20.0f, .y = 20.0f };
        tap_release.action = MouseAction::Release;
        EventDispatcher::dispatch(parent, tap_release);
        AURORA_TEST_CHECK(parent_clicked == 0);
        AURORA_LOG_INFO("test", "[1b] tap on draggable child: parent click suppressed OK");
    }

    // -------------------------------------------------------------------------
    // 场景 2：可点击父级 + 纯展示子级（Text，无自身手势）。
    // 点击子级应冒泡到父级，触发父级 Clickable（修复前“命中即止”会命中 Text 而非父级）。
    // -------------------------------------------------------------------------
    {
        int parent_clicked = 0;

        Text txt;
        txt.content.set(LocalizedString{ "hello" });

        Column parent{ ColumnProps{ .children = { Node{ std::move(txt) } } } };
        parent.modifier.set(Modifier{}.size(200.0f, 200.0f).clickable([&]() -> void { parent_clicked++; }));

        layout_root(parent, c, ctx);

        MouseEvent press;
        press.position = Point{ .x = 10.0f, .y = 10.0f };
        press.action = MouseAction::Press;
        EventDispatcher::dispatch(parent, press);
        MouseEvent release;
        release.position = Point{ .x = 10.0f, .y = 10.0f };
        release.action = MouseAction::Release;
        EventDispatcher::dispatch(parent, release);

        AURORA_TEST_CHECK(parent_clicked == 1); // 冒泡到父级 Clickable
        AURORA_LOG_INFO("test", "[2] click on display child bubbles to parent Clickable OK");
    }

    // -------------------------------------------------------------------------
    // 场景 3：长按与点击互斥。同一控件同时含 Clickable + LongPress：
    // 超过阈值触发长按后，松开不应再触发 click；正常短按则触发 click 且不触发长按。
    // -------------------------------------------------------------------------
    {
        int clicked = 0;
        int long_pressed = 0;

        Column w{ ColumnProps{} };
        w.modifier.set(Modifier{}.size(120.0f, 60.0f).clickable([&]() -> void { clicked++; }).long_press([&]() -> void {
            long_pressed++;
        }));
        layout_root(w, c, ctx);

        // 短按（立即抬起）：click 触发，长按不触发。
        {
            MouseEvent press;
            press.position = Point{ .x = 30.0f, .y = 30.0f };
            press.action = MouseAction::Press;
            EventDispatcher::dispatch(w, press);
            MouseEvent release;
            release.position = Point{ .x = 30.0f, .y = 30.0f };
            release.action = MouseAction::Release;
            EventDispatcher::dispatch(w, release);
            AURORA_TEST_CHECK(clicked == 1);
            AURORA_TEST_CHECK(long_pressed == 0);
            AURORA_LOG_INFO("test", "[3a] short tap fires click, no long press OK");
        }

        // 长按：Press 后 tick 超过阈值，触发一次长按；松开 click 被抑制。
        {
            MouseEvent press;
            press.position = Point{ .x = 30.0f, .y = 30.0f };
            press.action = MouseAction::Press;
            EventDispatcher::dispatch(w, press);

            const auto later = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
            w.tick(later); // 超过默认 500ms 阈值
            AURORA_TEST_CHECK(long_pressed == 1);

            MouseEvent release;
            release.position = Point{ .x = 30.0f, .y = 30.0f };
            release.action = MouseAction::Release;
            EventDispatcher::dispatch(w, release);
            AURORA_TEST_CHECK(long_pressed == 1); // 仅触发一次
            AURORA_TEST_CHECK(clicked == 1);      // 未被长按吞掉（仍为短按时的值）
            AURORA_LOG_INFO("test", "[3b] long press fires once, click suppressed after long press OK");
        }
    }

    AURORA_LOG_INFO("test", "ALL GESTURE TESTS PASSED");
}
} // namespace aurora::tests::sec_gesture

namespace aurora::tests::sec_multitouch {

// ---------- TouchEvent 基础 ----------

static void test_touch_event_basic() {
    TouchEvent e;
    AURORA_TEST_CHECK(e.active_count() == 0);

    e.points.push_back(TouchPoint{ .id = 0,
                                   .position = Point{ .x = 100, .y = 100 },
                                   .prev_position = Point{ .x = 100, .y = 100 },
                                   .active = true });
    AURORA_TEST_CHECK(e.active_count() == 1);

    e.points.push_back(TouchPoint{ .id = 1,
                                   .position = Point{ .x = 200, .y = 100 },
                                   .prev_position = Point{ .x = 200, .y = 100 },
                                   .active = true });
    AURORA_TEST_CHECK(e.active_count() == 2);

    // 距离 = 100
    const float dist = e.pinch_distance();
    AURORA_TEST_CHECK(std::abs(dist - 100.0f) < 0.01f);

    // 角度 = 0（水平）
    const float angle = e.pinch_angle();
    AURORA_TEST_CHECK(std::abs(angle) < 0.01f);
}

static void test_touch_inactive() {
    TouchEvent e;
    e.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e.points.push_back(TouchPoint{ .id = 1,
                                   .position = Point{ .x = 100, .y = 0 },
                                   .prev_position = Point{ .x = 100, .y = 0 },
                                   .active = false }); // 已抬起

    AURORA_TEST_CHECK(e.active_count() == 1);
    AURORA_TEST_CHECK(e.pinch_distance() == 0.0f); // 不足 2 个活跃点
}

// ---------- PinchRecognizer ----------

static void test_pinch_recognizer() {
    PinchRecognizer pinch;
    AURORA_TEST_CHECK(!pinch.is_active());
    AURORA_TEST_CHECK(pinch.scale() == 1.0f);

    // 初始双指距离 100
    TouchEvent e1;
    e1.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e1.points.push_back(TouchPoint{
        .id = 1, .position = Point{ .x = 100, .y = 0 }, .prev_position = Point{ .x = 100, .y = 0 }, .active = true });
    pinch.on_touch(e1);
    AURORA_TEST_CHECK(pinch.is_active());
    AURORA_TEST_CHECK(std::abs(pinch.scale() - 1.0f) < 0.01f);

    // 双指张开到 200 → scale = 2.0
    TouchEvent e2;
    e2.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e2.points.push_back(TouchPoint{
        .id = 1, .position = Point{ .x = 200, .y = 0 }, .prev_position = Point{ .x = 100, .y = 0 }, .active = true });
    pinch.on_touch(e2);
    AURORA_TEST_CHECK(std::abs(pinch.scale() - 2.0f) < 0.01f);

    // 双指捏合到 50 → scale = 0.5
    TouchEvent e3;
    e3.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e3.points.push_back(TouchPoint{
        .id = 1, .position = Point{ .x = 50, .y = 0 }, .prev_position = Point{ .x = 200, .y = 0 }, .active = true });
    pinch.on_touch(e3);
    AURORA_TEST_CHECK(std::abs(pinch.scale() - 0.5f) < 0.01f);

    // 抬起一指 → 不活跃
    TouchEvent e4;
    e4.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    pinch.on_touch(e4);
    AURORA_TEST_CHECK(!pinch.is_active());
    AURORA_TEST_CHECK(pinch.scale() == 1.0f);
}

// ---------- RotationRecognizer ----------

static void test_rotation_recognizer() {
    RotationRecognizer rot;
    AURORA_TEST_CHECK(!rot.is_active());
    AURORA_TEST_CHECK(rot.angle_delta() == 0.0f);

    // 初始角度：水平（0 弧度）
    TouchEvent e1;
    e1.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e1.points.push_back(TouchPoint{
        .id = 1, .position = Point{ .x = 100, .y = 0 }, .prev_position = Point{ .x = 100, .y = 0 }, .active = true });
    rot.on_touch(e1);
    AURORA_TEST_CHECK(rot.is_active());
    AURORA_TEST_CHECK(std::abs(rot.angle_delta()) < 0.1f);

    // 旋转 90 度（第二指移到正上方）
    TouchEvent e2;
    e2.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e2.points.push_back(TouchPoint{
        .id = 1, .position = Point{ .x = 0, .y = -100 }, .prev_position = Point{ .x = 100, .y = 0 }, .active = true });
    rot.on_touch(e2);
    // atan2(-100, 0) = -PI/2，delta = -PI/2 - 0 = -90 度
    AURORA_TEST_CHECK(std::abs(rot.angle_delta() - (-90.0f)) < 1.0f);
}

// ---------- 重置 ----------

static void test_reset() {
    PinchRecognizer pinch;
    TouchEvent e;
    e.points.push_back(TouchPoint{
        .id = 0, .position = Point{ .x = 0, .y = 0 }, .prev_position = Point{ .x = 0, .y = 0 }, .active = true });
    e.points.push_back(TouchPoint{
        .id = 1, .position = Point{ .x = 100, .y = 0 }, .prev_position = Point{ .x = 100, .y = 0 }, .active = true });
    pinch.on_touch(e);
    AURORA_TEST_CHECK(pinch.is_active());

    pinch.reset();
    AURORA_TEST_CHECK(!pinch.is_active());
    AURORA_TEST_CHECK(pinch.scale() == 1.0f);
}

// ---------- 并发派发：多指各自命中不同控件 ----------

static void test_concurrent_drag_routing() {
    constexpr BuildContext ctx;
    Constraints c;
    c.max = Size{ .width = 400.0f, .height = 400.0f };

    int drag_a = 0;
    int drag_b = 0;
    Button a;
    a.label.set(LocalizedString{ "A" });
    a.modifier.set(Modifier{}
                       .size(120.0f, 120.0f)
                       .draggable([&](Point, Point) -> void { drag_a++; }, []() -> void {}, []() -> void {}));
    Button b;
    b.label.set(LocalizedString{ "B" });
    b.modifier.set(Modifier{}
                       .size(120.0f, 120.0f)
                       .draggable([&](Point, Point) -> void { drag_b++; }, []() -> void {}, []() -> void {}));

    Column col{ ColumnProps{ .children = { Node{ std::move(a) }, Node{ std::move(b) } } } };
    col.layout(c, ctx);
    TouchDispatcher td;

    // 双指：pointer 0 落在 A(60,60)，pointer 1 落在 B(60,180)。
    TouchEvent press;
    press.points.push_back(TouchPoint{ .id = 0,
                                       .position = Point{ .x = 60.0f, .y = 60.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                       .active = true });
    press.points.push_back(TouchPoint{ .id = 1,
                                       .position = Point{ .x = 60.0f, .y = 180.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 180.0f },
                                       .active = true });
    td.dispatch(col, press);

    // 双指移动
    TouchEvent move;
    move.points.push_back(TouchPoint{ .id = 0,
                                      .position = Point{ .x = 90.0f, .y = 90.0f },
                                      .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                      .active = true });
    move.points.push_back(TouchPoint{ .id = 1,
                                      .position = Point{ .x = 90.0f, .y = 210.0f },
                                      .prev_position = Point{ .x = 60.0f, .y = 180.0f },
                                      .active = true });
    td.dispatch(col, move);

    // 双指抬起
    TouchEvent release;
    release.points.push_back(TouchPoint{ .id = 0,
                                         .position = Point{ .x = 90.0f, .y = 90.0f },
                                         .prev_position = Point{ .x = 90.0f, .y = 90.0f },
                                         .active = false });
    release.points.push_back(TouchPoint{ .id = 1,
                                         .position = Point{ .x = 90.0f, .y = 210.0f },
                                         .prev_position = Point{ .x = 90.0f, .y = 210.0f },
                                         .active = false });
    td.dispatch(col, release);

    AURORA_TEST_CHECK(drag_a > 0); // A 收到 pointer 0 的拖拽
    AURORA_TEST_CHECK(drag_b > 0); // B 收到 pointer 1 的拖拽（并发互不干扰）
}

// ---------- 指针捕获：抬起前移出控件仍路由到原控件 ----------

static void test_pointer_capture() {
    constexpr BuildContext ctx;
    Constraints c;
    c.max = Size{ .width = 400.0f, .height = 400.0f };

    int drag_a = 0;
    Button a;
    a.label.set(LocalizedString{ "A" });
    a.modifier.set(Modifier{}
                       .size(120.0f, 120.0f)
                       .draggable([&](Point, Point) -> void { drag_a++; }, []() -> void {}, []() -> void {}));

    Column col{ ColumnProps{ .children = { Node{ std::move(a) } } } };
    col.layout(c, ctx);
    TouchDispatcher td;

    // 在 A 内按下 pointer 0
    TouchEvent press;
    press.points.push_back(TouchPoint{ .id = 0,
                                       .position = Point{ .x = 60.0f, .y = 60.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                       .active = true });
    td.dispatch(col, press);

    // 移出 A（y=300）但同一 pointer id，应按捕获仍路由到 A
    TouchEvent move;
    move.points.push_back(TouchPoint{ .id = 0,
                                      .position = Point{ .x = 60.0f, .y = 300.0f },
                                      .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                      .active = true });
    td.dispatch(col, move);

    AURORA_TEST_CHECK(drag_a > 0); // 指针捕获生效

    // 抬起后清除捕获
    TouchEvent release;
    release.points.push_back(TouchPoint{ .id = 0,
                                         .position = Point{ .x = 60.0f, .y = 300.0f },
                                         .prev_position = Point{ .x = 60.0f, .y = 300.0f },
                                         .active = false });
    td.dispatch(col, release);
}

// ---------- 同一控件双指：仅首个指针绑定手势 ----------

static void test_gesture_pointer_binding() {
    constexpr BuildContext ctx;
    Constraints c;
    c.max = Size{ .width = 400.0f, .height = 400.0f };

    int drag_fires = 0;
    int start_count = 0;
    Button a;
    a.label.set(LocalizedString{ "A" });
    a.modifier.set(
        Modifier{}
            .size(120.0f, 120.0f)
            .draggable([&](Point, Point) -> void { drag_fires++; }, [&]() -> void { start_count++; }, []() -> void {}));

    Column col{ ColumnProps{ .children = { Node{ std::move(a) } } } };
    col.layout(c, ctx);
    TouchDispatcher td;

    // 双指都落在 A 上
    TouchEvent press;
    press.points.push_back(TouchPoint{ .id = 0,
                                       .position = Point{ .x = 60.0f, .y = 60.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                       .active = true });
    press.points.push_back(TouchPoint{ .id = 1,
                                       .position = Point{ .x = 80.0f, .y = 60.0f },
                                       .prev_position = Point{ .x = 80.0f, .y = 60.0f },
                                       .active = true });
    td.dispatch(col, press);

    AURORA_TEST_CHECK(start_count == 1); // 仅 pointer 0 触发 start（绑定首个指针）

    // 双指移动
    TouchEvent move;
    move.points.push_back(TouchPoint{ .id = 0,
                                      .position = Point{ .x = 90.0f, .y = 90.0f },
                                      .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                      .active = true });
    move.points.push_back(TouchPoint{ .id = 1,
                                      .position = Point{ .x = 100.0f, .y = 90.0f },
                                      .prev_position = Point{ .x = 80.0f, .y = 60.0f },
                                      .active = true });
    td.dispatch(col, move);

    AURORA_TEST_CHECK(drag_fires == 1); // 仅 pointer 0 拖拽（pointer 1 被绑定忽略）
}

static void run() {
    test_touch_event_basic();
    test_touch_inactive();
    test_pinch_recognizer();
    test_rotation_recognizer();
    test_reset();
    test_concurrent_drag_routing();
    test_pointer_capture();
    test_gesture_pointer_binding();
}
} // namespace aurora::tests::sec_multitouch

namespace aurora::tests::sec_pointer_concurrency {

static void run() {
    constexpr BuildContext ctx;
    Constraints c;
    c.max = Size{ .width = 400.0f, .height = 400.0f };

    // -------------------------------------------------------------------------
    // 场景 1：单个控件上的 `touch()` 修饰器应收到完整（多指）原始事件。
    // -------------------------------------------------------------------------
    {
        int raw_fires = 0;
        int last_point_count = 0;
        Button a;
        a.label.set(LocalizedString{ "A" });
        a.modifier.set(Modifier{}.size(120.0f, 120.0f).touch([&](const TouchEvent &e) -> void {
            raw_fires++;
            last_point_count = static_cast<int>(e.points.size());
        }));

        Column col{ ColumnProps{ .children = { Node{ std::move(a) } } } };
        col.layout(c, ctx);
        TouchDispatcher td;

        TouchEvent e;
        e.points.push_back(TouchPoint{ .id = 0,
                                       .position = Point{ .x = 60.0f, .y = 60.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                       .active = true });
        e.points.push_back(TouchPoint{ .id = 1,
                                       .position = Point{ .x = 80.0f, .y = 80.0f },
                                       .prev_position = Point{ .x = 80.0f, .y = 80.0f },
                                       .active = true });
        td.dispatch(col, e);

        AURORA_TEST_CHECK(raw_fires > 0);         // 原始流回调确实被调用
        AURORA_TEST_CHECK(last_point_count == 2); // 交付的是完整（双指）事件，而非单点
        AURORA_LOG_INFO("test", "[1] touch() modifier receives full multi-pointer event OK");
    }

    // -------------------------------------------------------------------------
    // 场景 2：两个控件各带 `touch()`，双指分别命中时各自收到原始流（并发）。
    // -------------------------------------------------------------------------
    {
        int raw_a = 0;
        int raw_b = 0;
        Button a;
        a.label.set(LocalizedString{ "A" });
        a.modifier.set(Modifier{}.size(120.0f, 120.0f).touch([&](const TouchEvent &) -> void { raw_a++; }));
        Button b;
        b.label.set(LocalizedString{ "B" });
        b.modifier.set(Modifier{}.size(120.0f, 120.0f).touch([&](const TouchEvent &) -> void { raw_b++; }));

        Column col{ ColumnProps{ .children = { Node{ std::move(a) }, Node{ std::move(b) } } } };
        col.layout(c, ctx);
        TouchDispatcher td;

        TouchEvent e;
        e.points.push_back(TouchPoint{ .id = 0,
                                       .position = Point{ .x = 60.0f, .y = 60.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 60.0f },
                                       .active = true });
        e.points.push_back(TouchPoint{ .id = 1,
                                       .position = Point{ .x = 60.0f, .y = 180.0f },
                                       .prev_position = Point{ .x = 60.0f, .y = 180.0f },
                                       .active = true });
        td.dispatch(col, e);

        AURORA_TEST_CHECK(raw_a > 0); // A 命中链收到原始流
        AURORA_TEST_CHECK(raw_b > 0); // B 命中链收到原始流（并发互不串台）
        AURORA_LOG_INFO("test", "[2] concurrent touch() streams per widget OK");
    }
}
} // namespace aurora::tests::sec_pointer_concurrency

AURORA_TEST() {
    aurora::tests::sec_gesture::run();
    aurora::tests::sec_multitouch::run();
    aurora::tests::sec_pointer_concurrency::run();
}

// test_drag_drop.cpp — 拖放协议测试。
#include <string>

#include "aurora/aurora.h"
#include "aurora/event/drag_drop.h"
#include "test_harness.h"

using aurora::DragData;
using aurora::DragSession;
using aurora::DropTargetCallbacks;
using aurora::Json;
using aurora::Point;

// ---------- DragData ----------

static void test_drag_data_text() {
    const DragData d = DragData::text("Hello");
    AURORA_TEST_CHECK(d.mime_type == "text/plain");
    AURORA_TEST_CHECK(d.payload == "Hello");
    AURORA_TEST_CHECK(!d.empty());
}

static void test_drag_data_widget() {
    const auto tree = Json{{"type", "Button"}, {"props", {{"label", "OK"}}}};
    DragData d = DragData::widget_tree(tree);
    AURORA_TEST_CHECK(d.mime_type == "aurora/widget");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(d.payload["type"] == "Button");
    AURORA_TEST_CHECK(!d.empty());
}

static void test_drag_data_empty() {
    const DragData d;
    AURORA_TEST_CHECK(d.empty());
    AURORA_TEST_CHECK(d.mime_type.empty());
}

// ---------- DragSession ----------

static void test_drag_session() {
    DragSession session;
    AURORA_TEST_CHECK(!session.is_active());

    session.begin(DragData::text("drag me"), Point{.x = 10, .y = 20});
    AURORA_TEST_CHECK(session.is_active());
    AURORA_TEST_CHECK(session.data().mime_type == "text/plain");
    AURORA_TEST_CHECK(session.origin().x == 10.0F);
    AURORA_TEST_CHECK(session.origin().y == 20.0F);

    session.end();
    AURORA_TEST_CHECK(!session.is_active());
    AURORA_TEST_CHECK(session.data().empty());
}

// ---------- DropTargetCallbacks ----------

static void test_drop_target() {
    bool entered = false;
    bool dropped = false;
    Point drop_pos;

    DropTargetCallbacks cb;
    cb.on_drag_enter = [&](const DragData &d) -> bool {
        entered = true;
        return d.mime_type == "text/plain";
    };
    cb.on_drop = [&](const DragData &, Point pos) -> void {
        dropped = true;
        drop_pos = pos;
    };

    // 模拟拖入
    const DragData text_data = DragData::text("hello");
    AURORA_TEST_CHECK(cb.on_drag_enter(text_data));
    AURORA_TEST_CHECK(entered);

    // 模拟放置
    cb.on_drop(text_data, Point{.x = 50, .y = 60});
    AURORA_TEST_CHECK(dropped);
    AURORA_TEST_CHECK(drop_pos.x == 50.0F);
    AURORA_TEST_CHECK(drop_pos.y == 60.0F);

    // 非文本类型被拒绝
    const DragData widget_data = DragData::widget_tree(Json::object());
    AURORA_TEST_CHECK(!cb.on_drag_enter(widget_data));
}

AURORA_TEST() {
    test_drag_data_text();
    test_drag_data_widget();
    test_drag_data_empty();
    test_drag_session();
    test_drop_target();
}
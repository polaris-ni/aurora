/// 测试类型: unit
/// 目标单元: include/aurora/event/drag_drop.h
/// 测试说明: DragData 的 text/widget_tree 工厂与 empty 语义、DragSession 会话状态机（begin/end/重复
/// end/重启替换数据与起点）、DropTargetCallbacks 回调契约（enter 返回接受与否、leave、drop 携带数据与局部坐标）

#include <string>

#include "aurora/event/drag_drop.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_drag_drop {

AURORA_TEST_CASE(drag_data_text_factory_sets_mime_and_payload) {
    const auto data = DragData::text("hello");
    AURORA_TEST_CHECK_STREQ(data.mime_type, "text/plain");
    AURORA_TEST_REQUIRE(data.payload.is_string());
    AURORA_TEST_CHECK_STREQ(data.payload.get<std::string>(), "hello");
    AURORA_TEST_CHECK_FALSE(data.empty());
}

AURORA_TEST_CASE(drag_data_widget_tree_factory_moves_json) {
    Json tree = Json::object();
    tree["type"] = "Button";
    tree["children"] = Json::array({Json("Text")});

    const auto data = DragData::widget_tree(tree);
    AURORA_TEST_CHECK_STREQ(data.mime_type, "aurora/widget");
    AURORA_TEST_REQUIRE(data.payload.is_object());
    AURORA_TEST_CHECK_EQ(data.payload["type"].get<std::string>(), std::string{"Button"});
    AURORA_TEST_CHECK_EQ(data.payload["children"].size(), std::size_t{1});
    AURORA_TEST_CHECK_FALSE(data.empty());
}

AURORA_TEST_CASE(default_drag_data_is_empty) {
    const DragData data{};
    AURORA_TEST_CHECK_TRUE(data.empty());
    AURORA_TEST_CHECK(data.mime_type.empty());
    AURORA_TEST_CHECK_TRUE(data.payload.is_null());
}

AURORA_TEST_CASE(drag_session_begin_end_round_trip) {
    DragSession session;
    AURORA_TEST_CHECK_FALSE(session.is_active());
    AURORA_TEST_CHECK_TRUE(session.data().empty());

    session.begin(DragData::text("payload"), Point{.x = 3.0F, .y = 4.0F});
    AURORA_TEST_CHECK_TRUE(session.is_active());
    AURORA_TEST_CHECK_STREQ(session.data().mime_type, "text/plain");
    AURORA_TEST_CHECK_NEAR(session.origin().x, 3.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(session.origin().y, 4.0F, 1e-6F);

    session.end();
    AURORA_TEST_CHECK_FALSE(session.is_active());
    AURORA_TEST_CHECK_TRUE(session.data().empty());  // 数据随会话一起清空

    session.end();  // 重复 end 安全
    AURORA_TEST_CHECK_FALSE(session.is_active());
}

AURORA_TEST_CASE(drag_session_rebegin_replaces_data_and_origin) {
    DragSession session;
    session.begin(DragData::text("first"), Point{.x = 0.0F, .y = 0.0F});
    session.begin(DragData::text("second"), Point{.x = 9.0F, .y = 9.0F});
    AURORA_TEST_CHECK_TRUE(session.is_active());
    AURORA_TEST_CHECK_STREQ(session.data().payload.get<std::string>(), "second");
    AURORA_TEST_CHECK_NEAR(session.origin().x, 9.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(session.origin().y, 9.0F, 1e-6F);
}

AURORA_TEST_CASE(drop_target_callbacks_fire_with_contract_arguments) {
    DropTargetCallbacks callbacks;
    int enter_calls = 0;
    int leave_calls = 0;
    int drop_calls = 0;
    std::string dropped_mime;
    Point dropped_at{};

    callbacks.on_drag_enter = [&enter_calls](const DragData& data) -> bool {
        ++enter_calls;
        return data.mime_type == "text/plain";  // 只接受文本
    };
    callbacks.on_drag_leave = [&leave_calls]() -> void { ++leave_calls; };
    callbacks.on_drop = [&](const DragData& data, Point local_pos) -> void {
        ++drop_calls;
        dropped_mime = data.mime_type;
        dropped_at = local_pos;
    };

    AURORA_TEST_CHECK_TRUE(callbacks.on_drag_enter(DragData::text("t")));
    AURORA_TEST_CHECK_FALSE(callbacks.on_drag_enter(DragData::widget_tree(Json::object())));
    AURORA_TEST_CHECK_EQ(enter_calls, 2);

    callbacks.on_drag_leave();
    AURORA_TEST_CHECK_EQ(leave_calls, 1);

    callbacks.on_drop(DragData::text("u"), Point{.x = 5.0F, .y = 6.0F});
    AURORA_TEST_CHECK_EQ(drop_calls, 1);
    AURORA_TEST_CHECK_STREQ(dropped_mime, "text/plain");
    AURORA_TEST_CHECK_NEAR(dropped_at.x, 5.0F, 1e-6F);
    AURORA_TEST_CHECK_NEAR(dropped_at.y, 6.0F, 1e-6F);
}

}  // namespace aurora::test_cases::utest_drag_drop

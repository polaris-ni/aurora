/// 测试类型: unit
/// 目标单元: include/aurora/render/display_list.h
/// 测试说明: 覆盖 DisplayList 的命令与数据池：空态判定、各池 add_* 的索引递增与独立编号、push_cmd 记账、
/// clear 全池复位，以及 replay 到 Painter 的像素等价性与空列表回放的无副作用

#include <cstdint>
#include <string>
#include <vector>

#include "aurora/core/color.h"
#include "aurora/core/font.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_display_list {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}
}  // namespace

AURORA_TEST_CASE(fresh_list_is_empty) {
    const DisplayList list;
    AURORA_TEST_CHECK_TRUE(list.empty());
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 0U);
}

AURORA_TEST_CASE(pools_assign_independent_increasing_indices) {
    // 每类池各自从 0 编号：DrawCmd 用不同字段分别引用，索引互不干扰。
    DisplayList list;
    AURORA_TEST_CHECK_EQ(list.add_string("first"), 0);
    AURORA_TEST_CHECK_EQ(list.add_string("second"), 1);
    AURORA_TEST_CHECK_EQ(list.add_colors({Color::red(), Color::blue()}), 0);
    AURORA_TEST_CHECK_EQ(list.add_floats({0.0F, 1.0F}), 0);
    AURORA_TEST_CHECK_EQ(list.add_font(Font{}), 0);
    AURORA_TEST_CHECK_EQ(list.add_image(Image{}), 0);
    AURORA_TEST_CHECK_EQ(list.add_matrix(Matrix2D{}), 0);
}

AURORA_TEST_CASE(push_cmd_tracks_count_and_breaks_emptiness) {
    DisplayList list;
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 8.0F, 8.0F);
    cmd.color = Color::red();
    list.push_cmd(cmd);

    AURORA_TEST_CHECK_FALSE(list.empty());
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 1U);

    list.push_cmd(cmd);
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 2U);
}

AURORA_TEST_CASE(clear_resets_commands) {
    DisplayList list;
    list.add_string("text");
    list.push_cmd(DrawCmd{});
    list.clear();

    AURORA_TEST_CHECK_TRUE(list.empty());
    AURORA_TEST_CHECK_EQ(list.cmd_count(), 0U);
    // 清空后池重新从 0 编号（旧索引不可复用）。
    AURORA_TEST_CHECK_EQ(list.add_string("fresh"), 0);
}

AURORA_TEST_CASE(replay_fill_rect_writes_pixels) {
    // 录制一条 FillRect 并回放：结果与直接调用 Painter 等价。
    DrawCmd cmd;
    cmd.kind = CmdKind::FillRect;
    cmd.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    cmd.color = Color::red();

    DisplayList list;
    list.push_cmd(cmd);

    Painter painter;
    painter.begin(4, 4);
    list.replay(painter);

    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).r), 255);  // R
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).g), 0);    // G
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).b), 0);    // B
}

AURORA_TEST_CASE(replay_preserves_command_order) {
    // 后录制的命令覆盖先录制的（画家用直接覆盖语义合成）。
    DisplayList list;
    DrawCmd red;
    red.kind = CmdKind::FillRect;
    red.bounds = rect_at(0.0F, 0.0F, 4.0F, 4.0F);
    red.color = Color::red();

    DrawCmd blue = red;
    blue.color = Color::blue();

    list.push_cmd(red);
    list.push_cmd(blue);

    Painter painter;
    painter.begin(4, 4);
    list.replay(painter);

    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).r), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(painter.get_pixel(0, 0).b), 255);
}

AURORA_TEST_CASE(replay_empty_list_is_noop) {
    const DisplayList list;
    Painter painter;
    painter.begin(2, 2);
    AURORA_TEST_CHECK_NO_THROW(list.replay(painter));
}

}  // namespace aurora::test_cases::utest_display_list

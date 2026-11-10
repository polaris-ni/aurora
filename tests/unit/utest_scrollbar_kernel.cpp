/// 测试类型: unit
/// 目标单元: include/aurora/widget/detail/scrollbar.h
/// 测试说明: 钉住四容器共用滚动条内核的几何与交互口径——内容二倍时的滑块半长、理论滑块长小于下界的夹取、
///           极小视口下不触发 std::clamp 的 lo > hi 前提、轨道长度非正时不绘制、拖拽映射与退化几何、
///           轨道翻页方向、悬停加粗对中、Overlay 保持窗口按注入时间推进。时钟一律注入，不读墙钟。

#include <chrono>

#include "aurora/widget/detail/scrollbar.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_scrollbar_kernel {

namespace {

using aurora::detail::AURORA_SCROLLBAR_HOVER_GROWTH;
using aurora::detail::AURORA_SCROLLBAR_INSET;
using aurora::detail::AURORA_SCROLLBAR_MIN_THUMB;
using aurora::detail::AURORA_SCROLLBAR_OVERLAY_HOLD;
using aurora::detail::scrollbar_contains;
using aurora::detail::scrollbar_drag_delta;
using aurora::detail::scrollbar_geometry;
using aurora::detail::scrollbar_hover_thumb;
using aurora::detail::scrollbar_page_offset;
using aurora::detail::scrollbar_thumb_contains;
using aurora::detail::scrollbar_thumb_length;
using aurora::detail::ScrollbarAxis;
using aurora::detail::ScrollbarInteraction;
using aurora::detail::ScrollbarMetrics;
using aurora::detail::ScrollbarPointerAction;
using aurora::testing::require_value;

using Clock = std::chrono::steady_clock;

/// @brief 造一份纵向、视口 100 dp 的度量；容器自身盒宽 40、高与视口长一致。
auto vertical_metrics(float content, float viewport = 100.0F, float offset = 0.0F) -> ScrollbarMetrics {
    return ScrollbarMetrics{.content_length = content,
                            .viewport_length = viewport,
                            .offset = offset,
                            .thickness = 4.0F,
                            .inset = AURORA_SCROLLBAR_INSET,
                            .min_thumb = AURORA_SCROLLBAR_MIN_THUMB};
}

auto vertical_box(float height = 100.0F) -> Rect {
    return Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 40.0F, .height = height}};
}

}  // namespace

AURORA_TEST_CASE(two_times_content_gives_half_track_thumb) {
    const auto m = vertical_metrics(200.0F);
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Vertical, vertical_box());
    AURORA_TEST_REQUIRE_TRUE(g.drawable);
    AURORA_TEST_CHECK_NEAR(g.track_length, 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.max_offset, 100.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(scrollbar_thumb_length(g, ScrollbarAxis::Vertical), 48.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.track.origin.x, 34.0F, 1e-4F);  // 40 - inset 2 - thickness 4
    AURORA_TEST_CHECK_NEAR(g.track.origin.y, 2.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.thumb.origin.y, 2.0F, 1e-4F);  // offset 0 ⇒ 贴轨道起点
}

AURORA_TEST_CASE(theoretical_thumb_below_floor_is_clamped_to_24) {
    // 视口 100 / 内容 800 ⇒ 理论滑块长 96 × 100 / 800 = 12，夹到 24。
    const auto m = vertical_metrics(800.0F);
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Vertical, vertical_box());
    AURORA_TEST_REQUIRE_TRUE(g.drawable);
    AURORA_TEST_CHECK_NEAR(scrollbar_thumb_length(g, ScrollbarAxis::Vertical), 24.0F, 1e-4F);
}

AURORA_TEST_CASE(tiny_viewport_clamps_floor_to_track_length) {
    // track_length = 20 - 4 = 16 < 24：下界须先与轨道长取小，否则 std::clamp 前提违例。
    const auto m = vertical_metrics(100.0F, 20.0F);
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Vertical, vertical_box(20.0F));
    AURORA_TEST_REQUIRE_TRUE(g.drawable);
    AURORA_TEST_CHECK_NEAR(g.track_length, 16.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(scrollbar_thumb_length(g, ScrollbarAxis::Vertical), 16.0F, 1e-4F);
    AURORA_TEST_CHECK_FALSE(scrollbar_drag_delta(g, ScrollbarAxis::Vertical, 10.0F).has_value());
}

AURORA_TEST_CASE(non_positive_track_length_and_no_overflow_do_not_draw) {
    const auto tiny = scrollbar_geometry(vertical_metrics(100.0F, 4.0F), ScrollbarAxis::Vertical, vertical_box(4.0F));
    AURORA_TEST_CHECK_TRUE(tiny.overflowing);
    AURORA_TEST_CHECK_FALSE(tiny.drawable);
    const auto fitted = scrollbar_geometry(vertical_metrics(100.0F), ScrollbarAxis::Vertical, vertical_box());
    AURORA_TEST_CHECK_FALSE(fitted.overflowing);
    AURORA_TEST_CHECK_FALSE(fitted.drawable);
}

AURORA_TEST_CASE(drag_mapping_uses_max_offset_over_free_track) {
    const auto m = vertical_metrics(200.0F);
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Vertical, vertical_box());
    // free track = 96 - 48 = 48；指针移 24 ⇒ 偏移 24 × 100 / 48 = 50。
    // 「断言持有值 + 取值」收进 `require_value`：判定交给报告器函数后，路径分析无法认定已检查，
    // 调用点直接 `*opt` 会被 `bugprone-unchecked-optional-access` 判为未检查访问。
    const auto delta = require_value(scrollbar_drag_delta(g, ScrollbarAxis::Vertical, 24.0F));
    AURORA_TEST_CHECK_NEAR(delta, 50.0F, 1e-4F);
}

AURORA_TEST_CASE(horizontal_axis_uses_the_same_formulas_along_x) {
    ScrollbarMetrics m = vertical_metrics(200.0F);
    const Rect box{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 100.0F, .height = 40.0F}};
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Horizontal, box);
    AURORA_TEST_REQUIRE_TRUE(g.drawable);
    AURORA_TEST_CHECK_NEAR(g.track.origin.y, 34.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.track.size.width, 96.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(scrollbar_thumb_length(g, ScrollbarAxis::Horizontal), 48.0F, 1e-4F);
    m.offset = 50.0F;
    const auto half = scrollbar_geometry(m, ScrollbarAxis::Horizontal, box);
    AURORA_TEST_CHECK_NEAR(half.thumb.origin.x - half.track.origin.x, 24.0F, 1e-4F);
}

AURORA_TEST_CASE(page_click_moves_one_viewport_in_each_direction) {
    const auto m = vertical_metrics(400.0F, 100.0F, 100.0F);
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Vertical, vertical_box());
    AURORA_TEST_REQUIRE_TRUE(g.drawable);
    const Point before{.x = 35.0F, .y = g.track.origin.y + 1.0F};
    const Point after{.x = 35.0F, .y = g.track.origin.y + 60.0F};
    AURORA_TEST_CHECK_NEAR(scrollbar_page_offset(g, m, ScrollbarAxis::Vertical, before), 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(scrollbar_page_offset(g, m, ScrollbarAxis::Vertical, after), 200.0F, 1e-4F);
    const Point on_thumb{.x = 35.0F, .y = g.thumb.origin.y + 2.0F};
    AURORA_TEST_CHECK_NEAR(scrollbar_page_offset(g, m, ScrollbarAxis::Vertical, on_thumb), 100.0F, 1e-4F);
}

AURORA_TEST_CASE(hover_thumb_grows_around_the_track_axis) {
    const auto g = scrollbar_geometry(vertical_metrics(200.0F), ScrollbarAxis::Vertical, vertical_box());
    const Rect grown = scrollbar_hover_thumb(g, ScrollbarAxis::Vertical, AURORA_SCROLLBAR_HOVER_GROWTH);
    AURORA_TEST_CHECK_NEAR(grown.origin.x + (grown.size.width / 2.0F), g.thumb.origin.x + (g.thumb.size.width / 2.0F),
                           1e-4F);
    AURORA_TEST_CHECK_NEAR(grown.size.width - g.thumb.size.width, AURORA_SCROLLBAR_HOVER_GROWTH, 1e-4F);
    AURORA_TEST_CHECK_NEAR(grown.size.height, g.thumb.size.height, 1e-4F);
    AURORA_TEST_CHECK_TRUE(scrollbar_thumb_contains(g, ScrollbarAxis::Vertical,
                                                    Point{.x = grown.origin.x + 1.0F, .y = g.thumb.origin.y + 2.0F}));
    AURORA_TEST_CHECK_TRUE(scrollbar_contains(g, ScrollbarAxis::Vertical, Point{.x = 35.0F, .y = 90.0F}));
    AURORA_TEST_CHECK_FALSE(scrollbar_contains(g, ScrollbarAxis::Vertical, Point{.x = 5.0F, .y = 50.0F}));
}

AURORA_TEST_CASE(pointer_actions_capture_drag_and_page_once) {
    const auto m = vertical_metrics(200.0F);
    const auto g = scrollbar_geometry(m, ScrollbarAxis::Vertical, vertical_box());
    ScrollbarInteraction state;
    const Clock::time_point now{};
    const Point on_thumb{.x = 35.0F, .y = g.thumb.origin.y + 5.0F};
    const auto press =
        state.on_pointer(ScrollbarPointerAction::Press, on_thumb, g, m, ScrollbarAxis::Vertical, false, now, true);
    AURORA_TEST_REQUIRE_TRUE(press.handled);
    AURORA_TEST_CHECK_FALSE(press.offset_changed);
    AURORA_TEST_CHECK_TRUE(state.dragging);
    const auto move = state.on_pointer(ScrollbarPointerAction::Move, Point{.x = 35.0F, .y = on_thumb.y + 24.0F}, g, m,
                                       ScrollbarAxis::Vertical, false, now, true);
    AURORA_TEST_REQUIRE_TRUE(move.offset_changed);
    AURORA_TEST_CHECK_NEAR(move.offset, 50.0F, 1e-4F);
    const auto release =
        state.on_pointer(ScrollbarPointerAction::Release, on_thumb, g, m, ScrollbarAxis::Vertical, false, now, true);
    AURORA_TEST_CHECK_TRUE(release.handled);
    AURORA_TEST_CHECK_FALSE(state.dragging);

    // 轨道点击翻一页，但按住不动（后续 Move）不重复翻页；条外按下不消费事件。
    const Point on_track{.x = 35.0F, .y = g.track.origin.y + 80.0F};
    const auto page =
        state.on_pointer(ScrollbarPointerAction::Press, on_track, g, m, ScrollbarAxis::Vertical, false, now, true);
    AURORA_TEST_REQUIRE_TRUE(page.offset_changed);
    const auto held =
        state.on_pointer(ScrollbarPointerAction::Move, on_track, g, m, ScrollbarAxis::Vertical, false, now, true);
    AURORA_TEST_CHECK_FALSE(held.offset_changed);
    const auto outside = state.on_pointer(ScrollbarPointerAction::Press, Point{.x = 1.0F, .y = 1.0F}, g, m,
                                          ScrollbarAxis::Vertical, false, now, true);
    AURORA_TEST_CHECK_FALSE(outside.handled);
}

AURORA_TEST_CASE(overlay_visibility_advances_only_with_injected_time) {
    ScrollbarInteraction state;
    const Clock::time_point start{};
    AURORA_TEST_CHECK_FALSE(state.advance(start, true));  // 无指针、无滚动 ⇒ 恒不显
    AURORA_TEST_CHECK_FALSE(state.visible);
    state.pointer_inside_container = true;
    AURORA_TEST_CHECK_TRUE(state.advance(start, true));  // 指针进入
    AURORA_TEST_CHECK_TRUE(state.visible);
    state.pointer_inside_container = false;
    state.on_scroll(start, true);
    const auto before_expiry = start + AURORA_SCROLLBAR_OVERLAY_HOLD - std::chrono::milliseconds{1};
    AURORA_TEST_CHECK_FALSE(state.advance(before_expiry, true));  // 保持窗口内仍显
    AURORA_TEST_CHECK_FALSE(state.advance(before_expiry, true));  // 同帧两次定夺不再翻转
    const auto after_expiry = start + AURORA_SCROLLBAR_OVERLAY_HOLD + std::chrono::milliseconds{1};
    AURORA_TEST_CHECK_TRUE(state.advance(after_expiry, true));  // 到期隐藏
    AURORA_TEST_CHECK_FALSE(state.visible);
    AURORA_TEST_CHECK_TRUE(state.advance(start, false));  // Classic 档恒可见
    AURORA_TEST_CHECK_TRUE(state.visible);
}

}  // namespace aurora::test_cases::utest_scrollbar_kernel

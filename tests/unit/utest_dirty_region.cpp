/// 测试类型: unit
/// 目标单元: include/aurora/render/dirty_region.h
/// 测试说明: 覆盖 DirtyRegionTracker 的空/整帧状态机、零面积矩形忽略、重叠合并为并集、相邻不合并、
/// 超限退化为整帧、上限可调与 clear 复位；全程经 fixture 保存/还原静态上限（避免跨用例污染）

#include <cstddef>

#include "aurora/core/types.h"
#include "aurora/render/dirty_region.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_dirty_region {

namespace {
[[nodiscard]] auto rect_at(float x, float y, float w, float h) -> Rect {
    return Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = h}};
}
}  // namespace

/// @brief 静态上限 `max_rects_` 是进程级共享状态，用例必须自净。
class MaxRectsGuard : public ::aurora::testing::Fixture {
  protected:
    auto SetUp() -> void override { saved_ = DirtyRegionTracker::max_rects(); }
    auto TearDown() -> void override { DirtyRegionTracker::set_max_rects(saved_); }

  private:
    std::size_t saved_ = DirtyRegionTracker::AURORA_MAX_RECTS;
};

AURORA_TEST_F(MaxRectsGuard, starts_empty) {
    DirtyRegionTracker tracker;
    AURORA_TEST_CHECK_TRUE(tracker.is_empty());
    AURORA_TEST_CHECK_FALSE(tracker.is_full());
    AURORA_TEST_CHECK_EQ(tracker.rects().size(), 0U);
}

AURORA_TEST_F(MaxRectsGuard, mark_records_rect_and_bounds) {
    DirtyRegionTracker tracker;
    tracker.mark(rect_at(10.0F, 20.0F, 30.0F, 40.0F));

    AURORA_TEST_CHECK_FALSE(tracker.is_empty());
    AURORA_TEST_CHECK_FALSE(tracker.is_full());
    AURORA_TEST_REQUIRE_EQ(tracker.rects().size(), 1U);

    const Rect bounds = tracker.merged_bounds();
    AURORA_TEST_CHECK_NEAR(bounds.origin.x, 10.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.origin.y, 20.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.width, 30.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.height, 40.0, 1e-6);
}

AURORA_TEST_F(MaxRectsGuard, mark_ignores_non_positive_area) {
    DirtyRegionTracker tracker;
    tracker.mark(rect_at(0.0F, 0.0F, 0.0F, 10.0F));  // 零宽
    tracker.mark(rect_at(0.0F, 0.0F, 10.0F, -5.0F));  // 负高
    AURORA_TEST_CHECK_TRUE(tracker.is_empty());
}

AURORA_TEST_F(MaxRectsGuard, overlapping_rects_merge_into_union) {
    DirtyRegionTracker tracker;
    tracker.mark(rect_at(0.0F, 0.0F, 10.0F, 10.0F));
    tracker.mark(rect_at(5.0F, 5.0F, 10.0F, 10.0F));

    AURORA_TEST_REQUIRE_EQ(tracker.rects().size(), 1U);
    const Rect bounds = tracker.merged_bounds();
    AURORA_TEST_CHECK_NEAR(bounds.origin.x, 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.origin.y, 0.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.width, 15.0, 1e-6);
    AURORA_TEST_CHECK_NEAR(bounds.size.height, 15.0, 1e-6);
}

AURORA_TEST_F(MaxRectsGuard, disjoint_rects_stay_separate) {
    // 仅边相接不算重叠（严格小于判定），故不合并 —— 与常见脏区实现一致。
    DirtyRegionTracker tracker;
    tracker.mark(rect_at(0.0F, 0.0F, 10.0F, 10.0F));
    tracker.mark(rect_at(10.0F, 0.0F, 10.0F, 10.0F));
    AURORA_TEST_CHECK_EQ(tracker.rects().size(), 2U);
}

AURORA_TEST_F(MaxRectsGuard, mark_all_enters_full_frame) {
    DirtyRegionTracker tracker;
    tracker.mark(rect_at(0.0F, 0.0F, 4.0F, 4.0F));
    tracker.mark_all();

    AURORA_TEST_CHECK_TRUE(tracker.is_full());
    AURORA_TEST_CHECK_FALSE(tracker.is_empty());
    AURORA_TEST_CHECK_EQ(tracker.rects().size(), 0U);
}

AURORA_TEST_F(MaxRectsGuard, mark_after_full_is_noop) {
    // 已整帧脏时再 mark 不再记账，避免无意义的合并开销。
    DirtyRegionTracker tracker;
    tracker.mark_all();
    tracker.mark(rect_at(0.0F, 0.0F, 4.0F, 4.0F));
    AURORA_TEST_CHECK_TRUE(tracker.is_full());
    AURORA_TEST_CHECK_EQ(tracker.rects().size(), 0U);
}

AURORA_TEST_F(MaxRectsGuard, exceeding_limit_degrades_to_full_frame) {
    DirtyRegionTracker tracker;
    DirtyRegionTracker::set_max_rects(4);
    for (int i = 0; i < 5; ++i) {
        tracker.mark(rect_at(static_cast<float>(i) * 100.0F, 0.0F, 10.0F, 10.0F));
    }
    AURORA_TEST_CHECK_TRUE(tracker.is_full());
}

AURORA_TEST_F(MaxRectsGuard, raising_limit_avoids_degradation) {
    // 提高上限只增加局部重绘精度，不改变正确性（结果像素与整帧重绘一致）。
    DirtyRegionTracker tracker;
    DirtyRegionTracker::set_max_rects(64);
    for (int i = 0; i < 5; ++i) {
        tracker.mark(rect_at(static_cast<float>(i) * 100.0F, 0.0F, 10.0F, 10.0F));
    }
    AURORA_TEST_CHECK_FALSE(tracker.is_full());
    AURORA_TEST_CHECK_EQ(tracker.rects().size(), 5U);
}

AURORA_TEST_F(MaxRectsGuard, clear_resets_state) {
    DirtyRegionTracker tracker;
    tracker.mark_all();
    tracker.clear();
    AURORA_TEST_CHECK_TRUE(tracker.is_empty());
    AURORA_TEST_CHECK_FALSE(tracker.is_full());

    tracker.mark(rect_at(0.0F, 0.0F, 8.0F, 8.0F));
    tracker.clear();
    AURORA_TEST_CHECK_TRUE(tracker.is_empty());
    AURORA_TEST_CHECK_EQ(tracker.rects().size(), 0U);
}

}  // namespace aurora::test_cases::utest_dirty_region

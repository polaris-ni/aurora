/// 测试类型: unit
/// 目标单元: include/aurora/widget/alignment.h
/// 测试说明: 覆盖九宫格 Alignment 的 align_origin 全枚举映射（容器 100x50、子项 40x20 →
/// cx=30/cy=15）与子项等于容器时的退化（全部归零）

#include "aurora/widget/alignment.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_alignment {

namespace {

constexpr Size AURORA_CONTAINER{.width = 100.0F, .height = 50.0F};
constexpr Size AURORA_CHILD{.width = 40.0F, .height = 20.0F};

}  // namespace

AURORA_TEST_CASE(align_origin_maps_all_nine_positions) {
    const auto o = [](Alignment a) -> Point { return align_origin(a, AURORA_CHILD, AURORA_CONTAINER); };

    const Point tl = o(Alignment::TopLeft);
    AURORA_TEST_CHECK_NEAR(tl.x, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(tl.y, 0.0F, 0.0F);

    const Point tc = o(Alignment::TopCenter);
    AURORA_TEST_CHECK_NEAR(tc.x, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(tc.y, 0.0F, 0.0F);

    const Point tr = o(Alignment::TopRight);
    AURORA_TEST_CHECK_NEAR(tr.x, 60.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(tr.y, 0.0F, 0.0F);

    const Point cl = o(Alignment::CenterLeft);
    AURORA_TEST_CHECK_NEAR(cl.x, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(cl.y, 15.0F, 0.0F);

    const Point c = o(Alignment::Center);
    AURORA_TEST_CHECK_NEAR(c.x, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(c.y, 15.0F, 0.0F);

    const Point cr = o(Alignment::CenterRight);
    AURORA_TEST_CHECK_NEAR(cr.x, 60.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(cr.y, 15.0F, 0.0F);

    const Point bl = o(Alignment::BottomLeft);
    AURORA_TEST_CHECK_NEAR(bl.x, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(bl.y, 30.0F, 0.0F);

    const Point bc = o(Alignment::BottomCenter);
    AURORA_TEST_CHECK_NEAR(bc.x, 30.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(bc.y, 30.0F, 0.0F);

    const Point br = o(Alignment::BottomRight);
    AURORA_TEST_CHECK_NEAR(br.x, 60.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(br.y, 30.0F, 0.0F);
}

AURORA_TEST_CASE(align_origin_degenerates_to_zero_when_child_fills_container) {
    // 子项与容器同尺寸：任何对齐都落在原点（无自由空间）。
    const Size same{.width = 100.0F, .height = 50.0F};
    for (const Alignment a :
         {Alignment::TopLeft, Alignment::TopCenter, Alignment::TopRight, Alignment::CenterLeft, Alignment::Center,
          Alignment::CenterRight, Alignment::BottomLeft, Alignment::BottomCenter, Alignment::BottomRight}) {
        const Point p = align_origin(a, same, AURORA_CONTAINER);
        AURORA_TEST_CHECK_NEAR(p.x, 0.0F, 0.0F);
        AURORA_TEST_CHECK_NEAR(p.y, 0.0F, 0.0F);
    }
}

AURORA_TEST_CASE(align_origin_handles_oversized_child) {
    // 子项大于容器：负偏移（居中时向左/上溢出），符合公式语义。
    const Size big{.width = 140.0F, .height = 90.0F};
    const Point c = align_origin(Alignment::Center, big, AURORA_CONTAINER);
    AURORA_TEST_CHECK_NEAR(c.x, -20.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(c.y, -20.0F, 0.0F);
    const Point br = align_origin(Alignment::BottomRight, big, AURORA_CONTAINER);
    AURORA_TEST_CHECK_NEAR(br.x, -40.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(br.y, -40.0F, 0.0F);
}

}  // namespace aurora::test_cases::utest_alignment

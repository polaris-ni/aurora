/// 测试类型: unit
/// 目标单元: include/aurora/layout/flex.h
/// 测试说明: 覆盖 Flex 参数结构的默认值契约（Row/Start/Start/gap 0/MainAxisSize::Min）与逐字段赋值回读

#include "aurora/layout/flex.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_flex {

AURORA_TEST_CASE(defaults_are_row_start_start_zero_gap_min) {
    const Flex f;
    AURORA_TEST_CHECK_EQ(f.direction, FlexDirection::Row);
    AURORA_TEST_CHECK_EQ(f.main_axis, MainAxisAlignment::Start);
    AURORA_TEST_CHECK_EQ(f.cross_axis, CrossAxisAlignment::Start);
    AURORA_TEST_CHECK_NEAR(f.gap, 0.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(f.main_axis_size, MainAxisSize::Min);
}

AURORA_TEST_CASE(fields_round_trip_after_assignment) {
    Flex f;
    f.direction = FlexDirection::ColumnReverse;
    f.main_axis = MainAxisAlignment::SpaceBetween;
    f.cross_axis = CrossAxisAlignment::Stretch;
    f.gap = 8.0F;
    f.main_axis_size = MainAxisSize::Max;

    AURORA_TEST_CHECK_EQ(f.direction, FlexDirection::ColumnReverse);
    AURORA_TEST_CHECK_EQ(f.main_axis, MainAxisAlignment::SpaceBetween);
    AURORA_TEST_CHECK_EQ(f.cross_axis, CrossAxisAlignment::Stretch);
    AURORA_TEST_CHECK_NEAR(f.gap, 8.0F, 1e-5F);
    AURORA_TEST_CHECK_EQ(f.main_axis_size, MainAxisSize::Max);
}

}  // namespace aurora::test_cases::utest_flex

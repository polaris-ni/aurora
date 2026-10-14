/// 测试类型: unit
/// 目标单元: include/aurora/theming/type_scale.h
/// 测试说明: 覆盖排版角色集——body_medium 逐字段等于默认构造的 Font（旧 Theme::font 的语义承接，
/// 保证读 theme.font 的图表族机械迁移后观感不变）；15 档取值单调、字重符合 M3 口径；值语义可拷贝。

#include "aurora/theming/type_scale.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_type_scale {

AURORA_TEST_CASE(body_medium_is_default_font) {
    // 旧 Theme::font 是默认构造 Font（sans-serif / 14pt / 400）；由 body_medium 承接。
    AURORA_TEST_CHECK(TypeScale{}.body_medium == Font{});
}

AURORA_TEST_CASE(scale_is_monotonic_within_display) {
    const TypeScale t;
    AURORA_TEST_CHECK(t.display_large.size_pt > t.display_medium.size_pt);
    AURORA_TEST_CHECK(t.display_medium.size_pt > t.display_small.size_pt);
    AURORA_TEST_CHECK(t.headline_large.size_pt > t.headline_small.size_pt);
}

AURORA_TEST_CASE(label_and_title_medium_are_medium_weight) {
    const TypeScale t;
    AURORA_TEST_CHECK_EQ(t.label_large.weight, 500);
    AURORA_TEST_CHECK_EQ(t.title_medium.weight, 500);
    AURORA_TEST_CHECK_EQ(t.body_large.weight, 400);  // 正文 Regular
}

AURORA_TEST_CASE(type_scale_is_value_copyable) {
    const TypeScale src;
    // 拷贝构造本身就是本用例的被测行为（TypeScale 须可值拷贝、随 Theme 深拷贝）。
    const TypeScale copy = src;  // NOLINT(performance-unnecessary-copy-initialization) 拷贝即测试点
    AURORA_TEST_CHECK(copy.body_medium == src.body_medium);
    AURORA_TEST_CHECK(copy.display_small.family == src.display_small.family);
}

}  // namespace aurora::test_cases::utest_type_scale

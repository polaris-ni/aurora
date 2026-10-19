/// 测试类型: unit
/// 目标单元: include/aurora/window/window_frame_style.h
/// 测试说明: 锁定可配窗框样式的默认值契约——`WindowFrameStyle{}` 必须逐值复现本库引入该类型
///   之前的阴影观感（厚度 10dp、基色 rgba(0,0,0,70)、模糊 = margin−1、直角），使「新增可配
///   能力」本身不构成视觉 Breaking；并锁聚合初始化的部分覆写语义。
/// 纯值断言，不连任何合成器 / 窗口系统。

#include "aurora/window/window_frame_style.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_window_frame_style {

AURORA_TEST_CASE(defaults_reproduce_legacy_shadow) {
    const WindowFrameStyle f;
    // 期望值全部手写字面量（不引内部常量）：引常量会把这条断言变成同义反复，
    // 改了常量却看不出默认值是否跟着漂。
    AURORA_TEST_CHECK_TRUE(f.shadow_enabled);
    AURORA_TEST_CHECK_EQ(f.shadow_margin_dp, 10);
    AURORA_TEST_CHECK_EQ(f.shadow_color.r, 0);
    AURORA_TEST_CHECK_EQ(f.shadow_color.g, 0);
    AURORA_TEST_CHECK_EQ(f.shadow_color.b, 0);
    AURORA_TEST_CHECK_EQ(f.shadow_color.a, 70);
    AURORA_TEST_CHECK_EQ(f.shadow_blur_radius, 0.0F);  // 0 = 走「margin − 1」推导
    AURORA_TEST_CHECK_EQ(f.corner_radius, 0.0F);  // 0 = 直角（现状）
}

AURORA_TEST_CASE(aggregate_init_overrides_only_listed_fields) {
    // 纯数据聚合：指定初始化只覆写列出字段，其余保持默认（可直接 WindowFrameStyle{...} 定制）。
    const WindowFrameStyle f{.shadow_margin_dp = 24, .corner_radius = 8.0F};
    AURORA_TEST_CHECK_EQ(f.shadow_margin_dp, 24);
    AURORA_TEST_CHECK_EQ(f.corner_radius, 8.0F);
    AURORA_TEST_CHECK_TRUE(f.shadow_enabled);  // 未列出 → 默认
    AURORA_TEST_CHECK_EQ(f.shadow_color.a, 70);
}

AURORA_TEST_CASE(shadow_enabled_false_keeps_other_components) {
    // 关闭投影只撤投影本身：圆角等不依赖合成能力的分量必须照常保留，
    // 否则「想关掉投影」会顺手把用户配的圆角也吃掉。
    const WindowFrameStyle f{.shadow_enabled = false, .corner_radius = 12.0F};
    AURORA_TEST_CHECK_FALSE(f.shadow_enabled);
    AURORA_TEST_CHECK_EQ(f.corner_radius, 12.0F);
    AURORA_TEST_CHECK_EQ(f.shadow_margin_dp, 10);  // 字段仍在，由 frame_margin_dp 折算为 0
}

}  // namespace aurora::test_cases::utest_window_frame_style

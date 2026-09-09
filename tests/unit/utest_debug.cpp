/// 测试类型: unit
/// 目标单元: include/aurora/core/debug.h
/// 测试说明: 渲染纯度检查门面（check_render_purity）的关闭态 no-op 与开启态硬失败（死亡测试），以及经 debug 叠层 API
/// 的运行时能力探测与全局状态恢复纪律（测试 TU 不写 #if 门控，按仓库既定模式运行时探测 + SKIP）

#include <string>

#include "aurora/core/debug.h"
#include "aurora/debug/debug_paint.h"  // 始终声明的运行时查询：借其探测 AURORA_ENABLE_DEBUG 是否生效
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_debug {

/// @brief 运行时探测 AURORA_ENABLE_DEBUG 是否生效。
///
/// core/debug.h 的深度守卫符号（g_paint_depth / PaintPurityGuard）按宏裁切、无 is_enabled 类
/// 查询，测试 TU 又禁止 #if 门控——故借用 debug 叠层 API（头文件始终声明，关闭时 set_flags 为
/// no-op）：置一个标志位再读回，读回成功即 DEBUG 构建探测成立。探测前后用先读原值、finally 式
/// set_flags 恢复，保证不污染其他用例。
[[nodiscard]] static auto probe_debug_enabled() -> bool {
    const auto original = aurora::debug::flags();
    aurora::debug::DebugPaintFlags probe;
    probe.layout_guides = true;
    aurora::debug::set_flags(probe);
    const bool enabled = aurora::debug::any_flag_enabled();
    aurora::debug::set_flags(original);
    return enabled;
}

AURORA_TEST_CASE(probe_detects_debug_capability_and_restores_state) {
    const auto before = aurora::debug::flags();
    const bool first = probe_debug_enabled();
    AURORA_TEST_CHECK_EQ(probe_debug_enabled(), first);  // 探测幂等

    // 探测不残留全局状态：flags 回到用例开始时的值（逐字段比较）。
    const auto after = aurora::debug::flags();
    AURORA_TEST_CHECK_EQ(after.layout_guides, before.layout_guides);
    AURORA_TEST_CHECK_EQ(after.relayout_boundaries, before.relayout_boundaries);
    AURORA_TEST_CHECK_EQ(after.layer_borders, before.layer_borders);
    AURORA_TEST_CHECK_EQ(after.repaint_highlight, before.repaint_highlight);
    AURORA_TEST_CHECK_EQ(after.overdraw, before.overdraw);
}

AURORA_TEST_CASE(check_render_purity_is_noop_when_debug_disabled) {
    // 注入点类 API 语义：头文件始终声明，关闭时返回 disabled 值（no-op），可无守卫安全调用。
    if (probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 已启用：无守卫调用的硬失败行为由死亡测试用例覆盖");
    }
    AURORA_TEST_CHECK_NO_THROW(aurora::debug::check_render_purity());
}

AURORA_TEST_CASE(check_render_purity_hard_fails_outside_paint_context_when_enabled) {
    // 开启态契约：check_render_purity 必须在绘制上下文（g_paint_depth > 0）内调用；
    // 脱离渲染遍历直接调用属反模式，由 AURORA_CHECK 兜底（stderr 输出消息后 abort，常开）。
    if (!probe_debug_enabled()) {
        AURORA_TEST_SKIP("AURORA_ENABLE_DEBUG 未启用：深度守卫符号按宏裁切，无硬失败行为");
    }
    AURORA_TEST_CHECK_DEATH(aurora::debug::check_render_purity(), "check_render_purity");
}

AURORA_TEST_CASE(always_declared_debug_api_returns_disabled_when_off) {
    // DebugPaintFlags 默认全 false（叠加层全关）。
    constexpr aurora::debug::DebugPaintFlags defaults{};
    AURORA_TEST_CHECK_FALSE(defaults.layout_guides);
    AURORA_TEST_CHECK_FALSE(defaults.relayout_boundaries);
    AURORA_TEST_CHECK_FALSE(defaults.layer_borders);
    AURORA_TEST_CHECK_FALSE(defaults.repaint_highlight);
    AURORA_TEST_CHECK_FALSE(defaults.overdraw);

    const bool debug_on = probe_debug_enabled();  // 探测自恢复：结束后 flags 回到原状
    const auto original = aurora::debug::flags();

    aurora::debug::DebugPaintFlags all_on;
    all_on.layout_guides = true;
    all_on.relayout_boundaries = true;
    all_on.layer_borders = true;
    all_on.repaint_highlight = true;
    all_on.overdraw = true;
    aurora::debug::set_flags(all_on);

    // 开启态写入生效；关闭态（disabled 值语义）恒为全 false、any_flag_enabled 恒 false。
    const auto readback = aurora::debug::flags();
    if (debug_on) {
        AURORA_TEST_CHECK_TRUE(readback.layout_guides);
        AURORA_TEST_CHECK_EQ(aurora::debug::any_flag_enabled(), true);
    } else {
        AURORA_TEST_CHECK_FALSE(readback.layout_guides);
        AURORA_TEST_CHECK_EQ(aurora::debug::any_flag_enabled(), false);
    }
    aurora::debug::set_flags(original);  // 恢复原状
}

}  // namespace aurora::test_cases::utest_debug

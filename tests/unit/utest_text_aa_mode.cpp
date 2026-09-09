/// 测试类型: unit
/// 目标单元: include/aurora/render/text_aa_mode.h
/// 测试说明: 覆盖 TextAAMode 的枚举取值稳定性与单字节布局（参与 display list 逐命令存储），
/// 并确认默认策略为 Supersample（灰度 AA，颜色安全、背景无关）

#include <cstdint>

#include "aurora/render/text_aa_mode.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_text_aa_mode {

AURORA_TEST_CASE(underlying_values_are_stable) {
    AURORA_TEST_CHECK_EQ(static_cast<int>(render::TextAAMode::Supersample), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(render::TextAAMode::ClearType), 1);
}

AURORA_TEST_CASE(default_is_supersample) {
    // 默认走灰度 AA：在渐变/多色背景上不会出现子像素羽化，是安全的默认值。
    const render::TextAAMode mode{};
    AURORA_TEST_CHECK_EQ(static_cast<int>(mode), static_cast<int>(render::TextAAMode::Supersample));
    AURORA_TEST_CHECK_EQ(sizeof(render::TextAAMode), sizeof(std::uint8_t));
}

}  // namespace aurora::test_cases::utest_text_aa_mode

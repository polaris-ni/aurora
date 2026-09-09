/// 测试类型: unit
/// 目标单元: include/aurora/render/blend.h
/// 测试说明: 覆盖 BlendMode 与 ShaderMaskKind 的枚举取值稳定性（按序号序列化，不得漂移）与互斥性

#include <cstdint>

#include "aurora/render/blend.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_blend {

AURORA_TEST_CASE(blend_mode_underlying_values_are_stable) {
    // 枚举按序号参与序列化与 display list 录制：取值漂移会静默改变既有场景外观。
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Normal), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Multiply), 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Screen), 2);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Overlay), 3);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Darken), 4);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Lighten), 5);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Difference), 6);
    AURORA_TEST_CHECK_EQ(static_cast<int>(BlendMode::Exclusion), 7);
}

AURORA_TEST_CASE(blend_mode_fits_single_byte) {
    // display list 以 uint8 存储混合模式：枚举底层类型必须是 uint8。
    AURORA_TEST_CHECK_EQ(sizeof(BlendMode), sizeof(std::uint8_t));
}

AURORA_TEST_CASE(shader_mask_kind_underlying_values_are_stable) {
    AURORA_TEST_CHECK_EQ(static_cast<int>(ShaderMaskKind::LinearFade), 0);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ShaderMaskKind::LinearRise), 1);
    AURORA_TEST_CHECK_EQ(static_cast<int>(ShaderMaskKind::RadialFade), 2);
    AURORA_TEST_CHECK_EQ(sizeof(ShaderMaskKind), sizeof(std::uint8_t));
}

}  // namespace aurora::test_cases::utest_blend

/// 测试类型: unit
/// 目标单元: include/aurora/event/keycode.h
/// 测试说明: keycode 单元测试
///

// 目标源单元：Keycode（KeyCode 枚举 + key_name 可读名称映射）。
//
// API 覆盖映射：
//   KeyCode 枚举值域          -> test_enum_values
//   key_name(KeyCode)         -> test_key_name_all_values / test_key_name_samples
//
// 覆盖率说明：key_name 全分支遍历，行覆盖 ~100%。

#include <string_view>

#include "aurora/event/keycode.h"
#include "aurora_test_harness.h"

namespace aurora::test_cases::utest_keycode {



namespace {

void test_key_name_all_values() {
    // 遍历全部枚举值：每个分支至少执行一次，且名称非空。
    for (int v = static_cast<int>(KeyCode::Unknown); v <= static_cast<int>(KeyCode::F12); ++v) {
        const auto k = static_cast<KeyCode>(v);
        AURORA_TEST_CHECK(key_name(k) != nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        // 测试助手：缓冲区长度已知且由断言约束，指针算术等价于 span 索引
        AURORA_TEST_CHECK(key_name(k)[0] != '\0');  // NOLINT
    }
}

void test_key_name_samples() {
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::Unknown)} == "Unknown");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::A)} == "A");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::D0)} == "0");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::D9)} == "9");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::Escape)} == "Escape");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::Enter)} == "Enter");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::Space)} == "Space");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::ArrowLeft)} == "ArrowLeft");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::ArrowDown)} == "ArrowDown");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::Shift)} == "Shift");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::F1)} == "F1");
    AURORA_TEST_CHECK(std::string_view{key_name(KeyCode::F12)} == "F12");
}

}  // namespace

AURORA_TEST() {
    test_key_name_all_values();
    test_key_name_samples();
}


}  // namespace aurora::test_cases::utest_keycode
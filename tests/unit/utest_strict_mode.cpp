/// 测试类型: unit
/// 目标单元: include/aurora/core/strict_mode.h
/// 测试说明: 覆盖 StrictMode 开关的默认值与读写、on_strict_failure 的处理器注入（消息透传 +
/// 可捕获）与默认处理器（std::terminate）死亡行为

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aurora/core/strict_mode.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_strict_mode {

AURORA_TEST_CASE(default_mode_is_off) {
    // 枚举取值锁定：Off=0 / On=1。
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(aurora::StrictMode::Off), 0);
    AURORA_TEST_CHECK_EQ(static_cast<std::uint8_t>(aurora::StrictMode::On), 1);
    // 线程局部开关默认关闭（本文件各用例进程内顺序执行，默认值即初始态）。
    AURORA_TEST_CHECK_EQ(aurora::strict_mode(), aurora::StrictMode::Off);
}

AURORA_TEST_CASE(set_and_read_roundtrip) {
    // 开关读写往返；用例结束前恢复 Off，避免状态泄漏到后续用例。
    aurora::set_strict_mode(aurora::StrictMode::On);
    AURORA_TEST_CHECK_EQ(aurora::strict_mode(), aurora::StrictMode::On);

    aurora::set_strict_mode(aurora::StrictMode::Off);
    AURORA_TEST_CHECK_EQ(aurora::strict_mode(), aurora::StrictMode::Off);
}

AURORA_TEST_CASE(injected_handler_receives_message_and_is_catchable) {
    // 测试缝隙契约：注入的处理器收到原始消息；处理器抛异常时 on_strict_failure 的失败
    // 可被调用方捕获（头文件文档明确建议测试注入抛 std::runtime_error 的处理器）。
    std::string captured;
    aurora::set_strict_failure_handler([&captured](std::string_view message) -> void {
        captured.assign(message);
        throw std::runtime_error{std::string{message}};
    });

    AURORA_TEST_CHECK_THROW(aurora::on_strict_failure("ctx-broken"), std::runtime_error);
    AURORA_TEST_CHECK_STREQ(captured, "ctx-broken");

    // 恢复默认处理器，避免影响本文件后续死亡测试与其他用例。
    aurora::set_strict_failure_handler(nullptr);
}

AURORA_TEST_CASE(default_handler_terminates_process) {
    // 默认（handler 为空）路径：硬失败必须终止进程（std::terminate），保证 Release/CI 阻断。
    // 防御性复位一次，确保父/子进程都处于默认处理器状态。
    aurora::set_strict_failure_handler(nullptr);
    AURORA_TEST_CHECK_DEATH(aurora::on_strict_failure("strict-default-terminate"), "");
}

}  // namespace aurora::test_cases::utest_strict_mode

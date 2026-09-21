/// 测试类型: unit
/// 目标单元: src/aurora/media/audio_alsa.cpp
/// 测试说明: ALSA 后端的**环境无关不变量**——在 libasound/设备有与无、真实后端与
/// disabled 桩四种组合下断言均成立：渲染格式契约恒 48000/2；未 start 即 stop 安全
/// 且幂等；start 成功/失败后 stop→restart 全生命周期无崩溃（线程 join 义务恒履行，
/// 即 WASAPI 审计缺陷 ③ 的对称回归面）；采集 start 失败不得置 failed()（启动期失败
/// ≠ 中段失败）。真实出声验收不在 CI——见 tools/verify/alsa_audio_live_probe.cpp。

#include <algorithm>
#include <chrono>
#include <thread>

#include "framework/aurora_test.h"
// 库内部头（src/aurora/media/）：测试目标私有 include 根含 src/，见 AuroraTests.cmake。
#include "aurora/media/audio_alsa.h"

namespace aurora::test_cases::utest_audio_alsa {

namespace {

/// 渲染回调：整块补零——在带真机的环境跑本测试也不出声。
auto zero_render(float *out, int frames) -> void { std::fill_n(out, static_cast<std::size_t>(frames) * 2U, 0.0F); }

auto noop_capture(const float * /*interleaved*/, int /*frames*/, int /*rate*/, int /*channels*/) -> void {}

}  // namespace

AURORA_TEST_CASE(device_format_contract_is_stable) {
    // 契约：图侧格式恒 48000/2（设备差异由 ALSA 插件层吸收，桩环境同值）。
    const aurora::AlsaDeviceBackend dev;
    AURORA_TEST_CHECK_EQ(dev.format().sample_rate, 48000);
    AURORA_TEST_CHECK_EQ(dev.format().channels, 2);
}

AURORA_TEST_CASE(device_stop_without_start_is_safe_and_idempotent) {
    aurora::AlsaDeviceBackend dev;
    dev.stop();  // 未启动即 stop：不得崩溃/挂起
    dev.stop();  // 幂等
    AURORA_TEST_CHECK_TRUE(true);
}

AURORA_TEST_CASE(device_restart_lifecycle_keeps_join_duty) {
    // start 成败取决于环境；无论何值，stop→再 start→stop 全程不得崩溃——
    // 覆盖「running 为 stop 握手位」不变量（线程先行退出时 stop 仍履行 join）。
    aurora::AlsaDeviceBackend dev;
    const bool first = dev.start(zero_render);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dev.stop();
    dev.stop();
    const bool second = dev.start(zero_render);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dev.stop();
    // 同环境下首次 start 成功则对象生命周期内契约一致（restart 失败也不得崩溃，
    // 仅观察记录，不断言——设备被独占属合法环境差异）。
    static_cast<void>(first);
    static_cast<void>(second);
}

AURORA_TEST_CASE(capture_failed_port_starts_false) {
    const aurora::AlsaCaptureBackend cap;
    AURORA_TEST_CHECK_FALSE(cap.failed());
}

AURORA_TEST_CASE(capture_start_failure_is_not_midstream_failure) {
    // 契约：failed() 只报告**中段**失败；启动期 start 返回 false 时不得置位
    // （AudioContext 据此把启动失败转显式错误、把中段失败交由观察口兜底）。
    aurora::AlsaCaptureBackend cap;
    const bool started = cap.start(noop_capture);
    if (!started) {
        AURORA_TEST_CHECK_FALSE(cap.failed());
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cap.stop();
        AURORA_TEST_CHECK_FALSE(cap.failed());  // 正常停止非失败
    }
    cap.stop();  // 二次 stop 幂等安全
}

AURORA_TEST_CASE(capture_restart_clears_stale_flag) {
    // 重开清旧败：stop 后再 start（无论成败），failed() 不携带上一轮状态。
    aurora::AlsaCaptureBackend cap;
    if (cap.start(noop_capture)) {
        cap.stop();
    }
    const bool again = cap.start(noop_capture);
    if (!again) {
        AURORA_TEST_CHECK_FALSE(cap.failed());
    }
    cap.stop();
}

}  // namespace aurora::test_cases::utest_audio_alsa

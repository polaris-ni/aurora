/// 测试类型: unit
/// 目标单元: src/aurora/media/audio_webaudio.cpp
/// 测试说明: Web Audio 后端的**环境无关不变量**。分两层：① 推式环 `detail::WebAudioRing`
/// 的纯折算数学（容量归一、满/空可辨、回绕两段写、对端推进记账、欠载计数由读端写入）——
/// 平台中立、恒编译，故在任何平台真值断言；② 后端外壳的生命周期契约（未 start 即 stop
/// 安全且幂等、start 结果不改变可析构性、`context_state()` 在无上下文时为 -1），在
/// 真实 WebAudio 构建与 disabled 桩构建两种口径下断言集合一致。浏览器出声、自动播放
/// 闸门与欠载真机面不在 CI：见 tools/verify/wasm_audio_live_probe.cpp。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "framework/aurora_test.h"
// 库内部头（src/aurora/media/）：测试目标私有 include 根含 src/，见 AuroraTests.cmake。
#include "aurora/media/audio_webaudio.h"

namespace aurora::test_cases::utest_audio_webaudio {

namespace {

constexpr int kCap = 1024;
constexpr int kCh = 2;

/// 生成交错帧序列：第 i 帧两声道为 (i, i+1000)，便于验证回绕后的段拼接顺序。
auto make_block(int frames) -> std::vector<float> {
    std::vector<float> out(static_cast<std::size_t>(frames) * kCh);
    for (int i = 0; i < frames; ++i) {
        out[static_cast<std::size_t>(i) * kCh] = static_cast<float>(i);
        out[static_cast<std::size_t>(i) * kCh + 1] = static_cast<float>(i + 1000);
    }
    return out;
}

/// 读端（浏览器侧 JS 的职责）在测试里的等价替身：按帧拷出并推进读指针。
auto read_out(aurora::detail::WebAudioRing &ring, int frames, std::vector<float> &dst) -> int {
    const int n = std::min(frames, ring.avail_frames());
    dst.assign(static_cast<std::size_t>(n) * ring.channels(), 0.0F);
    const float *data = ring.data();
    const int cap = ring.capacity_frames();
    const int stride = ring.channels();
    const int t = *ring.reader_position();
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < stride; ++c) {
            dst[static_cast<std::size_t>(i) * stride + c] = data[static_cast<std::size_t>((t + i) % cap) * stride + c];
        }
    }
    *ring.reader_position() = (t + n) % cap;
    return n;
}

auto zero_render(float *out, int frames) -> void {
    std::fill_n(out, static_cast<std::size_t>(frames) * kCh, 0.0F);
}

}  // namespace

AURORA_TEST_CASE(ring_normalizes_degenerate_geometry) {
    // 容量 ≤1 归一到 2、声道 ≤0 归一到 1：环数学全程按此不变量取模，防零/负容量取模。
    const aurora::detail::WebAudioRing a(0, kCh);
    AURORA_TEST_CHECK_EQ(a.capacity_frames(), 2);
    const aurora::detail::WebAudioRing b(kCap, 0);
    AURORA_TEST_CHECK_EQ(b.channels(), 1);
    AURORA_TEST_CHECK_EQ(b.capacity_frames(), kCap);
}

AURORA_TEST_CASE(ring_keeps_one_slot_free_to_tell_full_from_empty) {
    aurora::detail::WebAudioRing ring(kCap, kCh);
    AURORA_TEST_CHECK_EQ(ring.avail_frames(), 0);
    AURORA_TEST_CHECK_EQ(ring.free_frames(), kCap - 1);  // 恒留一格 ⇒ 满/空可辨
    const auto block = make_block(kCap);  // 故意多写一格
    AURORA_TEST_CHECK_EQ(ring.write(block.data(), kCap), kCap - 1);  // 裁剪到可用容量
    AURORA_TEST_CHECK_EQ(ring.free_frames(), 0);
    AURORA_TEST_CHECK_EQ(ring.write(block.data(), 1), 0);  // 满则拒写，绝不覆盖未读数据
    AURORA_TEST_CHECK_EQ(ring.avail_frames(), kCap - 1);
}

AURORA_TEST_CASE(ring_write_clips_and_rejects_invalid_input) {
    aurora::detail::WebAudioRing ring(kCap, kCh);
    AURORA_TEST_CHECK_EQ(ring.write(nullptr, 10), 0);
    const auto block = make_block(8);
    AURORA_TEST_CHECK_EQ(ring.write(block.data(), 0), 0);
    AURORA_TEST_CHECK_EQ(ring.write(block.data(), -3), 0);
    AURORA_TEST_CHECK_EQ(ring.avail_frames(), 0);
}

AURORA_TEST_CASE(ring_wraps_without_scrambling_interleaved_frames) {
    // 回绕处写必须断成两段且帧序不乱：这是「JS 按 head/tail 直接取 wasm 内存」的前提
    // ——任何错位都是耳朵听不出的爆音源。
    aurora::detail::WebAudioRing ring(kCap, kCh);
    const auto first = make_block(kCap - 4);
    AURORA_TEST_CHECK_EQ(ring.write(first.data(), kCap - 4), kCap - 4);
    std::vector<float> drained;
    AURORA_TEST_CHECK_EQ(read_out(ring, kCap - 4 - 1, drained), kCap - 5);  // 留 1 帧在环里
    ring.sync_reader();
    const auto second = make_block(8);
    AURORA_TEST_CHECK_EQ(ring.write(second.data(), 8), 8);  // 跨尾：4 帧前段 + 4 帧后段
    std::vector<float> got;
    AURORA_TEST_CHECK_EQ(read_out(ring, 9, got), 9);  // 残留 1 帧 + 新写 8 帧
    AURORA_TEST_CHECK_EQ(got[0], static_cast<float>(kCap - 5));  // 残留帧仍是上一块的末帧
    AURORA_TEST_CHECK_EQ(got[1], static_cast<float>(kCap - 5 + 1000));
    for (int i = 1; i <= 8; ++i) {
        AURORA_TEST_CHECK_EQ(got[static_cast<std::size_t>(i) * kCh], static_cast<float>(i - 1));
        AURORA_TEST_CHECK_EQ(got[static_cast<std::size_t>(i) * kCh + 1], static_cast<float>(i - 1 + 1000));
    }
    AURORA_TEST_CHECK_EQ(ring.avail_frames(), 0);  // 全数取走
}

AURORA_TEST_CASE(ring_accounts_reader_advance_across_wrap) {
    aurora::detail::WebAudioRing ring(kCap, kCh);
    const auto block = make_block(kCap - 1);
    AURORA_TEST_CHECK_EQ(ring.write(block.data(), kCap - 1), kCap - 1);
    std::vector<float> got;
    read_out(ring, kCap - 1, got);  // 读指针回绕到 0
    ring.sync_reader();
    AURORA_TEST_CHECK_EQ(ring.consumed_frames(), kCap - 1);
    AURORA_TEST_CHECK_EQ(ring.avail_frames(), 0);
    ring.sync_reader();  // 幂等：对端未推进则不重复计数
    AURORA_TEST_CHECK_EQ(ring.consumed_frames(), kCap - 1);
}

AURORA_TEST_CASE(ring_underrun_counter_is_written_by_reader) {
    // 欠载由读端（JS 回调）累加进 wasm 内存的独立 int32——写端只观测，二者地址分离。
    aurora::detail::WebAudioRing ring(kCap, kCh);
    AURORA_TEST_CHECK_EQ(ring.underruns(), 0);
    *ring.underrun_counter() += 3;
    AURORA_TEST_CHECK_EQ(ring.underruns(), 3);
    // 三个交接量必须各自可取址（JS 按地址读写，不能依赖成员连续布局）。
    AURORA_TEST_CHECK_TRUE(ring.writer_position() != ring.reader_position());
    AURORA_TEST_CHECK_TRUE(ring.reader_position() != ring.underrun_counter());
}

AURORA_TEST_CASE(device_stop_without_start_is_safe_and_idempotent) {
    aurora::WebAudioDeviceBackend dev;
    dev.stop();  // 未启动即 stop：不得崩溃
    dev.stop();  // 幂等
    AURORA_TEST_CHECK_TRUE(true);
}

AURORA_TEST_CASE(device_lifecycle_survives_whichever_way_start_goes) {
    // start 取决于宿主有无 AudioContext（真实浏览器 / 裸 Node / 非 WASM 桩三态）；
    // 无论何值，stop→再 start→stop 全程不得崩溃，且重复 start 不得叠挂定时器。
    aurora::WebAudioDeviceBackend dev;
    const bool first = dev.start(zero_render);
    dev.stop();
    dev.stop();
    const bool second = dev.start(zero_render);
    AURORA_TEST_CHECK_EQ(first, second);  // 同一宿主环境下结论一致
    dev.stop();
}

AURORA_TEST_CASE(device_format_and_gauge_stay_in_contract) {
    aurora::WebAudioDeviceBackend dev;
    const auto f = dev.format();
    AURORA_TEST_CHECK_GT(f.sample_rate, 0);  // 协商值或处理格式兜底，绝不为 0
    AURORA_TEST_CHECK_EQ(f.channels, 2);  // 图侧声道契约恒定
    AURORA_TEST_CHECK_GE(dev.underruns(), 0);
    AURORA_TEST_CHECK_GE(dev.consumed_frames(), 0);
    // 无上下文（未 start 成功、或非 WebAudio 构建）时观测口给 -1，不谎报状态。
    if (dev.context_state() == -1) {
        AURORA_TEST_CHECK_FALSE(dev.retry_resume());
    }
}

AURORA_TEST_CASE(capture_backend_declares_it_is_not_wired) {
    // 采集是**显式能力**：桩/未接线时 start 恒 false ⇒ create_microphone_source 显式
    // 报 audio-device-unavailable，不静默降级（契约见 media/audio.h 的 AudioContext 条）。
    aurora::WebAudioCaptureBackend cap;
    AURORA_TEST_CHECK_FALSE(cap.start([](const float *, int, int, int) {}));
    cap.stop();  // 未启动即 stop 安全
    AURORA_TEST_CHECK_FALSE(cap.start([](const float *, int, int, int) {}));
    cap.stop();
}

}  // namespace aurora::test_cases::utest_audio_webaudio

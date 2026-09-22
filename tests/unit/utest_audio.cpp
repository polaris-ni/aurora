/// 测试类型: unit
/// 目标单元: include/aurora/media/audio.h + src/aurora/media/audio.cpp
/// 测试说明: 覆盖 AudioContext 静默模式默认值与设备状态、连接拓扑校验（归属/重复/
/// 自环/双节点环/destination 终端/边不存在）、fake 设备渲染通路（Gain 恒定/主音量/
/// 双源汇聚/命令环排空）、推流节点（欠载静音停相位/溢出丢最旧/SRC 线性重采样/
/// 推入参数校验/中途变率拒绝）、缓冲源（一次性/循环/起播调度/非法缓冲/finished）、
/// AudioParam AutomationTimeline（set_value_at_time/linear/exponential/set_target
/// 插值曲线与校验/cancel）、suspend 时钟冻结、close 终态、设备启动失败静默降级；
/// 节点族（Panner 能量守恒/Analyser 峰值桶与时域静音 128/WAV 头结构与显式错误/麦克风采集全链路/Sinc 恒量保真）。
/// 注：原 utest_audio_nodes.cpp（节点类无独立源文件，实现同在 audio.cpp）已收编进本套件。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

#include "aurora/media/audio.h"
#include "framework/aurora_test.h"
#include "support/fake_audio.h"

namespace aurora::test_cases::utest_audio {

namespace {

using aurora::testing::FakeAudioDevice;

constexpr int AURORA_AUDIO_RATE = 48000;
constexpr int AURORA_AUDIO_FRAMES = 480;  // 10ms @ 48k

/// 活动设备 + 上下文组（设备堆分配，测试经裸指针手动驱动渲染）。
struct Rig {
    std::unique_ptr<FakeAudioDevice> owned;
    FakeAudioDevice *dev = nullptr;
    AudioContext ctx;

    explicit Rig(int rate = AURORA_AUDIO_RATE, std::size_t /*unused*/ = 0)
        : owned(std::make_unique<FakeAudioDevice>(rate, 2)), dev(owned.get()), ctx(std::move(owned)) {}
};

auto const_pcm(int frames, std::int16_t v) -> std::vector<std::int16_t> {
    // 花括号形式会先匹配 initializer_list 构造（size_t→int16_t 窄化为硬错误），fill 构造须具名走圆括号。
    const std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * 2, v);
    return pcm;
}

auto pcm_value(std::int16_t v) -> float { return static_cast<float>(v) / 32768.0F; }

/// 检查交错缓冲前 pairs 对样本全部 ≈ value。
auto check_all_near(const std::vector<float> &buf, float value, double tol, int pairs) -> bool {
    if (buf.size() < static_cast<std::size_t>(pairs) * 2U) {
        return false;
    }
    for (int i = 0; i < pairs; ++i) {
        if (std::fabs(static_cast<double>(buf[static_cast<std::size_t>(i) * 2U]) - static_cast<double>(value)) > tol ||
            std::fabs(static_cast<double>(buf[(static_cast<std::size_t>(i) * 2U) + 1U]) - static_cast<double>(value)) >
                tol) {
            return false;
        }
    }
    return true;
}

// ---- 来自 utest_audio_nodes：节点族辅助（静默上下文 + 3-arg 全缓冲近似） ----
using aurora::testing::FakeAudioCaptureDevice;

constexpr int AURORA_RATE = 48000;
constexpr int AURORA_FRAMES = 480;  // 10ms @ 48k
constexpr double AURORA_PI = std::numbers::pi;

/// 静默上下文（fail-start 桩保证跨构建配置确定性）+ 手动渲染辅助。
struct SilentRig {
    AudioContext ctx{std::make_unique<FakeAudioDevice>(AURORA_RATE, 2, /*fail_start=*/true)};

    auto render(int frames) -> std::vector<float> {
        std::vector<float> out(static_cast<std::size_t>(frames) * 2U, 0.0F);
        ctx.render_block(out.data(), frames);
        return out;
    }
};

auto const_buffer(float v, int frames) -> std::shared_ptr<AudioBuffer> {
    auto b = std::make_shared<AudioBuffer>();
    b->sample_rate = AURORA_RATE;
    b->channels = 1;
    b->samples.assign(static_cast<std::size_t>(frames), v);
    return b;
}

auto sine_buffer(float freq, float amp, int frames) -> std::shared_ptr<AudioBuffer> {
    auto b = std::make_shared<AudioBuffer>();
    b->sample_rate = AURORA_RATE;
    b->channels = 1;
    b->samples.resize(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        b->samples[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * AURORA_PI * static_cast<double>(freq) * static_cast<double>(i) /
                                              static_cast<double>(AURORA_RATE)));
    }
    return b;
}

/// 检查整段缓冲所有样本 ≈ value（3-arg 重载，与上面的 pairs 版并存）。
auto check_all_near(const std::vector<float> &buf, float value, double tol) -> bool {
    return std::ranges::all_of(
        buf, [value, tol](float i) { return std::fabs(static_cast<double>(i) - static_cast<double>(value)) <= tol; });
}

}  // namespace

// ---- 上下文默认值 / 静默模式 ----

AURORA_TEST_CASE(context_silent_defaults) {
    // 显式注入 fail-start 桩：跨构建配置（含 AURORA_ENABLE_AUDIO=ON）恒静默、可测。
    AudioContext ctx{std::make_unique<FakeAudioDevice>(AURORA_AUDIO_RATE, 2, /*fail_start=*/true)};
    AURORA_TEST_CHECK_TRUE(ctx.silent());
    AURORA_TEST_CHECK_EQ(ctx.device_state(), AudioDeviceState::Silent);
    AURORA_TEST_CHECK_EQ(ctx.sample_rate(), 48000);
    AURORA_TEST_CHECK_EQ(ctx.channel_count(), 2);
    AURORA_TEST_CHECK_NEAR(ctx.current_time(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(ctx.master_volume(), 1.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(ctx.closed());
    AURORA_TEST_CHECK_FALSE(ctx.suspended());
    AURORA_TEST_CHECK_TRUE(ctx.destination() != nullptr);
    AURORA_TEST_CHECK_EQ(ctx.connection_count(), 0U);

    // 静默模式手动驱动渲染：全零、时钟推进
    std::vector<float> out(static_cast<std::size_t>(AURORA_AUDIO_FRAMES) * 2U, 1.0F);
    ctx.render_block(out.data(), AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 0.0, AURORA_AUDIO_FRAMES));
    AURORA_TEST_CHECK_NEAR(ctx.current_time(), 0.01, 1e-9);
}

// ---- 拓扑校验 ----

AURORA_TEST_CASE(connect_topology_validation) {
    AudioContext ctx;
    auto dest = ctx.destination();
    auto gain = ctx.create_gain();
    auto gain2 = ctx.create_gain();

    auto r = ctx.connect(gain, dest);
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(ctx.connection_count(), 1U);

    // 重复连接
    auto dup = ctx.connect(gain, dest);
    AURORA_TEST_CHECK_FALSE(dup.ok());
    // 外来节点（未由本 context 创建）
    AudioContext other;
    auto foreign = other.create_gain();
    auto foreign_r = ctx.connect(foreign, dest);
    AURORA_TEST_CHECK_FALSE(foreign_r.ok());
    AURORA_TEST_CHECK_EQ(foreign_r.error().code_enum, ErrorCode::GeneralInvalidArgument);
    // 自环
    auto self_r = ctx.connect(gain, gain);
    AURORA_TEST_CHECK_FALSE(self_r.ok());
    AURORA_TEST_CHECK_EQ(self_r.error().code_enum, ErrorCode::AudioGraphCycle);
    AURORA_TEST_CHECK_EQ(self_r.error().code, std::string("audio-graph-cycle"));
    // 双节点环
    auto back = ctx.connect(gain2, gain);
    AURORA_TEST_REQUIRE(back.ok());
    auto fwd = ctx.connect(gain, gain2);
    AURORA_TEST_CHECK_FALSE(fwd.ok());
    AURORA_TEST_CHECK_EQ(fwd.error().code_enum, ErrorCode::AudioGraphCycle);
    // destination 作为源
    auto from_dest = ctx.connect(dest, gain2);
    AURORA_TEST_CHECK_FALSE(from_dest.ok());
    // 空节点
    auto null_r = ctx.connect(nullptr, dest);
    AURORA_TEST_CHECK_FALSE(null_r.ok());
}

AURORA_TEST_CASE(disconnect_validation) {
    AudioContext ctx;
    auto dest = ctx.destination();
    auto gain = ctx.create_gain();
    AURORA_TEST_REQUIRE(ctx.connect(gain, dest).ok());

    auto absent = ctx.disconnect(gain, gain);
    AURORA_TEST_CHECK_FALSE(absent.ok());
    AURORA_TEST_CHECK_EQ(absent.error().code_enum, ErrorCode::AudioEdgeNotFound);
    AURORA_TEST_CHECK_EQ(absent.error().code, std::string("audio-edge-not-found"));

    auto r = ctx.disconnect(gain, dest);
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(ctx.connection_count(), 0U);
    // 断开后重连可用
    AURORA_TEST_CHECK_TRUE(ctx.connect(gain, dest).ok());
}

// ---- 渲染通路（fake 设备手动驱动） ----

AURORA_TEST_CASE(gain_passthrough_master_volume) {
    Rig rig;
    auto &ctx = rig.ctx;
    AURORA_TEST_CHECK_FALSE(ctx.silent());
    AURORA_TEST_CHECK_EQ(ctx.device_state(), AudioDeviceState::Active);
    AURORA_TEST_CHECK_EQ(ctx.sample_rate(), AURORA_AUDIO_RATE);

    auto dest = ctx.destination();
    auto src = ctx.create_stream_source();
    auto gain = ctx.create_gain();
    AURORA_TEST_CHECK_TRUE(ctx.connect(src, gain).ok());
    AURORA_TEST_CHECK_TRUE(ctx.connect(gain, dest).ok());
    // 命令环模式：connect 在块首排空后生效，输出仍成立
    AURORA_TEST_REQUIRE(src->push(const_pcm(AURORA_AUDIO_FRAMES, 16384), AURORA_AUDIO_RATE, 2).ok());
    auto out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(16384), 1e-4, AURORA_AUDIO_FRAMES));
    AURORA_TEST_CHECK_NEAR(ctx.current_time(), 0.01, 1e-6);

    // 增益 0.5
    gain->gain().set_value(0.5F);
    AURORA_TEST_REQUIRE(src->push(const_pcm(AURORA_AUDIO_FRAMES, 16384), AURORA_AUDIO_RATE, 2).ok());
    out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(16384) * 0.5F, 1e-4, AURORA_AUDIO_FRAMES));

    // 主音量
    ctx.set_master_volume(0.5F);
    AURORA_TEST_REQUIRE(src->push(const_pcm(AURORA_AUDIO_FRAMES, 16384), AURORA_AUDIO_RATE, 2).ok());
    out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(16384) * 0.5F * 0.5F, 1e-4, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(destination_mixes_two_sources) {
    Rig rig;
    auto &ctx = rig.ctx;
    auto dest = ctx.destination();
    auto a = ctx.create_stream_source();
    auto b = ctx.create_stream_source();
    AURORA_TEST_CHECK_TRUE(ctx.connect(a, dest).ok());
    AURORA_TEST_CHECK_TRUE(ctx.connect(b, dest).ok());
    AURORA_TEST_REQUIRE(a->push(const_pcm(AURORA_AUDIO_FRAMES, 8192), AURORA_AUDIO_RATE, 2).ok());
    AURORA_TEST_REQUIRE(b->push(const_pcm(AURORA_AUDIO_FRAMES, 8192), AURORA_AUDIO_RATE, 2).ok());

    auto out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(8192) * 2.0F, 1e-4, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(silent_mode_graph_still_runs) {
    AudioContext ctx{std::make_unique<FakeAudioDevice>(AURORA_AUDIO_RATE, 2, /*fail_start=*/true)};  // 静默降级
    const auto dest = ctx.destination();
    const auto src = ctx.create_buffer_source();
    AURORA_TEST_CHECK_TRUE(ctx.connect(src, dest).ok());  // direct 生效

    const auto buf = std::make_shared<const AudioBuffer>(
        AudioBuffer{.sample_rate = AURORA_AUDIO_RATE, .channels = 1, .samples = std::vector<float>(64, 0.25F)});
    AURORA_TEST_REQUIRE(src->set_buffer(buf).ok());
    AURORA_TEST_REQUIRE(src->start().ok());

    std::vector<float> out(static_cast<std::size_t>(AURORA_AUDIO_FRAMES) * 2U, 9.0F);
    ctx.render_block(out.data(), AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_NEAR(out[0], 0.25F, 1e-6);  // 手动驱动下图照常运转
    AURORA_TEST_CHECK_NEAR(out[127], 0.25F, 1e-6);
    AURORA_TEST_CHECK_NEAR(out[256], 0.0F, 1e-6);  // 64 帧后静音
    AURORA_TEST_CHECK_TRUE(src->finished());
}

// ---- 推流节点 ----

AURORA_TEST_CASE(stream_underrun_outputs_silence) {
    Rig rig;
    const auto src = rig.ctx.create_stream_source();
    AURORA_TEST_CHECK_TRUE(rig.ctx.connect(src, rig.ctx.destination()).ok());

    const auto out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 0.0, AURORA_AUDIO_FRAMES));
    AURORA_TEST_CHECK_EQ(src->buffered_frames(), 0U);
}

AURORA_TEST_CASE(stream_overflow_drops_oldest) {
    Rig rig;
    auto src = rig.ctx.create_stream_source(256);  // 小环
    AURORA_TEST_CHECK_TRUE(rig.ctx.connect(src, rig.ctx.destination()).ok());

    // 推 512 帧：值 100（旧半）后 1000（新半）——环满丢最旧，保留最新 256 帧
    AURORA_TEST_CHECK_TRUE(src->push(const_pcm(256, 100), AURORA_AUDIO_RATE, 2).ok());
    AURORA_TEST_CHECK_TRUE(src->push(const_pcm(256, 1000), AURORA_AUDIO_RATE, 2).ok());
    AURORA_TEST_CHECK_EQ(src->buffered_frames(), 256U);
    AURORA_TEST_CHECK_GT(src->dropped_frames(), 0U);

    auto out = rig.dev->render(256);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(1000), 1e-4, 256));
    AURORA_TEST_CHECK_EQ(src->buffered_frames(), 0U);
}

AURORA_TEST_CASE(stream_src_resamples_constant) {
    Rig rig;
    auto &ctx = rig.ctx;
    auto src = ctx.create_stream_source(4096);
    AURORA_TEST_CHECK_TRUE(ctx.connect(src, ctx.destination()).ok());

    // 24k → 48k：输出帧数 ≈ 2×，幅度保持
    AURORA_TEST_REQUIRE(src->push(const_pcm(240, 8192), 24000, 2).ok());
    auto out = rig.dev->render(480);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(8192), 1e-4, 478));

    // 96k → 48k：降采样幅度保持（独立源）
    auto src2 = ctx.create_stream_source(4096);
    AURORA_TEST_CHECK_TRUE(ctx.connect(src2, ctx.destination()).ok());
    AURORA_TEST_REQUIRE(src2->push(const_pcm(480, 8192), 96000, 2).ok());
    out = rig.dev->render(240);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, pcm_value(8192), 1e-4, 238));
}

AURORA_TEST_CASE(stream_push_validation) {
    AudioContext ctx;
    auto src = ctx.create_stream_source();
    auto bad_rate = src->push(const_pcm(10, 100), 0, 2);
    AURORA_TEST_CHECK_FALSE(bad_rate.ok());
    auto bad_ch = src->push(const_pcm(10, 100), AURORA_AUDIO_RATE, 3);
    AURORA_TEST_CHECK_FALSE(bad_ch.ok());
    AURORA_TEST_CHECK_EQ(bad_ch.error().code_enum, ErrorCode::GeneralInvalidArgument);
    AURORA_TEST_CHECK_TRUE(src->push(const_pcm(10, 100), AURORA_AUDIO_RATE, 2).ok());
    // 中途变率拒绝
    auto changed = src->push(const_pcm(10, 100), 44100, 2);
    AURORA_TEST_CHECK_FALSE(changed.ok());
    // mono 推入上混
    std::vector<std::int16_t> mono(16, 4096);
    AURORA_TEST_CHECK_TRUE(src->push(std::span<const std::int16_t>(mono), AURORA_AUDIO_RATE, 1).ok());
}

// ---- 缓冲源 ----

AURORA_TEST_CASE(buffer_source_one_shot_and_loop) {
    Rig rig;
    auto &ctx = rig.ctx;
    auto src = ctx.create_buffer_source();
    AURORA_TEST_CHECK_TRUE(ctx.connect(src, ctx.destination()).ok());
    auto buf = std::make_shared<const AudioBuffer>(
        AudioBuffer{.sample_rate = AURORA_AUDIO_RATE, .channels = 2, .samples = std::vector<float>(400, 0.5F)});
    AURORA_TEST_REQUIRE(src->set_buffer(buf).ok());
    AURORA_TEST_CHECK_FALSE(src->finished());

    // 一次性：200 帧播完转静音
    AURORA_TEST_CHECK_TRUE(src->start().ok());
    auto out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_NEAR(out[0], 0.5F, 1e-6);
    AURORA_TEST_CHECK_NEAR(out[399], 0.5F, 1e-6);
    AURORA_TEST_CHECK_NEAR(out[400], 0.0F, 1e-6);
    AURORA_TEST_CHECK_TRUE(src->finished());

    // 循环：按 200 帧周期回绕，持续非零
    src->set_loop(true);
    AURORA_TEST_CHECK_TRUE(src->looping());
    AURORA_TEST_CHECK_TRUE(src->start().ok());
    out = rig.dev->render(500);
    AURORA_TEST_CHECK_NEAR(out[0], 0.5F, 1e-6);
    AURORA_TEST_CHECK_NEAR(out[static_cast<std::size_t>(200) * 2], 0.5F, 1e-6);  // 回绕后首帧
    AURORA_TEST_CHECK_NEAR(out[static_cast<std::size_t>(499) * 2], 0.5F, 1e-6);
    AURORA_TEST_CHECK_FALSE(src->finished());
    src->stop();
    AURORA_TEST_CHECK_TRUE(src->finished());
    out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 0.0, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(buffer_source_start_scheduling_and_errors) {
    // 本用例断言的是「块级起播算术」，必须恒静默：默认设备后端（WASAPI/ALSA）一旦启动成功，
    // 其回调线程会与手工 `render_block` 并发消费同一个 source，起播块位随负载漂移。
    AudioContext ctx{std::make_unique<FakeAudioDevice>(AURORA_AUDIO_RATE, 2, /*fail_start=*/true)};
    auto src = ctx.create_buffer_source();
    // 非法缓冲
    auto bad = src->set_buffer(
        std::make_shared<const AudioBuffer>(AudioBuffer{.sample_rate = 0, .channels = 2, .samples = {0.1F, 0.1F}}));
    AURORA_TEST_CHECK_FALSE(bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::AudioBufferInvalid);
    auto empty = src->set_buffer(std::make_shared<const AudioBuffer>(
        AudioBuffer{.sample_rate = AURORA_AUDIO_RATE, .channels = 2, .samples = {}}));
    AURORA_TEST_CHECK_FALSE(empty.ok());

    auto buf = std::make_shared<const AudioBuffer>(
        AudioBuffer{.sample_rate = AURORA_AUDIO_RATE, .channels = 1, .samples = std::vector<float>(48, 0.5F)});
    AURORA_TEST_REQUIRE(src->set_buffer(buf).ok());
    AURORA_TEST_CHECK_TRUE(ctx.connect(src, ctx.destination()).ok());
    // 负时刻
    AURORA_TEST_CHECK_FALSE(src->start(-1.0).ok());
    // 延迟起播（块级精度）：t=0 块静音，t≥0.01 后起播
    AURORA_TEST_CHECK_TRUE(src->start(0.01).ok());
    std::vector<float> out(static_cast<std::size_t>(AURORA_AUDIO_FRAMES) * 2U, 1.0F);
    ctx.render_block(out.data(), AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 0.0, AURORA_AUDIO_FRAMES));
    ctx.render_block(out.data(), AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_NEAR(out[0], 0.5F, 1e-6);
    AURORA_TEST_CHECK_NEAR(out[96], 0.0F, 1e-6);  // 48 帧后静音
    AURORA_TEST_CHECK_NEAR(ctx.current_time(), 0.02, 1e-6);
}

// ---- AudioParam / AutomationTimeline ----

AURORA_TEST_CASE(param_automation_curves) {
    AudioContext ctx;
    auto &p = ctx.create_gain()->gain();
    p.set_value(1.0F);
    AURORA_TEST_CHECK_FALSE(p.has_automation());

    // 线性 1 → 0（1 秒）
    AURORA_TEST_CHECK_TRUE(p.linear_ramp_to_value_at_time(0.0F, 1.0).ok());
    AURORA_TEST_CHECK_TRUE(p.has_automation());
    std::vector<float> curve(48);
    p.evaluate_block(0.0, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 1.0, 1e-6);
    p.evaluate_block(0.5, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.5, 1e-6);
    p.evaluate_block(1.0, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.0, 1e-6);
    p.evaluate_block(1.2, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.0, 1e-6);  // Ramp 完成后定格

    // Set 跳变后接指数 0.25 → 0.0625（1.5→2.5 秒）：锚点起于 1.5，中点 0.125（几何均值）
    AURORA_TEST_CHECK_TRUE(p.set_value_at_time(0.25F, 1.5).ok());
    AURORA_TEST_CHECK_TRUE(p.exponential_ramp_to_value_at_time(0.0625F, 2.5).ok());
    p.evaluate_block(1.5, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.25, 1e-6);
    p.evaluate_block(2.0, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.125, 1e-4);
    p.evaluate_block(2.5, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.0625, 1e-6);

    // set_target：v(t) = target + (v0 - target)·e^{-(t-t0)/tc}
    AURORA_TEST_CHECK_TRUE(p.set_target_at_time(0.03F, 2.6, 0.05).ok());
    p.evaluate_block(2.65, 48, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.03F + ((0.0625F - 0.03F) * static_cast<float>(std::exp(-1.0))), 1e-5);
    AURORA_TEST_CHECK_NEAR(p.value(), curve[47], 1e-6);  // 块末回写 value()
}

AURORA_TEST_CASE(param_validation_and_cancel) {
    AudioContext ctx;
    auto &p = ctx.create_gain()->gain();
    // 负时刻
    AURORA_TEST_CHECK_FALSE(p.set_value_at_time(1.0F, -0.1).ok());
    AURORA_TEST_CHECK_EQ(p.set_value_at_time(1.0F, -0.1).error().code_enum, ErrorCode::AudioParamInvalid);
    // 时刻回退
    AURORA_TEST_CHECK_TRUE(p.set_value_at_time(1.0F, 0.5).ok());
    AURORA_TEST_CHECK_FALSE(p.set_value_at_time(0.0F, 0.25).ok());
    // 指数过零（起点为当前值 0）
    auto &p2 = ctx.create_gain()->gain();
    p2.set_value(0.0F);
    AURORA_TEST_CHECK_FALSE(p2.exponential_ramp_to_value_at_time(1.0F, 1.0).ok());
    // 指数过零（终点 0）
    auto &p3 = ctx.create_gain()->gain();
    p3.set_value(1.0F);
    AURORA_TEST_CHECK_FALSE(p3.exponential_ramp_to_value_at_time(0.0F, 1.0).ok());
    // 指数异号
    auto &p4 = ctx.create_gain()->gain();
    p4.set_value(1.0F);
    AURORA_TEST_CHECK_FALSE(p4.exponential_ramp_to_value_at_time(-1.0F, 1.0).ok());
    // set_target tc 非正
    AURORA_TEST_CHECK_FALSE(p.set_target_at_time(0.0F, 0.6, 0.0).ok());
    AURORA_TEST_CHECK_FALSE(p.set_target_at_time(0.0F, 0.6, -1.0).ok());

    // cancel 后回到纯值
    p.cancel_scheduled_values();
    AURORA_TEST_CHECK_FALSE(p.has_automation());
    p.set_value(0.75F);
    std::vector<float> curve(16);
    p.evaluate_block(0.0, 16, AURORA_AUDIO_RATE, curve.data());
    AURORA_TEST_CHECK_NEAR(curve[0], 0.75F, 1e-6);
}

AURORA_TEST_CASE(gain_automation_render_integration) {
    Rig rig;
    auto &ctx = rig.ctx;
    auto dest = ctx.destination();
    auto src = ctx.create_stream_source(65536);  // 容纳 1 秒推流不触发溢出丢帧
    auto gain = ctx.create_gain();
    AURORA_TEST_CHECK_TRUE(ctx.connect(src, gain).ok());
    AURORA_TEST_CHECK_TRUE(ctx.connect(gain, dest).ok());

    // 线性 1 → 0（1 秒），常量源推 1 秒
    AURORA_TEST_CHECK_TRUE(gain->gain().linear_ramp_to_value_at_time(0.0F, 1.0).ok());
    AURORA_TEST_REQUIRE(src->push(const_pcm(AURORA_AUDIO_RATE, 16384), AURORA_AUDIO_RATE, 2).ok());
    auto out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_NEAR(out[0], pcm_value(16384), 1e-4);  // t=0 增益 1
    for (int i = 0; i < 50; ++i) {
        out = rig.dev->render(AURORA_AUDIO_FRAMES);
    }
    // t=0.50 块起点增益 ≈ 0.5
    AURORA_TEST_CHECK_NEAR(out[0], pcm_value(16384) * 0.5F, 2e-3);
}

// ---- 生命周期 ----

AURORA_TEST_CASE(suspend_freezes_clock) {
    Rig rig;
    AURORA_TEST_CHECK_TRUE(rig.ctx.suspend().ok());
    const auto out = rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 0.0, AURORA_AUDIO_FRAMES));
    AURORA_TEST_CHECK_NEAR(rig.ctx.current_time(), 0.0, 1e-9);  // 冻结
    AURORA_TEST_CHECK_TRUE(rig.ctx.resume().ok());
    rig.dev->render(AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_NEAR(rig.ctx.current_time(), 0.01, 1e-6);
}

AURORA_TEST_CASE(close_is_terminal) {
    auto device = std::make_unique<FakeAudioDevice>();
    auto *dev_ptr = device.get();
    AudioContext ctx(std::move(device));
    AURORA_TEST_CHECK_TRUE(ctx.close().ok());
    AURORA_TEST_CHECK_TRUE(ctx.closed());
    AURORA_TEST_CHECK_FALSE(dev_ptr->is_started());
    AURORA_TEST_CHECK_EQ(dev_ptr->stop_calls, 1);

    AURORA_TEST_CHECK_FALSE(ctx.close().ok());
    AURORA_TEST_CHECK_FALSE(ctx.suspend().ok());
    AURORA_TEST_CHECK_FALSE(ctx.resume().ok());
    auto r = ctx.connect(ctx.create_gain(), ctx.destination());
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, ErrorCode::AudioContextClosed);
    AURORA_TEST_CHECK_EQ(r.error().code, std::string("audio-context-closed"));
}

AURORA_TEST_CASE(device_start_failure_falls_back_silent) {
    AudioContext ctx(std::make_unique<FakeAudioDevice>(AURORA_AUDIO_RATE, 2, /*fail_start=*/true));
    AURORA_TEST_CHECK_TRUE(ctx.silent());
    AURORA_TEST_CHECK_EQ(ctx.device_state(), AudioDeviceState::Silent);
    // direct 图生效：静默模式全功能（见 silent_mode_graph_still_runs）
    const auto gain = ctx.create_gain();
    AURORA_TEST_CHECK_TRUE(ctx.connect(gain, ctx.destination()).ok());
    AURORA_TEST_CHECK_EQ(ctx.connection_count(), 1U);
}

// ---- PannerNode ----

AURORA_TEST_CASE(panner_center_energy) {
    SilentRig rig;
    auto src = rig.ctx.create_buffer_source();
    AURORA_TEST_REQUIRE(src->set_buffer(const_buffer(1.0F, AURORA_FRAMES)).ok());
    src->start();
    auto panner = rig.ctx.create_panner();
    panner->set_position(0.0F, 0.0F, -4.0F);  // 听者正前方 4 米（ref=1, rolloff=1）
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, panner).ok());
    AURORA_TEST_REQUIRE(rig.ctx.connect(panner, rig.ctx.destination()).ok());

    const auto out = rig.render(AURORA_FRAMES);
    // inverse 距离：gain = 1/(1 + (4-1)) = 0.25；正前方 pan=0 → L=R=cos(π/4)·0.25
    const float expected = 0.25F * std::cos(static_cast<float>(AURORA_PI) * 0.25F);
    AURORA_TEST_CHECK_NEAR(out[0], static_cast<double>(expected), 1e-4);
    AURORA_TEST_CHECK_NEAR(out[1], static_cast<double>(expected), 1e-4);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, expected, 1e-4F));
}

AURORA_TEST_CASE(panner_right_and_attenuation) {
    SilentRig rig;
    const auto src = rig.ctx.create_buffer_source();
    AURORA_TEST_REQUIRE(src->set_buffer(const_buffer(1.0F, AURORA_FRAMES)).ok());
    src->start();
    const auto panner = rig.ctx.create_panner();
    panner->set_position(1.0F, 0.0F, 0.0F);  // 正右方，距离 = ref → 无衰减
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, panner).ok());
    AURORA_TEST_REQUIRE(rig.ctx.connect(panner, rig.ctx.destination()).ok());

    const auto out = rig.render(AURORA_FRAMES);
    AURORA_TEST_CHECK_NEAR(out[0], 0.0, 1e-6);  // pan=1 → L = cos(π/2) ≈ 0
    AURORA_TEST_CHECK_NEAR(out[1], 1.0, 1e-6);  // R = sin(π/2) = 1（等于输入）
}

AURORA_TEST_CASE(panner_energy_conservation) {
    SilentRig rig;
    const auto src = rig.ctx.create_buffer_source();
    AURORA_TEST_REQUIRE(src->set_buffer(const_buffer(1.0F, AURORA_FRAMES)).ok());
    src->start();
    const auto panner = rig.ctx.create_panner();
    panner->set_position(1.0F, 0.0F, -1.0F);  // 右前方 45° 对角
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, panner).ok());
    AURORA_TEST_REQUIRE(rig.ctx.connect(panner, rig.ctx.destination()).ok());

    const auto out = rig.render(AURORA_FRAMES);
    const float dist = std::numbers::sqrt2_v<float>;
    const float dist_gain = 1.0F / (1.0F + (dist - 1.0F));  // ref=1, rolloff=1
    // equal-power：L² + R² = dist_gain²（能量守恒）
    const double energy = (static_cast<double>(out[0]) * static_cast<double>(out[0])) +
                          (static_cast<double>(out[1]) * static_cast<double>(out[1]));
    AURORA_TEST_CHECK_NEAR(energy, static_cast<double>(dist_gain) * static_cast<double>(dist_gain), 1e-6);
}

// ---- AnalyserNode ----

AURORA_TEST_CASE(analyser_sine_peak_bin) {
    SilentRig rig;
    auto src = rig.ctx.create_buffer_source();
    AURORA_TEST_REQUIRE(src->set_buffer(sine_buffer(4400.0F, 0.9F, 4800)).ok());
    src->start();
    auto analyser = rig.ctx.create_analyser();
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, analyser).ok());
    AURORA_TEST_REQUIRE(rig.ctx.connect(analyser, rig.ctx.destination()).ok());

    // 5 × 480 = 2400 帧喂足 2048 点窗
    for (int i = 0; i < 5; ++i) {
        (void)rig.render(AURORA_FRAMES);
    }
    std::vector<float> spec(static_cast<std::size_t>(analyser->frequency_bin_count()), 0.0F);
    analyser->get_float_frequency_data(spec);
    std::size_t peak = 0;
    for (std::size_t k = 1; k < spec.size(); ++k) {
        if (spec[k] > spec[peak]) {
            peak = k;
        }
    }
    // 4400/48000 × 2048 = 187.7 → 峰值桶 188（±1 容窗泄漏）
    AURORA_TEST_CHECK_TRUE(static_cast<int>(peak) >= 187 && static_cast<int>(peak) <= 189);
}

AURORA_TEST_CASE(analyser_time_domain_silence_128) {
    SilentRig rig;
    auto analyser = rig.ctx.create_analyser();
    AURORA_TEST_REQUIRE(rig.ctx.connect(analyser, rig.ctx.destination()).ok());
    (void)rig.render(AURORA_FRAMES);

    std::vector<std::uint8_t> time_data(static_cast<std::size_t>(analyser->fft_size()), 0U);
    analyser->get_byte_time_data(time_data);
    bool all_128 = true;
    for (std::uint8_t b : time_data) {
        if (b != 128U) {
            all_128 = false;
        }
    }
    AURORA_TEST_CHECK_TRUE(all_128);  // 静音 = 128
}

AURORA_TEST_CASE(analyser_passthrough) {
    SilentRig rig;
    auto src = rig.ctx.create_buffer_source();
    AURORA_TEST_REQUIRE(src->set_buffer(const_buffer(0.25F, AURORA_FRAMES)).ok());
    src->start();
    auto analyser = rig.ctx.create_analyser();
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, analyser).ok());
    AURORA_TEST_REQUIRE(rig.ctx.connect(analyser, rig.ctx.destination()).ok());

    const auto out = rig.render(AURORA_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.25F, 1e-6F));  // 直通不改样本
}

AURORA_TEST_CASE(analyser_fft_size_validation) {
    SilentRig rig;
    auto analyser = rig.ctx.create_analyser();
    auto bad = analyser->set_fft_size(100);  // 非 2 的幂
    AURORA_TEST_CHECK_FALSE(bad.ok());
    AURORA_TEST_CHECK_EQ(bad.error().code_enum, ErrorCode::AudioParamInvalid);
    AURORA_TEST_CHECK_FALSE(analyser->set_fft_size(16).ok());  // 低于下限
    AURORA_TEST_CHECK_FALSE(analyser->set_fft_size(65536).ok());  // 高于上限
    AURORA_TEST_REQUIRE(analyser->set_fft_size(1024).ok());
    AURORA_TEST_CHECK_EQ(analyser->fft_size(), 1024);
    AURORA_TEST_CHECK_EQ(analyser->frequency_bin_count(), 512);
}

// ---- 录制链 ----

AURORA_TEST_CASE(recording_wav_header_and_samples) {
    SilentRig rig;
    auto src = rig.ctx.create_buffer_source();
    AURORA_TEST_REQUIRE(src->set_buffer(const_buffer(0.25F, AURORA_FRAMES)).ok());
    src->start();
    auto rec = rig.ctx.create_recording_destination();
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, rec).ok());
    AURORA_TEST_REQUIRE(rig.ctx.connect(rec, rig.ctx.destination()).ok());

    AURORA_TEST_REQUIRE(rec->start().ok());
    AURORA_TEST_CHECK_TRUE(rec->is_recording());
    (void)rig.render(AURORA_FRAMES);
    rec->stop();
    AURORA_TEST_CHECK_FALSE(rec->is_recording());
    AURORA_TEST_CHECK_EQ(rec->recorded_frames(), static_cast<std::size_t>(AURORA_FRAMES));

    const auto wav = rec->to_wav_bytes();
    AURORA_TEST_CHECK_EQ(wav.size(), 44U + (static_cast<std::size_t>(AURORA_FRAMES) * 4U));
    AURORA_TEST_CHECK_TRUE(wav[0] == 'R' && wav[1] == 'I' && wav[2] == 'F' && wav[3] == 'F');
    AURORA_TEST_CHECK_TRUE(wav[8] == 'W' && wav[9] == 'A' && wav[10] == 'V' && wav[11] == 'E');
    // offset 24：采样率 48000（LE）
    const std::uint32_t rate = static_cast<std::uint32_t>(wav[24]) | (static_cast<std::uint32_t>(wav[25]) << 8U) |
                               (static_cast<std::uint32_t>(wav[26]) << 16U) |
                               (static_cast<std::uint32_t>(wav[27]) << 24U);
    AURORA_TEST_CHECK_EQ(rate, 48000U);
    AURORA_TEST_CHECK_TRUE(wav[36] == 'd' && wav[37] == 'a' && wav[38] == 't' && wav[39] == 'a');
    // offset 40：数据字节数 = 帧数 × 2ch × 2B
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(wav[40]) | (static_cast<std::uint32_t>(wav[41]) << 8U) |
                                     (static_cast<std::uint32_t>(wav[42]) << 16U) |
                                     (static_cast<std::uint32_t>(wav[43]) << 24U);
    AURORA_TEST_CHECK_EQ(data_bytes, static_cast<std::uint32_t>(AURORA_FRAMES) * 4U);
    // 首个 int16 样本 ≈ 0.25 · 32767
    const auto s0 = static_cast<std::int16_t>(static_cast<std::uint16_t>(wav[44]) |
                                              static_cast<std::uint16_t>(static_cast<unsigned>(wav[45]) << 8U));
    AURORA_TEST_CHECK_NEAR(static_cast<double>(s0), 0.25 * 32767.0, 1.0);
}

AURORA_TEST_CASE(recording_save_wav_explicit_error) {
    SilentRig rig;
    auto rec = rig.ctx.create_recording_destination();
    // 目录不存在 → 落盘失败显式报错（audio-recording-failed），不静默
    auto r = rec->save_wav("./no_such_dir_a6/rec.wav");
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, ErrorCode::AudioRecordingFailed);
}

AURORA_TEST_CASE(microphone_capture_explicit_error) {
    auto cap = std::make_unique<FakeAudioCaptureDevice>(/*fail_start=*/true);
    auto *cap_ptr = cap.get();
    AudioContext ctx{std::make_unique<FakeAudioDevice>(AURORA_RATE, 2, /*fail_start=*/true), std::move(cap)};
    // 录制是显式能力：采集启动失败 → audio-device-unavailable，不静默降级
    auto r = ctx.create_microphone_source();
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_EQ(r.error().code_enum, ErrorCode::AudioDeviceUnavailable);
    AURORA_TEST_CHECK_EQ(cap_ptr->start_calls, 1);
    AURORA_TEST_CHECK_EQ(cap_ptr->stop_calls, 1);  // 失败节点析构即停采集
}

AURORA_TEST_CASE(microphone_capture_to_recording) {
    auto cap = std::make_unique<FakeAudioCaptureDevice>();
    auto *cap_ptr = cap.get();
    AudioContext ctx{std::make_unique<FakeAudioDevice>(AURORA_RATE, 2, /*fail_start=*/true), std::move(cap)};
    auto mic_r = ctx.create_microphone_source();
    AURORA_TEST_REQUIRE(mic_r.ok());
    const auto &mic = mic_r.value();
    AURORA_TEST_CHECK_TRUE(cap_ptr->is_started());

    auto rec = ctx.create_recording_destination();
    AURORA_TEST_REQUIRE(ctx.connect(mic, rec).ok());
    AURORA_TEST_REQUIRE(ctx.connect(rec, ctx.destination()).ok());
    AURORA_TEST_REQUIRE(rec->start().ok());

    // 采集线程侧（fake 手动触发）：480 帧 0.5 stereo @ 48k → 经 int16 往返推入环
    std::vector<float> pcm(static_cast<std::size_t>(AURORA_FRAMES) * 2U, 0.5F);
    cap_ptr->emit(pcm, AURORA_FRAMES, AURORA_RATE, 2);

    std::vector<float> out(static_cast<std::size_t>(AURORA_FRAMES) * 2U, 0.0F);
    ctx.render_block(out.data(), AURORA_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.5F, 1e-6F));
    AURORA_TEST_CHECK_EQ(rec->recorded_frames(), static_cast<std::size_t>(AURORA_FRAMES));
    const auto snap = rec->recording();
    AURORA_TEST_CHECK_NEAR(static_cast<double>(snap[0]), 0.5, 1e-3);
    AURORA_TEST_CHECK_NEAR(static_cast<double>(snap.back()), 0.5, 1e-3);

    rec->stop();
    AURORA_TEST_CHECK_FALSE(rec->is_recording());
}

// ---- Sinc SRC ----

AURORA_TEST_CASE(sinc_quality_constant_fidelity) {
    SilentRig rig;
    auto src = rig.ctx.create_stream_source(4096);
    src->set_src_quality(SrcQuality::Sinc);
    AURORA_TEST_CHECK_EQ(src->src_quality(), SrcQuality::Sinc);
    AURORA_TEST_REQUIRE(rig.ctx.connect(src, rig.ctx.destination()).ok());

    // 恒量 0.5 × 544 帧（480 渲染 + 64 前瞻裕量）：行归一核保证 DC 保真
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(544U) * 2U, 16384);
    AURORA_TEST_REQUIRE(src->push(pcm, AURORA_RATE, 2).ok());
    const auto out = rig.render(AURORA_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.5F, 5e-3F));
}

AURORA_TEST_CASE(sinc_default_linear) {
    SilentRig rig;
    auto src = rig.ctx.create_stream_source();
    AURORA_TEST_CHECK_EQ(src->src_quality(), SrcQuality::Linear);  // 默认档位
}

}  // namespace aurora::test_cases::utest_audio

// tools/verify/wasapi_audio_live_probe.cpp — WASAPI 音频后端真机探针（非 CTest）。
//
// 覆盖：AURORA_ENABLE_AUDIO + AURORA_ENABLE_AUDIO_WASAPI 构建下，音频图经真实
// WASAPI 设备线程的接线——无头 CI 无法证明的部分（真实格式协商 / 引擎事件泵 /
// 图时钟推进 / 出声路径）。
//
// 自动段（无需人工）：
//   1. 默认上下文激活：AudioContext 走内置 WASAPI 后端，device_state()==Active
//      （静默 = 环境无输出设备，退出码 2）。
//   2. 格式契约：sample_rate()==48000、channel_count()==2（AUTOCONVERTPCM 吸收设备差异）。
//   3. 图时钟推进：设备线程持续 render_block，300ms 内 current_time() 推进 ≥ 0.2s。
//   4. 缓冲源出声通路：440Hz 正弦（2s）经 Gain(0.25) 汇入 destination，播毕 finished()
//      且无崩溃。
//   5. 推流通路：StreamSource 推 0.2s 正弦并排空——buffered 归零、零溢出丢弃。
//   6. suspend/resume：挂起时钟冻结、恢复后继续推进（真实设备线程上）。
//   7. 采集后端观察口直连（库内部契约）：`failed()` 干净启停不误报、停后再启成功
//      （守卫审计回归：通知回调/事件句柄生命周期）；无采集设备时该项 SKIP。
// 人工段（--interactive，出声目视/耳听）：
//   a. 扫频：200Hz→2kHz 线性扫频 3s——确认可闻且无爆音。
//   b. 双源混音：440Hz + 660Hz 两路 StreamSource 同时推流——确认可闻混合无削波异响。
//   c. 设备热切换：连续音播放中按提示切换系统默认输出设备——确认不崩溃、时钟持续推进
//      （重路由生效）。退出码：0 全过；1 自动段断言失败；2 环境不可用（无输出设备）。
//
// 运行：build 目录下 ./aurora_verify_wasapi_audio [--interactive]

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/media/audio_wasapi.h"  // 库内部头（探针经 src include 直连，见 AuroraVerify.cmake）

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

int failures = 0;

auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        failures++;
    }
}

constexpr double kPi = 3.14159265358979323846;

/// 生成正弦缓冲（float32 交错 stereo）。
auto sine_buffer(int sample_rate, double seconds, double freq, float amp) -> aurora::AudioBuffer {
    const int frames = static_cast<int>(static_cast<double>(sample_rate) * seconds);
    aurora::AudioBuffer buf;
    buf.sample_rate = sample_rate;
    buf.channels = 2;
    buf.samples.resize(static_cast<std::size_t>(frames) * 2U);
    for (int i = 0; i < frames; ++i) {
        const float v = amp * static_cast<float>(std::sin(2.0 * kPi * freq * static_cast<double>(i) / sample_rate));
        buf.samples[static_cast<std::size_t>(i) * 2U] = v;
        buf.samples[static_cast<std::size_t>(i) * 2U + 1U] = v;
    }
    return buf;
}

/// 生成 int16 正弦包（StreamSource 推入口径）。
auto sine_pcm(int sample_rate, int frames, double freq, std::int16_t amp) -> std::vector<std::int16_t> {
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * 2U);
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<std::int16_t>(static_cast<double>(amp) *
                                                 std::sin(2.0 * kPi * freq * static_cast<double>(i) / sample_rate));
        pcm[static_cast<std::size_t>(i) * 2U] = v;
        pcm[static_cast<std::size_t>(i) * 2U + 1U] = v;
    }
    return pcm;
}

auto sleep_ms(int ms) -> void { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

auto main(int argc, char **argv) -> int {
    const bool interactive = argc > 1 && std::string(argv[1]) == "--interactive";

    emit("== Aurora WASAPI audio live probe ==");
    emit("auto segment: activation / format / clock / buffer source / stream / suspend-resume / capture port");
    if (interactive) {
        emit("interactive segment: sweep / dual-source mix / device hot-switch (audible)");
    }

    // 1. 默认上下文（内置 WASAPI 后端）
    const auto ctx = std::make_shared<aurora::AudioContext>();
    if (ctx->silent()) {
        emit("[SKIP] device_state()==Silent: no usable output device on this machine");
        emit("result: ENV-UNAVAILABLE (exit 2)");
        return 2;
    }
    check(ctx->device_state() == aurora::AudioDeviceState::Active, "default context activates WASAPI backend");

    // 2. 格式契约（AUTOCONVERTPCM 吸收设备差异，图侧恒 48000/2）
    check(ctx->sample_rate() == 48000, "format contract: sample_rate == 48000");
    check(ctx->channel_count() == 2, "format contract: channels == 2");

    // 3. 图时钟由设备线程推进
    const double t0 = ctx->current_time();
    sleep_ms(300);
    const double t1 = ctx->current_time();
    check(t1 - t0 >= 0.2,
          "render clock advances via device thread (" + std::to_string(t1 - t0).substr(0, 5) + "s in 300ms)");

    // 4. 缓冲源出声通路（440Hz × 2s → Gain 0.25 → destination）
    {
        auto src = ctx->create_buffer_source();
        auto gain = ctx->create_gain();
        const bool c1 = ctx->connect(src, gain).ok();
        const bool c2 = ctx->connect(gain, ctx->destination()).ok();
        gain->gain().set_value(0.25F);
        const bool b =
            src->set_buffer(std::make_shared<const aurora::AudioBuffer>(sine_buffer(48000, 2.0, 440.0, 0.8F))).ok();
        const bool s = src->start().ok();
        check(c1 && c2 && b && s, "buffer source path: connect + set_buffer + start");
        sleep_ms(2500);
        check(src->finished(), "buffer source finished after 2s playback");
    }

    // 5. 推流通路（0.2s 正弦排空）
    {
        auto src = ctx->create_stream_source(16384);
        check(ctx->connect(src, ctx->destination()).ok(), "stream source connect");
        check(src->push(sine_pcm(48000, 9600, 660.0, 12000), 48000, 2).ok(), "stream push 0.2s PCM");
        sleep_ms(600);
        check(src->buffered_frames() == 0, "stream drained to 0 buffered frames");
        check(src->dropped_frames() == 0, "no overflow drops on steady push");
    }

    // 6. suspend/resume（真实设备线程上时钟冻结/恢复）
    {
        check(ctx->suspend().ok(), "suspend accepted on live device");
        const double s0 = ctx->current_time();
        sleep_ms(250);
        const double s1 = ctx->current_time();
        check(s1 - s0 < 0.05, "clock frozen while suspended");
        check(ctx->resume().ok(), "resume accepted");
        sleep_ms(250);
        check(ctx->current_time() > s1, "clock resumes after resume()");
    }

    // 7. 采集后端观察口直连（failed() + 二次 start 生命周期回归）
    {
        aurora::WasapiCaptureBackend cap;
        std::atomic<int> packets{0};
        auto sink_fn = [&packets](const float * /*pcm*/, int frames, int /*rate*/, int /*channels*/) {
            packets.fetch_add(frames, std::memory_order_relaxed);
        };
        if (!cap.start(sink_fn)) {
            emit("[SKIP] no usable capture device: observation-port items skipped");
        } else {
            sleep_ms(250);
            cap.stop();
            check(!cap.failed(), "capture failed()==false after clean start/stop");
            const bool again = cap.start(sink_fn);
            check(again, "capture restart after stop succeeds (lifecycle regression)");
            if (again) {
                cap.stop();
            }
        }
    }

    if (interactive) {
        auto wait_key = [](const std::string &prompt) {
            emit(prompt + " [press ENTER]");
            std::string line;
            static_cast<void>(std::getline(std::cin, line));
        };

        // a. 扫频 200Hz→2kHz（3s）
        {
            const auto src = ctx->create_buffer_source();
            const auto gain = ctx->create_gain();
            static_cast<void>(ctx->connect(src, gain));
            static_cast<void>(ctx->connect(gain, ctx->destination()));
            gain->gain().set_value(0.2F);
            constexpr int frames = 48000 * 3;
            aurora::AudioBuffer buf;
            buf.sample_rate = 48000;
            buf.channels = 2;
            buf.samples.resize(static_cast<std::size_t>(frames) * 2U);
            double phase = 0.0;
            for (int i = 0; i < frames; ++i) {
                const double f = 200.0 + 1800.0 * static_cast<double>(i) / frames;
                phase += 2.0 * kPi * f / 48000.0;
                const float v = 0.5F * static_cast<float>(std::sin(phase));
                buf.samples[static_cast<std::size_t>(i) * 2U] = v;
                buf.samples[static_cast<std::size_t>(i) * 2U + 1U] = v;
            }
            static_cast<void>(src->set_buffer(std::make_shared<const aurora::AudioBuffer>(std::move(buf))));
            static_cast<void>(src->start());
            wait_key("[a] sweep 200Hz->2kHz playing: audible, no clicks/pops?");
        }

        // b. 双源混音（440 + 660）
        {
            const auto a = ctx->create_stream_source(48000);
            const auto b = ctx->create_stream_source(48000);
            static_cast<void>(ctx->connect(a, ctx->destination()));
            static_cast<void>(ctx->connect(b, ctx->destination()));
            for (int chunk = 0; chunk < 8; ++chunk) {
                static_cast<void>(a->push(sine_pcm(48000, 4800, 440.0, 10000), 48000, 2));
                static_cast<void>(b->push(sine_pcm(48000, 4800, 660.0, 10000), 48000, 2));
                sleep_ms(80);
            }
            wait_key("[b] dual-source mix (440+660) playing: audible blend, no clipping noise?");
        }

        // c. 设备热切换（连续音中切换默认设备）
        {
            const auto src = ctx->create_buffer_source();
            const auto gain = ctx->create_gain();
            static_cast<void>(ctx->connect(src, gain));
            static_cast<void>(ctx->connect(gain, ctx->destination()));
            gain->gain().set_value(0.15F);
            const auto buf = std::make_shared<const aurora::AudioBuffer>(sine_buffer(48000, 60.0, 523.25, 0.6F));
            static_cast<void>(src->set_buffer(buf));
            src->set_loop(true);
            static_cast<void>(src->start());
            emit("[c] continuous tone playing. Switch the system DEFAULT output device now");
            wait_key("    (Settings > Sound > Output). Then confirm reroute kept it alive");
            const double h0 = ctx->current_time();
            sleep_ms(400);
            const double h1 = ctx->current_time();
            check(ctx->device_state() == aurora::AudioDeviceState::Active, "still Active after hot-switch");
            check(h1 - h0 >= 0.3, "clock kept advancing across reroute");
            src->stop();
        }
    }

    static_cast<void>(ctx->close());
    emit(failures == 0 ? "result: ALL PASS (exit 0)" : "result: " + std::to_string(failures) + " FAILURE(S) (exit 1)");
    return failures == 0 ? 0 : 1;
}

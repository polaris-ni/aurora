// tools/verify/coreaudio_audio_live_probe.cpp — CoreAudio 音频后端真机探针（非 CTest）。
//
// 覆盖：AURORA_ENABLE_AUDIO + AURORA_ENABLE_AUDIO_COREAUDIO 构建下，音频图经真实
// CoreAudio 设备腿的接线——无头 CI 无法证明的部分（AudioQueue 建队列 / 格式协商 /
// 队列线程泵 / 图时钟推进 / 出声路径 / 重路由收尾 / 采集观察口）。段结构与
// wasapi / alsa 两探针逐项对齐，便于三端读数横向比较。
//
// 自动段（无需人工）：
//   0. 无设备口径（**与本机环境无关、恒执行**，故无设备机器上也有读数）：
//      a) 渲染腿 start() 在无输出设备时如实返回 false（判据取 AudioQueueStart 的
//         -66680 kAudioQueueErr_InvalidDevice，而非队列创建），随后 stop() 幂等无崩溃；
//      b) 采集腿在宿主未声明 NSMicrophoneUsageDescription 时拒启且 failed()==false
//         ——本探针是无 bundle 的裸可执行文件，故恒走此路，据此保证探针永不被 TCC 终止。
//   1. 默认上下文激活：AudioContext 走内置 CoreAudio 后端，device_state()==Active
//      （静默 = 本机无输出设备，此时段 0 的读数已给出，探针以退出码 2 收尾）。
//   2. 格式契约：sample_rate()==48000、channel_count()==2（队列内建转换吸收设备差异）。
//   3. 图时钟推进：队列线程持续 render_block，300ms 内 current_time() 推进 ≥ 0.2s。
//   4. 缓冲源出声通路：440Hz 正弦（2s）经 Gain(0.25) 汇入 destination，播毕
//      finished() 且无崩溃。
//   5. 推流通路：StreamSource 推 0.2s 正弦并排空——buffered 归零、零溢出丢弃。
//   6. suspend/resume：挂起时钟冻结、恢复后继续推进（真实队列线程上）。
//   7. 重路由收尾：CoreAudioDeviceBackend 直用——start→stop→start→stop 全生命周期
//      无崩溃（控制线程 join 义务 + 队列同步销毁的回归面），且同一环境下两向 start
//      的成败判据一致（不抖动）。
//      采集段（段 0b）在有设备的真机上须换到声明了 NSMicrophoneUsageDescription 的
//      .app 壳内跑，读数与本探针的拒启读数分列。
// 人工段（--interactive，出声耳听）：
//   a. 扫频：200Hz→2kHz 线性扫频 3s——确认可闻且无爆音。
//   b. 双源混音：440Hz + 660Hz 两路 StreamSource 同时推流——确认可闻混合无削波异响。
//   c. 设备热切换：播放中在系统设置里切默认输出设备（或拔出当前设备）——确认
//      重连后继续出声、无崩溃；期间静音属设计内行为（图时钟冻结）。
// 已知口径差异（相对 WASAPI 探针）：延迟水位高于 WASAPI event-driven（约 40–60ms
// vs 约 20ms），本期取 AudioQueue 的换法见 specification/03-layout-render.md §9。
// 退出码：0 全过；1 自动段断言失败；2 环境不可用（无输出设备）。
//
// 运行：build 目录下 ./aurora_verify_coreaudio_audio [--interactive]
// 注：本仓开发用的 VMware Fusion 虚拟机 CoreAudio device_count==0（无虚拟声卡），
// 本机自动段合法落点是退出码 2——与 alsa 探针在无 ALSA 栈的 WSL 上同形态。

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/media/audio_coreaudio.h"
#include "verify_args.h"

namespace {

auto emit(const std::string &text) -> void { AURORA_LOG_RAW("verify", text, "\n"); }

/// 失败计数：函数局部静态 + 访问器收敛（探针退出码的依据），避免命名空间级可变全局量。
auto failures() -> int & {
    static int count = 0;
    return count;
}

auto check(bool ok, const std::string &label) -> void {
    emit(std::string("[") + (ok ? "PASS" : "FAIL") + "] " + label);
    if (!ok) {
        failures()++;
    }
}

constexpr double AURORA_PI = std::numbers::pi;

/// 生成正弦缓冲（float32 交错 stereo）。
auto sine_buffer(int sample_rate, double seconds, double freq, float amp) -> aurora::AudioBuffer {
    const int frames = static_cast<int>(static_cast<double>(sample_rate) * seconds);
    aurora::AudioBuffer buf;
    buf.sample_rate = sample_rate;
    buf.channels = 2;
    buf.samples.resize(static_cast<std::size_t>(frames) * 2U);
    for (int i = 0; i < frames; ++i) {
        const float v =
            amp * static_cast<float>(std::sin(2.0 * AURORA_PI * freq * static_cast<double>(i) / sample_rate));
        buf.samples[static_cast<std::size_t>(i) * 2U] = v;
        buf.samples[(static_cast<std::size_t>(i) * 2U) + 1U] = v;
    }
    return buf;
}

/// 生成 int16 正弦包（StreamSource 推入口径）。
auto sine_pcm(int sample_rate, int frames, double freq, std::int16_t amp) -> std::vector<std::int16_t> {
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) * 2U);
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<std::int16_t>(
            static_cast<double>(amp) * std::sin(2.0 * AURORA_PI * freq * static_cast<double>(i) / sample_rate));
        pcm[static_cast<std::size_t>(i) * 2U] = v;
        pcm[(static_cast<std::size_t>(i) * 2U) + 1U] = v;
    }
    return pcm;
}

auto sleep_ms(int ms) -> void { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

// 入口不吞异常：未捕获异常 → 非零退出码/terminate 呈现，捕获反而把失败压成 0。
// 口径与 tools/verify/ 其余探针、examples/ 各 demo 入口同。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main(int argc, char **argv) -> int {
    const auto cli = aurora_verify::parse_interactive("CoreAudio audio live probe", argc, argv);
    if (!cli.arguments) {
        return cli.exit_code;
    }
    const bool interactive = cli.arguments->flag("interactive");

    emit("== Aurora CoreAudio audio live probe ==");
    emit("auto segment: no-device verdict / activation / format / clock / buffer source / stream / suspend-resume / reroute");
    if (interactive) {
        emit("interactive segment: sweep / dual-source mix / default-device hot switch (audible)");
    }

    // 0. 无设备口径（恒执行：本机有声卡与否都先给出这两条读数，无设备机器不至于一走了之）
    {
        // 0a 渲染腿：无输出设备 → start false（判据取 AudioQueueStart 的 -66680），
        //     stop 幂等、重启不抖；有设备 → 只记下「两向判据一致」这一硬不变量。
        aurora::CoreAudioDeviceBackend dev;
        const auto pump = [](float *out, int frames) {
            for (int i = 0; i < frames * 2; ++i) {
                out[static_cast<std::size_t>(i)] = 0.0F;
            }
        };
        const bool first = dev.start(pump);
        dev.stop();
        dev.stop();  // 幂等
        const bool second = dev.start(pump);
        dev.stop();
        check(first == second, "device backend start verdict is stable across restart (no flapping)");
        emit(first ? "[INFO] output device present: live segments below run in full"
                   : "[INFO] no output device: AudioQueueStart refused -> AudioContext will be Silent");

        // 0b 采集腿：裸可执行文件无 bundle Info.plist → 按设计拒启，且拒启不得置
        //    failed()（启动期失败 ≠ 中段失败）；这一腿同时保证探针永不被 TCC 终止。
        aurora::CoreAudioCaptureBackend cap;
        const auto sink_fn = [](const float *, int, int, int) {};
        if (!cap.start(sink_fn)) {
            check(!cap.failed(), "capture refusal keeps failed()==false (startup != mid-stream; TCC untouched)");
            emit("[SKIP] bare executable declares no NSMicrophoneUsageDescription: capture legs skipped");
        } else {
            sleep_ms(250);
            cap.stop();
            check(!cap.failed(), "capture stop is clean shutdown (failed()==false)");
            const bool again = cap.start(sink_fn);
            check(again, "capture restart after stop succeeds (lifecycle regression)");
            if (again) {
                cap.stop();
            }
        }
    }

    // 1. 默认上下文（内置 CoreAudio 后端）——**无设备机器在此给出硬判据而非一走了之**：
    //    静默降级是本后端对「无输出设备」的契约形态，故 silent() 与 Silent 态必须可断言，
    //    只有其后的出声段才需要真设备。
    const auto ctx = std::make_shared<aurora::AudioContext>();
    if (ctx->silent()) {
        check(ctx->device_state() == aurora::AudioDeviceState::Silent, "silent degradation: device_state()==Silent (no crash, no fake-active)");
        emit("[INFO] no CoreAudio output device on this machine: audible segments below skipped");
        emit("result: ENV-UNAVAILABLE (exit 2)");
        return 2;
    }
    check(ctx->device_state() == aurora::AudioDeviceState::Active, "default context activates CoreAudio backend");

    // 2. 格式契约（队列内建转换吸收设备差异，图侧恒 48000/2）
    check(ctx->sample_rate() == 48000, "format contract: sample_rate == 48000");
    check(ctx->channel_count() == 2, "format contract: channels == 2");

    // 3. 图时钟由队列线程推进
    const double t0 = ctx->current_time();
    sleep_ms(300);
    const double t1 = ctx->current_time();
    check(t1 - t0 >= 0.2, "render clock advances via queue thread (" + std::to_string(t1 - t0).substr(0, 5) + "s in 300ms)");

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

    // 6. suspend/resume（真实队列线程上时钟冻结/恢复）
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

    // 7. 重路由收尾（CoreAudioDeviceBackend 直用：控制线程 join + 队列同步销毁）
    {
        aurora::CoreAudioDeviceBackend dev;
        const auto pump = [](float *out, int frames) {
            for (int i = 0; i < frames * 2; ++i) {
                out[static_cast<std::size_t>(i)] = 0.0F;
            }
        };
        const bool first = dev.start(pump);
        sleep_ms(300);
        dev.stop();
        dev.stop();  // 幂等
        const bool second = dev.start(pump);
        sleep_ms(300);
        dev.stop();
        check(first && second, "device backend restart across stop keeps join duty (reroute path alive)");
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
                const double f = 200.0 + (1800.0 * static_cast<double>(i) / frames);
                phase += 2.0 * AURORA_PI * f / 48000.0;
                const float v = 0.5F * static_cast<float>(std::sin(phase));
                buf.samples[static_cast<std::size_t>(i) * 2U] = v;
                buf.samples[(static_cast<std::size_t>(i) * 2U) + 1U] = v;
            }
            static_cast<void>(src->set_buffer(std::make_shared<const aurora::AudioBuffer>(std::move(buf))));
            static_cast<void>(src->start());
            wait_key("[a] sweep 200Hz->2kHz playing: audible, no clicks/pops?");
        }

        // b. 双源混音（440 + 660）
        {
            auto a = ctx->create_stream_source(48000);
            auto b = ctx->create_stream_source(48000);
            static_cast<void>(ctx->connect(a, ctx->destination()));
            static_cast<void>(ctx->connect(b, ctx->destination()));
            for (int chunk = 0; chunk < 8; ++chunk) {
                static_cast<void>(a->push(sine_pcm(48000, 4800, 440.0, 10000), 48000, 2));
                static_cast<void>(b->push(sine_pcm(48000, 4800, 660.0, 10000), 48000, 2));
                sleep_ms(80);
            }
            wait_key("[b] dual-source mix (440+660) playing: audible blend, no clipping noise?");
        }

        // c. 默认输出设备热切换（重路由腿的唯一实证口径）
        {
            auto src = ctx->create_stream_source(48000);
            static_cast<void>(ctx->connect(src, ctx->destination()));
            bool running = true;
            std::thread feeder([&src, &running] {
                while (running) {
                    static_cast<void>(src->push(sine_pcm(48000, 2400, 520.0, 9000), 48000, 2));
                    sleep_ms(50);
                }
            });
            emit("[c] now switch the system default output device (or unplug it), then watch for resume");
            wait_key("[c] audio stopped while switching and resumed on the new device? no crash?");
            running = false;
            feeder.join();
        }
    }

    static_cast<void>(ctx->close());
    emit(failures() == 0 ? "result: ALL PASS (exit 0)"
                         : "result: " + std::to_string(failures()) + " FAILURE(S) (exit 1)");
    return failures() == 0 ? 0 : 1;
}

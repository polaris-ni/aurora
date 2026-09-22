/// 真机验收探针：Web Audio 设备后端（浏览器 AudioContext + 主线程推样环）。
///
/// 目标单元：src/aurora/media/audio_webaudio.cpp
/// 无头 CI 证不了的四件事，本探针在真实浏览器里逐项取证（CDP 驱动
/// tools/verify/wasm_audio_cdp_drive.mjs）：
///   ① 后端真在：`AudioContext::silent()` 为假 ⇒ 设备协商成功、图由真设备驱动
///      （裸 Node 下无 AudioContext，此项必为真静默，故只可能在浏览器里成立）。
///   ② 自动播放闸门：建上下文后 `context_state()` 必为 0（suspended）且零消费——
///      「start() 返回 true」≠「已出声」，这条把该差异钉成判据。
///   ③ 手势开闸：CDP 派发**真实**鼠标按下后 → state=1（running），消费时长按墙钟推进
///      （秒/秒 ≈ 1.00，容差 ±15%），且推式环稳态零欠载。
///   ④ 饿死与自愈：JS 侧制造一次主线程长任务（忙等 ≥ 环水位时长）⇒ 欠载计数上升，
///      之后消费继续推进 ⇒ 证「补零 + 下拍回补」的自愈路径，而非一死了之。
/// 另有信号真达目的地的旁证：驱动侧把 `globalThis.__auroraWa.node` 分一路接
/// AnalyserNode 读 RMS（无音频设备的无头环境也能取证），须 > 0.1（440 Hz 满幅正弦
/// 理论 RMS ≈ 0.707）。图时钟领先设备时钟一个水位（`ctime - consumed` ≤ 0.3s）。
/// 状态串里的 `consumed`/`ctime` 均以**秒**计（按采样率归一），驱动侧判据同此口径。
///
/// 构建（需音频特性开启）：
///   cmake --preset wasm -DAURORA_ENABLE_AUDIO=ON
///   cmake --build build-wasm --target aurora_verify_wasm_audio
/// 运行：node tools/verify/wasm_audio_cdp_drive.mjs
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "aurora/aurora.h"

#ifdef AURORA_ENABLE_AUDIO_WEBAUDIO
#include "aurora/media/audio_webaudio.h"
#include "emscripten/eventloop.h"
#endif

namespace {

#ifdef AURORA_ENABLE_AUDIO_WEBAUDIO

constexpr int AURORA_RATE = 48000;
constexpr double AURORA_TONE_HZ = 440.0;
constexpr int AURORA_TONE_SECONDS = 1;

/// 探针观测态：设备协商产物 + 观测口集中一处，经 `emscripten_set_interval` 的 userData 递给
/// 回调——定时器回调是 C 函数指针（无捕获能力），又须活到 main 之后，故实例由 main 的函数级
/// static 持有一次，不再散落为文件作用域全局量。
struct Probe {
    std::unique_ptr<au::AudioContext> ctx;
    au::WebAudioDeviceBackend *dev = nullptr;  ///< 裸指针：所有权已交给 ctx，生命周期随其存活
    std::shared_ptr<au::AudioBufferSourceNode> src;
};

/// 每拍把观测状态写进 `window.__auState`（驱动侧唯一读取面）。
/// EM_JS 形参为具名 C/C++ 参数（无 `$` 占位符），不触发 -Wdollar-in-identifier-extension。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, publish_state_js, (const char *base), { window.__auState = UTF8ToString(base); });
// clang-format on

auto publish_tick(void *user_data) -> void {
    auto *probe = static_cast<Probe *>(user_data);
    if (probe == nullptr || probe->ctx == nullptr || probe->dev == nullptr) {
        return;
    }
    const double consumed = static_cast<double>(probe->dev->consumed_frames()) / static_cast<double>(AURORA_RATE);
    char buf[320];
    std::snprintf(buf, sizeof(buf), "silent=%d state=%d rate=%d ch=%d consumed=%.3f underrun=%d ctime=%.3f playing=%d",
                  probe->ctx->silent() ? 1 : 0, au::WebAudioDeviceBackend::context_state(), probe->ctx->sample_rate(),
                  probe->ctx->channel_count(), consumed, probe->dev->underruns(), probe->ctx->current_time(),
                  probe->src != nullptr && !probe->src->finished() ? 1 : 0);
    publish_state_js(buf);
}

auto make_tone_buffer() -> std::shared_ptr<const au::AudioBuffer> {
    auto buffer = std::make_shared<au::AudioBuffer>();
    buffer->sample_rate = AURORA_RATE;
    buffer->channels = 2;
    buffer->samples.assign(static_cast<std::size_t>(AURORA_RATE) * AURORA_TONE_SECONDS * 2U, 0.0F);
    for (std::size_t frame = 0; frame < static_cast<std::size_t>(AURORA_RATE) * AURORA_TONE_SECONDS; ++frame) {
        const auto s = static_cast<float>(std::sin(2.0 * std::numbers::pi * AURORA_TONE_HZ *
                                                   static_cast<double>(frame) / static_cast<double>(AURORA_RATE)));
        buffer->samples[frame * 2U] = s;
        buffer->samples[(frame * 2U) + 1U] = s;
    }
    return buffer;
}

#endif  // AURORA_ENABLE_AUDIO_WEBAUDIO

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸（浏览器下即未捕获异常，不吞失败）
int main() {
#ifdef AURORA_ENABLE_AUDIO_WEBAUDIO
    // 观测态须活过 main（定时器回调在 main 返回后继续跑），故走函数级 static 而非栈对象。
    static Probe probe;
    // 后端由探针自造并注入：AudioContext 接走所有权，裸指针留作观测口（生命周期随 ctx）。
    auto backend = std::make_unique<au::WebAudioDeviceBackend>();
    probe.dev = backend.get();
    probe.ctx = std::make_unique<au::AudioContext>(std::move(backend));
    probe.src = probe.ctx->create_buffer_source();
    static_cast<void>(probe.src->set_buffer(make_tone_buffer()));
    probe.src->set_loop(true);
    static_cast<void>(probe.ctx->connect(probe.src, probe.ctx->destination()));
    static_cast<void>(probe.src->start(0.0));
    // 主线程定间隔发布状态（音频侧另有后端自己的排空定时器，二者互不相干）。
    // 本探针不跑帧循环，生命周期全悬在这两个定时器上：main 返回前推一枚 runtime
    // keepalive，免得 Emscripten 在 main 退出后收尾时把它们连根拔掉。
    emscripten_runtime_keepalive_push();
    emscripten_set_interval(&publish_tick, 100.0, &probe);
    publish_tick(&probe);
    return 0;
#else
    return 0;
#endif
}

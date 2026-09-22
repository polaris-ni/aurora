// ============================================================
// media/audio_webaudio.cpp — Web Audio 音频设备后端实现
// ------------------------------------------------------------
// 契约与线程口径见 audio_webaudio.h 文件头（无设备线程 / 推式环 / 自动播放闸门 /
// 延迟与欠载）。本文件按「纯折算层 + 宿主接线」两段组织：
//   · `detail::WebAudioRing` 是平台中立的环数学，**恒编译**（含非 WASM 目标），
//     故 utest_audio_webaudio 可在任意平台真值断言；
//   · 宿主接线（AudioContext / ScriptProcessor / 主线程排空定时器）仅在
//     AURORA_ENABLE_AUDIO_WEBAUDIO 开启时编译，宏关闭时为 disabled 桩（start 恒
//     false，与 Wasapi*/Alsa* 桩对称）。
// ============================================================

#include "audio_webaudio.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace aurora::detail {

// ---- 推式环数学（平台中立，恒编译） ----

WebAudioRing::WebAudioRing(int capacity_frames, int channels)
    : capacity_frames_(capacity_frames > 1 ? capacity_frames : 2), channels_(channels > 0 ? channels : 1) {
    data_.assign(static_cast<std::size_t>(capacity_frames_) * static_cast<std::size_t>(channels_), 0.0F);
}

auto WebAudioRing::avail_frames() const -> int {
    const int w = writer_;
    const int r = reader_;
    return (w - r + capacity_frames_) % capacity_frames_;
}

auto WebAudioRing::free_frames() const -> int {
    // 恒留一格空：writer == reader 即「空」，writer == reader + 1 即「满」，二者可辨。
    return capacity_frames_ - 1 - avail_frames();
}

auto WebAudioRing::write(const float *interleaved, int frames) -> int {
    if (interleaved == nullptr || frames <= 0) {
        return 0;
    }
    const int n = std::min(frames, free_frames());
    if (n <= 0) {
        return 0;
    }
    const int stride = channels_;
    const int first = std::min(n, capacity_frames_ - writer_);  // 环至多断成两段（回绕处）
    // 环形写入按「帧 × 声道」做偏移，裸指针/迭代器算术是本结构的既有形态（数据直接来自 wasm 线性内存）。
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::copy_n(interleaved, first * stride, data_.begin() + (static_cast<std::ptrdiff_t>(writer_) * stride));
    if (n > first) {
        std::copy_n(interleaved + (static_cast<std::size_t>(first) * stride),
                    static_cast<std::size_t>(n - first) * stride, data_.begin());
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    writer_ = (writer_ + n) % capacity_frames_;
    return n;
}

auto WebAudioRing::sync_reader() -> void {
    const int now = reader_;
    consumed_ += static_cast<long long>((now - last_reader_ + capacity_frames_) % capacity_frames_);
    last_reader_ = now;
}

}  // namespace aurora::detail

namespace aurora {

// 环水位与拍频——延迟与抗饿之间的折中，取值理由见各常量注释与头文件第 4 条。
namespace {

constexpr double AURORA_WA_PUMP_INTERVAL_MS = 20.0;  // 排空定时器周期（浏览器主线程）
constexpr int AURORA_WA_PROCESSOR_BLOCK = 2048;  // ScriptProcessorNode 块长（浏览器固定取值）
constexpr int AURORA_WA_MAX_CATCH_UP_FRAMES = 4096;  // 单拍至多渲染这么多帧（UI 长任务后不过补）
constexpr int AURORA_WA_RESUME_RETRY_TICKS = 50;  // 20ms × 50 ⇒ 自动播放闸门每秒至多重试一次
constexpr int AURORA_WA_GRAPH_CHANNELS = 2;  // 图侧声道契约恒定（多声道由浏览器上混，见 wa_open 注）

[[maybe_unused]] auto wa_target_frames(int rate) -> int {
    // 目标水位 ≈ 85ms，且不低于 2 个块长：JS 每次取走整块，水位须恒 ≥ 2 块，
    // 才容得下一整块时长（≈43ms）的排空拍迟到。
    return std::max(rate * 85 / 1000, 2 * AURORA_WA_PROCESSOR_BLOCK);
}

}  // namespace

struct WebAudioDeviceBackend::Impl {
    explicit Impl(int capacity_frames, int channels) : ring(capacity_frames, channels) {}

    detail::WebAudioRing ring;
    AudioDeviceFormat format{};
    RenderFn render;  // std::function 默认构造即空，无需 `{}`
    std::vector<float> scratch;  // 单拍渲染缓冲（交错；容量 = AURORA_WA_MAX_CATCH_UP_FRAMES × 声道）
    int target_frames = 0;
    int retry_ticks = 0;  // resume 限速计数
    bool pumping = false;  // 排空定时器在册
    int timer_id = 0;  // `emscripten_set_interval` 句柄（0 = 无，非零 = 在册）
};

}  // namespace aurora

#ifdef AURORA_ENABLE_AUDIO_WEBAUDIO

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

namespace aurora {

namespace {

// ---- 宿主接线（零链接标志：跨边界只传 wasm 内存地址，不导出任何 wasm 符号） ----

// 宿主是否有 AudioContext（裸 Node 无 ⇒ 静默降级，保 WASM 侧 ctest 口径不变）。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, wa_available, (), { return typeof AudioContext === 'undefined' ? 0 : 1; });
// clang-format on

// 建上下文并把协商采样率写到给定 int32 地址。返回 0 = 成功。
// 声道**不随设备**：图侧契约恒 stereo（`AudioDeviceFormat` 首切片约定），多声道输出
// 由浏览器在 connect(destination) 时自动上混——与 WASAPI/ALSA「设备差异不外露」同口径。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, wa_open, (intptr_t out_rate), {
    try {
        const ctx = new AudioContext();
        globalThis.__auroraWa = {ctx: ctx, node: null};
        HEAP32[out_rate >> 2] = Math.round(ctx.sampleRate);
        return 0;
    } catch (e) {
        return -1;
    }
});
// clang-format on

/// 建 ScriptProcessor 消费链：JS 回调按 head/tail 两个 int32 地址从 wasm 环取帧；
/// 环空则补零并累加欠载计数。**不导出 wasm 函数**——这是本后端零链接标志的关键。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, wa_attach_processor,
      (intptr_t data_ptr, intptr_t head_ptr, intptr_t tail_ptr, intptr_t underrun_ptr, int capacity_frames,
       int channels), {
          try {
              const w = globalThis.__auroraWa;
              if (!w || !w.ctx) {
                  return -1;
              }
              const node = w.ctx.createScriptProcessor(2048, 1, channels);
              const ring = new Float32Array(HEAPU8.buffer, data_ptr, capacity_frames * channels);
              node.onaudioprocess = (e) => {
                  const n = e.outputBuffer.length;
                  const h = HEAP32[head_ptr >> 2];
                  const t = HEAP32[tail_ptr >> 2];
                  if ((h - t + capacity_frames * 2) % capacity_frames < n) {
                      HEAP32[underrun_ptr >> 2] += 1;  // 欠载：补零，等下一拍补齐
                      for (let c = 0; c < channels; c++) {
                          e.outputBuffer.getChannelData(c).fill(0);
                      }
                      return;
                  }
                  for (let c = 0; c < channels; c++) {
                      const dst = e.outputBuffer.getChannelData(c);
                      for (let i = 0; i < n; i++) {
                          dst[i] = ring[((t + i) % capacity_frames) * channels + c];
                      }
                  }
                  HEAP32[tail_ptr >> 2] = (t + n) % capacity_frames;
              };
              node.connect(w.ctx.destination);
              w.node = node;
              return 0;
          } catch (err) {
              return -1;
          }
      });
// clang-format on

// 上下文状态：-1 无实例，0 suspended（待用户手势），1 running，2 其他（closed 等）。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, wa_state, (), {
    const w = globalThis.__auroraWa;
    if (!w || !w.ctx) {
        return -1;
    }
    const s = w.ctx.state;
    return s === 'running' ? 1 : (s === 'suspended' ? 0 : 2);
});
// clang-format on

// 请求解除自动播放闸门；无手势时浏览器 reject，故吞掉 promise 免脏控制台。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(int, wa_resume, (), {
    const w = globalThis.__auroraWa;
    if (!w || !w.ctx) {
        return -1;
    }
    try {
        const p = w.ctx.resume();
        if (p && typeof p.catch === 'function') {
            p.catch(() => { /* 闸门未开：下一拍再试 */ });
        }
    } catch (e) {
        return -2;
    }
    return 0;
});
// clang-format on

// 拆链并关闭上下文（`__auroraWa` 一并清除，防下次实例读到别人的节点）。
// EM_JS/EM_ASM 体是 JavaScript：clang-format 按 C++ 解析会拆坏 === / => / 实参括号，故整块不排版。
// clang-format off
EM_JS(void, wa_teardown, (), {
    const w = globalThis.__auroraWa;
    if (w) {
        try {
            if (w.node) {
                w.node.onaudioprocess = null;
                w.node.disconnect();
            }
            if (w.ctx) {
                w.ctx.close();
            }
        } catch (e) { /* 已关闭 */ }
        delete globalThis.__auroraWa;
    }
});
// clang-format on

constexpr int wa_ring_capacity(int rate) {
    // 环容量 ≈ 0.5s（48k 立体声 ⇒ 192 KB 线性内存），下限 4 块。取 0.5s 而非更小：
    // 水位维持在 ~85ms，余量是给主线程抖动与节流回补的，不是常态延迟。
    return std::max(4 * AURORA_WA_PROCESSOR_BLOCK, rate / 2);
}

}  // namespace

auto WebAudioDeviceBackend::pump(void *user_data) -> void {
    auto *impl = static_cast<WebAudioDeviceBackend::Impl *>(user_data);  // NOLINT: Emscripten 回调签名
    const int state = wa_state();
    if (state != 1) {
        // 上下文未跑（自动播放闸门 / 浏览器挂起）⇒ **不渲染**：图时钟如实冻结，免得
        // current_time() 领先于实际出声。限速重试 resume，用户一有交互即自动开声。
        if (state == 0 && ++impl->retry_ticks >= AURORA_WA_RESUME_RETRY_TICKS) {
            impl->retry_ticks = 0;
            wa_resume();
        }
        return;
    }
    impl->retry_ticks = 0;
    impl->ring.sync_reader();
    int need = impl->target_frames - impl->ring.avail_frames();
    if (need <= 0) {
        return;
    }
    need = std::min(need, AURORA_WA_MAX_CATCH_UP_FRAMES);
    const int chunk = std::min(need, AURORA_WA_PROCESSOR_BLOCK);
    for (int left = need; left > 0; left -= chunk) {
        const int frames = std::min(left, chunk);
        impl->render(impl->scratch.data(), frames);
        if (impl->ring.write(impl->scratch.data(), frames) < frames) {
            break;  // 环满：留下拍，绝不覆盖未读数据
        }
    }
    impl->ring.sync_reader();
}

WebAudioDeviceBackend::WebAudioDeviceBackend() {
    int rate = 0;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): EM_JS 形参只收整型，与 JS 侧
    // 交接的是 wasm 线性内存地址，指针必须落成 intptr_t
    const bool usable = wa_available() != 0 && wa_open(reinterpret_cast<intptr_t>(&rate)) == 0;
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!usable) {
        // 不可用（无 AudioContext / 创建被拒）：format() 给处理格式，start() 恒 false。
        impl_ = std::make_unique<Impl>(4 * AURORA_WA_PROCESSOR_BLOCK, AURORA_WA_GRAPH_CHANNELS);
        return;
    }
    const int negotiated_rate = rate > 0 ? rate : 48000;
    impl_ = std::make_unique<Impl>(wa_ring_capacity(negotiated_rate), AURORA_WA_GRAPH_CHANNELS);
    impl_->format = AudioDeviceFormat{.sample_rate = negotiated_rate, .channels = AURORA_WA_GRAPH_CHANNELS};
    impl_->target_frames = wa_target_frames(negotiated_rate);
    impl_->scratch.assign(
        static_cast<std::size_t>(AURORA_WA_MAX_CATCH_UP_FRAMES) * static_cast<std::size_t>(AURORA_WA_GRAPH_CHANNELS),
        0.0F);
}

WebAudioDeviceBackend::~WebAudioDeviceBackend() { stop(); }

auto WebAudioDeviceBackend::format() const -> AudioDeviceFormat { return impl_->format; }

auto WebAudioDeviceBackend::start(RenderFn render_block) -> bool {
    if (impl_->pumping || wa_state() == -1) {
        return false;  // 已启动，或构造期协商失败（无上下文 ⇒ 静默降级）
    }
    impl_->render = std::move(render_block);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): 零导出符号的跨语言边界只传
    // 地址——JS 侧按 intptr_t 建 HEAP 视图，故环的四个 wasm 内存指针必须落成整型
    const int rc = wa_attach_processor(reinterpret_cast<intptr_t>(impl_->ring.data()),
                                       reinterpret_cast<intptr_t>(impl_->ring.writer_position()),
                                       reinterpret_cast<intptr_t>(impl_->ring.reader_position()),
                                       reinterpret_cast<intptr_t>(impl_->ring.underrun_counter()),
                                       impl_->ring.capacity_frames(), impl_->format.channels);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (rc != 0) {
        wa_teardown();
        impl_->render = nullptr;
        return false;
    }
    // `emscripten_set_interval` 返回句柄（0 = 失败），`clear` 按句柄注销——不是按函数指针。
    impl_->timer_id = emscripten_set_interval(&WebAudioDeviceBackend::pump, AURORA_WA_PUMP_INTERVAL_MS, impl_.get());
    if (impl_->timer_id == 0) {
        wa_teardown();
        impl_->render = nullptr;
        return false;
    }
    impl_->pumping = true;
    return true;
}

auto WebAudioDeviceBackend::stop() -> void {
    if (impl_ == nullptr || !impl_->pumping) {
        return;
    }
    // 同线程回调：清定时器即断源，无需 join（桌面后端「等设备线程退出」在此天然满足）。
    emscripten_clear_interval(impl_->timer_id);
    impl_->timer_id = 0;
    impl_->pumping = false;
    impl_->render = nullptr;
    wa_teardown();
}

auto WebAudioDeviceBackend::context_state() -> int { return wa_state(); }

auto WebAudioDeviceBackend::retry_resume() -> bool {
    if (wa_state() != 0) {
        return false;
    }
    wa_resume();
    return true;
}

auto WebAudioDeviceBackend::underruns() const -> int { return impl_->ring.underruns(); }

auto WebAudioDeviceBackend::consumed_frames() const -> long long {
    impl_->ring.sync_reader();  // 顺带吸收对端推进（const 观测口兼记账，见头文件说明）
    return impl_->ring.consumed_frames();
}

auto WebAudioCaptureBackend::start(CaptureFn /*on_pcm*/) -> bool {
    return false;  // 采集未接线（getUserMedia 异步权限流与本契约不同形），见头文件申报
}

auto WebAudioCaptureBackend::stop() -> void {}

}  // namespace aurora

#else  // !AURORA_ENABLE_AUDIO_WEBAUDIO —— disabled 桩：start 恒 false → 静默模式

namespace aurora {

WebAudioDeviceBackend::WebAudioDeviceBackend()
    : impl_(std::make_unique<Impl>(4 * AURORA_WA_PROCESSOR_BLOCK, AURORA_WA_GRAPH_CHANNELS)) {}

WebAudioDeviceBackend::~WebAudioDeviceBackend() = default;

auto WebAudioDeviceBackend::format() const -> AudioDeviceFormat {
    return AudioDeviceFormat{};  // 处理格式（图在静默模式下按此运转）
}

auto WebAudioDeviceBackend::start(RenderFn /*render_block*/) -> bool { return false; }

auto WebAudioDeviceBackend::stop() -> void {}

auto WebAudioDeviceBackend::context_state() -> int {
    return -1;  // 无上下文（更无闸门可言）
}

auto WebAudioDeviceBackend::retry_resume() -> bool { return false; }

auto WebAudioDeviceBackend::underruns() const -> int { return 0; }

auto WebAudioDeviceBackend::consumed_frames() const -> long long { return 0; }

auto WebAudioCaptureBackend::start(CaptureFn /*on_pcm*/) -> bool { return false; }

auto WebAudioCaptureBackend::stop() -> void {}

}  // namespace aurora

#endif  // AURORA_ENABLE_AUDIO_WEBAUDIO

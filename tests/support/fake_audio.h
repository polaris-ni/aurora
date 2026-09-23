#pragma once

// ============================================================
// fake 音频设备桩（tests/support/fake_audio.h）
// ------------------------------------------------------------
// 供 utest_audio（AudioContext/节点族契约断言）共用：不依赖测试框架，仅依赖
// media/audio.h 公共头。不起线程——start() 记录渲染回调，由测试手动驱动
// render() 确定性推进；可注入失败点：start 失败（→ 静默模式契约）。
// ============================================================

#include <span>
#include <utility>
#include <vector>

#include "aurora/media/audio.h"

namespace aurora::testing {

/// @brief fake 音频设备桩：记录 start/stop 调用，测试手动驱动渲染块。
class FakeAudioDevice final : public AudioDeviceBackend {
  public:
    explicit FakeAudioDevice(int sample_rate = 48000, int channels = 2, bool fail_start = false)
        : format_{.sample_rate = sample_rate, .channels = channels}, fail_start_(fail_start) {}

    [[nodiscard]] auto format() const -> AudioDeviceFormat override { return format_; }
    auto start(RenderFn render_block) -> bool override {
        ++start_calls;
        if (fail_start_) {
            return false;
        }
        render_ = std::move(render_block);
        started_ = true;
        return true;
    }
    auto stop() -> void override {
        ++stop_calls;
        started_ = false;
        render_ = nullptr;
    }

    /// @brief 手动驱动一块渲染（经 context 渲染入口，返回副本供断言）。
    auto render(int frames) -> std::vector<float> {
        std::vector<float> buf(static_cast<std::size_t>(frames) * static_cast<std::size_t>(format_.channels));
        if (render_ != nullptr) {
            render_(buf.data(), frames);
        }
        return buf;
    }

    [[nodiscard]] auto is_started() const -> bool { return started_; }

    int start_calls = 0;
    int stop_calls = 0;

  private:
    AudioDeviceFormat format_;
    bool fail_start_;
    bool started_ = false;
    RenderFn render_;
};

/// @brief fake 音频采集桩：记录 start/stop 与回调，测试手动 emit 推 PCM；可注入 start 失败。
class FakeAudioCaptureDevice final : public AudioCaptureBackend {
  public:
    explicit FakeAudioCaptureDevice(bool fail_start = false) : fail_start_(fail_start) {}

    auto start(CaptureFn on_pcm) -> bool override {
        ++start_calls;
        if (fail_start_) {
            return false;
        }
        on_pcm_ = std::move(on_pcm);
        started_ = true;
        return true;
    }
    auto stop() -> void override {
        ++stop_calls;
        started_ = false;
        on_pcm_ = nullptr;
    }

    /// @brief 手动注入一包交错 float32 PCM（模拟采集线程回调；经被测链路转推环）。
    auto emit(std::span<const float> interleaved, int frames, int rate, int channels) -> void {
        if (on_pcm_ != nullptr) {
            on_pcm_(interleaved.data(), frames, rate, channels);
        }
    }

    [[nodiscard]] auto is_started() const -> bool { return started_; }

    int start_calls = 0;
    int stop_calls = 0;

  private:
    bool fail_start_;
    bool started_ = false;
    CaptureFn on_pcm_;
};

}  // namespace aurora::testing

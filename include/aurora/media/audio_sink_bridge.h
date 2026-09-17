#pragma once

// ============================================================
// media/audio_sink_bridge.h — AudioSink → 音频图桥适配器
// ------------------------------------------------------------
// 兜底既有 AudioSink 契约（media/video_source.h）：App 已有的
// `AudioSink` 实现可零改动改为把 PCM 路由进音频图（`AudioContext`），
// 也可被 `VideoPlayer` 内部用于「`set_audio_callback` 通道的 PCM
// 自动接 `AudioStreamSourceNode`」的默认接线。
//
// 音量/静音经图内 `GainNode` 施加（单点衰减，源侧不再自行缩放）。
// ============================================================

#include <cstddef>
#include <memory>
#include <span>

#include "aurora/media/audio.h"
#include "aurora/media/video_source.h"

namespace aurora {

/// @brief `AudioSink` 实现桥：`play_samples` 推入自持 `AudioStreamSourceNode`，
///        经图内 `GainNode` 施加音量/静音后汇入 `AudioContext::destination()`。
///
/// 典型用法：
/// - **App 侧兜底**：既有 `MySink : AudioSink` 改接图——`AudioSinkGraphBridge bridge{ctx};`
///   然后把原来调用 `my_sink.play_samples(...)` 的路径改为 `bridge.play_samples(...)`。
/// - **播放器默认接线**：`VideoPlayer::set_audio_context(ctx)` 内部创建本桥并接管
///   `set_audio_callback` 的 PCM 通道（见 video_player.h）。
///
/// @note Thread: 推流（play_samples）允许来自解码器线程；音量/静音/析构走 main-thread
///       （图变更经命令环块首生效）
/// @note Rebuildable: no
class AudioSinkGraphBridge final : public AudioSink {
  public:
    /// @brief 创建桥：在 `ctx` 上建 `AudioStreamSourceNode`（环形容量
    ///        `ring_capacity_frames` 帧推流环）+ `GainNode` 并接至 `destination()`。
    ///        连接失败（环成环等异常）时桥保持静默（推入即丢弃），不抛出。
    explicit AudioSinkGraphBridge(std::shared_ptr<AudioContext> ctx, std::size_t ring_capacity_frames = 16384)
        : ctx_(std::move(ctx)) {
        if (!ctx_) {
            return;
        }
        stream_ = ctx_->create_stream_source(ring_capacity_frames);
        gain_ = ctx_->create_gain();
        connected_ = ctx_->connect(stream_, gain_).ok() && ctx_->connect(gain_, ctx_->destination()).ok();
    }

    ~AudioSinkGraphBridge() override {
        if (!ctx_ || !stream_ || !gain_) {
            return;
        }
        static_cast<void>(ctx_->disconnect(stream_, gain_));
        static_cast<void>(ctx_->disconnect(gain_, ctx_->destination()));
    }

    AudioSinkGraphBridge(const AudioSinkGraphBridge &) = delete;
    auto operator=(const AudioSinkGraphBridge &) -> AudioSinkGraphBridge & = delete;

    /// @brief 写入一包 16-bit PCM（AudioSink 契约）：推入图内推流环。
    ///        图未接好或上下文关闭时丢弃样本（不报错——桥为兜底路径）。
    auto play_samples(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> void override {
        if (connected_ && !ctx_->closed()) {
            static_cast<void>(stream_->push(pcm, sample_rate, channels));
        }
    }

    /// @brief 音量（v ∈ [0,1]）：经图内 GainNode 施加。
    auto set_volume(double v) -> void override {
        volume_ = v;
        apply_gain();
    }

    /// @brief 静音：图内 GainNode 归零。
    auto set_muted(bool m) -> void override {
        muted_ = m;
        apply_gain();
    }

    /// @brief 桥是否已成功接入图（上下文有效且两条边连接成功）。
    [[nodiscard]] auto connected() const -> bool { return connected_; }

  private:
    auto apply_gain() const -> void {
        if (gain_) {
            gain_->gain().set_value(muted_ ? 0.0F : static_cast<float>(volume_));
        }
    }

    std::shared_ptr<AudioContext> ctx_;
    std::shared_ptr<AudioStreamSourceNode> stream_;
    std::shared_ptr<GainNode> gain_;
    bool connected_ = false;
    double volume_ = 1.0;
    bool muted_ = false;
};

}  // namespace aurora

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

/// @brief Aurora 公共命名空间：本头在其中声明 AudioSink → 音频图桥适配器 `AudioSinkGraphBridge`。
namespace aurora {

/// @brief `AudioSink` 实现桥：`play_samples` 推入自持 `AudioStreamSourceNode`，
///        经图内 `GainNode` 施加音量/静音后汇入 `AudioContext::destination()`。
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
    /// @param ctx 目标音频上下文（共享所有权；nullptr 时桥为 inert 空壳）。
    /// @param ring_capacity_frames 推流环容量（源采样率帧口径，默认 16384）。
    explicit AudioSinkGraphBridge(std::shared_ptr<AudioContext> ctx, std::size_t ring_capacity_frames = 16384)
        : ctx_(std::move(ctx)) {
        if (!ctx_) {
            return;
        }
        stream_ = ctx_->create_stream_source(ring_capacity_frames);
        gain_ = ctx_->create_gain();
        connected_ = ctx_->connect(stream_, gain_).ok() && ctx_->connect(gain_, ctx_->destination()).ok();
    }

    /// @brief 析构：断开本桥建立的 stream→gain 与 gain→destination 两条边（context 已失效则跳过）。
    ~AudioSinkGraphBridge() override {
        if (!ctx_ || !stream_ || !gain_) {
            return;
        }
        static_cast<void>(ctx_->disconnect(stream_, gain_));
        static_cast<void>(ctx_->disconnect(gain_, ctx_->destination()));
    }

    // 五法则（CODING_STANDARDS.md §5.1）：桥自持 stream_/gain_ 两条图边并在析构时断边，
    // 拷贝/移动会造成重复断边或旁落的边；调用方只经 shared_ptr/栈对象使用它。
    /// @brief 拷贝构造禁用：见上方五法则说明（析构断边不可双份执行）。
    AudioSinkGraphBridge(const AudioSinkGraphBridge &) = delete;
    /// @brief 拷贝赋值禁用：见上方五法则说明。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(const AudioSinkGraphBridge &) -> AudioSinkGraphBridge & = delete;
    /// @brief 移动构造禁用：见上方五法则说明（移动会旁落析构期断边的所有权）。
    AudioSinkGraphBridge(AudioSinkGraphBridge &&) = delete;
    /// @brief 移动赋值禁用：见上方五法则说明。
    /// @return 签名声明返回左操作数引用（函数已删除，永不可调用）。
    auto operator=(AudioSinkGraphBridge &&) -> AudioSinkGraphBridge & = delete;

    /// @brief 写入一包 16-bit PCM（AudioSink 契约）：推入图内推流环。
    ///        图未接好或上下文关闭时丢弃样本（不报错——桥为兜底路径）。
    /// @param pcm 交错 16-bit 样本（长度须为 channels 的整数倍）。
    /// @param sample_rate 源采样率（Hz）。
    /// @param channels 声道数（1 = mono / 2 = stereo）。
    auto play_samples(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> void override {
        if (connected_ && !ctx_->closed()) {
            static_cast<void>(stream_->push(pcm, sample_rate, channels));
        }
    }

    /// @brief 音量（v ∈ [0,1]）：经图内 GainNode 施加。
    /// @param v 线性音量因子（0 = 静音响度，1 = 原样）。
    auto set_volume(double v) -> void override {
        volume_ = v;
        apply_gain();
    }

    /// @brief 静音：图内 GainNode 归零。
    /// @param m true = 静音（增益 0）；false = 恢复 set_volume 设定的音量。
    auto set_muted(bool m) -> void override {
        muted_ = m;
        apply_gain();
    }

    /// @brief 桥是否已成功接入图（上下文有效且两条边连接成功）。
    /// @return connected_ 标记。
    [[nodiscard]] auto connected() const -> bool { return connected_; }

  private:
    auto apply_gain() const -> void {
        if (gain_) {
            gain_->gain().set_value(muted_ ? 0.0F : static_cast<float>(volume_));
        }
    }

    std::shared_ptr<AudioContext> ctx_;  ///< 目标音频上下文（共享所有权；nullptr = inert 桥）
    std::shared_ptr<AudioStreamSourceNode> stream_;  ///< 图内推流节点（play_samples 的落点）
    std::shared_ptr<GainNode> gain_;  ///< 图内增益节点（音量/静音单点衰减）
    bool connected_ = false;  ///< stream→gain→destination 两条边均连接成功
    double volume_ = 1.0;  ///< 最近 set_volume 值（[0,1]，经 apply_gain 施加到 GainNode）
    bool muted_ = false;  ///< 最近 set_muted 状态（true 时增益强制 0）
};

}  // namespace aurora

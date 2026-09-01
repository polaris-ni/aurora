#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/core/types.h"
#include "aurora/state/reactive.h"

namespace aurora {

/// @brief 单帧视频画面（已解码 RGBA8 像素，与 `core::Image` 同构，可直接 `Painter::draw_image`）。
struct VideoFrame {
    Image image;                        ///< 解码后的像素（RGBA8）。
    std::chrono::microseconds pts{ 0 }; ///< 该帧的呈现时间戳（相对起点）。
};

/// @brief 视频源抽象（**核心扩展点①**：可插拔解码/源）。
///
/// 继承它即可接入任意解码后端（ffmpeg / 平台媒体框架 / 相机 / 网络流），无需改动播放器本体。
///
/// 提供双模型取帧：
///  - **拉模型** `frame_at(pos)`：播放器按播放时钟主动拉取某时刻的帧（适合软件栅格、可 seek、可单测）。
///  - **推模型** `set_frame_callback(...)`：流式解码器在自有线程解码后主动推送帧（默认空实现）。
///
/// 音频以 `set_volume` / `set_muted` 与 `on_audio_samples`（`set_audio_callback`）暴露；具体音频
/// 输出由独立的 `AudioSink` 抽象交给 App 实现（见 `ImageSequenceSource`：无音频轨道）。
///
/// @note Thread: main-thread only (pull model); push model may call from decoder thread
/// @note Side-effects: none
/// @note Rebuildable: no
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 接口基类仅声明虚析构，拷贝/移动语义无意为禁用而非默认
class VideoSource {
  public:
    virtual ~VideoSource() = default;

    /// @brief 打开 URI（文件路径 / URL / 设备名……）。返回 false 表示无视频轨道。
    [[nodiscard]] virtual auto open(std::string_view uri) -> Result<bool> = 0;
    /// @brief 关闭并释放资源。
    virtual auto close() -> void = 0;

    [[nodiscard]] virtual auto has_video() const -> bool = 0;
    [[nodiscard]] virtual auto has_audio() const -> bool = 0;
    /// @brief 画面自然尺寸（像素）；未知返回 {0,0}。
    [[nodiscard]] virtual auto natural_size() const -> Size = 0;
    [[nodiscard]] virtual auto duration() const -> std::chrono::microseconds = 0;

    virtual auto play() -> void = 0;
    virtual auto pause() -> void = 0;
    [[nodiscard]] virtual auto is_playing() const -> bool = 0;
    virtual auto seek(std::chrono::microseconds pos) -> void = 0;
    [[nodiscard]] virtual auto position() const -> std::chrono::microseconds = 0;

    virtual auto set_volume(double v) -> void = 0; ///< v ∈ [0,1]
    virtual auto set_muted(bool m) -> void = 0;

    /// @brief 拉模型：取 `pos` 时刻的帧。
    [[nodiscard]] virtual auto frame_at(std::chrono::microseconds pos) -> Result<VideoFrame> = 0;

    // ---- 推模型（可选，默认空实现） ----
    /// @brief 流式解码器推送帧时调用（播放器据此刷新当前帧）。
    // NOLINTBEGIN(performance-unnecessary-value-param):
    // 推模型回调以值接收std::function，契约接口不改const，以免破坏override
    virtual auto set_frame_callback(std::function<void(const VideoFrame &)> /*cb*/) -> void {}
    /// @brief 推送音频 PCM（16-bit）样本，供 `AudioSink` 播放。
    virtual auto
    set_audio_callback(std::function<void(std::span<const std::int16_t>, int /*sample_rate*/, int /*channels*/)> /*cb*/)
        -> void {}
    /// @brief 播放状态变化回调（true=播放中）。
    virtual auto set_state_callback(std::function<void(bool /*playing*/)> /*cb*/) -> void {}
    // NOLINTEND(performance-unnecessary-value-param)
};

/// @brief 音频输出抽象（**音频抽象**承载者）。
///
/// App 继承它，把 `VideoSource` 通过 `set_audio_callback` 推送的 PCM 送到平台音频后端
/// （OpenAL / WASAPI / AAudio / CoreAudio……）。库本身不提供音频实现。
///
/// @note Thread: thread-safe (interface contract)
/// @note Side-effects: none
/// @note Rebuildable: no
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 接口基类仅声明虚析构，拷贝/移动语义无意为禁用而非默认
class AudioSink {
  public:
    virtual ~AudioSink() = default;
    /// @brief 写入一包 16-bit PCM 采样。
    virtual auto play_samples(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> void = 0;
    virtual auto set_volume(double v) -> void = 0; ///< v ∈ [0,1]
    virtual auto set_muted(bool m) -> void = 0;
};

/// @brief 播放器能力接口（**扩展点②的解耦基石**）。
///
/// `VideoControls` 等 UI 通过该接口操纵播放器，而不直接耦合 `VideoPlayer`，便于整体替换控件。
/// 同时暴露 `Reactive` 信号指针，使进度条 / 按钮等可直接绑定（共享底层 `State`，更新即刷新 UI）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): 接口基类仅声明虚析构，拷贝/移动语义无意为禁用而非默认
class VideoController {
  public:
    virtual ~VideoController() = default;

    virtual auto toggle_play() -> void = 0;
    virtual auto seek_fraction(double f) -> void = 0; ///< f ∈ [0,1]
    virtual auto set_volume(double v) -> void = 0;    ///< v ∈ [0,1]
    virtual auto set_muted(bool m) -> void = 0;

    [[nodiscard]] virtual auto is_playing() const -> bool = 0;
    [[nodiscard]] virtual auto position_fraction() const -> double = 0; ///< ∈ [0,1]
    [[nodiscard]] virtual auto duration() const -> std::chrono::microseconds = 0;
    [[nodiscard]] virtual auto volume() const -> double = 0;
    [[nodiscard]] virtual auto muted() const -> bool = 0;

    // 响应式信号（指针，便于 UI 绑定；生命周期由播放器管理）。
    [[nodiscard]] virtual auto playing_signal() -> Reactive<bool> * = 0;
    [[nodiscard]] virtual auto progress_signal() -> Reactive<double> * = 0;
    [[nodiscard]] virtual auto volume_signal() -> Reactive<double> * = 0;
    [[nodiscard]] virtual auto muted_signal() -> Reactive<bool> * = 0;
};

} // namespace aurora

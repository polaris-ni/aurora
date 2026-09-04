#pragma once

#include <chrono>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/media/video_source.h"

namespace aurora {

/// @brief 零依赖图片序列源（**内置轻量源**）。
///
/// 把一组 `Image`（或图片文件）当作定帧率动画播放。用于自包含演示与单元测试，
/// 同时也是「如何接入自定义解码器」的最小参考实现：继承 `VideoSource`、实现 `frame_at` 即可。
/// 无音频轨道（`has_audio()==false`），音频方法为 no-op。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class ImageSequenceSource : public VideoSource {
  public:
    ImageSequenceSource() = default;
    explicit ImageSequenceSource(std::vector<Image> frames, double fps = 24.0)
        : frames_(std::move(frames)), fps_(fps) {}

    /// @brief 直接喂入已解码帧（不读文件）。
    auto set_frames(std::vector<Image> frames) -> void { frames_ = std::move(frames); }
    auto append_frame(Image frame) -> void { frames_.push_back(std::move(frame)); }
    /// @brief 设定帧率（fps，>0）。
    auto set_fps(double fps) -> void {
        if (fps > 0.0) {
            fps_ = fps;
        }
    }
    [[nodiscard]] auto fps() const -> double { return fps_; }
    [[nodiscard]] auto frame_count() const -> size_t { return frames_.size(); }

    /// @brief 打开：uri 为 `;` / `|` 分隔的若干图片路径时逐张加载；单路径则作为单帧静画。
    [[nodiscard]] auto open(std::string_view uri) -> Result<bool> override;
    auto close() -> void override { frames_.clear(); }

    [[nodiscard]] auto has_video() const -> bool override { return !frames_.empty(); }
    [[nodiscard]] auto has_audio() const -> bool override { return false; }
    [[nodiscard]] auto natural_size() const -> Size override;
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override;

    auto play() -> void override { playing_ = true; }
    auto pause() -> void override { playing_ = false; }
    [[nodiscard]] auto is_playing() const -> bool override { return playing_; }
    auto seek(std::chrono::microseconds pos) -> void override;
    [[nodiscard]] auto position() const -> std::chrono::microseconds override { return pos_; }

    auto set_volume(double /*v*/) -> void override {}
    auto set_muted(bool /*m*/) -> void override {}

    [[nodiscard]] auto frame_at(std::chrono::microseconds pos) -> Result<VideoFrame> override;

  private:
    std::vector<Image> frames_;
    double fps_ = 24.0;
    bool playing_ = false;
    std::chrono::microseconds pos_{0};
};

}  // namespace aurora

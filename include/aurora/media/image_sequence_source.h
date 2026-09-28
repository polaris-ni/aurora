#pragma once

#include <chrono>
#include <vector>

#include "aurora/core/image.h"
#include "aurora/core/result.h"
#include "aurora/media/video_source.h"

/// @brief Aurora 公共命名空间：本头在其中声明零依赖图片序列视频源 `ImageSequenceSource`。
namespace aurora {

/// @brief 零依赖图片序列源（**内置轻量源**）。
/// 把一组 `Image`（或图片文件）当作定帧率动画播放。用于自包含演示与单元测试，
/// 同时也是「如何接入自定义解码器」的最小参考实现：继承 `VideoSource`、实现 `frame_at` 即可。
/// 无音频轨道（`has_audio()==false`），音频方法为 no-op。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
class ImageSequenceSource : public VideoSource {
  public:
    /// @brief 默认构造：零帧、fps 24.0，经 set_frames()/append_frame() 喂帧或 open() 加载后可用。
    ImageSequenceSource() = default;
    /// @brief 以已解码帧序列与定帧率构造。
    /// @param frames 按播放顺序排列的 RGBA8 帧。
    /// @param fps 帧率（须 > 0，默认 24.0）。
    explicit ImageSequenceSource(std::vector<Image> frames, double fps = 24.0)
        : frames_(std::move(frames)), fps_(fps) {}

    /// @brief 直接喂入已解码帧（不读文件；替换现有全部帧）。
    /// @param frames 按播放顺序排列的 RGBA8 帧。
    auto set_frames(std::vector<Image> frames) -> void { frames_ = std::move(frames); }
    /// @brief 追加一帧到序列末尾。
    /// @param frame 待追加的帧。
    auto append_frame(Image frame) -> void { frames_.push_back(std::move(frame)); }
    /// @brief 设定帧率（fps，>0）。
    /// @param fps 目标帧率；≤ 0 时忽略、保持原值。
    auto set_fps(double fps) -> void {
        if (fps > 0.0) {
            fps_ = fps;
        }
    }
    /// @brief 当前帧率。
    /// @return fps（恒 > 0，默认 24.0）。
    [[nodiscard]] auto fps() const -> double { return fps_; }
    /// @brief 序列帧数。
    /// @return 已持有帧数量（未 open/未喂帧时为 0）。
    [[nodiscard]] auto frame_count() const -> size_t { return frames_.size(); }

    /// @brief 打开：uri 为 `;` / `|` 分隔的若干图片路径时逐张加载；单路径则作为单帧静画。
    /// @param uri 图片路径列表（`;` 或 `|` 分隔；空串返回错误）。
    /// @return 全部加载成功返回 true；任一张解码失败传播该 Image::load 错误。
    [[nodiscard]] auto open(std::string_view uri) -> Result<bool> override;
    /// @brief 关闭：清空帧序列。
    auto close() -> void override { frames_.clear(); }

    /// @brief 是否有视频轨道。
    /// @return 帧序列非空即为 true。
    [[nodiscard]] auto has_video() const -> bool override { return !frames_.empty(); }
    /// @brief 是否有音频轨道。
    /// @return 恒 false（本源无音频，音量/静音方法为 no-op）。
    [[nodiscard]] auto has_audio() const -> bool override { return false; }
    /// @brief 画面自然尺寸（取首帧宽高；空序列返回 {0,0}）。
    /// @return 首帧图像的宽高；序列为空时为 {0,0}。
    [[nodiscard]] auto natural_size() const -> Size override;
    /// @brief 总时长（= 帧数 / fps，换算为微秒）。
    /// @return 按当前 fps 折算的总微秒数。
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override;

    /// @brief 置播放态（帧内容仍按 pos 拉取，本方法只切换 playing_ 标记）。
    auto play() -> void override { playing_ = true; }
    /// @brief 清播放态。
    auto pause() -> void override { playing_ = false; }
    /// @brief 是否处于播放态。
    /// @return play()/pause() 的最新标记。
    [[nodiscard]] auto is_playing() const -> bool override { return playing_; }
    /// @brief 跳转：钳位到 [0, duration] 后更新内部位置。
    /// @param pos 目标位置（微秒；负值按 0、超出时长按时长处理）。
    auto seek(std::chrono::microseconds pos) -> void override;
    /// @brief 当前位置（seek()/播放时钟推进后的钳位值）。
    /// @return 位置（微秒）。
    [[nodiscard]] auto position() const -> std::chrono::microseconds override { return pos_; }

    /// @brief 音量设置（无音频轨道：no-op）。
    /// @param v 音量因子；本实现忽略。
    auto set_volume([[maybe_unused]] double v) -> void override {}
    /// @brief 静音设置（无音频轨道：no-op）。
    /// @param m 静音状态；本实现忽略。
    auto set_muted([[maybe_unused]] bool m) -> void override {}

    /// @brief 拉模型取帧：按 pos / fps 折算帧下标（钳位到末帧），并回填该帧 nominal pts。
    /// @param pos 呈现时刻（微秒；负值按 0、超出时长按时长处理）。
    /// @return 空序列返回错误；否则含 image 与 pts 的 VideoFrame。
    [[nodiscard]] auto frame_at(std::chrono::microseconds pos) -> Result<VideoFrame> override;

  private:
    std::vector<Image> frames_;
    double fps_ = 24.0;
    bool playing_ = false;
    std::chrono::microseconds pos_{0};
};

}  // namespace aurora

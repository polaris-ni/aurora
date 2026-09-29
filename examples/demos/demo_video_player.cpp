#include <chrono>
#include <cstdint>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/media/image_sequence_source.h"
#include "aurora/media/video_player.h"
#include "demo_common.h"

using aurora::Image;

namespace {

// 生成一段「变色」动画帧，用于演示内置零依赖源。
auto make_frames(int n) -> std::vector<Image> {
    std::vector<Image> frames;
    constexpr int w = 160;
    constexpr int h = 90;
    for (int i = 0; i < n; ++i) {
        const auto r = static_cast<std::uint8_t>((i * 37) % 256);
        const auto g = static_cast<std::uint8_t>((i * 91) % 256);
        const auto b = static_cast<std::uint8_t>((i * 151) % 256);
        std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4U, 0U);
        for (size_t p = 0; p < px.size(); p += 4U) {
            px.at(p) = r;
            px.at(p + 1U) = g;
            px.at(p + 2U) = b;
            px.at(p + 3U) = 255U;
        }
        frames.emplace_back(w, h, std::move(px));
    }
    return frames;
}

/// @brief 演示「可子类化播放器本体」：覆写 `on_frame` 给画面叠加一层半透明色调，
///        覆写 `on_playback_tick` 在播完时回到起点（源仅 2 秒，单次播放看不出循环）。
class TintedVideoPlayer : public aurora::VideoPlayer {
  protected:
    auto on_frame(const Image &frame) -> void override {
        // 复制并整体染上一层青色调，再交给基类缓存 + 重绘。
        Image tinted = frame;
        for (size_t p = 0; p < tinted.pixels.size(); p += 4U) {
            tinted.pixels.at(p) = static_cast<std::uint8_t>(static_cast<float>(tinted.pixels.at(p)) * 0.6F);
            tinted.pixels.at(p + 1U) = static_cast<std::uint8_t>(static_cast<float>(tinted.pixels.at(p + 1U)) * 0.9F);
            tinted.pixels.at(p + 2U) = static_cast<std::uint8_t>(static_cast<float>(tinted.pixels.at(p + 2U)) * 0.9F);
        }
        VideoPlayer::on_frame(tinted);
    }

    auto on_playback_tick(std::chrono::steady_clock::time_point now) -> void override {
        const bool was_playing = is_playing();
        VideoPlayer::on_playback_tick(now);
        // 只在「播到末尾自停」这一跳上重开；手动暂停于末帧不会触发（was_playing 为假）。
        if (was_playing && !is_playing() && position_fraction() >= 1.0) {
            seek(std::chrono::microseconds{0});
            play();
        }
    }
};

}  // namespace

// 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做 try/catch 包装。
// 理由必须写在紧邻式豁免指令 **之前**：夹在它与目标行之间会被 clang-format 折行，导致抑制失效。
// NOLINTNEXTLINE(bugprone-exception-escape)
auto main() -> int {
    const auto src = std::make_shared<aurora::ImageSequenceSource>(make_frames(48), 24.0);  // 2 秒 @ 24fps
    auto player = std::make_unique<TintedVideoPlayer>();
    player->set_source(src);
    player->width(aurora::px(640));
    player->height(aurora::px(360));
    // 演示「装好源即播」：不显式 play() 时播放器会停在第 0 帧（该帧是全黑），画面看不出推进。
    player->play();

    // 提示：要自定义控件 UI，可继承 VideoControls 或调用 player->set_controls(...)。
    return run_demo(aurora::Node{std::move(player)}, "Video Player", 640.0F, 360.0F);
}
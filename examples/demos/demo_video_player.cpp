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
        std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4u, 0u);
        for (size_t p = 0; p < px.size(); p += 4u) {
            px[p] = r;
            px[p + 1u] = g;
            px[p + 2u] = b;
            px[p + 3u] = 255u;
        }
        frames.emplace_back(w, h, std::move(px));
    }
    return frames;
}

/// @brief 演示「可子类化播放器本体」：覆写 on_frame 给画面叠加一层半透明色调。
class TintedVideoPlayer : public aurora::VideoPlayer {
  protected:
    auto on_frame(const Image &frame) -> void override {
        // 复制并整体染上一层青色调，再交给基类缓存 + 重绘。
        Image tinted = frame;
        for (size_t p = 0; p < tinted.pixels.size(); p += 4u) {
            tinted.pixels[p] = static_cast<std::uint8_t>(static_cast<float>(tinted.pixels[p]) * 0.6f);
            tinted.pixels[p + 1u] = static_cast<std::uint8_t>(static_cast<float>(tinted.pixels[p + 1U]) * 0.9f);
            tinted.pixels[p + 2u] = static_cast<std::uint8_t>(static_cast<float>(tinted.pixels[p + 2U]) * 0.9f);
        }
        VideoPlayer::on_frame(tinted);
    }
};

} // namespace

auto main() -> int {
    const auto src = std::make_shared<aurora::ImageSequenceSource>(make_frames(48), 24.0); // 2 秒 @ 24fps
    auto player = std::make_unique<TintedVideoPlayer>();
    player->set_source(src);
    player->width(aurora::px(640));
    player->height(aurora::px(360));

    // 提示：要自定义控件 UI，可继承 VideoControls 或调用 player->set_controls(...)。
    return run_demo(aurora::Node{ std::move(player) }, "Video Player", 640.0f, 360.0f);
}

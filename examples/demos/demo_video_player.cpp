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

/// @brief 演示「可子类化播放器本体」：覆写 on_frame 给画面叠加一层半透明色调。
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
};

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) 入口函数允许库异常逃逸到 main（terminate 即失败路径），示例/CLI 不做 try/catch 包装
auto main() -> int {
    const auto src = std::make_shared<aurora::ImageSequenceSource>(make_frames(48), 24.0); // 2 秒 @ 24fps
    auto player = std::make_unique<TintedVideoPlayer>();
    player->set_source(src);
    player->width(aurora::px(640));
    player->height(aurora::px(360));

    // 提示：要自定义控件 UI，可继承 VideoControls 或调用 player->set_controls(...)。
    return run_demo(aurora::Node{std::move(player)}, "Video Player", 640.0F, 360.0F);
}
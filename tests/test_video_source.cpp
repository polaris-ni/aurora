// ── API 覆盖映射 ─────────────────────────────
// media/image_sequence_source.h(ImageSequenceSource 帧序列源)。

#include <chrono>
#include <vector>

#include "aurora/aurora.h"
#include "aurora/media/image_sequence_source.h"
#include "test_harness.h"

using aurora::Image;
using aurora::ImageSequenceSource;

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""us;

namespace {

// 生成一张纯色 RGBA8 帧。
auto solid_frame(int w, const int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> Image {
    std::vector<std::uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U, 0U);
    for (size_t i = 0; i < px.size(); i += 4U) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i] = r;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i + 1U] = g;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i + 2U] = b;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        px[i + 3U] = 255U;
    }
    return Image{.width = w, .height = h, .pixels = std::move(px)};
}

}  // namespace

AURORA_TEST() {
    ImageSequenceSource src;
    src.set_fps(2.0);  // 2 fps → 每帧 0.5s
    src.set_frames({solid_frame(4, 4, 255, 0, 0), solid_frame(4, 4, 0, 255, 0), solid_frame(4, 4, 0, 0, 255)});

    AURORA_TEST_CHECK(src.has_video());
    AURORA_TEST_CHECK(!src.has_audio());
    AURORA_TEST_CHECK(src.frame_count() == 3U);
    AURORA_TEST_CHECK(src.natural_size().width == 4.0F && src.natural_size().height == 4.0F);
    AURORA_TEST_CHECK(src.duration() == 1500000us);  // 3 帧 @ 2fps = 1.5s

    // frame_at 取样
    auto f0 = src.frame_at(0us);
    AURORA_TEST_CHECK(static_cast<bool>(f0));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(f0.value().image.pixels[0] == 255);

    auto f_mid = src.frame_at(600000us);  // 0.6s → 第 1 帧
    AURORA_TEST_CHECK(static_cast<bool>(f_mid));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(f_mid.value().image.pixels[1] == 255);

    auto f_last = src.frame_at(2000000us);  // 超出末尾 → 钳制到末帧
    AURORA_TEST_CHECK(static_cast<bool>(f_last));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(f_last.value().image.pixels[2] == 255);

    // seek
    src.seek(500000us);
    AURORA_TEST_CHECK(src.position() == 500000us);
    auto f_seek = src.frame_at(src.position());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    AURORA_TEST_CHECK(f_seek.value().image.pixels[1] == 255);

    // play / pause
    src.play();
    AURORA_TEST_CHECK(src.is_playing());
    src.pause();
    AURORA_TEST_CHECK(!src.is_playing());

    // open 空 URI 应返回错误
    auto bad = ImageSequenceSource{}.open("");
    AURORA_TEST_CHECK(!bad);
}
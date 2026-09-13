/// 测试类型: unit
/// 目标单元: include/aurora/media/image_sequence_source.h
/// 测试说明: 覆盖图片序列源的默认不变量、帧管理、fps 钳制、时长/自然尺寸推导、
/// seek 与 frame_at 的钳位及索引映射、open() 的 URI 解析与错误传播、close 清空、
/// 音频轨缺席（no-op）

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include "aurora/media/image_sequence_source.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_image_sequence_source {

namespace {

/// @brief 构造指定尺寸、统一填充色 RGBA 图像。
auto solid_image(int w, int h, std::uint8_t v) -> Image {
    return Image{.width = w, .height = h, .pixels = std::vector<std::uint8_t>(static_cast<size_t>(w * h * 4), v)};
}

/// @brief 构造最小 2×2 未压缩 24 位 BMP（与 utest_image 相同布局）。
auto make_2x2_bmp() -> std::vector<std::uint8_t> {
    return {
        'B', 'M', 70U, 0U, 0U,   0U, 0U,  0U,   0U,   0U,   54U, 0U, 0U,  0U, 40U,  0U, 0U,   0U, 2U, 0U, 0U, 0U,
        2U,  0U,  0U,  0U, 1U,   0U, 24U, 0U,   0U,   0U,   0U,  0U, 16U, 0U, 0U,   0U, 0U,   0U, 0U, 0U, 0U, 0U,
        0U,  0U,  0U,  0U, 255U, 0U, 0U,  255U, 255U, 255U, 0U,  0U, 0U,  0U, 255U, 0U, 255U, 0U, 0U, 0U,
    };
}

auto write_temp_file(const std::string& file_name, const std::vector<std::uint8_t>& bytes) -> std::filesystem::path {
    const auto dir = std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_image_sequence";
    std::filesystem::create_directories(dir);
    const auto file = dir / file_name;
    std::ofstream out{file, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(*-pro-type-reinterpret-cast)
              static_cast<std::streamsize>(bytes.size()));
    return file;
}

auto us(long long v) -> std::chrono::microseconds { return std::chrono::microseconds{v}; }

}  // namespace

AURORA_TEST_CASE(default_source_is_empty_and_silent) {
    const ImageSequenceSource s;
    AURORA_TEST_CHECK_NEAR(s.fps(), 24.0, 1e-9);
    AURORA_TEST_CHECK_EQ(s.frame_count(), 0U);
    AURORA_TEST_CHECK_FALSE(s.has_video());
    AURORA_TEST_CHECK_FALSE(s.has_audio());  // 序列源无音频轨道
    const Size nat = s.natural_size();
    AURORA_TEST_CHECK_NEAR(nat.width, 0.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(nat.height, 0.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(s.duration().count(), 0);
    AURORA_TEST_CHECK_FALSE(s.is_playing());
}

AURORA_TEST_CASE(frame_management_and_natural_size_from_first) {
    ImageSequenceSource s;
    s.append_frame(solid_image(4, 3, 1));
    s.append_frame(solid_image(8, 6, 2));
    AURORA_TEST_CHECK_EQ(s.frame_count(), 2U);
    AURORA_TEST_CHECK_TRUE(s.has_video());
    // 自然尺寸取第一帧。
    const Size nat = s.natural_size();
    AURORA_TEST_CHECK_NEAR(nat.width, 4.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(nat.height, 3.0F, 0.0F);
    // set_frames 整体替换。
    s.set_frames(std::vector{solid_image(10, 5, 3)});
    AURORA_TEST_CHECK_EQ(s.frame_count(), 1U);
    AURORA_TEST_CHECK_NEAR(s.natural_size().width, 10.0F, 0.0F);
}

AURORA_TEST_CASE(set_fps_rejects_non_positive) {
    ImageSequenceSource s;
    s.set_fps(30.0);
    AURORA_TEST_CHECK_NEAR(s.fps(), 30.0, 1e-9);
    s.set_fps(0.0);
    AURORA_TEST_CHECK_NEAR(s.fps(), 30.0, 1e-9);
    s.set_fps(-5.0);
    AURORA_TEST_CHECK_NEAR(s.fps(), 30.0, 1e-9);
}

AURORA_TEST_CASE(duration_derived_from_frames_and_fps) {
    ImageSequenceSource s;
    s.set_fps(10.0);
    for (int i = 0; i < 5; ++i) {
        s.append_frame(solid_image(2, 2, static_cast<std::uint8_t>(i)));
    }
    // 5 帧 / 10fps = 0.5s = 500'000us。
    AURORA_TEST_CHECK_EQ(s.duration().count(), 500'000);
}

AURORA_TEST_CASE(seek_clamps_into_zero_duration_range) {
    ImageSequenceSource s;
    s.set_fps(10.0);
    s.append_frame(solid_image(2, 2, 0));  // 时长 100ms
    s.seek(us(-5));
    AURORA_TEST_CHECK_EQ(s.position().count(), 0);
    s.seek(us(1'000'000));
    AURORA_TEST_CHECK_EQ(s.position().count(), 100'000);
    s.seek(us(40'000));
    AURORA_TEST_CHECK_EQ(s.position().count(), 40'000);
}

AURORA_TEST_CASE(frame_at_maps_time_to_index_with_pts) {
    ImageSequenceSource s;
    s.set_fps(10.0);  // 每帧 100ms
    s.append_frame(solid_image(1, 1, 10));
    s.append_frame(solid_image(1, 1, 20));
    s.append_frame(solid_image(1, 1, 30));

    // 0ms → idx 0，pts = 0。
    auto r = s.frame_at(us(0));
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().image.pixels[0], 10);
    AURORA_TEST_CHECK_EQ(r.value().pts.count(), 0);
    // 150ms → idx 1，pts = 100ms。
    r = s.frame_at(us(150'000));
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().image.pixels[0], 20);
    AURORA_TEST_CHECK_EQ(r.value().pts.count(), 100'000);
    // 999ms → idx 2，pts = 200ms。
    r = s.frame_at(us(999'000));
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().image.pixels[0], 30);
    AURORA_TEST_CHECK_EQ(r.value().pts.count(), 200'000);
    // 超界钳到最后一帧。
    r = s.frame_at(us(50'000'000));
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().image.pixels[0], 30);
    // 负值钳到第 0 帧。
    r = s.frame_at(us(-1));
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().image.pixels[0], 10);
}

AURORA_TEST_CASE(frame_at_empty_source_errors) {
    ImageSequenceSource s;
    const auto r = s.frame_at(us(0));
    AURORA_TEST_CHECK_FALSE(r.ok());
}

AURORA_TEST_CASE(play_pause_and_audio_noops) {
    ImageSequenceSource s;
    s.play();
    AURORA_TEST_CHECK_TRUE(s.is_playing());
    s.pause();
    AURORA_TEST_CHECK_FALSE(s.is_playing());
    // 音频方法 no-op（has_audio()==false 的契约配套）。
    AURORA_TEST_CHECK_NO_THROW(s.set_volume(0.3));
    AURORA_TEST_CHECK_NO_THROW(s.set_muted(true));
}

AURORA_TEST_CASE(open_empty_uri_returns_error) {
    ImageSequenceSource s;
    const auto r = s.open("");
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_FALSE(s.has_video());
}

AURORA_TEST_CASE(open_propagates_load_error) {
    ImageSequenceSource s;
    const auto r = s.open("Z:/definitely/missing/dir/frame.bmp");
    AURORA_TEST_CHECK_FALSE(r.ok());
    AURORA_TEST_CHECK_FALSE(s.has_video());
}

AURORA_TEST_CASE(open_single_path_loads_single_frame) {
    const auto file = write_temp_file("one.bmp", make_2x2_bmp());
    ImageSequenceSource s;
    const auto r = s.open(file.string());
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_TRUE(r.value());
    AURORA_TEST_CHECK_EQ(s.frame_count(), 1U);
    AURORA_TEST_CHECK_NEAR(s.natural_size().width, 2.0F, 0.0F);
    AURORA_TEST_CHECK_EQ(s.position().count(), 0);
    std::filesystem::remove_all(std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_image_sequence");
}

AURORA_TEST_CASE(open_semicolon_separated_paths_load_all) {
    const auto dir = std::filesystem::path{aurora::testing::isolation::temp_dir()} / "aurora_utest_image_sequence";
    std::filesystem::create_directories(dir);
    const auto f1 = dir / "a.bmp";
    const auto f2 = dir / "b.bmp";
    {
        std::ofstream o1{f1, std::ios::binary};
        std::ofstream o2{f2, std::ios::binary};
        const auto bmp = make_2x2_bmp();
        o1.write(reinterpret_cast<const char*>(bmp.data()),  // NOLINT(*-pro-type-reinterpret-cast)
                 static_cast<std::streamsize>(bmp.size()));
        o2.write(reinterpret_cast<const char*>(bmp.data()),  // NOLINT(*-pro-type-reinterpret-cast)
                 static_cast<std::streamsize>(bmp.size()));
    }
    ImageSequenceSource s;
    const auto r = s.open(f1.string() + ";" + f2.string());
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(s.frame_count(), 2U);
    // `|` 分隔符等价。
    ImageSequenceSource s2;
    const auto r2 = s2.open(f1.string() + "|" + f2.string());
    AURORA_TEST_REQUIRE(r2.ok());
    AURORA_TEST_CHECK_EQ(s2.frame_count(), 2U);
    std::filesystem::remove_all(dir);
}

AURORA_TEST_CASE(close_clears_frames) {
    ImageSequenceSource s;
    s.append_frame(solid_image(2, 2, 0));
    s.play();
    s.close();
    AURORA_TEST_CHECK_EQ(s.frame_count(), 0U);
    AURORA_TEST_CHECK_FALSE(s.has_video());
}

}  // namespace aurora::test_cases::utest_image_sequence_source

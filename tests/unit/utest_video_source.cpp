/// 测试类型: unit
/// 目标单元: include/aurora/media/video_source.h
/// 测试说明: 覆盖 VideoFrame 默认值、VideoSource 拉模型接口契约（自定义最小源）、
/// 推模型回调默认空实现（可安全注册）、AudioSink 抽象的 fake 实现回放

#include <span>

#include "aurora/media/video_source.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_video_source {

namespace {

/// 最小自定义源：拉模型，返回固定帧；记录音量/静音与播放状态调用。
class StubSource final : public VideoSource {
  public:
    auto open(std::string_view uri) -> Result<bool> override {
        opened_uri = std::string(uri);
        return true;
    }
    auto close() -> void override { closed = true; }
    [[nodiscard]] auto has_video() const -> bool override { return true; }
    [[nodiscard]] auto has_audio() const -> bool override { return false; }
    [[nodiscard]] auto natural_size() const -> Size override { return Size{.width = 8.0F, .height = 6.0F}; }
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override {
        return std::chrono::milliseconds{1'000};
    }
    auto play() -> void override { playing = true; }
    auto pause() -> void override { playing = false; }
    [[nodiscard]] auto is_playing() const -> bool override { return playing; }
    auto seek(std::chrono::microseconds pos) -> void override { last_seek = pos; }
    [[nodiscard]] auto position() const -> std::chrono::microseconds override { return last_seek; }
    auto set_volume(double v) -> void override { last_volume = v; }
    auto set_muted(bool m) -> void override { muted = m; }
    [[nodiscard]] auto frame_at(std::chrono::microseconds pos) -> Result<VideoFrame> override {
        return VideoFrame{.image = Image{.width = 2, .height = 2, .pixels = std::vector<std::uint8_t>(16, 7)},
                          .pts = pos};
    }

    std::string opened_uri;
    bool closed = false;
    bool playing = false;
    bool muted = false;
    double last_volume = -1.0;
    std::chrono::microseconds last_seek{0};
};

/// AudioSink 的 fake：记录写入的样本与参数。
class FakeSink final : public AudioSink {
  public:
    auto play_samples(std::span<const std::int16_t> pcm, int sample_rate, int channels) -> void override {
        calls += 1;
        samples.insert(samples.end(), pcm.begin(), pcm.end());
        this->sample_rate = sample_rate;
        this->channels = channels;
    }
    auto set_volume(double v) -> void override { volume = v; }
    auto set_muted(bool m) -> void override { muted = m; }

    int calls = 0;
    std::vector<std::int16_t> samples;
    int sample_rate = 0;
    int channels = 0;
    double volume = 1.0;
    bool muted = false;
};

}  // namespace

AURORA_TEST_CASE(video_frame_defaults) {
    const VideoFrame f;
    AURORA_TEST_CHECK_EQ(f.image.width, 0);
    AURORA_TEST_CHECK_EQ(f.image.height, 0);
    AURORA_TEST_CHECK_EQ(f.pts.count(), 0);
}

AURORA_TEST_CASE(stub_source_open_close_and_tracks_state) {
    StubSource s;
    AURORA_TEST_CHECK_FALSE(s.is_playing());
    s.play();
    AURORA_TEST_CHECK_TRUE(s.is_playing());
    s.pause();
    AURORA_TEST_CHECK_FALSE(s.is_playing());

    const auto r = s.open("stub://demo");
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_TRUE(r.value());
    AURORA_TEST_CHECK_EQ(s.opened_uri, "stub://demo");
    s.close();
    AURORA_TEST_CHECK_TRUE(s.closed);
}

AURORA_TEST_CASE(stub_source_volume_mute_seek_natural) {
    StubSource s;
    s.set_volume(0.5);
    AURORA_TEST_CHECK_NEAR(s.last_volume, 0.5, 1e-9);
    s.set_muted(true);
    AURORA_TEST_CHECK_TRUE(s.muted);

    const auto dur = s.duration();
    AURORA_TEST_CHECK_EQ(dur.count(), 1'000'000);
    const auto pos = std::chrono::microseconds{250'000};
    s.seek(pos);
    AURORA_TEST_CHECK_EQ(s.position().count(), 250'000);

    const Size nat = s.natural_size();
    AURORA_TEST_CHECK_NEAR(nat.width, 8.0F, 0.0F);
    AURORA_TEST_CHECK_NEAR(nat.height, 6.0F, 0.0F);
}

AURORA_TEST_CASE(stub_source_frame_at_returns_pts) {
    StubSource s;
    const auto pos = std::chrono::microseconds{125'000};
    const auto r = s.frame_at(pos);
    AURORA_TEST_REQUIRE(r.ok());
    AURORA_TEST_CHECK_EQ(r.value().pts.count(), pos.count());
    AURORA_TEST_CHECK_EQ(r.value().image.width, 2);
    AURORA_TEST_CHECK_EQ(r.value().image.pixels.size(), 16U);
    AURORA_TEST_CHECK_EQ(r.value().image.pixels[0], 7);
}

AURORA_TEST_CASE(push_model_callbacks_default_noop) {
    // 基类推模型回调为默认空实现：注册不崩溃、不触发。
    StubSource s;
    int frames = 0;
    int state_changes = 0;
    int audio_calls = 0;
    s.set_frame_callback([&frames](const VideoFrame&) -> void { ++frames; });
    s.set_state_callback([&state_changes](bool) -> void { ++state_changes; });
    s.set_audio_callback([&audio_calls](std::span<const std::int16_t>, int, int) -> void { ++audio_calls; });
    AURORA_TEST_CHECK_EQ(frames, 0);
    AURORA_TEST_CHECK_EQ(state_changes, 0);
    AURORA_TEST_CHECK_EQ(audio_calls, 0);
}

AURORA_TEST_CASE(audio_sink_fake_records_pcm_stream) {
    FakeSink sink;
    const std::vector<std::int16_t> pcm{100, -200, 300};
    sink.play_samples(std::span<const std::int16_t>(pcm), 44'100, 2);
    AURORA_TEST_CHECK_EQ(sink.calls, 1);
    AURORA_TEST_REQUIRE_EQ(sink.samples.size(), 3U);
    AURORA_TEST_CHECK_EQ(sink.samples[1], -200);
    AURORA_TEST_CHECK_EQ(sink.sample_rate, 44'100);
    AURORA_TEST_CHECK_EQ(sink.channels, 2);
    sink.set_volume(0.25);
    AURORA_TEST_CHECK_NEAR(sink.volume, 0.25, 1e-9);
    sink.set_muted(true);
    AURORA_TEST_CHECK_TRUE(sink.muted);
}

}  // namespace aurora::test_cases::utest_video_source

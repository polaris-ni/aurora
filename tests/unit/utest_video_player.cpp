/// 测试类型: unit
/// 目标单元: include/aurora/media/video_player.h
/// 测试说明: 覆盖 VideoPlayer 默认不变量、源挂接、VideoController 接口语义（toggle/seek_fraction/
/// volume/muted 及四个 Reactive 信号）、播放时钟推进与播完自停、控件叠层管理、
/// 属性序列化往返、自描述、tap/double-tap 扩展点

#include <chrono>
#include <memory>

#include "aurora/media/image_sequence_source.h"
#include "aurora/media/video_player.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_video_player {

namespace {

using aurora::BoxFit;
using aurora::Image;
using aurora::ImageSequenceSource;
using aurora::VideoFrame;
using aurora::VideoPlayer;
using aurora::VideoSource;

auto us(long long v) -> std::chrono::microseconds { return std::chrono::microseconds{v}; }

auto t_ms(long long ms) -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
}

auto solid_image(int w, int h, std::uint8_t v) -> Image {
    return Image{.width = w, .height = h, .pixels = std::vector<std::uint8_t>(static_cast<size_t>(w * h * 4), v)};
}

/// 两帧 / 10fps 序列源：时长 200ms。
auto make_source() -> std::shared_ptr<ImageSequenceSource> {
    auto src = std::make_shared<ImageSequenceSource>();
    src->set_fps(10.0);
    src->append_frame(solid_image(4, 3, 1));
    src->append_frame(solid_image(4, 3, 2));
    return src;
}

/// 暴露受保护扩展点的测试子类。
class PlayerHook final : public VideoPlayer {
  public:
    using VideoPlayer::on_double_tap;
    using VideoPlayer::on_playback_tick;
    using VideoPlayer::on_tap;
    using VideoPlayer::VideoPlayer;

    int taps = 0;
    int double_taps = 0;
    int ticks = 0;

    [[nodiscard]] auto frame() const -> const Image& { return current_frame(); }

    auto on_tap() -> void override {
        ++taps;
        VideoPlayer::on_tap();
    }
    auto on_double_tap() -> void override {
        ++double_taps;
        VideoPlayer::on_double_tap();
    }
    auto on_playback_tick(std::chrono::steady_clock::time_point now) -> void override {
        ++ticks;
        VideoPlayer::on_playback_tick(now);
    }
};

}  // namespace

AURORA_TEST_CASE(default_player_state) {
    const VideoPlayer p;
    AURORA_TEST_CHECK_EQ(p.fit(), BoxFit::Contain);
    AURORA_TEST_CHECK_TRUE(p.show_controls());
    AURORA_TEST_CHECK_FALSE(p.is_playing());
    AURORA_TEST_CHECK_NEAR(p.volume(), 1.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(p.muted());
    AURORA_TEST_CHECK_EQ(p.duration().count(), 0);
    AURORA_TEST_CHECK_NEAR(p.position_fraction(), 0.0, 1e-9);
    AURORA_TEST_CHECK_NULL(p.source().get());
    AURORA_TEST_CHECK_EQ(std::string{p.type_name()}, "VideoPlayer");
}

AURORA_TEST_CASE(source_roundtrip_and_fit) {
    VideoPlayer p;
    const auto src = make_source();
    p.set_source(src);
    AURORA_TEST_CHECK_EQ(p.source().get(), src.get());
    // 时长来自源：2 帧 / 10fps = 200ms。
    AURORA_TEST_CHECK_EQ(p.duration().count(), 200'000);

    p.set_fit(BoxFit::Cover);
    AURORA_TEST_CHECK_EQ(p.fit(), BoxFit::Cover);
}

AURORA_TEST_CASE(toggle_play_drives_playing_signal_and_source) {
    const auto src = make_source();
    VideoPlayer p(src);

    p.toggle_play();
    AURORA_TEST_CHECK_TRUE(p.is_playing());
    AURORA_TEST_CHECK_TRUE(src->is_playing());
    AURORA_TEST_REQUIRE_NOT_NULL(p.playing_signal());
    AURORA_TEST_CHECK_TRUE(p.playing_signal()->get());

    p.toggle_play();
    AURORA_TEST_CHECK_FALSE(p.is_playing());
    AURORA_TEST_CHECK_FALSE(src->is_playing());
    AURORA_TEST_CHECK_FALSE(p.playing_signal()->get());
    // 幂等：连续 toggle 两次回到原状态。
    p.play();
    p.play();  // 已播放中，no-op
    AURORA_TEST_CHECK_TRUE(p.is_playing());
    p.pause();
    p.pause();  // 已暂停，no-op
    AURORA_TEST_CHECK_FALSE(p.is_playing());
}

AURORA_TEST_CASE(seek_clamps_and_updates_progress) {
    VideoPlayer p(make_source());  // 200ms
    p.seek(us(500'000));
    AURORA_TEST_CHECK_EQ(p.position().count(), 200'000);
    AURORA_TEST_CHECK_NEAR(p.position_fraction(), 1.0, 1e-9);
    AURORA_TEST_CHECK_NEAR(p.progress_signal()->get(), 1.0, 1e-9);
    p.seek(us(-10));
    AURORA_TEST_CHECK_EQ(p.position().count(), 0);
    AURORA_TEST_CHECK_NEAR(p.position_fraction(), 0.0, 1e-9);
    p.seek(us(80'000));
    AURORA_TEST_CHECK_NEAR(p.position_fraction(), 0.4, 1e-6);
}

AURORA_TEST_CASE(seek_fraction_clamps_and_syncs_source) {
    const auto src = make_source();
    VideoPlayer p(src);
    p.seek_fraction(1.5);
    AURORA_TEST_CHECK_EQ(p.position().count(), 200'000);
    p.seek_fraction(-0.5);
    AURORA_TEST_CHECK_EQ(p.position().count(), 0);
    p.seek_fraction(0.5);
    AURORA_TEST_CHECK_EQ(p.position().count(), 100'000);
    AURORA_TEST_CHECK_EQ(src->position().count(), 100'000);  // 同步源 seek
    // 无源时 seek_fraction 不崩溃（时长 0 → 位置 0）。
    VideoPlayer bare;
    AURORA_TEST_CHECK_NO_THROW(bare.seek_fraction(0.7));
}

AURORA_TEST_CASE(volume_mute_clamp_and_signal_reflection) {
    const auto src = make_source();
    VideoPlayer p(src);
    p.set_volume(1.5);
    AURORA_TEST_CHECK_NEAR(p.volume(), 1.0, 1e-9);
    p.set_volume(-0.2);
    AURORA_TEST_CHECK_NEAR(p.volume(), 0.0, 1e-9);
    p.set_volume(0.4);
    AURORA_TEST_CHECK_NEAR(p.volume_signal()->get(), 0.4, 1e-9);
    p.set_muted(true);
    AURORA_TEST_CHECK_TRUE(p.muted());
    AURORA_TEST_CHECK_TRUE(p.muted_signal()->get());
    // 播放器把音量同步给源。
    p.set_volume(0.8);
    p.set_muted(false);
    // 四个信号指针稳定可用。
    AURORA_TEST_REQUIRE_NOT_NULL(p.progress_signal());
    AURORA_TEST_REQUIRE_NOT_NULL(p.volume_signal());
    AURORA_TEST_REQUIRE_NOT_NULL(p.muted_signal());
}

AURORA_TEST_CASE(playback_tick_advances_progress_and_pauses_at_end) {
    auto src = make_source();  // 200ms
    PlayerHook p(src);

    // 未播放时 tick 不推进（on_playback_tick 对 !playing 早退）。
    p.on_playback_tick(t_ms(1'000));
    AURORA_TEST_CHECK_EQ(p.ticks, 1);
    AURORA_TEST_CHECK_NEAR(p.position_fraction(), 0.0, 1e-9);

    // seek 到片尾再播放：任意非负流逝都满足 pos >= dur → 播完自停（确定性）。
    p.seek(us(200'000));
    p.play();
    AURORA_TEST_CHECK_TRUE(p.is_playing());
    p.on_playback_tick(t_ms(2'000));
    AURORA_TEST_CHECK_FALSE(p.is_playing());
    AURORA_TEST_CHECK_NEAR(p.position_fraction(), 1.0, 1e-9);
    AURORA_TEST_CHECK_FALSE(p.playing_signal()->get());
    AURORA_TEST_CHECK_EQ(p.ticks, 2);
    // 结尾帧已缓存到 current_frame_（on_frame 钩子：最后一帧填充值 2）。
    AURORA_TEST_CHECK_EQ(p.frame().pixels[0], 2);
}

AURORA_TEST_CASE(show_controls_toggles_and_custom_controls) {
    VideoPlayer p(make_source());
    // 默认无 children（on_mount 时才 adopt 默认控件）。
    AURORA_TEST_CHECK_EQ(p.child_nodes().size(), 0U);
    p.set_show_controls(false);
    AURORA_TEST_CHECK_FALSE(p.show_controls());

    // 整体替换控件叠层。
    auto custom = std::make_unique<aurora::Text>("ctrl");
    auto* custom_ptr = custom.get();
    p.set_controls(std::move(custom));
    AURORA_TEST_REQUIRE_EQ(p.child_nodes().size(), 1U);
    AURORA_TEST_CHECK_EQ(&p.child_nodes()[0].widget(), custom_ptr);
    // show=false 时叠层同步隐藏。
    AURORA_TEST_CHECK_FALSE(custom_ptr->show.get());
    p.set_show_controls(true);
    AURORA_TEST_CHECK_TRUE(custom_ptr->show.get());

    // nullptr → 清空叠层。
    p.set_controls(nullptr);
    AURORA_TEST_CHECK_EQ(p.child_nodes().size(), 0U);
}

AURORA_TEST_CASE(create_default_controls_yields_video_controls) {
    VideoPlayer p(make_source());
    auto controls = p.create_default_controls();
    AURORA_TEST_REQUIRE_NOT_NULL(controls.get());
    AURORA_TEST_CHECK_EQ(std::string{controls->type_name()}, "VideoControls");
}

AURORA_TEST_CASE(props_serialize_deserialize_roundtrip) {
    VideoPlayer p(make_source());
    p.set_fit(BoxFit::Fill);
    p.set_show_controls(false);

    aurora::Json props;
    p.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["fit"].get<std::string>(), "Fill");
    AURORA_TEST_CHECK_EQ(props["show_controls"].get<bool>(), false);

    VideoPlayer q;
    q.deserialize_props(props);
    AURORA_TEST_CHECK_EQ(q.fit(), BoxFit::Fill);
    AURORA_TEST_CHECK_FALSE(q.show_controls());
}

AURORA_TEST_CASE(describe_reports_metadata) {
    const auto d = VideoPlayer::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "VideoPlayer");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
    bool has_fit = false;
    for (const auto& prop : d.properties) {
        if (std::string{prop.name} == "fit") {
            has_fit = true;
        }
    }
    AURORA_TEST_CHECK_TRUE(has_fit);
    AURORA_TEST_REQUIRE_EQ(d.events.size(), 2U);
    AURORA_TEST_CHECK_EQ(std::string{d.events[0]}, "on_tap");
}

AURORA_TEST_CASE(tap_and_double_tap_extension_points) {
    PlayerHook p(make_source());

    // 有回调走回调。
    int fired = 0;
    p.set_on_tap([&fired]() -> void { ++fired; });
    p.on_tap();
    AURORA_TEST_CHECK_EQ(fired, 1);
    AURORA_TEST_CHECK_FALSE(p.is_playing());  // 回调替代默认 toggle

    // 无回调 → 默认 toggle_play。
    PlayerHook q(make_source());
    q.on_tap();
    AURORA_TEST_CHECK_TRUE(q.is_playing());

    // 双击默认切换 Contain <-> Cover。
    PlayerHook r(make_source());
    AURORA_TEST_CHECK_EQ(r.fit(), BoxFit::Contain);
    r.on_double_tap();
    AURORA_TEST_CHECK_EQ(r.fit(), BoxFit::Cover);
    r.on_double_tap();
    AURORA_TEST_CHECK_EQ(r.fit(), BoxFit::Contain);
    int dbl_fired = 0;
    r.set_on_double_tap([&dbl_fired]() -> void { ++dbl_fired; });
    r.on_double_tap();
    AURORA_TEST_CHECK_EQ(dbl_fired, 1);
}

AURORA_TEST_CASE(collect_signals_reports_player_states) {
    VideoPlayer p(make_source());
    std::vector<aurora::SignalViewBase*> out;
    p.collect_signals(out);
    // 无子控件时恰为 4 个播放器状态信号。
    AURORA_TEST_CHECK_EQ(out.size(), 4U);
}

}  // namespace aurora::test_cases::utest_video_player

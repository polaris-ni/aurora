/// 测试类型: unit
/// 目标单元: include/aurora/media/video_controls.h
/// 测试说明: 覆盖 VideoControls 控件叠层——构造建立 Row 子控件结构（播放/进度/时间/静音/音量）、
/// 按钮点击经 VideoController 操纵播放器、tick_gestures 刷新时间文本与按钮文案、
/// 无控制器降级构造、自描述与属性序列化

#include <chrono>
#include <memory>

#include "aurora/media/image_sequence_source.h"
#include "aurora/media/video_controls.h"
#include "aurora/media/video_player.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_video_controls {

namespace {

using aurora::Button;
using aurora::Image;
using aurora::LocalizedString;
using aurora::Node;
using aurora::Row;
using aurora::Slider;
using aurora::Text;
using aurora::VideoController;
using aurora::VideoControls;
using aurora::VideoPlayer;

auto t_ms(long long ms) -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds{ms}};
}

auto solid_image(int w, int h, std::uint8_t v) -> Image {
    return Image{.width = w, .height = h, .pixels = std::vector<std::uint8_t>(static_cast<size_t>(w * h * 4), v)};
}

/// 6 帧 @ 0.1fps = 60s 时长（便于验证 mm:ss 时间格式化）。
auto make_long_source() -> std::shared_ptr<aurora::ImageSequenceSource> {
    auto src = std::make_shared<aurora::ImageSequenceSource>();
    src->set_fps(0.1);
    for (int i = 0; i < 6; ++i) {
        src->append_frame(solid_image(4, 3, static_cast<std::uint8_t>(i + 1)));
    }
    return src;
}

/// 暴露受保护成员与 tick_gestures 的测试子类。
class ControlsHook final : public VideoControls {
  public:
    explicit ControlsHook(VideoController* c) : VideoControls(c) {}
    using VideoControls::mute_button;
    using VideoControls::play_button;
    using VideoControls::tick_gestures;
    using VideoControls::time_text;
};

/// 从叠层 Row 中取第 i 个子控件（结构契约：[Button, Slider, Text, Button, Slider]）。
auto row_child(const VideoControls& c, size_t i) -> const aurora::Widget& {
    const auto& row = dynamic_cast<const Row&>(c.child_nodes()[0].widget());
    return row.child_nodes()[i].widget();
}

}  // namespace

AURORA_TEST_CASE(type_and_descriptor_metadata) {
    const VideoControls c;
    AURORA_TEST_CHECK_EQ(std::string{c.type_name()}, "VideoControls");
    const auto d = VideoControls::describe_static();
    AURORA_TEST_CHECK_EQ(std::string{d.name}, "VideoControls");
    AURORA_TEST_CHECK_EQ(std::string{d.children_policy}, "single");
}

AURORA_TEST_CASE(builds_row_with_five_children_in_order) {
    VideoPlayer player;
    VideoControls c(&player);
    AURORA_TEST_REQUIRE_EQ(c.child_nodes().size(), 1U);
    AURORA_TEST_REQUIRE_EQ(c.child_nodes()[0].widget().child_nodes().size(), 5U);
    // 结构契约：[播放按钮, 进度条, 时间文本, 静音按钮, 音量条]。
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Button*>(&row_child(c, 0)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Slider*>(&row_child(c, 1)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Text*>(&row_child(c, 2)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Button*>(&row_child(c, 3)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Slider*>(&row_child(c, 4)));
}

AURORA_TEST_CASE(play_button_click_toggles_controller) {
    VideoPlayer player;
    ControlsHook c(&player);
    AURORA_TEST_CHECK_FALSE(player.is_playing());
    c.play_button()->on_click();
    AURORA_TEST_CHECK_TRUE(player.is_playing());
    c.play_button()->on_click();
    AURORA_TEST_CHECK_FALSE(player.is_playing());
}

AURORA_TEST_CASE(mute_button_click_toggles_muted) {
    VideoPlayer player;
    ControlsHook c(&player);
    AURORA_TEST_CHECK_FALSE(player.muted());
    c.mute_button()->on_click();
    AURORA_TEST_CHECK_TRUE(player.muted());
    c.mute_button()->on_click();
    AURORA_TEST_CHECK_FALSE(player.muted());
}

AURORA_TEST_CASE(tick_refreshes_playback_labels) {
    VideoPlayer player;
    ControlsHook c(&player);
    // 初始：未播放 → "Play"；未静音 → "Mute"。
    c.tick_gestures(t_ms(0));
    AURORA_TEST_CHECK_TRUE(c.play_button()->label.get() == LocalizedString{"Play"});
    AURORA_TEST_CHECK_TRUE(c.mute_button()->label.get() == LocalizedString{"Mute"});
    // 播放 + 静音后 tick 反转文案。
    player.toggle_play();
    player.set_muted(true);
    c.tick_gestures(t_ms(16));
    AURORA_TEST_CHECK_TRUE(c.play_button()->label.get() == LocalizedString{"Pause"});
    AURORA_TEST_CHECK_TRUE(c.mute_button()->label.get() == LocalizedString{"Unmute"});
}

AURORA_TEST_CASE(tick_updates_time_text_from_controller) {
    VideoPlayer player(make_long_source());  // 60s
    ControlsHook c(&player);
    player.seek_fraction(0.5);  // 30s
    c.tick_gestures(t_ms(0));
    AURORA_TEST_CHECK_EQ(c.time_text()->content.get().text, "0:30 / 1:00");
    player.seek_fraction(0.0);
    c.tick_gestures(t_ms(0));
    AURORA_TEST_CHECK_EQ(c.time_text()->content.get().text, "0:00 / 1:00");
}

AURORA_TEST_CASE(constructs_without_controller_no_crash) {
    // 默认构造不建子控件（build_children 仅在控制器构造时执行）；
    // 独立反序列化场景下序列化仍可用。
    VideoControls c;
    AURORA_TEST_CHECK_EQ(c.child_nodes().size(), 0U);
    aurora::Json props;
    AURORA_TEST_CHECK_NO_THROW(c.serialize_props(props));
}

AURORA_TEST_CASE(props_roundtrip_generic_fields) {
    VideoControls c;
    aurora::Json props;
    c.serialize_props(props);
    AURORA_TEST_CHECK_EQ(props["show"].get<bool>(), true);
    props["show"] = false;
    VideoControls d;
    d.deserialize_props(props);
    AURORA_TEST_CHECK_FALSE(d.show.get());
}

}  // namespace aurora::test_cases::utest_video_controls

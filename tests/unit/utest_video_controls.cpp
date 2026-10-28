/// 测试类型: unit
/// 目标单元: include/aurora/media/video_controls.h
/// 测试说明: 覆盖 VideoControls 控件叠层——构造建立 Row 子控件结构（播放/进度/时间/静音/音量）、
/// 按钮点击经 VideoController 操纵播放器、tick_gestures 刷新时间文本与按钮文案、
/// 公开 tick 入口对叠层读数的驱动、无控制器降级构造、自描述与属性序列化

#include <chrono>
#include <memory>

#include "aurora/media/image_sequence_source.h"
#include "aurora/media/video_controls.h"
#include "aurora/media/video_player.h"
#include "aurora/state/state.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/props_io.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"
#include "framework/aurora_test.h"
#include "framework/json_access.h"

namespace aurora::test_cases::utest_video_controls {
using aurora::testing::require_field;

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
    explicit ControlsHook(VideoController *c) : VideoControls(c) {}
    using VideoControls::mute_button;
    using VideoControls::play_button;
    using VideoControls::tick_gestures;
    using VideoControls::time_text;
};

/// 从叠层 Row 中取第 i 个子控件（结构契约：[Button, Slider, Text, Button, Slider]）。
auto row_child(const VideoControls &c, size_t i) -> const aurora::Widget & {
    const auto &row = dynamic_cast<const Row &>(c.child_nodes()[0].widget());
    return row.child_nodes()[i].widget();
}

/// 可写版本：用于模拟真实用户输入（滑块编辑）。
auto row_child_mut(VideoControls &c, size_t i) -> aurora::Widget & {
    return c.child_nodes_mut()[0].widget().child_nodes_mut()[i].widget();
}

/// 计数型假播放器：事实读数由可控字段/来源给出，命令侧只计数——用于区分「后端回填」与「用户输入」。
class CountingController final : public VideoController {
  public:
    int toggle_calls = 0;
    int seek_calls = 0;
    int volume_calls = 0;
    int mute_calls = 0;
    bool play_state = false;
    double pos = 0.0;
    double vol = 1.0;
    bool muted_state = false;
    std::chrono::microseconds dur{std::chrono::seconds{60}};

    auto toggle_play() -> void override {
        ++toggle_calls;
        play_state = !play_state;
        playing_source_.set(play_state);
    }
    auto seek_fraction(double f) -> void override {
        ++seek_calls;
        pos = f;
        progress_source_.set(f);
    }
    auto set_volume(double v) -> void override {
        ++volume_calls;
        vol = v;
        volume_source_.set(v);
    }
    auto set_muted(bool m) -> void override {
        ++mute_calls;
        muted_state = m;
        muted_source_.set(m);
    }
    [[nodiscard]] auto is_playing() const -> bool override { return play_state; }
    [[nodiscard]] auto position_fraction() const -> double override { return pos; }
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override { return dur; }
    [[nodiscard]] auto volume() const -> double override { return vol; }
    [[nodiscard]] auto muted() const -> bool override { return muted_state; }
    [[nodiscard]] auto playing_signal() -> ReadOnlyState<bool> override { return playing_source_; }
    [[nodiscard]] auto progress_signal() -> ReadOnlyState<double> override { return progress_source_; }
    [[nodiscard]] auto volume_signal() -> ReadOnlyState<double> override { return volume_source_; }
    [[nodiscard]] auto muted_signal() -> ReadOnlyState<bool> override { return muted_source_; }

  private:
    State<bool> playing_source_{false};
    State<double> progress_source_{0.0};
    State<double> volume_source_{1.0};
    State<bool> muted_source_{false};
};

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
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Button *>(&row_child(c, 0)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Slider *>(&row_child(c, 1)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Text *>(&row_child(c, 2)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Button *>(&row_child(c, 3)));
    AURORA_TEST_CHECK_NOT_NULL(dynamic_cast<const Slider *>(&row_child(c, 4)));
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
    AURORA_TEST_CHECK_TRUE(c.play_button()->label().get() == LocalizedString{"Play"});
    AURORA_TEST_CHECK_TRUE(c.mute_button()->label().get() == LocalizedString{"Mute"});
    // 播放 + 静音后 tick 反转文案。
    player.toggle_play();
    player.set_muted(true);
    c.tick_gestures(t_ms(16));
    AURORA_TEST_CHECK_TRUE(c.play_button()->label().get() == LocalizedString{"Pause"});
    AURORA_TEST_CHECK_TRUE(c.mute_button()->label().get() == LocalizedString{"Unmute"});
}

AURORA_TEST_CASE(tick_updates_time_text_from_controller) {
    VideoPlayer player(make_long_source());  // 60s
    ControlsHook c(&player);
    player.seek_fraction(0.5);  // 30s
    c.tick_gestures(t_ms(0));
    AURORA_TEST_CHECK_EQ(c.time_text()->content().get().text, "0:30 / 1:00");
    player.seek_fraction(0.0);
    c.tick_gestures(t_ms(0));
    AURORA_TEST_CHECK_EQ(c.time_text()->content().get().text, "0:00 / 1:00");
}

AURORA_TEST_CASE(public_tick_entry_refreshes_readouts) {
    VideoPlayer player(make_long_source());  // 60s
    ControlsHook c(&player);
    player.seek_fraction(0.5);  // 30s
    player.toggle_play();
    c.tick(t_ms(0));
    // 回归点：`Widget::tick` 在 `needs_gesture_tick_` 为假时直接早退，叠层便永远停在初始
    // 「Play / 0:00 / 0:00」读数上——既有用例直调受保护 `tick_gestures`，掩盖了这个门。
    AURORA_TEST_CHECK_TRUE(c.play_button()->label().get() == LocalizedString{"Pause"});
    AURORA_TEST_CHECK_EQ(c.time_text()->content().get().text, "0:30 / 1:00");
}

AURORA_TEST_CASE(backend_facts_do_not_fire_user_commands) {
    // 事实回填与用户输入分离：后端读数推进只落展示来源，命令计数必须为零；
    // 真实滑块编辑恰发一次对应命令，之后的回填不追加命令。
    CountingController ctl;
    ControlsHook c(&ctl);

    ctl.play_state = true;
    ctl.pos = 0.4;
    ctl.vol = 0.25;
    c.tick(t_ms(0));  // 事实回填路径
    AURORA_TEST_CHECK_EQ(ctl.toggle_calls, 0);
    AURORA_TEST_CHECK_EQ(ctl.seek_calls, 0);
    AURORA_TEST_CHECK_EQ(ctl.volume_calls, 0);
    AURORA_TEST_CHECK_EQ(ctl.mute_calls, 0);
    // tick 确实走了回填：文案与时间读数已随事实更新。
    AURORA_TEST_CHECK_TRUE(c.play_button()->label().get() == LocalizedString{"Pause"});
    AURORA_TEST_CHECK_EQ(c.time_text()->content().get().text, "0:24 / 1:00");

    // 子控件类型由 VideoControls 的固定布局决定（seek / volume 行内是 Slider），按已知结构取具体类型。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)
    auto &seek = static_cast<Slider &>(row_child_mut(c, 1));
    auto &vol = static_cast<Slider &>(row_child_mut(c, 4));
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
    seek.set_value(0.5);  // 用户编辑语义：恰一次 seek
    AURORA_TEST_CHECK_EQ(ctl.seek_calls, 1);
    AURORA_TEST_CHECK_NEAR(ctl.pos, 0.5, 1e-6);
    vol.set_value(0.75);  // 恰一次 set_volume
    AURORA_TEST_CHECK_EQ(ctl.volume_calls, 1);
    AURORA_TEST_CHECK_NEAR(ctl.vol, 0.75, 1e-6);

    c.tick(t_ms(16));  // 回填不冒充用户输入：命令计数保持不变
    AURORA_TEST_CHECK_EQ(ctl.seek_calls, 1);
    AURORA_TEST_CHECK_EQ(ctl.volume_calls, 1);
}

AURORA_TEST_CASE(constructs_without_controller_no_crash) {
    // 默认构造不建子控件（build_children 仅在控制器构造时执行）；
    // 独立反序列化场景下序列化仍可用。
    VideoControls c;
    AURORA_TEST_CHECK_EQ(c.child_nodes().size(), 0U);
    aurora::Json props = aurora::Json::object();
    AURORA_TEST_CHECK_NO_THROW(c.serialize_props(props));
}

AURORA_TEST_CASE(props_roundtrip_generic_fields) {
    VideoControls c;
    aurora::Json props = aurora::Json::object();
    c.serialize_props(props);
    AURORA_TEST_CHECK_EQ(require_field<bool>(props, "show"), true);
    props.set("show", Json{false});
    VideoControls d;
    d.deserialize_props(props);
    AURORA_TEST_CHECK_FALSE(d.show());
}

}  // namespace aurora::test_cases::utest_video_controls

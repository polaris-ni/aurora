/// 测试类型: unit
/// 目标单元: include/aurora/media/audio_sink_bridge.h（+ VideoPlayer 音频接线，media/video_player.h）
/// 测试说明: AudioSinkGraphBridge 作为 AudioSink 把 PCM 路由进音频图（音量/静音经图内
/// GainNode 单点施加、析构断边、未连图丢弃不报错）；VideoPlayer::set_audio_context
/// 自动接线（回调通道接管 / 音量路由 / 解除接线断边并清空回调 / 未接线保持既有语义）。
/// 全部用 fail-start 静默上下文 + 手动 render_block 保证确定性（跨构建配置可测）。

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "aurora/media/audio_sink_bridge.h"
#include "aurora/media/video_player.h"
#include "framework/aurora_test.h"
#include "support/fake_audio.h"

namespace aurora::test_cases::utest_audio_sink_bridge {

namespace {

using aurora::testing::FakeAudioDevice;

constexpr int AURORA_AUDIO_RATE = 48000;
constexpr int AURORA_AUDIO_FRAMES = 480;
constexpr float AURORA_AUDIO_PCM_VALUE = 0.25F;

/// 静默模式上下文（设备启动失败 → direct 模式，命令即时生效，手动泵图确定可测）。
auto silent_ctx() -> std::shared_ptr<AudioContext> {
    return std::make_shared<AudioContext>(std::make_unique<FakeAudioDevice>(AURORA_AUDIO_RATE, 2, /*fail_start=*/true));
}

auto const_pcm(int frames, std::int16_t v = 8192) -> std::vector<std::int16_t> {
    return std::vector<std::int16_t>(static_cast<std::size_t>(frames) * 2U, v);
}

auto render_block(AudioContext &ctx, int frames) -> std::vector<float> {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2U);
    ctx.render_block(out.data(), frames);
    return out;
}

auto check_all_near(const std::vector<float> &v, float expected, float eps, int pairs) -> bool {
    for (int i = 0; i < pairs; ++i) {
        const float l = v[static_cast<std::size_t>(i) * 2U];
        const float r = v[static_cast<std::size_t>(i) * 2U + 1U];
        if (l < expected - eps || l > expected + eps || r < expected - eps || r > expected + eps) {
            return false;
        }
    }
    return true;
}

/// 可触发音频回调的最小源（记录回调与音量/静音转发，供播放器接线验证）。
class CallbackSource final : public VideoSource {
  public:
    auto open(std::string_view /*uri*/) -> Result<bool> override { return true; }
    auto close() -> void override {}
    [[nodiscard]] auto has_video() const -> bool override { return false; }
    [[nodiscard]] auto has_audio() const -> bool override { return true; }
    [[nodiscard]] auto natural_size() const -> Size override { return Size{}; }
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override { return {}; }
    auto play() -> void override {}
    auto pause() -> void override {}
    [[nodiscard]] auto is_playing() const -> bool override { return false; }
    auto seek(std::chrono::microseconds) -> void override {}
    [[nodiscard]] auto position() const -> std::chrono::microseconds override { return {}; }
    auto set_volume(double v) -> void override { last_volume = v; }
    auto set_muted(bool m) -> void override { last_muted = m; }
    [[nodiscard]] auto frame_at(std::chrono::microseconds) -> Result<VideoFrame> override { return VideoFrame{}; }
    auto set_audio_callback(std::function<void(std::span<const std::int16_t>, int, int)> cb) -> void override {
        audio_cb = std::move(cb);
    }

    auto emit(const std::vector<std::int16_t> &pcm, int rate, int channels) const -> void {
        if (audio_cb) {
            audio_cb(std::span<const std::int16_t>(pcm), rate, channels);
        }
    }

    std::function<void(std::span<const std::int16_t>, int, int)> audio_cb;
    double last_volume = -1.0;
    bool last_muted = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// AudioSinkGraphBridge（AudioSink 契约 → 图）
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(bridge_pushes_pcm_and_applies_gain) {
    const auto ctx = silent_ctx();
    AudioSinkGraphBridge bridge(ctx);

    AURORA_TEST_CHECK_TRUE(bridge.connected());
    AURORA_TEST_CHECK_EQ(ctx->connection_count(), 2);  // stream→gain→destination

    bridge.play_samples(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);
    auto out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, AURORA_AUDIO_PCM_VALUE, 1e-4, AURORA_AUDIO_FRAMES));

    bridge.set_volume(0.5);  // 图内 GainNode 单点施加
    bridge.play_samples(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);  // 环已排空，每次渲染前重推
    out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, AURORA_AUDIO_PCM_VALUE * 0.5F, 1e-4, AURORA_AUDIO_FRAMES));

    bridge.set_muted(true);
    bridge.play_samples(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);
    out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 1e-6, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(bridge_disconnects_on_destruction) {
    const auto ctx = silent_ctx();
    {
        AudioSinkGraphBridge bridge(ctx);
        AURORA_TEST_CHECK_EQ(ctx->connection_count(), 2);
    }
    AURORA_TEST_CHECK_EQ(ctx->connection_count(), 0);  // 析构断边，上下文仍可用
    const auto out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 1e-6, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(bridge_null_context_or_closed_is_silent_noop) {
    AudioSinkGraphBridge bridge(nullptr);  // 无上下文：不崩溃、connected=false
    AURORA_TEST_CHECK_FALSE(bridge.connected());
    bridge.play_samples(const_pcm(64), AURORA_AUDIO_RATE, 2);
    bridge.set_volume(0.5);
    bridge.set_muted(true);

    const auto ctx = silent_ctx();
    AURORA_TEST_CHECK_EQ(ctx->close().ok(), true);
    AudioSinkGraphBridge closed_bridge(ctx);  // 上下文已关闭：连接失败 → 静默
    AURORA_TEST_CHECK_FALSE(closed_bridge.connected());
    closed_bridge.play_samples(const_pcm(64), AURORA_AUDIO_RATE, 2);  // 不崩溃、样本丢弃
}

// ---------------------------------------------------------------------------
// VideoPlayer::set_audio_context（PCM 通道自动接图）
// ---------------------------------------------------------------------------

AURORA_TEST_CASE(video_player_bridge_receives_pcm_and_routes_volume) {
    const auto ctx = silent_ctx();
    const auto src = std::make_shared<CallbackSource>();
    VideoPlayer player;
    player.set_source(src);
    player.set_audio_context(ctx);

    AURORA_TEST_CHECK_TRUE(player.audio_context() == ctx);
    AURORA_TEST_CHECK_EQ(ctx->connection_count(), 2);

    src->emit(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);  // 经 set_audio_callback 通道推入
    auto out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, AURORA_AUDIO_PCM_VALUE, 1e-4, AURORA_AUDIO_FRAMES));

    player.set_volume(0.5);  // 已接图：路由到图内 GainNode（不转发源）
    src->emit(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);  // 环已排空，每次渲染前重推
    out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, AURORA_AUDIO_PCM_VALUE * 0.5F, 1e-4, AURORA_AUDIO_FRAMES));
    AURORA_TEST_CHECK_NEAR(src->last_volume, -1.0, 1e-9);  // 未转发源

    player.set_muted(true);
    src->emit(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);
    out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 1e-6, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(video_player_detach_clears_callback_and_edges) {
    const auto ctx = silent_ctx();
    const auto src = std::make_shared<CallbackSource>();
    VideoPlayer player;
    player.set_source(src);
    player.set_audio_context(ctx);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(src->audio_cb));

    player.set_audio_context(nullptr);  // 解除接线
    AURORA_TEST_CHECK_TRUE(player.audio_context() == nullptr);
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(src->audio_cb));  // 回调已清空
    AURORA_TEST_CHECK_EQ(ctx->connection_count(), 0);

    src->emit(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);  // 无回调：no-op 不崩溃
    const auto out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, 0.0F, 1e-6, AURORA_AUDIO_FRAMES));
}

AURORA_TEST_CASE(video_player_unattached_volume_forwards_to_source) {
    const auto src = std::make_shared<CallbackSource>();
    VideoPlayer player;
    player.set_source(src);  // 未接音频图：保持既有语义（转发源）
    player.set_volume(0.3);
    AURORA_TEST_CHECK_NEAR(src->last_volume, 0.3, 1e-9);
    player.set_muted(true);
    AURORA_TEST_CHECK_TRUE(src->last_muted);
}

AURORA_TEST_CASE(video_player_set_source_after_attach_rebinds_channel) {
    const auto ctx = silent_ctx();
    const auto first = std::make_shared<CallbackSource>();
    const auto second = std::make_shared<CallbackSource>();
    VideoPlayer player;
    player.set_source(first);
    player.set_audio_context(ctx);
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(first->audio_cb));

    player.set_source(second);  // 换源：旧源解绑、新源接管
    AURORA_TEST_CHECK_FALSE(static_cast<bool>(first->audio_cb));
    AURORA_TEST_CHECK_TRUE(static_cast<bool>(second->audio_cb));
    AURORA_TEST_CHECK_EQ(ctx->connection_count(), 2);

    second->emit(const_pcm(AURORA_AUDIO_FRAMES), AURORA_AUDIO_RATE, 2);
    const auto out = render_block(*ctx, AURORA_AUDIO_FRAMES);
    AURORA_TEST_CHECK_TRUE(check_all_near(out, AURORA_AUDIO_PCM_VALUE, 1e-4, AURORA_AUDIO_FRAMES));
}

}  // namespace aurora::test_cases::utest_audio_sink_bridge

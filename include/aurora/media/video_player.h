#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "aurora/core/enums.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/media/video_source.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 视频播放器控件（继承 `Container`，故可叠加子节点 = 控件叠层）。
///
/// 设计目标：**易于被继承定制**。提供四类扩展点：
///  1. **可插拔源**：`set_source(shared_ptr<VideoSource>)` 接入任意解码后端。
///  2. **可子类化本体**：覆写 `on_frame` / `on_playback_tick` / `on_paint` / `on_layout`。
///  3. **可定制控件 UI**：`set_controls(...)` 整体替换叠层；默认 `VideoControls`。
///  4. **可插拔事件/手势**：覆写 `on_tap` / `on_double_tap` 或设置 `set_on_tap` / `set_on_double_tap` 回调。
///
/// 播放时钟由 `Application::tick` 经 `tick_gestures` 驱动；`Reactive` 状态（`playing` / `progress` /
/// `volume` / `muted`）可供控件与 UI 绑定。
///
/// @note Thread: main-thread only
/// @note Side-effects: paints
/// @note Rebuildable: yes, via from_json
class VideoPlayer : public Container, public VideoController {
  public:
    VideoPlayer() = default;
    explicit VideoPlayer(std::shared_ptr<VideoSource> src) : source_(std::move(src)) {}

    /// @brief 设置 / 获取解码源。
    auto set_source(std::shared_ptr<VideoSource> src) -> void { source_ = std::move(src); }
    [[nodiscard]] auto source() const -> std::shared_ptr<VideoSource> { return source_; }

    /// @brief 适配模式（letterbox）：Contain 留黑边 / Fill 拉伸 / Cover 裁剪。
    auto set_fit(BoxFit fit) -> void { fit_ = fit; }
    [[nodiscard]] auto fit() const -> BoxFit { return fit_; }

    /// @brief 播放控制（公开便捷封装）。
    auto play() -> void;
    auto pause() -> void;
    auto toggle_play() -> void override;
    [[nodiscard]] auto is_playing() const -> bool override { return playing_; }
    auto seek(std::chrono::microseconds pos) -> void;
    auto seek_fraction(double f) -> void override;
    [[nodiscard]] auto position() const -> std::chrono::microseconds;
    [[nodiscard]] auto position_fraction() const -> double override;
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override;
    auto set_volume(double v) -> void override;
    auto set_muted(bool m) -> void override;
    [[nodiscard]] auto volume() const -> double override { return volume_.get(); }
    [[nodiscard]] auto muted() const -> bool override { return muted_.get(); }

    /// @brief 显示 / 隐藏控件叠层。
    auto set_show_controls(bool show) -> void;
    [[nodiscard]] auto show_controls() const -> bool { return show_controls_; }

    /// @brief 整体替换控件叠层（传入 nullptr 表示无控件）。
    auto set_controls(std::unique_ptr<Widget> controls) -> void;
    /// @brief 创建默认控件叠层（子类可覆写以更换默认 UI）。
    [[nodiscard]] virtual auto create_default_controls() -> std::unique_ptr<Widget>;

    /// @brief 点击 / 双击手势回调（非子类场景的快捷扩展）。
    auto set_on_tap(std::function<void()> fn) -> void { on_tap_ = std::move(fn); }
    auto set_on_double_tap(std::function<void()> fn) -> void { on_double_tap_ = std::move(fn); }

    // ---- VideoController 实现（供控件解耦绑定） ----
    [[nodiscard]] auto playing_signal() -> Reactive<bool> * override { return &playing_state_; }
    [[nodiscard]] auto progress_signal() -> Reactive<double> * override { return &progress_; }
    [[nodiscard]] auto volume_signal() -> Reactive<double> * override { return &volume_; }
    [[nodiscard]] auto muted_signal() -> Reactive<bool> * override { return &muted_; }

    [[nodiscard]] auto type_name() const -> const char * override { return "VideoPlayer"; }
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override;
    auto serialize_props(Json &props) const -> void override;
    auto deserialize_props(const Json &props) -> void override;
    auto on_pointer_event(MouseEvent &e) -> void override;
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

  protected:
    /// @brief 取到新帧时的钩子（默认缓存并请求重绘），子类可覆写做滤镜 / 叠加。
    virtual auto on_frame(const Image &frame) -> void;
    /// @brief 每个播放时钟 tick（仅 playing 时推进），子类可覆写扩展。
    virtual auto on_playback_tick(std::chrono::steady_clock::time_point now) -> void;
    /// @brief 单击手势（默认：有回调则调用，否则 toggle_play）。
    virtual auto on_tap() -> void;
    /// @brief 双击手势（默认：切换适配模式）。
    virtual auto on_double_tap() -> void;

    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override;

    /// @brief 子类 on_paint 可读取当前解码帧（用于滤镜/水印/叠加）。
    [[nodiscard]] auto current_frame() const -> const Image & { return current_frame_; }
    /// @brief 子类 on_paint 复用私有 draw_frame 在当前 bounds 绘制帧。
    auto paint_frame(Painter &p, const Rect &bounds) const -> void;

    /// @brief 挂载期补齐默认控件叠层（#1）：仅当尚无自定义控件时 adopt，随后挂载子树。
    auto on_mount(const BuildContext &ctx) -> void override;

  private:
    auto adopt_default_controls() -> void;
    auto draw_frame(Painter &p, const Rect &bounds) const -> void;
    [[nodiscard]] auto current_video_pos() const -> std::chrono::microseconds;
    [[nodiscard]] auto resolve_width(const Constraints &c, float natural) const -> float;
    [[nodiscard]] auto resolve_height(const Constraints &c, float natural) const -> float;

    std::shared_ptr<VideoSource> source_;
    Image current_frame_;
    BoxFit fit_ = BoxFit::Contain;
    bool show_controls_ = true;

    bool playing_ = false;
    std::chrono::microseconds video_pos_{0};
    std::optional<std::chrono::steady_clock::time_point> play_start_wall_;

    Reactive<bool> playing_state_{false};
    Reactive<double> progress_{0.0};
    Reactive<double> volume_{1.0};
    Reactive<bool> muted_{false};

    bool pressed_ = false;
    std::chrono::steady_clock::time_point last_tap_;
    std::function<void()> on_tap_;
    std::function<void()> on_double_tap_;
};

}  // namespace aurora

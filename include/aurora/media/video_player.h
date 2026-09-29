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

class AudioContext;
class AudioSinkGraphBridge;

/// @brief 视频播放器控件（继承 `Container`，故可叠加子节点 = 控件叠层）。
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
    /// @brief 默认构造：无源播放（经 `set_source` 后接）。
    /// 播放时钟走 Widget::tick → tick_gestures，而 Widget::tick 在 needs_gesture_tick_ 为假时
    /// 直接早退：不开此门则 play() 之后 on_playback_tick 永不运行，画面停在第 0 帧。
    VideoPlayer() { needs_gesture_tick_ = true; }
    /// @brief 带源构造：立即接入解码源，其余初始化同默认构造。
    /// @param src 视频源共享指针（可为空，后续经 `set_source` 替换）。
    explicit VideoPlayer(std::shared_ptr<VideoSource> src) : source_(std::move(src)) { needs_gesture_tick_ = true; }

    /// @brief 换入新解码源。
    /// @param src 新源共享指针；若已接音频桥则自动解除旧源、接管新源的 PCM 通道。
    auto set_source(std::shared_ptr<VideoSource> src) -> void;
    /// @brief 当前解码源。
    /// @return 源共享指针（未接入为 nullptr）。
    [[nodiscard]] auto source() const -> std::shared_ptr<VideoSource> { return source_; }

    /// @brief 接入音频图（alpha 音频子系统）：此后 `set_audio_callback` 通道收到的 PCM
    ///        自动经图内 `AudioStreamSourceNode` 播放（`AudioSinkGraphBridge` 承载）。
    /// 接线后 `set_volume`/`set_muted` 改为经图内 `GainNode` 施加（不重复转发给源，
    /// 避免双重衰减）；未接线时保持既有语义（转发给 `VideoSource::set_volume`）。
    /// 传入 nullptr 解除接线（清空源的音频回调并断开图内边）。
    /// @note 典型接线：`player.set_audio_context(app.audio_shared());`
    /// @param ctx 目标音频上下文；nullptr 表示解除接线（清空源音频回调并断开图内边）。
    auto set_audio_context(std::shared_ptr<AudioContext> ctx) -> void;
    /// @brief 当前接入的音频上下文（未接线为 nullptr）。
    /// @return audio_ctx_ 共享指针常引用。
    [[nodiscard]] auto audio_context() const -> const std::shared_ptr<AudioContext> & { return audio_ctx_; }

    /// @brief 适配模式（letterbox）：Contain 留黑边 / Fill 拉伸 / Cover 裁剪。
    /// @param fit 目标适配模式。
    auto set_fit(BoxFit fit) -> void { fit_ = fit; }
    /// @brief 当前适配模式。
    /// @return fit_（默认 Contain；双击默认在 Contain/Cover 间切换）。
    [[nodiscard]] auto fit() const -> BoxFit { return fit_; }

    /// @brief 播放控制（公开便捷封装）。
    auto play() -> void;
    /// @brief 暂停：固化当前位置并转发源。
    auto pause() -> void;
    /// @brief 播放 / 暂停互切（当前播放则暂停，反之起播）。
    auto toggle_play() -> void override;
    /// @brief 是否处于播放态。
    /// @return 内部 playing_ 标记最新值。
    [[nodiscard]] auto is_playing() const -> bool override { return playing_; }
    /// @brief 跳转到指定位置（钳制到 [0, duration] 并重置播放基线）。
    /// @param pos 目标位置（相对起点的微秒）。
    auto seek(std::chrono::microseconds pos) -> void;
    /// @brief 按比例跳转（f 先钳制到 [0,1] 再折算为绝对位置）。
    /// @param f 进度比例。
    auto seek_fraction(double f) -> void override;
    /// @brief 当前播放位置。
    /// @return 播放中为墙钟推算位置，暂停时为固化位置（微秒）。
    [[nodiscard]] auto position() const -> std::chrono::microseconds;
    /// @brief 当前进度比例。
    /// @return progress_ 信号值（[0,1]，源无时长时为 0）。
    [[nodiscard]] auto position_fraction() const -> double override;
    /// @brief 总时长。
    /// @return 取自当前源；无源时为 {0}。
    [[nodiscard]] auto duration() const -> std::chrono::microseconds override;
    /// @brief 设置音量（钳制 [0,1]）。
    /// @param v 线性音量因子；已接音频图时经图内 GainNode 施加，否则转发源。
    auto set_volume(double v) -> void override;
    /// @brief 静音开关。
    /// @param m 静音状态；施加路径同 set_volume。
    auto set_muted(bool m) -> void override;
    /// @brief 当前音量。
    /// @return volume_ 信号值（[0,1]）。
    [[nodiscard]] auto volume() const -> double override { return volume_.get(); }
    /// @brief 当前静音状态。
    /// @return muted_ 信号值。
    [[nodiscard]] auto muted() const -> bool override { return muted_.get(); }

    /// @brief 显示 / 隐藏控件叠层。
    /// @param show 同步到叠层节点的 show 状态并影响后续布局。
    auto set_show_controls(bool show) -> void;
    /// @brief 控件叠层是否显示。
    /// @return show_controls_ 最新值（默认 true）。
    [[nodiscard]] auto show_controls() const -> bool { return show_controls_; }

    /// @brief 整体替换控件叠层（传入 nullptr 表示无控件）。
    /// @param controls 新叠层所有权；非空时继承当前 show_controls_ 状态。
    auto set_controls(std::unique_ptr<Widget> controls) -> void;
    /// @brief 创建默认控件叠层（子类可覆写以更换默认 UI）。
    /// @return 新建的 `VideoControls`（绑定本玩家的 VideoController 接口）。
    [[nodiscard]] virtual auto create_default_controls() -> std::unique_ptr<Widget>;

    /// @brief 点击 / 双击手势回调（非子类场景的快捷扩展）。
    /// @param fn 无参回调：设给 tap 则单击触发，设给 double_tap 则双击触发；未设时走默认行为。
    auto set_on_tap(std::function<void()> fn) -> void { on_tap_ = std::move(fn); }
    /// @brief 双击手势回调（同 set_on_tap，触发时机为双击判定）。
    /// @param fn 无参回调；未设时双击走默认适配模式切换。
    auto set_on_double_tap(std::function<void()> fn) -> void { on_double_tap_ = std::move(fn); }

    /// @brief 播放状态信号（VideoController 实现：供控件解耦绑定）。
    /// @return 底层 Reactive<bool> 的地址，恒非空。
    [[nodiscard]] auto playing_signal() -> Reactive<bool> * override { return &playing_state_; }
    /// @brief 进度信号（0..1，VideoController 实现）。
    /// @return 底层 Reactive<double> 的地址，恒非空。
    [[nodiscard]] auto progress_signal() -> Reactive<double> * override { return &progress_; }
    /// @brief 音量信号（[0,1]，VideoController 实现）。
    /// @return 底层 Reactive<double> 的地址，恒非空。
    [[nodiscard]] auto volume_signal() -> Reactive<double> * override { return &volume_; }
    /// @brief 静音信号（VideoController 实现）。
    /// @return 底层 Reactive<bool> 的地址，恒非空。
    [[nodiscard]] auto muted_signal() -> Reactive<bool> * override { return &muted_; }

    /// @brief 控件类型名（诊断 / 序列化路由）。
    /// @return "VideoPlayer"。
    [[nodiscard]] auto type_name() const -> const char * override { return "VideoPlayer"; }
    /// @brief 静态自描述表（属性：fit / show_controls）。
    /// @return 控件描述符（供 Inspector / from_json 校验）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 本控件自描述（转发静态描述）。
    /// @return describe_static() 的描述表。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }
    /// @brief 汇集可绑定信号：先收集子控件信号，再追加播放/进度/音量/静音四路状态。
    /// @param out 追加目标信号视图列表。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override;
    /// @brief 序列化专有属性（fit / show_controls）。
    /// @param props 写入目标 JSON 对象（先由基类写入公共属性）。
    auto serialize_props(Json &props) const -> void override;
    /// @brief 反序列化专有属性：fit / show_controls 键存在才应用（缺失保持现值）。
    /// @param props 来源 JSON 对象。
    auto deserialize_props(const Json &props) -> void override;
    /// @brief 指针事件分发：叠层区域内事件放行给控件；区域外经按压跟踪产生单击/双击手势。
    /// @param e 鼠标事件（消费时置 is_handled）。
    auto on_pointer_event(MouseEvent &e) -> void override;
    /// @brief 声明本控件需要点击事件路由（基类手势分发用）。
    /// @return 恒 true。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

  protected:
    /// @brief 取到新帧时的钩子（默认缓存并请求重绘），子类可覆写做滤镜 / 叠加。
    /// @param frame 新解码出的帧像素（缓存进当前帧并标记流式纹理）。
    virtual auto on_frame(const Image &frame) -> void;
    /// @brief 每个播放时钟 tick（仅 playing 时推进），子类可覆写扩展。
    /// @param now 本次 tick 的墙钟时刻（默认实现按墙钟推算位置取帧，未直接使用该参数）。
    virtual auto on_playback_tick(std::chrono::steady_clock::time_point now) -> void;
    /// @brief 单击手势（默认：有回调则调用，否则 toggle_play）。
    virtual auto on_tap() -> void;
    /// @brief 双击手势（默认：切换适配模式）。
    virtual auto on_double_tap() -> void;

    /// @brief 布局：按源自然尺寸（无源时 160x90 占位）解析宽高，并把叠层控件贴底布局。
    /// @param c 父级约束。
    /// @param ctx 构建上下文（传递给叠层子控件布局）。
    /// @return 经约束钳制后的控件尺寸。
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    /// @brief 绘制：先画当前帧（draw_frame），再交由 Container 绘制叠层控件。
    /// @param p 绘制器。
    /// @param bounds 本控件绘制区域。
    /// @param ctx 构建上下文。
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    /// @brief 时钟入口：先驱动播放 tick（on_playback_tick），再走 Container 手势节拍。
    /// @param now 本次 tick 的墙钟时刻。
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override;

    /// @brief 子类 on_paint 可读取当前解码帧（用于滤镜/水印/叠加）。
    /// @return 当前缓存帧引用；尚未解码时为空 Image。
    [[nodiscard]] auto current_frame() const -> const Image & { return current_frame_; }
    /// @brief 子类 on_paint 复用私有 draw_frame 在当前 bounds 绘制帧。
    /// @param p 绘制器。
    /// @param bounds 帧绘制区域（按 fit 模式对齐缩放）。
    auto paint_frame(Painter &p, const Rect &bounds) const -> void;

    /// @brief 挂载期补齐默认控件叠层（#1）：仅当尚无自定义控件时 adopt，随后挂载子树。
    /// @param ctx 构建上下文（传递给 Container::on_mount）。
    auto on_mount(const BuildContext &ctx) -> void override;

  private:
    auto adopt_default_controls() -> void;
    auto attach_audio_bridge_to_source() const -> void;
    auto draw_frame(Painter &p, const Rect &bounds) const -> void;
    [[nodiscard]] auto current_video_pos() const -> std::chrono::microseconds;
    [[nodiscard]] auto resolve_width(const Constraints &c, float natural) const -> float;
    [[nodiscard]] auto resolve_height(const Constraints &c, float natural) const -> float;

    std::shared_ptr<VideoSource> source_;
    Image current_frame_;
    /// @brief 流式纹理标识（GPU 常驻流式通道，specification/03 §8.7）：键惰性分配、
    ///        进程内唯一；`on_frame` 每帧递增版本触发后端增量 sub-upload。软件路径忽略。
    std::uint64_t stream_key_ = 0;
    std::uint64_t stream_version_ = 0;
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

    // ---- 音频图接线（audio_sink_bridge.h）----
    std::shared_ptr<AudioContext> audio_ctx_;  ///< 接入的上下文（未接线为 nullptr）。
    std::shared_ptr<AudioSinkGraphBridge> audio_bridge_;  ///< PCM → 图桥（接线期持有）。
};

}  // namespace aurora

#pragma once

#include <string>

#include "aurora/media/video_source.h"
#include "aurora/widget/button.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

/// @brief Aurora 公共命名空间：本头在其中声明视频控件叠层 `VideoControls`。
namespace aurora {

/// @brief 可定制的视频控件叠层（继承 `Container`）：播放 / 暂停、进度条、时间、音量。
/// 通过 `VideoController` 接口与播放器解耦——可整体替换为自定义子类，或子类化本类换肤 / 重排。
/// 默认布局为底部半透明条：`[播放] [进度(撑满)] [时间] [静音] [音量]`。
///
/// @note Thread: main-thread only
/// @note Side-effects: paints
/// @note Rebuildable: yes, via from_json
class VideoControls : public Container {
  public:
    /// @brief 默认构造（无控制器）：开 needs_gesture_tick_ 门——播放/暂停文案与时间读数在
    ///        tick_gestures 里刷新，不开门则 Widget::tick 早退、叠层停在初始的「Play / 0:00」。
    ///        不组装子控件——默认叠层由带控制器的构造经 build_children() 组装。
    VideoControls() { needs_gesture_tick_ = true; }
    /// @brief 构造并绑定播放器接口：随即调用 build_children() 组装默认叠层。
    /// @param controller 播放器能力接口（非拥有，生命周期须覆盖本控件）。
    explicit VideoControls(VideoController *controller);

    /// @brief 控件类型名（自描述键，供 Inspector/序列化定位）。
    /// @return "VideoControls"。
    [[nodiscard]] auto type_name() const -> const char * override { return "VideoControls"; }
    /// @brief 静态自描述：控件属性键与可重建契约描述。
    /// @return WidgetDescriptor（from_json 重建依据）。
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    /// @brief 实例自描述（与 describe_static 同源）。
    /// @return WidgetDescriptor。
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }
    /// @brief 收集可绑定的响应式信号（供 Inspector 观测）。
    /// @param out 追加收集到的 SignalViewBase 指针（非拥有）的输出向量。
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override;
    /// @brief 序列化本控件属性到 props JSON。
    /// @param props 写入目标 JSON 对象。
    auto serialize_props(Json &props) const -> void override;
    /// @brief 从 props JSON 还原本控件属性。
    /// @param props 来源 JSON 对象（与 serialize_props 往返一致）。
    auto deserialize_props(const Json &props) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override;

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes):
    // 受保护子控件指针为有意设计，供子类换肤/重排访问
    VideoController *controller_ = nullptr;

    Button *play_btn_ = nullptr;
    Text *time_text_ = nullptr;
    Button *mute_btn_ = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    [[nodiscard]] auto play_button() const -> Button * { return play_btn_; }
    [[nodiscard]] auto time_text() const -> Text * { return time_text_; }
    [[nodiscard]] auto mute_button() const -> Button * { return mute_btn_; }

    virtual auto build_children() -> void;

  private:
    static auto format_time(long long ms) -> std::string;
};

}  // namespace aurora

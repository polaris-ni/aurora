#pragma once

#include <string>

#include "aurora/media/video_source.h"
#include "aurora/widget/button.h"
#include "aurora/widget/text.h"
#include "aurora/widget/widget.h"

namespace aurora {

/// @brief 可定制的视频控件叠层（继承 `Container`）：播放 / 暂停、进度条、时间、音量。
///
/// 通过 `VideoController` 接口与播放器解耦——可整体替换为自定义子类，或子类化本类换肤 / 重排。
/// 默认布局为底部半透明条：`[播放] [进度(撑满)] [时间] [静音] [音量]`。
///
/// @note Thread: main-thread only
/// @note Side-effects: paints
/// @note Rebuildable: yes, via from_json
class VideoControls : public Container {
  public:
    VideoControls() = default;
    explicit VideoControls(VideoController *controller);

    [[nodiscard]] auto type_name() const -> const char * override { return "VideoControls"; }
    [[nodiscard]] static auto describe_static() -> WidgetDescriptor;
    [[nodiscard]] auto describe() const -> WidgetDescriptor override { return describe_static(); }
    auto collect_signals(std::vector<SignalViewBase *> &out) -> void override;
    auto serialize_props(Json &props) const -> void override;
    auto deserialize_props(const Json &props) -> void override;

  protected:
    auto on_layout(const Constraints &c, const BuildContext &ctx) -> Size override;
    auto on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void override;
    auto tick_gestures(std::chrono::steady_clock::time_point now) -> void override;

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes):
    // 受保护子控件指针为有意设计，供子类换肤/重排访问
    VideoController *m_controller = nullptr;

    Button *m_play_btn = nullptr;
    Text *m_time_text = nullptr;
    Button *m_mute_btn = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    [[nodiscard]] auto play_button() const -> Button * { return m_play_btn; }
    [[nodiscard]] auto time_text() const -> Text * { return m_time_text; }
    [[nodiscard]] auto mute_button() const -> Button * { return m_mute_btn; }

    virtual auto build_children() -> void;

  private:
    static auto format_time(long long ms) -> std::string;
};

} // namespace aurora

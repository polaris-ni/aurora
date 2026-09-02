#include "aurora/media/video_controls.h"

#include <chrono>
#include <sstream>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/dimension.h"
#include "aurora/i18n/localized_string.h"
#include "aurora/modifier/modifier.h"
#include "aurora/widget/button.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/slider.h"
#include "aurora/widget/text.h"

namespace aurora {

auto VideoControls::format_time(long long ms) -> std::string {
    const long long total_s = ms / 1000;
    const long long m = total_s / 60;
    const long long s = total_s % 60;
    std::ostringstream oss;
    oss << m << ':' << (s < 10 ? "0" : "") << s;
    return oss.str();
}

VideoControls::VideoControls(VideoController *controller) : m_controller(controller) {
    VideoControls::build_children();
}

auto VideoControls::build_children() -> void {
    auto play = std::make_unique<Button>("Play");
    play->set_on_click([this]() -> void {
        if (m_controller) {
            m_controller->toggle_play();
        }
    });

    // 控制器为空（独立反序列化场景）时使用本地占位信号，避免空指针。
    Reactive fallback_progress{ 0.0 };
    Reactive fallback_volume{ 1.0 };
    auto seek = std::make_unique<Slider>(
        (m_controller != nullptr) ? *m_controller->progress_signal() : fallback_progress, [this](double f) -> void {
            if (m_controller) {
                m_controller->seek_fraction(f);
            }
        });
    seek->modifier.set(Modifier{}.expand()); // 在 Row 中撑满剩余空间

    auto time = std::make_unique<Text>("0:00 / 0:00");
    time->color(Color::white());

    auto mute = std::make_unique<Button>("Mute");
    mute->set_on_click([this]() -> void {
        if (m_controller) {
            m_controller->set_muted(!m_controller->muted());
        }
    });

    auto vol = std::make_unique<Slider>((m_controller != nullptr) ? *m_controller->volume_signal() : fallback_volume,
                                        [this](double v) -> void {
                                            if (m_controller) {
                                                m_controller->set_volume(v);
                                            }
                                        });
    vol->width(px(80.0f));

    Row row{ Node(std::move(play)), Node(std::move(seek)), Node(std::move(time)), Node(std::move(mute)),
             Node(std::move(vol)) };
    adopt_children({ Node(std::make_unique<Row>(std::move(row))) });

    // 子控件已归容器所有，从成员 m_children 中反查指针，避免保存局部 unique_ptr 的地址。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): 子控件类型由本函数构造顺序保证
    auto &row_widget = static_cast<Row &>(m_children[0].widget());
    m_play_btn = &static_cast<Button &>(row_widget.child(0).widget());
    m_time_text = &static_cast<Text &>(row_widget.child(2).widget());
    m_mute_btn = &static_cast<Button &>(row_widget.child(3).widget());
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
}

auto VideoControls::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    if (m_children.empty()) {
        return c.constrain(Size{ .width = 0.0f, .height = 0.0f });
    }
    Node &row = m_children[0];
    row.widget().layout(c, ctx);
    const Size sz = row.widget().size();
    row.set_bounds(Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f }, .size = sz });
    return c.constrain(sz);
}

auto VideoControls::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    p.fill_rect(bounds, Color{ 0, 0, 0, 140 }); // 半透明黑底条
    Container::on_paint(p, bounds, ctx);
}

auto VideoControls::tick_gestures(std::chrono::steady_clock::time_point now) -> void {
    if (m_controller != nullptr) {
        const double frac = m_controller->position_fraction();
        const auto dur = m_controller->duration();
        const long long total_ms = dur.count() / 1000;
        const long long cur_ms = static_cast<long long>(frac * static_cast<double>(dur.count())) / 1000;
        if (m_time_text != nullptr) {
            m_time_text->set_content(format_time(cur_ms) + " / " + format_time(total_ms));
        }
        if (m_play_btn != nullptr) {
            m_play_btn->label.set(LocalizedString{ m_controller->is_playing() ? "Pause" : "Play" });
        }
        if (m_mute_btn != nullptr) {
            m_mute_btn->label.set(LocalizedString{ m_controller->muted() ? "Unmute" : "Mute" });
        }
    }
    Container::tick_gestures(now);
}

auto VideoControls::collect_signals(std::vector<SignalViewBase *> &out) -> void {
    for (Node &child : m_children) {
        child.widget().collect_signals(out);
    }
}

auto VideoControls::serialize_props(Json &props) const -> void { Container::serialize_props(props); }

auto VideoControls::deserialize_props(const Json &props) -> void { Container::deserialize_props(props); }

auto VideoControls::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "VideoControls",
        .properties = {
            { .name="width", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
            { .name="height", .type="Length", .default_value="auto", .required=false, .note="", .json_type="array" },
            { .name="show", .type="bool", .default_value="true", .required=false, .note="", .json_type="boolean" },
        },
        .events = {},
        .children_policy = "single",
        .examples = { "au::VideoControls{ controller }" },
    };
}

} // namespace aurora

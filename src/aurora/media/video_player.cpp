#include "aurora/media/video_player.h"

#include <algorithm>
#include <chrono>

#include "aurora/core/color.h"
#include "aurora/core/types.h"
#include "aurora/media/video_controls.h"
#include "aurora/widget/props_io.h"

namespace aurora {

auto VideoPlayer::adopt_default_controls() -> void { set_controls(create_default_controls()); }

auto VideoPlayer::create_default_controls() -> std::unique_ptr<Widget> {
    return std::make_unique<VideoControls>(static_cast<VideoController *>(this));
}

auto VideoPlayer::set_show_controls(bool show) -> void {
    m_show_controls = show;
    if (!m_children.empty()) {
        m_children[0].widget().show.set(show);
    }
}

auto VideoPlayer::set_controls(std::unique_ptr<Widget> controls) -> void {
    if (controls) {
        controls->show.set(m_show_controls);
        m_children = { Node(std::move(controls)) };
    } else {
        m_children.clear();
    }
    mark_needs_layout();
}

// ---- 播放控制 ----

auto VideoPlayer::current_video_pos() const -> std::chrono::microseconds {
    if (m_playing && m_play_start_wall) {
        const auto elapsed = std::chrono::steady_clock::now() - *m_play_start_wall;
        return m_video_pos + std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    }
    return m_video_pos;
}

auto VideoPlayer::play() -> void {
    if (m_playing) {
        return;
    }
    m_playing = true;
    m_play_start_wall = std::chrono::steady_clock::now() - m_video_pos; // 以当前位置为基线
    m_playing_state.set(true);
    if (m_source) {
        m_source->play();
    }
}

auto VideoPlayer::pause() -> void {
    if (!m_playing) {
        return;
    }
    m_video_pos = current_video_pos();
    m_playing = false;
    m_play_start_wall.reset();
    m_playing_state.set(false);
    if (m_source) {
        m_source->pause();
    }
}

auto VideoPlayer::toggle_play() -> void {
    if (m_playing) {
        pause();
    } else {
        play();
    }
}

auto VideoPlayer::seek(std::chrono::microseconds pos) -> void {
    const auto dur = duration();
    if (pos < std::chrono::microseconds{ 0 }) {
        pos = std::chrono::microseconds{ 0 };
    }
    if (dur.count() > 0 && pos > dur) {
        pos = dur;
    }
    m_video_pos = pos;
    if (m_playing && m_play_start_wall) {
        m_play_start_wall = std::chrono::steady_clock::now() - pos; // 重置播放基线
    }
    m_progress.set(dur.count() > 0 ? static_cast<double>(pos.count()) / static_cast<double>(dur.count()) : 0.0);
    if (m_source) {
        m_source->seek(pos);
    }
}

auto VideoPlayer::seek_fraction(double f) -> void {
    f = std::clamp(f, 0.0, 1.0);
    const auto dur = duration();
    seek(std::chrono::microseconds{ static_cast<long long>(f * static_cast<double>(dur.count())) });
}

auto VideoPlayer::position() const -> std::chrono::microseconds { return current_video_pos(); }

auto VideoPlayer::position_fraction() const -> double { return m_progress.get(); }

auto VideoPlayer::duration() const -> std::chrono::microseconds {
    return m_source ? m_source->duration() : std::chrono::microseconds{ 0 };
}

auto VideoPlayer::set_volume(double v) -> void {
    v = std::clamp(v, 0.0, 1.0);
    m_volume.set(v);
    if (m_source) {
        m_source->set_volume(v);
    }
}

auto VideoPlayer::set_muted(bool m) -> void {
    m_muted.set(m);
    if (m_source) {
        m_source->set_muted(m);
    }
}

// ---- 受保护虚函数（扩展点） ----

auto VideoPlayer::on_frame(const Image &frame) -> void {
    m_current_frame = frame;
    mark_needs_paint();
}

auto VideoPlayer::on_playback_tick(std::chrono::steady_clock::time_point now) -> void {
    (void)now;
    if (!m_source || !m_playing) {
        return;
    }
    const auto pos = current_video_pos();
    const auto dur = m_source->duration();
    if (dur.count() > 0 && pos >= dur) {
        // 到结尾：停在末尾并暂停。
        m_video_pos = dur;
        m_playing = false;
        m_play_start_wall.reset();
        m_playing_state.set(false);
        m_source->pause();
        m_progress.set(1.0);
        const auto fr = m_source->frame_at(dur);
        if (fr) {
            on_frame(fr.value().image);
        }
        return;
    }
    const double frac = dur.count() > 0 ? static_cast<double>(pos.count()) / static_cast<double>(dur.count()) : 0.0;
    m_progress.set(frac);
    const auto fr = m_source->frame_at(pos);
    if (fr) {
        on_frame(fr.value().image);
    }
}

auto VideoPlayer::on_tap() -> void {
    if (m_on_tap) {
        m_on_tap();
    } else {
        toggle_play();
    }
}

auto VideoPlayer::on_double_tap() -> void {
    if (m_on_double_tap) {
        m_on_double_tap();
    } else {
        // 默认双击切换适配模式（Contain <-> Cover）。
        m_fit = (m_fit == BoxFit::Contain) ? BoxFit::Cover : BoxFit::Contain;
        mark_needs_paint();
    }
}

// ---- 布局 / 绘制 ----

auto VideoPlayer::resolve_width(const Constraints &c, float natural) const -> float {
    switch (m_width.kind) {
    case LengthKind::Fixed: return m_width.value;
    case LengthKind::Fraction: return c.max.width * m_width.value;
    case LengthKind::Expand: return c.max.width;
    case LengthKind::WrapContent:
    default: return natural;
    }
}

auto VideoPlayer::resolve_height(const Constraints &c, float natural) const -> float {
    switch (m_height.kind) {
    case LengthKind::Fixed: return m_height.value;
    case LengthKind::Fraction: return c.max.height * m_height.value;
    case LengthKind::Expand: return c.max.height;
    case LengthKind::WrapContent:
    default: return natural;
    }
}

auto VideoPlayer::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    const Size nat = m_source ? m_source->natural_size() : Size{ .width = 0.0f, .height = 0.0f };
    const float natural_w = nat.width > 0.0f ? nat.width : 160.0f;
    const float natural_h = nat.height > 0.0f ? nat.height : 90.0f;
    const float w = resolve_width(c, natural_w);
    const float h = resolve_height(c, natural_h);
    const Size s = c.constrain(Size{ .width = w, .height = h });

    if (!m_children.empty()) {
        Node &ctl = m_children[0];
        ctl.widget().show.set(m_show_controls);
        if (m_show_controls) {
            const Constraints cc{ .min = Size{ .width = 0.0f, .height = 0.0f }, .max = s };
            ctl.widget().layout(cc, ctx);
            const Size cs = ctl.widget().size();
            const float y = std::max(0.0f, s.height - cs.height);
            ctl.set_bounds(
                Rect{ .origin = Point{ .x = 0.0f, .y = y }, .size = Size{ .width = s.width, .height = cs.height } });
        }
    }
    return s;
}

auto VideoPlayer::draw_frame(Painter &p, const Rect &bounds) const -> void {
    if (m_current_frame.pixels.empty() || m_current_frame.width <= 0 || m_current_frame.height <= 0) {
        p.fill_rect(bounds, Color{ 20, 20, 24, 255 }); // 视频背景占位
        return;
    }
    const auto iw = static_cast<float>(m_current_frame.width);
    const auto ih = static_cast<float>(m_current_frame.height);
    float dw = bounds.size.width;
    float dh = bounds.size.height;
    if (m_fit != BoxFit::Fill) {
        const float scale = (m_fit == BoxFit::Cover) ? std::max(bounds.size.width / iw, bounds.size.height / ih)
                                                     : std::min(bounds.size.width / iw, bounds.size.height / ih);
        dw = iw * scale;
        dh = ih * scale;
    }
    const float dx = bounds.origin.x + ((bounds.size.width - dw) * 0.5f);
    // NOLINTNEXTLINE(readability-math-missing-parentheses): 乘法优先级高于加法，表达式语义明确无需额外括号
    const float dy = bounds.origin.y + (bounds.size.height - dh) * 0.5f;
    p.draw_image(m_current_frame,
                 Rect{ .origin = Point{ .x = dx, .y = dy }, .size = Size{ .width = dw, .height = dh } });
}

auto VideoPlayer::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    draw_frame(p, bounds);
    Container::on_paint(p, bounds, ctx); // 绘制叠层控件
}

auto VideoPlayer::tick_gestures(std::chrono::steady_clock::time_point now) -> void {
    on_playback_tick(now);
    Container::tick_gestures(now);
}

auto VideoPlayer::on_pointer_event(MouseEvent &e) -> void {
    const bool on_controls = !m_children.empty() && m_children[0].bounds().contains(e.local_position);
    switch (e.action) {
    case MouseAction::Press:
        if (!on_controls) {
            m_pressed = true;
            e.handled = true;
        }
        break;
    case MouseAction::Move:
        if (!on_controls && m_pressed) {
            e.handled = true;
        }
        break;
    case MouseAction::Release:
        if (!on_controls && m_pressed) {
            const auto now = std::chrono::steady_clock::now();
            const bool dbl = now - m_last_tap <= std::chrono::milliseconds{ 300 };
            if (dbl) {
                on_double_tap();
            } else {
                on_tap();
            }
            m_last_tap = now;
            e.handled = true;
        }
        m_pressed = false;
        break;
    default: break;
    }
}

// ---- 自描述 / 序列化 ----

auto VideoPlayer::collect_signals(std::vector<SignalViewBase *> &out) -> void {
    for (Node &child : m_children) {
        child.widget().collect_signals(out);
    }
    out.push_back(&m_playing_state);
    out.push_back(&m_progress);
    out.push_back(&m_volume);
    out.push_back(&m_muted);
}

auto VideoPlayer::on_mount(const BuildContext &ctx) -> void {
    if (m_children.empty()) {
        adopt_default_controls();
    }
    Container::on_mount(ctx);
}

auto VideoPlayer::paint_frame(Painter &p, const Rect &bounds) const -> void { draw_frame(p, bounds); }

auto VideoPlayer::serialize_props(Json &props) const -> void {
    Container::serialize_props(props);
    props["fit"] = box_fit_to_json(m_fit);
    props["show_controls"] = m_show_controls;
}

auto VideoPlayer::deserialize_props(const Json &props) -> void {
    Container::deserialize_props(props);
    if (props.contains("fit")) {
        m_fit = json_to_box_fit(props["fit"]);
    }
    if (props.contains("show_controls")) {
        m_show_controls = props["show_controls"].get<bool>();
        if (!m_children.empty()) {
            m_children[0].widget().show.set(m_show_controls);
        }
    }
}

auto VideoPlayer::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "VideoPlayer",
        .properties = {
            { .name="fit", .type="BoxFit", .default_value="Contain", .required=false, .note="缩放适配（Contain/Fill/Cover）", .json_type="string",
              .enum_values={"Fill", "Contain", "Cover", "FitWidth", "FitHeight", "None", "ScaleDown"} },
            { .name="show_controls", .type="bool", .default_value="true", .required=false, .note="是否显示控件叠层", .json_type="boolean" },
            { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
            { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
            { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
        },
        .events = { "on_tap", "on_double_tap" },
        .children_policy = "single",
        .examples = { "au::VideoPlayer{}.set_source(std::make_shared<au::ImageSequenceSource>(frames, 24.0))" },
    };
}

} // namespace aurora

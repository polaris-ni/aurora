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
    show_controls_ = show;
    if (!children_.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        children_[0].widget().show.set(show);
    }
}

auto VideoPlayer::set_controls(std::unique_ptr<Widget> controls) -> void {
    if (controls) {
        controls->show.set(show_controls_);
        children_ = {Node(std::move(controls))};
    } else {
        children_.clear();
    }
    mark_needs_layout();
}

// ---- 播放控制 ----

auto VideoPlayer::current_video_pos() const -> std::chrono::microseconds {
    if (playing_ && play_start_wall_) {
        const auto elapsed = std::chrono::steady_clock::now() - *play_start_wall_;
        return video_pos_ + std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
    }
    return video_pos_;
}

auto VideoPlayer::play() -> void {
    if (playing_) {
        return;
    }
    playing_ = true;
    play_start_wall_ = std::chrono::steady_clock::now() - video_pos_;  // 以当前位置为基线
    playing_state_.set(true);
    if (source_) {
        source_->play();
    }
}

auto VideoPlayer::pause() -> void {
    if (!playing_) {
        return;
    }
    video_pos_ = current_video_pos();
    playing_ = false;
    play_start_wall_.reset();
    playing_state_.set(false);
    if (source_) {
        source_->pause();
    }
}

auto VideoPlayer::toggle_play() -> void {
    if (playing_) {
        pause();
    } else {
        play();
    }
}

auto VideoPlayer::seek(std::chrono::microseconds pos) -> void {
    const auto dur = duration();
    if (pos < std::chrono::microseconds{0}) {
        pos = std::chrono::microseconds{0};
    }
    if (dur.count() > 0 && pos > dur) {
        pos = dur;
    }
    video_pos_ = pos;
    if (playing_ && play_start_wall_) {
        play_start_wall_ = std::chrono::steady_clock::now() - pos;  // 重置播放基线
    }
    progress_.set(dur.count() > 0 ? static_cast<double>(pos.count()) / static_cast<double>(dur.count()) : 0.0);
    if (source_) {
        source_->seek(pos);
    }
}

auto VideoPlayer::seek_fraction(double f) -> void {
    f = std::clamp(f, 0.0, 1.0);
    const auto dur = duration();
    seek(std::chrono::microseconds{static_cast<long long>(f * static_cast<double>(dur.count()))});
}

auto VideoPlayer::position() const -> std::chrono::microseconds { return current_video_pos(); }

auto VideoPlayer::position_fraction() const -> double { return progress_.get(); }

auto VideoPlayer::duration() const -> std::chrono::microseconds {
    return source_ ? source_->duration() : std::chrono::microseconds{0};
}

auto VideoPlayer::set_volume(double v) -> void {
    v = std::clamp(v, 0.0, 1.0);
    volume_.set(v);
    if (source_) {
        source_->set_volume(v);
    }
}

auto VideoPlayer::set_muted(bool m) -> void {
    muted_.set(m);
    if (source_) {
        source_->set_muted(m);
    }
}

// ---- 受保护虚函数（扩展点） ----

auto VideoPlayer::on_frame(const Image &frame) -> void {
    current_frame_ = frame;
    mark_needs_paint();
}

auto VideoPlayer::on_playback_tick(std::chrono::steady_clock::time_point now) -> void {
    (void)now;
    if (!source_ || !playing_) {
        return;
    }
    const auto pos = current_video_pos();
    const auto dur = source_->duration();
    if (dur.count() > 0 && pos >= dur) {
        // 到结尾：停在末尾并暂停。
        video_pos_ = dur;
        playing_ = false;
        play_start_wall_.reset();
        playing_state_.set(false);
        source_->pause();
        progress_.set(1.0);
        const auto fr = source_->frame_at(dur);
        if (fr) {
            on_frame(fr.value().image);
        }
        return;
    }
    const double frac = dur.count() > 0 ? static_cast<double>(pos.count()) / static_cast<double>(dur.count()) : 0.0;
    progress_.set(frac);
    const auto fr = source_->frame_at(pos);
    if (fr) {
        on_frame(fr.value().image);
    }
}

auto VideoPlayer::on_tap() -> void {
    if (on_tap_) {
        on_tap_();
    } else {
        toggle_play();
    }
}

auto VideoPlayer::on_double_tap() -> void {
    if (on_double_tap_) {
        on_double_tap_();
    } else {
        // 默认双击切换适配模式（Contain <-> Cover）。
        fit_ = (fit_ == BoxFit::Contain) ? BoxFit::Cover : BoxFit::Contain;
        mark_needs_paint();
    }
}

// ---- 布局 / 绘制 ----

auto VideoPlayer::resolve_width(const Constraints &c, float natural) const -> float {
    switch (width_.kind) {
        case LengthKind::Fixed:
            return width_.value;
        case LengthKind::Fraction:
            return c.max.width * width_.value;
        case LengthKind::Expand:
            return c.max.width;
        case LengthKind::WrapContent:
        default:
            return natural;
    }
}

auto VideoPlayer::resolve_height(const Constraints &c, float natural) const -> float {
    switch (height_.kind) {
        case LengthKind::Fixed:
            return height_.value;
        case LengthKind::Fraction:
            return c.max.height * height_.value;
        case LengthKind::Expand:
            return c.max.height;
        case LengthKind::WrapContent:
        default:
            return natural;
    }
}

auto VideoPlayer::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    const Size nat = source_ ? source_->natural_size() : Size{.width = 0.0F, .height = 0.0F};
    const float natural_w = nat.width > 0.0F ? nat.width : 160.0F;
    const float natural_h = nat.height > 0.0F ? nat.height : 90.0F;
    const float w = resolve_width(c, natural_w);
    const float h = resolve_height(c, natural_h);
    const Size s = c.constrain(Size{.width = w, .height = h});

    if (!children_.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        Node &ctl = children_[0];
        ctl.widget().show.set(show_controls_);
        if (show_controls_) {
            const Constraints cc{.min = Size{.width = 0.0F, .height = 0.0F}, .max = s};
            ctl.widget().layout(cc, ctx);
            const Size cs = ctl.widget().size();
            const float y = std::max(0.0F, s.height - cs.height);
            ctl.set_bounds(
                Rect{.origin = Point{.x = 0.0F, .y = y}, .size = Size{.width = s.width, .height = cs.height}});
        }
    }
    return s;
}

auto VideoPlayer::draw_frame(Painter &p, const Rect &bounds) const -> void {
    if (current_frame_.pixels.empty() || current_frame_.width <= 0 || current_frame_.height <= 0) {
        p.fill_rect(bounds, Color{20, 20, 24, 255});  // 视频背景占位
        return;
    }
    const auto iw = static_cast<float>(current_frame_.width);
    const auto ih = static_cast<float>(current_frame_.height);
    float dw = bounds.size.width;
    float dh = bounds.size.height;
    if (fit_ != BoxFit::Fill) {
        const float scale = (fit_ == BoxFit::Cover) ? std::max(bounds.size.width / iw, bounds.size.height / ih)
                                                     : std::min(bounds.size.width / iw, bounds.size.height / ih);
        dw = iw * scale;
        dh = ih * scale;
    }
    const float dx = bounds.origin.x + ((bounds.size.width - dw) * 0.5F);
    // NOLINTNEXTLINE(readability-math-missing-parentheses): 乘法优先级高于加法，表达式语义明确无需额外括号
    const float dy = bounds.origin.y + (bounds.size.height - dh) * 0.5F;
    p.draw_image(current_frame_, Rect{.origin = Point{.x = dx, .y = dy}, .size = Size{.width = dw, .height = dh}});
}

auto VideoPlayer::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    draw_frame(p, bounds);
    Container::on_paint(p, bounds, ctx);  // 绘制叠层控件
}

auto VideoPlayer::tick_gestures(std::chrono::steady_clock::time_point now) -> void {
    on_playback_tick(now);
    Container::tick_gestures(now);
}

auto VideoPlayer::on_pointer_event(MouseEvent &e) -> void {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    const bool on_controls = !children_.empty() && children_[0].bounds().contains(e.local_position);
    switch (e.action) {
        case MouseAction::Press:
            if (!on_controls) {
                pressed_ = true;
                e.is_handled = true;
            }
            break;
        case MouseAction::Move:
            if (!on_controls && pressed_) {
                e.is_handled = true;
            }
            break;
        case MouseAction::Release:
            if (!on_controls && pressed_) {
                const auto now = std::chrono::steady_clock::now();
                const bool dbl = now - last_tap_ <= std::chrono::milliseconds{300};
                if (dbl) {
                    on_double_tap();
                } else {
                    on_tap();
                }
                last_tap_ = now;
                e.is_handled = true;
            }
            pressed_ = false;
            break;
        default:
            break;
    }
}

// ---- 自描述 / 序列化 ----

auto VideoPlayer::collect_signals(std::vector<SignalViewBase *> &out) -> void {
    for (Node &child : children_) {
        child.widget().collect_signals(out);
    }
    out.push_back(&playing_state_);
    out.push_back(&progress_);
    out.push_back(&volume_);
    out.push_back(&muted_);
}

auto VideoPlayer::on_mount(const BuildContext &ctx) -> void {
    if (children_.empty()) {
        adopt_default_controls();
    }
    Container::on_mount(ctx);
}

auto VideoPlayer::paint_frame(Painter &p, const Rect &bounds) const -> void { draw_frame(p, bounds); }

auto VideoPlayer::serialize_props(Json &props) const -> void {
    Container::serialize_props(props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["fit"] = box_fit_to_json(fit_);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["show_controls"] = show_controls_;
}

auto VideoPlayer::deserialize_props(const Json &props) -> void {
    Container::deserialize_props(props);
    if (props.contains("fit")) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        fit_ = json_to_box_fit(props["fit"]);
    }
    if (props.contains("show_controls")) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        show_controls_ = props["show_controls"].get<bool>();
        if (!children_.empty()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            children_[0].widget().show.set(show_controls_);
        }
    }
}

auto VideoPlayer::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "VideoPlayer",
        .properties =
            {
                {.name = "fit",
                 .type = "BoxFit",
                 .default_value = "Contain",
                 .required = false,
                 .note = "缩放适配（Contain/Fill/Cover）",
                 .json_type = "string",
                 .enum_values = {"Fill", "Contain", "Cover", "FitWidth", "FitHeight", "None", "ScaleDown"}},
                {.name = "show_controls",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "是否显示控件叠层",
                 .json_type = "boolean"},
                {.name = "width",
                 .type = "Length",
                 .default_value = "auto",
                 .required = false,
                 .note = "",
                 .json_type = "array"},
                {.name = "height",
                 .type = "Length",
                 .default_value = "auto",
                 .required = false,
                 .note = "",
                 .json_type = "array"},
                {.name = "show",
                 .type = "bool",
                 .default_value = "true",
                 .required = false,
                 .note = "",
                 .json_type = "boolean"},
            },
        .events = {"on_tap", "on_double_tap"},
        .children_policy = "single",
        .examples = {"au::VideoPlayer{}.set_source(std::make_shared<au::ImageSequenceSource>(frames, 24.0))"},
    };
}

}  // namespace aurora
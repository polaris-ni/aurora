#include "aurora/widget/rich_text_edit.h"

#include "aurora/app/clipboard.h"
#include "aurora/core/diagnostics.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"

namespace aurora {

auto RichTextEdit::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "RichTextEdit",
        .properties =
            {
                {.name = "text",
                 .type = "string",
                 .default_value = "\"\"",
                 .required = false,
                 .note = "纯文本内容（序列化用）",
                 .json_type = "string"},
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
        .events = {"on_text_input"},
        .children_policy = "none",
        .examples = {"au::RichTextEdit()"},
    };
}

auto RichTextEdit::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    line_height_ = render::FontEngine::measure_height(cur_font_);
    if (line_height_ < 1.0F) {
        line_height_ = 20.0F;
    }
    relayout_lines(c.max.width);
    const float h = static_cast<float>(lines_.size()) * line_height_;
    return c.constrain(Size{.width = c.max.width, .height = std::max(h, line_height_)});
}

auto RichTextEdit::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    if (lines_.empty()) {
        relayout_lines(bounds.size.width);
    }
    if (has_selection()) {
        paint_selection_highlight(p, bounds);
    }
    float y = bounds.origin.y;
    for (const auto &line : lines_) {
        float x = bounds.origin.x;
        for (const auto &sc : line.chars) {
            if (sc.ch != '\n') {
                const float cw = render::FontEngine::measure_width(std::string(1, sc.ch), sc.font);
                p.draw_text(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = cw, .height = line_height_}},
                            std::string(1, sc.ch), sc.font, sc.color);
                if (sc.underline) {
                    p.fill_rect(Rect{.origin = Point{.x = x, .y = y + line_height_ - 2.0F},
                                     .size = Size{.width = cw, .height = 1.0F}},
                                sc.color);
                }
                x += cw;
            }
        }
        y += line_height_;
    }
    if (is_focused()) {
        paint_cursor(p, bounds);
    }
}

auto RichTextEdit::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Press && e.button == MouseButton::Left) {
        request_focus();
        const float avg_w = render::FontEngine::measure_width("M", cur_font_);
        if (avg_w <= 0.0F) {
            return;
        }
        const size_t line_idx = static_cast<size_t>(std::max(0.0F, e.local_position.y) / line_height_);
        const size_t col = static_cast<size_t>(std::max(0.0F, e.local_position.x) / avg_w);
        caret_ = pos_from_line_col(line_idx, col);
        sel_start_ = sel_end_ = caret_;
        mark_needs_paint();
        e.is_handled = true;
    }
}

auto RichTextEdit::on_key_event(KeyEvent &e) -> void {
    if (e.action != KeyAction::Down) {
        return;
    }
    const bool ctrl = (e.modifiers & ModifierKey::Control) != 0U;  // NOLINT(*-redundant-parentheses)
    const bool shift = (e.modifiers & ModifierKey::Shift) != 0U;  // NOLINT(*-redundant-parentheses)
    const std::size_t n = doc_.size();

    if (ctrl && handle_control_shortcut(e, shift, n)) {
        e.is_handled = true;
        return;
    }
    if (handle_move_key(e, shift, n)) {
        e.is_handled = true;
        return;
    }
    if (handle_text_key(e, n)) {
        e.is_handled = true;
    }
}

auto RichTextEdit::handle_control_shortcut(KeyEvent &e, bool shift, std::size_t n) -> bool {
    const auto code = static_cast<KeyCode>(e.key);
    switch (code) {
        case KeyCode::Z:
            // Ctrl+Shift+Z 重做，Ctrl+Z 撤销
            if (shift) {
                if ((undo_ != nullptr) && undo_->can_redo()) {
                    undo_->redo();
                    mark_needs_paint();
                }
            } else {
                if ((undo_ != nullptr) && undo_->can_undo()) {
                    undo_->undo();
                    mark_needs_paint();
                }
            }
            return true;
        case KeyCode::Y:
            if ((undo_ != nullptr) && undo_->can_redo()) {
                undo_->redo();
                mark_needs_paint();
            }
            return true;
        case KeyCode::B:
            toggle_bold();
            return true;
        case KeyCode::I:
            toggle_italic();
            return true;
        case KeyCode::U:
            toggle_underline();
            return true;
        case KeyCode::A:
            sel_start_ = 0;
            sel_end_ = n;
            caret_ = n;
            mark_needs_paint();
            return true;
        case KeyCode::C: {
            const std::string t = selected_text();
            if (!t.empty()) {
                Clipboard::set_text(t);
            }
            return true;
        }
        case KeyCode::X: {
            const std::string t = selected_text();
            if (!t.empty()) {
                Clipboard::set_text(t);
                do_delete_selection();
                mark_needs_paint();
            }
            return true;
        }
        case KeyCode::V: {
            const std::string clip = Clipboard::get_text();
            if (!clip.empty()) {
                do_insert(clip);
                mark_needs_paint();
            }
            return true;
        }
        default:
            return false;
    }
}

auto RichTextEdit::handle_move_key(KeyEvent &e, bool shift, std::size_t n) -> bool {
    // 方向键
    int dir = 0;
    if (e.key == static_cast<int>(KeyCode::ArrowLeft)) {
        dir = -1;
    } else if (e.key == static_cast<int>(KeyCode::ArrowRight)) {
        dir = 1;
    }
    if (dir != 0) {
        if (shift) {
            const auto nc = static_cast<long long>(caret_) + dir;
            caret_ = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
            sel_end_ = caret_;
        } else {
            if (has_selection()) {
                caret_ = (dir < 0) ? std::min(sel_start_, sel_end_) : std::max(sel_start_, sel_end_);
            } else {
                const auto nc = static_cast<long long>(caret_) + dir;
                caret_ = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
            }
            sel_start_ = sel_end_ = caret_;
        }
        mark_needs_paint();
        e.is_handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::Home)) {
        caret_ = 0;
        if (!shift) {
            sel_start_ = sel_end_ = 0;
        } else {
            sel_end_ = 0;
        }
        mark_needs_paint();
        e.is_handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::End)) {
        caret_ = n;
        if (!shift) {
            sel_start_ = sel_end_ = n;
        } else {
            sel_end_ = n;
        }
        mark_needs_paint();
        e.is_handled = true;
        return true;
    }
    return false;
}

auto RichTextEdit::handle_text_key(KeyEvent &e, std::size_t n) -> bool {
    if (e.key == static_cast<int>(KeyCode::Backspace)) {
        if (has_selection()) {
            do_delete_selection();
        } else if (caret_ > 0) {
            const auto old_doc = doc_;
            const auto old_caret = caret_;
            doc_.erase(doc_.begin() + static_cast<long long>(caret_) - 1);
            --caret_;
            sel_start_ = sel_end_ = caret_;
            push_undo(UndoSnapshot{.description = "backspace", .doc = old_doc, .caret = old_caret});
        }
        mark_needs_paint();
        e.is_handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::Delete)) {
        if (has_selection()) {
            do_delete_selection();
        } else if (caret_ < n) {
            const auto old_doc = doc_;
            const auto old_caret = caret_;
            doc_.erase(doc_.begin() + static_cast<long long>(caret_));
            push_undo(UndoSnapshot{.description = "delete", .doc = old_doc, .caret = old_caret});
        }
        mark_needs_paint();
        e.is_handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::Enter)) {
        do_insert("\n");
        mark_needs_paint();
        e.is_handled = true;
        return true;
    }
    return false;
}

auto RichTextEdit::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
    props["text"] = plain_text();
}

auto RichTextEdit::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("text")) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        if (props["text"].is_string()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
            const std::string t = props["text"].get<std::string>();
            doc_.clear();
            for (const char ch : t) {
                doc_.push_back(StyledChar{.ch = ch, .font = cur_font_, .color = cur_color_, .underline = false});
            }
            caret_ = 0;
            sel_start_ = sel_end_ = 0;
        } else {
            Diagnostics::degraded("text expects string", type_name(), "invalid-prop-value");
        }
    }
}

auto RichTextEdit::relayout_lines(float max_width) -> void {
    lines_.clear();
    Line cur;
    float cur_w = 0.0F;
    for (const auto &sc : doc_) {
        if (sc.ch == '\n') {
            lines_.push_back(std::move(cur));
            cur = Line{};
            cur_w = 0.0F;
            continue;
        }
        const float cw = render::FontEngine::measure_width(std::string(1, sc.ch), sc.font);
        if (cur_w + cw > max_width && !cur.chars.empty()) {
            lines_.push_back(std::move(cur));
            cur = Line{};
            cur_w = 0.0F;
        }
        cur.chars.push_back(sc);
        cur_w += cw;
    }
    lines_.push_back(std::move(cur));
    if (lines_.empty()) {
        lines_.emplace_back();
    }
}

auto RichTextEdit::pos_from_line_col(size_t line_idx, size_t col) const -> size_t {
    size_t offset = 0;
    for (size_t li = 0; li < lines_.size(); ++li) {
        if (li == line_idx) {
            return std::min(offset + col, doc_.size());
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
        offset += lines_[li].chars.size() + 1;  // +1 for '\n'
    }
    return doc_.size();
}

auto RichTextEdit::push_undo(const UndoSnapshot &snap) -> void {
    if (undo_ == nullptr) {
        return;
    }
    auto old_doc = snap.doc;
    auto old_caret = snap.caret;
    auto new_doc = doc_;
    auto new_caret = caret_;
    undo_->push(UndoCommand{
        .redo = [this, d = new_doc, c = new_caret]() -> void {
            doc_ = d;
            caret_ = c;
            sel_start_ = sel_end_ = c;
        },
        .undo = [this, d = old_doc, c = old_caret]() -> void {
            doc_ = d;
            caret_ = c;
            sel_start_ = sel_end_ = c;
        },
        .description = snap.description,
    });
}

auto RichTextEdit::do_insert(const std::string &text) -> void {
    const auto old_doc = doc_;
    const auto old_caret = caret_;
    if (has_selection()) {
        do_delete_selection_no_undo();
    }
    StyledChar proto;
    proto.font = cur_font_;
    proto.color = cur_color_;
    proto.underline = cur_underline_;
    for (const char ch : text) {
        proto.ch = ch;
        doc_.insert(doc_.begin() + static_cast<long long>(caret_), proto);
        ++caret_;
    }
    sel_start_ = sel_end_ = caret_;
    push_undo(UndoSnapshot{.description = "insert", .doc = old_doc, .caret = old_caret});
}

auto RichTextEdit::do_delete_selection() -> void {
    if (!has_selection()) {
        return;
    }
    const auto old_doc = doc_;
    const auto old_caret = caret_;
    do_delete_selection_no_undo();
    push_undo(UndoSnapshot{.description = "delete selection", .doc = old_doc, .caret = old_caret});
}

auto RichTextEdit::do_delete_selection_no_undo() -> void {
    const auto a = std::min(sel_start_, sel_end_);
    const auto b = std::max(sel_start_, sel_end_);
    doc_.erase(doc_.begin() + static_cast<long long>(a), doc_.begin() + static_cast<long long>(b));
    caret_ = a;
    sel_start_ = sel_end_ = a;
}

auto RichTextEdit::paint_selection_highlight(Painter &p, const Rect &bounds) const -> void {
    const auto a = std::min(sel_start_, sel_end_);
    const auto b = std::max(sel_start_, sel_end_);
    constexpr Color sel_bg{51, 153, 255, 80};
    float y = bounds.origin.y;
    size_t idx = 0;
    for (const auto &line : lines_) {
        const size_t line_start = idx;
        const size_t line_end = idx + line.chars.size();
        if (line_end > a && line_start < b) {
            float x_start = bounds.origin.x;
            float x_end = bounds.origin.x;
            float x = bounds.origin.x;
            for (const auto &i : line.chars) {
                const float cw = render::FontEngine::measure_width(std::string(1, i.ch), i.font);
                if (idx >= a && idx < b) {
                    if (idx == a) {
                        x_start = x;
                    }
                    x_end = x + cw;
                }
                x += cw;
                ++idx;
            }
            if (x_end > x_start) {
                p.fill_rect(Rect{.origin = Point{.x = x_start, .y = y},
                                 .size = Size{.width = x_end - x_start, .height = line_height_}},
                            sel_bg);
            }
            ++idx;  // skip conceptual '\n'
        } else {
            idx = line_end + 1;
        }
        y += line_height_;
    }
    (void)a;
    (void)b;  // selection range used above
}

auto RichTextEdit::paint_cursor(Painter &p, const Rect &bounds) const -> void {
    float y = bounds.origin.y;
    size_t idx = 0;
    for (const auto &line : lines_) {
        const size_t line_end = idx + line.chars.size();
        if (caret_ >= idx && caret_ <= line_end) {
            float x = bounds.origin.x;
            for (size_t i = 0; i < line.chars.size() && i + idx < caret_; ++i) {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
                // 容器类型无法本地确证为顺序容器，operator[] 与 .at() 语义不同（map/json 的 [] 会插入键）
                x += render::FontEngine::measure_width(std::string(1, line.chars[i].ch), line.chars[i].font);
            }
            p.fill_rect(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = 1.0F, .height = line_height_}},
                        Color::black());
            return;
        }
        idx = line_end + 1;
        y += line_height_;
    }
}

}  // namespace aurora
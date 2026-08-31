#include "aurora/widget/rich_text_edit.h"

#include "aurora/app/clipboard.h"
#include "aurora/core/diagnostics.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"

namespace aurora {

auto RichTextEdit::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "RichTextEdit",
        .properties = {
            { .name = "text", .type = "string", .default_value = "\"\"", .required = false, .note = "纯文本内容（序列化用）", .json_type = "string" },
            { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
            { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
            { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
        },
        .events = { "on_text_input" },
        .children_policy = "none",
        .examples = { "au::RichTextEdit()" },
    };
}

auto RichTextEdit::on_layout(const Constraints &c, const BuildContext & /*ctx*/) -> Size {
    m_line_height = render::FontEngine::measure_height(m_cur_font);
    if (m_line_height < 1.0f) {
        m_line_height = 20.0f;
    }
    relayout_lines(c.max.width);
    const float h = static_cast<float>(m_lines.size()) * m_line_height;
    return c.constrain(Size{ .width = c.max.width, .height = std::max(h, m_line_height) });
}

auto RichTextEdit::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    if (m_lines.empty()) {
        relayout_lines(bounds.size.width);
    }
    if (has_selection()) {
        paint_selection_highlight(p, bounds);
    }
    float y = bounds.origin.y;
    for (const auto &line : m_lines) {
        float x = bounds.origin.x;
        for (const auto &sc : line.chars) {
            if (sc.ch != '\n') {
                const float cw = render::FontEngine::measure_width(std::string(1, sc.ch), sc.font);
                p.draw_text(
                    Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = cw, .height = m_line_height } },
                    std::string(1, sc.ch), sc.font, sc.color);
                if (sc.underline) {
                    p.fill_rect(Rect{ .origin = Point{ .x = x, .y = y + m_line_height - 2.0f },
                                      .size = Size{ .width = cw, .height = 1.0f } },
                                sc.color);
                }
                x += cw;
            }
        }
        y += m_line_height;
    }
    if (is_focused()) {
        paint_cursor(p, bounds);
    }
}

auto RichTextEdit::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Press && e.button == MouseButton::Left) {
        request_focus();
        const float avg_w = render::FontEngine::measure_width("M", m_cur_font);
        if (avg_w <= 0.0f) {
            return;
        }
        const size_t line_idx = static_cast<size_t>(std::max(0.0f, e.local_position.y) / m_line_height);
        const size_t col = static_cast<size_t>(std::max(0.0f, e.local_position.x) / avg_w);
        m_caret = pos_from_line_col(line_idx, col);
        m_sel_start = m_sel_end = m_caret;
        mark_needs_paint();
        e.handled = true;
    }
}

auto RichTextEdit::on_key_event(KeyEvent &e) -> void {
    if (e.action != KeyAction::Down) {
        return;
    }
    const bool ctrl = (e.modifiers & ModifierKey::Control) != 0u; // NOLINT(*-redundant-parentheses)
    const bool shift = (e.modifiers & ModifierKey::Shift) != 0u;  // NOLINT(*-redundant-parentheses)
    const std::size_t n = m_doc.size();

    if (ctrl && handle_control_shortcut(e, shift, n)) {
        e.handled = true;
        return;
    }
    if (handle_move_key(e, shift, n)) {
        e.handled = true;
        return;
    }
    if (handle_text_key(e, n)) {
        e.handled = true;
    }
}

auto RichTextEdit::handle_control_shortcut(KeyEvent &e, bool shift, std::size_t n) -> bool {
    const auto code = static_cast<KeyCode>(e.key);
    switch (code) {
    case KeyCode::Z:
        // Ctrl+Shift+Z 重做，Ctrl+Z 撤销
        if (shift) {
            if ((m_undo != nullptr) && m_undo->can_redo()) {
                m_undo->redo();
                mark_needs_paint();
            }
        } else {
            if ((m_undo != nullptr) && m_undo->can_undo()) {
                m_undo->undo();
                mark_needs_paint();
            }
        }
        return true;
    case KeyCode::Y:
        if ((m_undo != nullptr) && m_undo->can_redo()) {
            m_undo->redo();
            mark_needs_paint();
        }
        return true;
    case KeyCode::B: toggle_bold(); return true;
    case KeyCode::I: toggle_italic(); return true;
    case KeyCode::U: toggle_underline(); return true;
    case KeyCode::A:
        m_sel_start = 0;
        m_sel_end = n;
        m_caret = n;
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
    default: return false;
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
            const auto nc = static_cast<long long>(m_caret) + dir;
            m_caret = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
            m_sel_end = m_caret;
        } else {
            if (has_selection()) {
                m_caret = (dir < 0) ? std::min(m_sel_start, m_sel_end) : std::max(m_sel_start, m_sel_end);
            } else {
                const auto nc = static_cast<long long>(m_caret) + dir;
                m_caret = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
            }
            m_sel_start = m_sel_end = m_caret;
        }
        mark_needs_paint();
        e.handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::Home)) {
        m_caret = 0;
        if (!shift) {
            m_sel_start = m_sel_end = 0;
        } else {
            m_sel_end = 0;
        }
        mark_needs_paint();
        e.handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::End)) {
        m_caret = n;
        if (!shift) {
            m_sel_start = m_sel_end = n;
        } else {
            m_sel_end = n;
        }
        mark_needs_paint();
        e.handled = true;
        return true;
    }
    return false;
}

auto RichTextEdit::handle_text_key(KeyEvent &e, std::size_t n) -> bool {
    if (e.key == static_cast<int>(KeyCode::Backspace)) {
        if (has_selection()) {
            do_delete_selection();
        } else if (m_caret > 0) {
            const auto old_doc = m_doc;
            const auto old_caret = m_caret;
            m_doc.erase(m_doc.begin() + static_cast<long long>(m_caret) - 1);
            --m_caret;
            m_sel_start = m_sel_end = m_caret;
            push_undo(UndoSnapshot{ .description = "backspace", .doc = old_doc, .caret = old_caret });
        }
        mark_needs_paint();
        e.handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::Delete)) {
        if (has_selection()) {
            do_delete_selection();
        } else if (m_caret < n) {
            const auto old_doc = m_doc;
            const auto old_caret = m_caret;
            m_doc.erase(m_doc.begin() + static_cast<long long>(m_caret));
            push_undo(UndoSnapshot{ .description = "delete", .doc = old_doc, .caret = old_caret });
        }
        mark_needs_paint();
        e.handled = true;
        return true;
    }
    if (e.key == static_cast<int>(KeyCode::Enter)) {
        do_insert("\n");
        mark_needs_paint();
        e.handled = true;
        return true;
    }
    return false;
}

auto RichTextEdit::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["text"] = plain_text();
}

auto RichTextEdit::deserialize_props(const Json &props) -> void {
    Widget::deserialize_props(props);
    if (props.contains("text")) {
        if (props["text"].is_string()) {
            const std::string t = props["text"].get<std::string>();
            m_doc.clear();
            for (const char ch : t) {
                m_doc.push_back(StyledChar{ .ch = ch, .font = m_cur_font, .color = m_cur_color, .underline = false });
            }
            m_caret = 0;
            m_sel_start = m_sel_end = 0;
        } else {
            Diagnostics::degraded("text expects string", type_name(), "invalid-prop-value");
        }
    }
}

auto RichTextEdit::relayout_lines(float max_width) -> void {
    m_lines.clear();
    Line cur;
    float cur_w = 0.0f;
    for (const auto &sc : m_doc) {
        if (sc.ch == '\n') {
            m_lines.push_back(std::move(cur));
            cur = Line{};
            cur_w = 0.0f;
            continue;
        }
        const float cw = render::FontEngine::measure_width(std::string(1, sc.ch), sc.font);
        if (cur_w + cw > max_width && !cur.chars.empty()) {
            m_lines.push_back(std::move(cur));
            cur = Line{};
            cur_w = 0.0f;
        }
        cur.chars.push_back(sc);
        cur_w += cw;
    }
    m_lines.push_back(std::move(cur));
    if (m_lines.empty()) {
        m_lines.emplace_back();
    }
}

auto RichTextEdit::pos_from_line_col(size_t line_idx, size_t col) const -> size_t {
    size_t offset = 0;
    for (size_t li = 0; li < m_lines.size(); ++li) {
        if (li == line_idx) {
            return std::min(offset + col, m_doc.size());
        }
        offset += m_lines[li].chars.size() + 1; // +1 for '\n'
    }
    return m_doc.size();
}

auto RichTextEdit::push_undo(const UndoSnapshot &snap) -> void {
    if (m_undo == nullptr) {
        return;
    }
    auto old_doc = snap.doc;
    auto old_caret = snap.caret;
    auto new_doc = m_doc;
    auto new_caret = m_caret;
    m_undo->push(UndoCommand{
        .redo = [this, d = new_doc, c = new_caret]() -> void {
            m_doc = d;
            m_caret = c;
            m_sel_start = m_sel_end = c;
        },
        .undo = [this, d = old_doc, c = old_caret]() -> void {
            m_doc = d;
            m_caret = c;
            m_sel_start = m_sel_end = c;
        },
        .description = snap.description,
    });
}

auto RichTextEdit::do_insert(const std::string &text) -> void {
    const auto old_doc = m_doc;
    const auto old_caret = m_caret;
    if (has_selection()) {
        do_delete_selection_no_undo();
    }
    StyledChar proto;
    proto.font = m_cur_font;
    proto.color = m_cur_color;
    proto.underline = m_cur_underline;
    for (const char ch : text) {
        proto.ch = ch;
        m_doc.insert(m_doc.begin() + static_cast<long long>(m_caret), proto);
        ++m_caret;
    }
    m_sel_start = m_sel_end = m_caret;
    push_undo(UndoSnapshot{ .description = "insert", .doc = old_doc, .caret = old_caret });
}

auto RichTextEdit::do_delete_selection() -> void {
    if (!has_selection()) {
        return;
    }
    const auto old_doc = m_doc;
    const auto old_caret = m_caret;
    do_delete_selection_no_undo();
    push_undo(UndoSnapshot{ .description = "delete selection", .doc = old_doc, .caret = old_caret });
}

auto RichTextEdit::do_delete_selection_no_undo() -> void {
    const auto a = std::min(m_sel_start, m_sel_end);
    const auto b = std::max(m_sel_start, m_sel_end);
    m_doc.erase(m_doc.begin() + static_cast<long long>(a), m_doc.begin() + static_cast<long long>(b));
    m_caret = a;
    m_sel_start = m_sel_end = a;
}

auto RichTextEdit::paint_selection_highlight(Painter &p, const Rect &bounds) const -> void {
    const auto a = std::min(m_sel_start, m_sel_end);
    const auto b = std::max(m_sel_start, m_sel_end);
    constexpr Color sel_bg{ 51, 153, 255, 80 };
    float y = bounds.origin.y;
    size_t idx = 0;
    for (const auto &line : m_lines) {
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
                p.fill_rect(Rect{ .origin = Point{ .x = x_start, .y = y },
                                  .size = Size{ .width = x_end - x_start, .height = m_line_height } },
                            sel_bg);
            }
            ++idx; // skip conceptual '\n'
        } else {
            idx = line_end + 1;
        }
        y += m_line_height;
    }
    (void)a;
    (void)b; // selection range used above
}

auto RichTextEdit::paint_cursor(Painter &p, const Rect &bounds) const -> void {
    float y = bounds.origin.y;
    size_t idx = 0;
    for (const auto &line : m_lines) {
        const size_t line_end = idx + line.chars.size();
        if (m_caret >= idx && m_caret <= line_end) {
            float x = bounds.origin.x;
            for (size_t i = 0; i < line.chars.size() && i + idx < m_caret; ++i) {
                x += render::FontEngine::measure_width(std::string(1, line.chars[i].ch), line.chars[i].font);
            }
            p.fill_rect(
                Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = 1.0f, .height = m_line_height } },
                Color::black());
            return;
        }
        idx = line_end + 1;
        y += m_line_height;
    }
}

} // namespace aurora

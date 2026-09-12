#include "aurora/widget/rich_text_edit.h"

// 完整 BuildContext 定义（explicit_text_direction 模板在 on_layout 内实例化，需用到
// ctx.environment<Directionality>()，前向声明不足以编译）。
#include "aurora/environment/build_context.h"

#include "aurora/app/clipboard.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/directionality.h"
#include "aurora/core/utf8.h"
#include "aurora/event/keycode.h"
#include <algorithm>
#include <cmath>

#include "aurora/render/bidi.h"
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
                {.name = "direction",
                 .type = "TextDirection",
                 .default_value = "\"auto\"",
                 .required = false,
                 .note = "书写方向(auto=继承环境)",
                 .json_type = "string"},
            },
        .events = {"on_text_input", "on_text_composition"},
        .children_policy = "none",
        .examples = {"au::RichTextEdit()"},
    };
}

auto RichTextEdit::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    line_height_ = render::FontEngine::measure_height(cur_font_);
    if (line_height_ < 1.0F) {
        line_height_ = 20.0F;
    }
    // A2：解析生效书写方向（控件显式属性 > Environment 注入 > 进程级；均无则 nullopt，
    // shaping 保持按内容 guess，默认行为逐位不变）。
    cached_direction_ = direction_.has_value() ? direction_ : explicit_text_direction(ctx);
    relayout_lines(c.max.width);
    const float h = static_cast<float>(lines_.size()) * line_height_;
    return c.constrain(Size{.width = c.max.width, .height = std::max(h, line_height_)});
}

auto RichTextEdit::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    if (lines_.empty()) {
        relayout_lines(bounds.size.width);
    }
    const float line_h = line_height_;
    std::size_t doc_idx = 0;
    float y = bounds.origin.y;
    for (const auto &line : lines_) {
        const std::size_t line_begin = doc_idx;
        const std::size_t line_end = doc_idx + line.chars.size();

        std::string line_text;
        line_text.reserve(line.chars.size());
        for (const auto &c : line.chars) {
            line_text.push_back(c.ch);
        }
        // A2：段落基准方向（显式 direction 优先，否则按该行内容 guess）。
        const TextDirection base =
            cached_direction_.value_or(aurora::render::detail::guess_paragraph_direction(line_text));

        if (has_selection()) {
            const auto a = std::min(sel_start_, sel_end_);
            const auto b = std::max(sel_start_, sel_end_);
            if (line_end > a && line_begin < b) {
                paint_selection_highlight(p, bounds);
            }
        }

        // 逐 run 整形绘制（run 内 hb 按自身内容方向 shaping，跨 run 顺序按段落基准方向）。
        const auto runs = compute_line_runs(line, base, bounds);
        for (const auto &r : runs) {
            const TextDirection run_dir = r.dir;  // UBA 层级奇偶（compute_line_runs 预解析）
            const render::TextLayoutOpts ropts{.direction = run_dir};
            render::FontEngine::draw_text(
                p, Rect{.origin = Point{.x = r.x, .y = y}, .size = Size{.width = r.w, .height = line_h}}, r.text,
                r.font, r.color, ropts);
            // 逐字下划线（仅带 underline 者；caret_x 返回视觉偏移，RTL 下仍正确）。
            for (std::size_t k = 0; k < r.text.size(); ++k) {
                if (r.underline[k] == 0) {
                    continue;
                }
                const float x0 = r.x + render::FontEngine::caret_x(r.text, k, r.font, ropts);
                const float x1 = r.x + render::FontEngine::caret_x(r.text, k + 1, r.font, ropts);
                const float xL = std::min(x0, x1);
                const float xR = std::max(x0, x1);
                if (xR > xL) {
                    p.fill_rect(Rect{.origin = Point{.x = xL, .y = y + line_h - 2.0F},
                                     .size = Size{.width = xR - xL, .height = 1.0F}},
                                r.color);
                }
            }
        }

        // 组合串绘制（A1）：在文档光标处叠加 preedit（下划线 + 组合内选区 + 候选插入点光标）。
        if (is_composing() && caret_ >= line_begin && caret_ <= line_end) {
            const float cx = caret_visual_x(line, base, caret_ - line_begin, bounds);
            if (cx >= 0.0F) {
                paint_preedit(p, cx, y);
            }
        }

        y += line_h;
        doc_idx = line_end + 1;
    }
    // 组合时光标画在 preedit 内（`paint_preedit` 负责），避免与文档光标重叠。
    if (is_focused() && !is_composing()) {
        paint_cursor(p, bounds);
    }
}

auto RichTextEdit::on_pointer_event(MouseEvent &e) -> void {
    if (e.action == MouseAction::Press && e.button == MouseButton::Left) {
        request_focus();
        const float line_h = line_height_;
        if (line_h <= 0.0F || lines_.empty()) {
            e.is_handled = true;
            return;
        }
        const std::size_t line_idx =
            static_cast<std::size_t>(std::max(0.0F, e.local_position.y) / line_h);
        // 命中测试用控件本地坐标（左缘为 0），与绘制侧 run 布局一致。
        const Rect bounds{.origin = Point{0.0F, 0.0F}, .size = size()};
        if (line_idx >= lines_.size()) {
            caret_ = doc_.size();
            sel_start_ = sel_end_ = caret_;
            mark_needs_paint();
            e.is_handled = true;
            return;
        }
        // 累加得到该行在 doc_ 中的起始下标。
        std::size_t doc_idx = 0;
        for (std::size_t li = 0; li < line_idx; ++li) {
            doc_idx += lines_[li].chars.size() + 1;
        }
        const auto &line = lines_[line_idx];
        const std::size_t line_begin = doc_idx;

        std::string line_text;
        for (const auto &c : line.chars) {
            line_text.push_back(c.ch);
        }
        // A2：段落基准方向（显式 direction 优先，否则按行内容 guess）。
        const TextDirection base =
            cached_direction_.value_or(aurora::render::detail::guess_paragraph_direction(line_text));
        const auto runs = compute_line_runs(line, base, bounds);

        // 取距点击 x 最近 run 中的命中字符（含头含尾，消除端点 off-by-one）。
        std::size_t best_doc = line_begin;
        float best_dist = 1e18F;
        for (const auto &r : runs) {
            const float rx0 = r.x;
            const float rx1 = r.x + r.w;
            const float clamped = std::clamp(e.local_position.x, rx0, rx1);
            const float d = std::fabs(e.local_position.x - clamped);
            if (d < best_dist) {
                best_dist = d;
                const TextDirection run_dir = r.dir;  // UBA 层级奇偶（compute_line_runs 预解析）
                const render::TextLayoutOpts ropts{.direction = run_dir};
                const std::size_t idx =
                    render::FontEngine::hit_test_char_inclusive(r.text, e.local_position.x - r.x, r.font, ropts);
                best_doc = line_begin + r.begin + idx;
            }
        }
        caret_ = best_doc;
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
    // 方向键（A2 RTL：逻辑↔视觉镜像——RTL 下 ArrowLeft = 逻辑前进）。
    int dir = 0;
    const bool rtl =
        cached_direction_.value_or(aurora::render::detail::guess_paragraph_direction(plain_text())) ==
        TextDirection::RTL;
    if (e.key == static_cast<int>(KeyCode::ArrowLeft)) {
        dir = rtl ? 1 : -1;
    } else if (e.key == static_cast<int>(KeyCode::ArrowRight)) {
        dir = rtl ? -1 : 1;
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
    // A2：仅显式设置时序列化方向（继承环境语义不落盘）。
    if (direction_.has_value()) {
        props["direction"] = (*direction_ == TextDirection::RTL) ? "RTL" : "LTR";
    }
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
    // A2：书写方向反序列化（仅当显式提供）。auto/继承环境由运行期 Directionality 注入决定。
    if (props.contains("direction") && props["direction"].is_string()) {
        direction_ = props["direction"].get<std::string>() == "RTL" ? TextDirection::RTL : TextDirection::LTR;
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

auto RichTextEdit::compute_line_runs(const Line &line, TextDirection base, const Rect &bounds) const
    -> std::vector<RunLayout> {
    using aurora::render::detail::guess_paragraph_direction;
    std::vector<RunLayout> runs;
    const std::size_t n = line.chars.size();
    std::size_t i = 0;
    while (i < n) {
        const auto &sc = line.chars[i];
        RunLayout r;
        r.begin = i;
        std::string s;
        std::vector<char> ul;
        s.push_back(sc.ch);
        ul.push_back(sc.underline ? 1 : 0);
        std::size_t j = i + 1;
        while (j < n && line.chars[j].font == sc.font && line.chars[j].color == sc.color) {
            s.push_back(line.chars[j].ch);
            ul.push_back(line.chars[j].underline ? 1 : 0);
            ++j;
        }
        r.end = j;
        r.text = std::move(s);
        r.underline = std::move(ul);
        r.font = sc.font;
        r.color = sc.color;
        runs.push_back(std::move(r));
        i = j;
    }

    // 逐样式 run 解码码点并按完整 UBA（UAX #9）求逐码点嵌入层级；样式 run 内层级变化处
    // 再切分为层级 run（层级单一 → hb 方向 = 层级奇偶），随后跨 run 按 L2 重排。
    const std::uint8_t base_level = (base == TextDirection::RTL) ? 1U : 0U;
    auto decode_cp = [](const std::string &s, std::size_t &p) -> char32_t {
        const auto b0 = static_cast<unsigned char>(s[p]);
        std::size_t len = 1U;
        std::uint32_t cp = b0;
        if ((b0 & 0x80U) != 0U) {
            const std::size_t extra = ((b0 & 0xE0U) == 0xC0U) ? 1U
                : ((b0 & 0xF0U) == 0xE0U)                     ? 2U
                : ((b0 & 0xF8U) == 0xF0U)                     ? 3U
                                                              : 0U;
            cp = static_cast<std::uint32_t>(b0 & static_cast<unsigned char>(0xFFU >> (extra + 1U)));
            len = 1U + extra;
            for (std::size_t q = 1U; q <= extra && p + q < s.size(); ++q) {
                cp = (cp << 6U) | (static_cast<unsigned char>(s[p + q]) & 0x3FU);
            }
        }
        p += len;
        return static_cast<char32_t>(cp);
    };
    std::vector<RunLayout> level_runs;
    std::vector<std::uint8_t> run_levels;
    for (auto &r : runs) {
        std::vector<char32_t> cps;
        for (std::size_t p = 0; p < r.text.size();) {
            cps.push_back(decode_cp(r.text, p));
        }
        const auto lv = render::detail::uba_levels(cps, base_level);
        std::size_t s = 0;
        while (s < cps.size()) {
            std::size_t e = s + 1;
            while (e < cps.size() && lv[e] == lv[s]) {
                ++e;
            }
            // 子 run = [s, e)：按码点字节区间取子串并保留逐字下划线标记。
            RunLayout sub;
            sub.begin = r.begin + s;
            sub.end = r.begin + e;
            std::size_t byte_s = 0;
            std::size_t byte_e = r.text.size();
            {
                std::size_t p = 0;
                std::size_t idx = 0;
                while (idx < e && p < r.text.size()) {
                    const std::size_t byte_start = p;
                    decode_cp(r.text, p);
                    if (idx == s) {
                        byte_s = byte_start;
                    }
                    if (idx == e - 1U) {
                        byte_e = p;
                    }
                    ++idx;
                }
            }
            sub.text = r.text.substr(byte_s, byte_e - byte_s);
            sub.underline.assign(sub.text.size(), 0);
            for (std::size_t k = 0; k < sub.underline.size(); ++k) {
                sub.underline[k] = r.underline[s + k];
            }
            sub.font = r.font;
            sub.color = r.color;
            sub.dir = (lv[s] % 2U != 0U) ? TextDirection::RTL : TextDirection::LTR;
            run_levels.push_back(lv[s]);
            level_runs.push_back(std::move(sub));
            s = e;
        }
    }

    // 逐 run 测宽（方向取 UBA 层级奇偶）。
    for (auto &r : level_runs) {
        r.w = render::FontEngine::measure_width(r.text, r.font, render::TextLayoutOpts{.direction = r.dir});
    }

    // 跨 run 视觉重排：完整 UBA L2 层叠反转（RTL 段整体翻转/段内数字内序保持等均由此推出）。
    const auto order = render::detail::uba_visual_order(run_levels);
    float total_w = 0.0F;
    for (const auto &r : level_runs) {
        total_w += r.w;
    }

    // 视觉起点：LTR 左对齐（控件左缘）；RTL 整体右对齐（右缘 - 总宽）。
    const float x0 = (base == TextDirection::RTL) ? bounds.origin.x + bounds.size.width - total_w
                                                 : bounds.origin.x;
    float cursor = x0;
    for (const std::size_t idx : order) {
        if (idx < level_runs.size()) {
            level_runs[idx].x = cursor;
            cursor += level_runs[idx].w;
        }
    }
    return level_runs;
}

auto RichTextEdit::caret_visual_x(const Line &line, TextDirection base, std::size_t caret_local,
                                 const Rect &bounds) const -> float {
    const auto runs = compute_line_runs(line, base, bounds);
    for (const auto &r : runs) {
        if (caret_local >= r.begin && caret_local <= r.end) {
            const std::size_t k = caret_local - r.begin;
            const TextDirection run_dir = r.dir;  // UBA 层级奇偶（compute_line_runs 预解析）
            return r.x + render::FontEngine::caret_x(r.text, k, r.font, render::TextLayoutOpts{.direction = run_dir});
        }
    }
    return -1.0F;
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
    const float line_h = line_height_;
    std::size_t doc_idx = 0;
    float y = bounds.origin.y;
    for (const auto &line : lines_) {
        const std::size_t line_begin = doc_idx;
        const std::size_t line_end = doc_idx + line.chars.size();
        if (line_end > a && line_begin < b) {
            std::string line_text;
            for (const auto &c : line.chars) {
                line_text.push_back(c.ch);
            }
            const TextDirection base =
                cached_direction_.value_or(aurora::render::detail::guess_paragraph_direction(line_text));
            const auto runs = compute_line_runs(line, base, bounds);
            for (const auto &r : runs) {
                for (std::size_t k = 0; k < r.text.size(); ++k) {
                    const std::size_t doc_pos = line_begin + r.begin + k;
                    if (doc_pos >= a && doc_pos < b) {
                        const TextDirection run_dir = r.dir;  // UBA 层级奇偶（compute_line_runs 预解析）
                        const render::TextLayoutOpts ropts{.direction = run_dir};
                        const float x0 = r.x + render::FontEngine::caret_x(r.text, k, r.font, ropts);
                        const float x1 = r.x + render::FontEngine::caret_x(r.text, k + 1, r.font, ropts);
                        const float xL = std::min(x0, x1);
                        const float xR = std::max(x0, x1);
                        if (xR > xL) {
                            p.fill_rect(Rect{.origin = Point{.x = xL, .y = y},
                                             .size = Size{.width = xR - xL, .height = line_h}},
                                        sel_bg);
                        }
                    }
                }
            }
        }
        y += line_h;
        doc_idx = line_end + 1;
    }
}

auto RichTextEdit::paint_preedit(Painter &p, float x, float y) const -> float {
    constexpr Color preedit_underline{30, 110, 220, 255};
    constexpr Color preedit_selection{255, 200, 80, 140};
    const float w = render::FontEngine::measure_width(preedit_, cur_font_);
    const std::size_t pn = utf8_cp_count(preedit_);

    // ① preedit 内选区（输入法高亮「待转换片段」）
    if (preedit_sel_end_ != kNoPreeditSelection) {
        const auto a = std::min(preedit_sel_start_, preedit_sel_end_);
        const auto b = std::min(std::max(preedit_sel_start_, preedit_sel_end_) + 1U, pn);
        const float sx0 = x + render::FontEngine::measure_width(utf8_cp_slice(preedit_, 0, a), cur_font_);
        const float sx1 = x + render::FontEngine::measure_width(utf8_cp_slice(preedit_, 0, b), cur_font_);
        if (sx1 > sx0) {
            p.fill_rect(
                Rect{.origin = Point{.x = sx0, .y = y}, .size = Size{.width = sx1 - sx0, .height = line_height_}},
                preedit_selection);
        }
    }

    // ② preedit 文本 + 组合下划线（区别于正式文本）
    p.draw_text(Rect{.origin = Point{.x = x, .y = y}, .size = Size{.width = w, .height = line_height_}}, preedit_,
                cur_font_, cur_color_);
    p.fill_rect(Rect{.origin = Point{.x = x, .y = y + line_height_ - 2.0F}, .size = Size{.width = w, .height = 2.0F}},
                preedit_underline);

    // ③ 候选插入点光标（仅聚焦时；组合期间它取代文档光标）
    if (is_focused()) {
        const float cx = x + render::FontEngine::measure_width(utf8_cp_slice(preedit_, 0, preedit_cursor_), cur_font_);
        p.fill_rect(Rect{.origin = Point{.x = cx, .y = y}, .size = Size{.width = 2.0F, .height = line_height_}},
                    Color::black());
    }
    return w;
}

auto RichTextEdit::cancel_composition() -> void {
    preedit_.clear();
    preedit_cursor_ = 0;
    preedit_sel_start_ = 0;
    preedit_sel_end_ = kNoPreeditSelection;
}

auto RichTextEdit::paint_cursor(Painter &p, const Rect &bounds) const -> void {
    const float line_h = line_height_;
    std::size_t doc_idx = 0;
    float y = bounds.origin.y;
    for (const auto &line : lines_) {
        const std::size_t line_begin = doc_idx;
        const std::size_t line_end = doc_idx + line.chars.size();
        if (caret_ >= line_begin && caret_ <= line_end) {
            std::string line_text;
            for (const auto &c : line.chars) {
                line_text.push_back(c.ch);
            }
            // A2：段落基准方向（显式 direction 优先，否则按行内容 guess）。
            const TextDirection base =
                cached_direction_.value_or(aurora::render::detail::guess_paragraph_direction(line_text));
            const float cx = caret_visual_x(line, base, caret_ - line_begin, bounds);
            if (cx >= 0.0F) {
                p.fill_rect(Rect{.origin = Point{.x = cx, .y = y}, .size = Size{.width = 1.0F, .height = line_h}},
                            Color::black());
            }
            return;
        }
        doc_idx = line_end + 1;
        y += line_h;
    }
}

}  // namespace aurora
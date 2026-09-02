#include "aurora/widget/text.h"

#include "aurora/app/clipboard.h"
#include "aurora/core/diagnostics.h"
#include "aurora/core/utf8.h"
#include "aurora/event/keycode.h"
#include "aurora/render/font_engine.h"
#include "aurora/widget/descriptor.h"

namespace aurora {

auto Text::describe_static() -> WidgetDescriptor {
    return WidgetDescriptor{
        .name = "Text",
        .properties = {
            { .name = "content", .type = "LocalizedString", .default_value = "\"\"", .required = true, .note = "文本内容", .json_type = "string" },
            { .name = "font_size", .type = "float", .default_value = "14.0", .required = false, .note = "字号(pt)", .json_type = "number", .enum_values = {}, .min_value = "0" },
            { .name = "color", .type = "Color", .default_value = "Color::black()", .required = false, .note = "文字色", .json_type = "array" },
            { .name = "text_align", .type = "TextAlign", .default_value = "Left", .required = false, .note = "水平对齐", .json_type = "string",
              .enum_values = {"Left", "Right", "Center", "Start", "End", "Justify"} },
            { .name = "max_lines", .type = "int", .default_value = "0", .required = false, .note = "最大行数(0=不限)", .json_type = "integer", .enum_values = {}, .min_value = "0" },
            { .name = "overflow", .type = "TextOverflow", .default_value = "Clip", .required = false, .note = "溢出处理", .json_type = "string",
              .enum_values = {"Clip", "Ellipsis", "Fade"} },
            { .name = "soft_wrap", .type = "bool", .default_value = "true", .required = false, .note = "自动换行", .json_type = "boolean" },
            { .name = "line_height", .type = "float", .default_value = "1.0", .required = false, .note = "行高倍数", .json_type = "number", .enum_values = {}, .min_value = "0" },
            { .name = "letter_spacing", .type = "float", .default_value = "0.0", .required = false, .note = "字形间距", .json_type = "number" },
            { .name = "word_spacing", .type = "float", .default_value = "0.0", .required = false, .note = "词间距", .json_type = "number" },
            { .name = "font_weight", .type = "FontWeight", .default_value = "Normal", .required = false, .note = "字重", .json_type = "string",
              .enum_values = {"Thin", "ExtraLight", "Light", "Normal", "Medium", "SemiBold", "Bold", "ExtraBold", "Black"} },
            { .name = "font_style", .type = "FontStyle", .default_value = "Normal", .required = false, .note = "字形风格", .json_type = "string",
              .enum_values = {"Normal", "Italic"} },
            { .name = "decoration", .type = "TextDecoration", .default_value = "None", .required = false, .note = "装饰线", .json_type = "string",
              .enum_values = {"None", "Underline", "Overline", "LineThrough"} },
            { .name = "decoration_color", .type = "Color", .default_value = "Color::black()", .required = false, .note = "装饰线颜色", .json_type = "array" },
            { .name = "background_color", .type = "Color", .default_value = "transparent", .required = false, .note = "文本底色", .json_type = "array" },
            { .name = "width", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
            { .name = "height", .type = "Length", .default_value = "auto", .required = false, .note = "", .json_type = "array" },
            { .name = "show", .type = "bool", .default_value = "true", .required = false, .note = "", .json_type = "boolean" },
        },
        .events = {},
        .children_policy = "none",
        .examples = { "au::Text(\"Hello\")", "au::Text(au::TextProps{ .content = \"Hi\" }).font_size(16)" },
    };
}

auto Text::serialize_props(Json &props) const -> void {
    Widget::serialize_props(props);
    props["content"] = content.get().text;
    props["font_size"] = font.size_pt;
    props["color"] = color_to_json(text_color);

    props["text_align"] = text_align_to_json(text_align);
    props["max_lines"] = max_lines;
    props["overflow"] = text_overflow_to_json(overflow);
    props["soft_wrap"] = soft_wrap;
    props["line_height"] = line_height;
    props["letter_spacing"] = letter_spacing;
    props["word_spacing"] = word_spacing;
    props["font_weight"] = font_weight_to_json(static_cast<FontWeight>(font.weight));
    props["font_style"] = font_style_to_json(font_style);
    props["decoration"] = text_decoration_to_json(decoration);
    props["decoration_color"] = color_to_json(decoration_color);
    props["background_color"] = color_to_json(background_color);
}

auto Text::deserialize_props(const Json &props) -> void { // NOLINT(readability-function-cognitive-complexity)
    Widget::deserialize_props(props);
    if (props.contains("content")) {
        static const PropDescriptor d_content{ .name = "content", .json_type = "string" };
        content.set(validate_or_default<LocalizedString>(props["content"], d_content, LocalizedString{}));
    }
    if (props.contains("font_size")) {
        static const PropDescriptor d_font_size{ .name = "font_size", .json_type = "number", .min_value = "0" };
        font.size_pt = validate_or_default<float>(props["font_size"], d_font_size, 14.0f);
    }
    if (props.contains("color")) {
        static const PropDescriptor d_color{ .name = "color", .json_type = "array" };
        text_color = validate_or_default<Color>(props["color"], d_color, Color::black());
    }
    if (props.contains("text_align")) {
        if (props["text_align"].is_string()) {
            text_align = json_to_text_align(props["text_align"]);
        } else {
            Diagnostics::degraded("text_align expects string", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("max_lines")) {
        if (props["max_lines"].is_number()) {
            max_lines = props["max_lines"].get<int>();
        } else {
            Diagnostics::degraded("max_lines expects integer", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("overflow")) {
        if (props["overflow"].is_string()) {
            overflow = json_to_text_overflow(props["overflow"]);
        } else {
            Diagnostics::degraded("overflow expects string", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("soft_wrap")) {
        if (props["soft_wrap"].is_boolean()) {
            soft_wrap = props["soft_wrap"].get<bool>();
        } else {
            Diagnostics::degraded("soft_wrap expects boolean", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("line_height")) {
        if (props["line_height"].is_number()) {
            const float v = props["line_height"].get<float>();
            if (v > 0) {
                line_height = v;
            } else {
                Diagnostics::degraded("line_height must be > 0, got " + std::to_string(v), type_name(),
                                      "invalid-prop-value");
            }
        } else {
            Diagnostics::degraded("line_height expects number", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("letter_spacing")) {
        if (props["letter_spacing"].is_number()) {
            letter_spacing = props["letter_spacing"].get<float>();
        } else {
            Diagnostics::degraded("letter_spacing expects number", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("word_spacing")) {
        if (props["word_spacing"].is_number()) {
            word_spacing = props["word_spacing"].get<float>();
        } else {
            Diagnostics::degraded("word_spacing expects number", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("font_weight")) {
        if (props["font_weight"].is_string() || props["font_weight"].is_number()) {
            font.weight = static_cast<int>(json_to_font_weight(props["font_weight"]));
        } else {
            Diagnostics::degraded("font_weight expects string or number", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("font_style")) {
        if (props["font_style"].is_string()) {
            font_style = json_to_font_style(props["font_style"]);
        } else {
            Diagnostics::degraded("font_style expects string", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("decoration")) {
        decoration = json_to_text_decoration(props["decoration"]);
    }
    if (props.contains("decoration_color")) {
        if (props["decoration_color"].is_array() && props["decoration_color"].size() >= 4) {
            decoration_color = json_to_color(props["decoration_color"]);
        } else {
            Diagnostics::degraded("decoration_color expects [r,g,b,a] array", type_name(), "invalid-prop-value");
        }
    }
    if (props.contains("background_color")) {
        if (props["background_color"].is_array() && props["background_color"].size() >= 4) {
            background_color = json_to_color(props["background_color"]);
        } else {
            Diagnostics::degraded("background_color expects [r,g,b,a] array", type_name(), "invalid-prop-value");
        }
    }
}

auto Text::validate_props() const -> Result<void> {
    if (font.size_pt <= 0.0f) {
        return make_error(ErrorCode::WidgetInvalidProp,
                          "Text.font_size must be > 0, got " + std::to_string(font.size_pt),
                          "Use positive font size (e.g. 14)");
    }
    return Result<void>{};
}

auto Text::on_layout(const Constraints &c, const BuildContext &ctx) -> Size {
    const Font f = effective_font(font);
    // resolved_text 缓存：on_layout 在 content 变化时必被调用，此处计算并缓存
    m_cached_resolved_text = resolved_text(ctx);
    m_resolved_dirty = false;
    const std::string &s = m_cached_resolved_text;
    m_display_text = s; // 缓存显示文本供命中测试/选区：即便尚未绘制，拖选也应可用
    const bool bounded = std::isfinite(c.max.width);
    const float max_w = bounded ? c.max.width : 1e9f;
    const render::TextLayoutOpts opts{ .letter_spacing = letter_spacing,
                                       .word_spacing = word_spacing,
                                       .italic = (font_style == FontStyle::Italic) };
    m_line_h = render::FontEngine::measure_height(f) * std::max(0.1f, line_height);
    auto [lines, cp_start] = wrap_lines(s, f, max_w, soft_wrap, max_lines, overflow, opts);
    m_lines = std::move(lines);
    m_line_cp_start = std::move(cp_start);

    // 文本固有（单行）宽度。soft_wrap 仅当文本确需换行（固有宽度超出约束）时才填满整行，
    // 否则按内容宽度上报——避免「默认 soft_wrap=true」的相邻文本互相抢占空间、命中盒重叠、
    // 导致后者无法选中的问题；长段落仍正常换行填满。
    const float intrinsic = render::FontEngine::measure_width(s, f, opts) + 2.0f;
    float w = 0.0f;
    if (bounded && soft_wrap) {
        w = (intrinsic <= max_w) ? intrinsic : c.max.width;
    } else {
        w = intrinsic;
    }
    const float h = (static_cast<float>(m_lines.empty() ? 1 : m_lines.size()) * m_line_h) + 2.0f;
    const Size sz = c.constrain(Size{ .width = std::max(1.0f, w), .height = std::max(1.0f, h) });
    m_layout_w = sz.width;
    return sz;
}

auto Text::on_pointer_event(MouseEvent &e) -> void {
    // 保留基类行为：Clickable 回调 / activate / 长按计时。
    // 注意：Text 不在此无条件置 e.handled —— 仅当自身带 Clickable（wants_click）时基类已标记；
    // 否则事件冒泡给父级（如父 Clickable 容器），避免「可点击父级 + Text 子」时父点击被吞。
    Widget::on_pointer_event(e);

    if (m_display_text.empty()) {
        return;
    }
    const Font f = effective_font(font);
    const render::TextLayoutOpts opts{ .letter_spacing = letter_spacing,
                                       .word_spacing = word_spacing,
                                       .italic = (font_style == FontStyle::Italic) };
    const float lx = e.local_position.x;
    const float ly = e.local_position.y;
    // 命中测试须先按 ly 定位到可视行，再在该行内按 x 命中——否则多行文本会被当成「整段单行」
    // 处理，点击第 2 行的小 x 会落回第 1 行，导致选区跨越多行（不可预期）。
    if (m_lines.empty() || m_line_h <= 0.0f) {
        return;
    }
    const std::size_t li =
        std::min<std::size_t>(static_cast<std::size_t>(std::max(0.0f, ly) / m_line_h), m_lines.size() - 1u);
    // 按对齐方式计算该行文本相对控件左缘的水平偏移（local 坐标下控件左缘为 0）。
    const float line_w = render::FontEngine::measure_width(m_lines[li], f, opts);
    float line_off = 0.0f;
    switch (text_align) {
    case TextAlign::Right:
    case TextAlign::End: line_off = m_layout_w - line_w; break;
    case TextAlign::Center: line_off = (m_layout_w - line_w) * 0.5f; break;
    default: line_off = 0.0f; break;
    }
    // 含头含尾：ch 为点击所在字符的码点下标（该字符应被选中）；
    // rawCaret 为同位置的 caret 位置（用于单击定位 / 键盘导航起点）。
    // Justify 行经 line_hit_test 按逐词均分布局反解，与绘制位置一致；
    // 非 Justify 行走实显命中（字符边界取物理 DPI 前缀 extent，与实绘字形逐字符对齐）。
    const auto [raw_caret, ch] = line_hit_test(li, lx - line_off, f, opts, m_layout_w);
    const size_t cp = m_line_cp_start[li] + ch;

    if (e.action == MouseAction::Press) {
        m_caret = m_line_cp_start[li] + raw_caret;
        m_sel_start = cp;     // 锚点（含入字符）
        m_sel_end = m_no_sel; // 尚未形成选区，待拖拽
        m_selecting = true;
        request_focus();
        mark_needs_paint();
    } else if (e.action == MouseAction::Move && m_selecting) {
        m_caret = m_line_cp_start[li] + raw_caret;
        m_sel_end = cp; // 拖拽终点（含入字符）：按下与松开所在字符均计入选区
        mark_needs_paint();
    } else if (e.action == MouseAction::Release) {
        m_selecting = false;
    }
}

auto Text::on_key_event(KeyEvent &e) -> void {
    if (!is_focused() || m_display_text.empty() || e.action != KeyAction::Down) {
        return;
    }
    const Font f = effective_font(font);
    const bool shift = (e.modifiers & ModifierKey::Shift) != 0u;  // NOLINT(*-redundant-parentheses)
    const bool ctrl = (e.modifiers & ModifierKey::Control) != 0u; // NOLINT(*-redundant-parentheses)
    const size_t n = cp_count(m_display_text);

    if (ctrl && e.key == static_cast<int>(KeyCode::A)) {
        m_sel_start = 0;
        m_sel_end = (n == 0) ? m_no_sel : n - 1; // 含尾：末字符下标 n-1
        m_caret = n;
        mark_needs_paint();
        e.handled = true;
        return;
    }
    if (ctrl && e.key == static_cast<int>(KeyCode::C)) {
        const std::string t = selected_text();
        if (!t.empty()) {
            Clipboard::set_text(t);
            e.handled = true;
        }
        return;
    }

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
            // 含尾：向右延伸覆盖到 caret 前的字符，向左覆盖到 caret 处字符。
            if (dir > 0) {
                m_sel_end = (m_caret == 0) ? 0 : m_caret - 1;
            } else {
                m_sel_end = m_caret;
            }
        } else {
            m_sel_start = m_caret; // 锚点 = 当前 caret
            const auto nc = static_cast<long long>(m_caret) + dir;
            m_caret = static_cast<size_t>(std::clamp(nc, 0LL, static_cast<long long>(n)));
            m_sel_end = m_no_sel; // 收起选区
        }
        mark_needs_paint();
        e.handled = true;
    }
}

/// @brief 按空格分词（用于换行）；处理 UTF-8 多字节字符为整体。
auto Text::split_words(const std::string &text) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t cl = cp_len(c);
        const std::string ch = text.substr(i, cl);
        i += cl;
        if (c == ' ') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

/// @brief 应用 max_lines + overflow（Clip/Ellipsis/Fade→Clip）；含排版 opts（间距/斜体）。
///        与 `cp_start` 一一对应截断；Ellipsis 仅改末行文本，行首码点下标不变。
auto Text::finalize_lines(std::vector<std::string> lines, std::vector<size_t> cp_start, const Font &f, float max_w,
                          int max_lines, TextOverflow overflow, const render::TextLayoutOpts &opts) -> WrapResult {
    if (max_lines > 0 && std::cmp_greater(lines.size(), max_lines)) {
        lines.resize(static_cast<std::size_t>(max_lines));
        cp_start.resize(static_cast<std::size_t>(max_lines));
        if (overflow == TextOverflow::Ellipsis) {
            std::string &last = lines.back();
            const std::string ell = "…";
            while (!last.empty() && render::FontEngine::measure_width(last + ell, f, opts) > max_w) {
                size_t cut = 0;
                size_t i = 0;
                while (i < last.size()) {
                    const size_t step = cp_len(static_cast<unsigned char>(last[i]));
                    if (i + step >= last.size()) {
                        break;
                    }
                    cut = i;
                    i += step;
                }
                last = last.substr(0, cut);
            }
            last += ell;
        }
        // Clip / Fade 直接截断（Fade 降级为 Clip）
    }
    return { std::move(lines), std::move(cp_start) };
}

/// @brief 将文本按 max_w 折行（soft_wrap=false 时单行）；超长词回退按字符折；含排版 opts。
///        同时记录每个可视行首字符在原始 `text` 中的码点下标（m_line_cp_start），
///        供多行命中测试 / 选区把「可视行 + 行内偏移」还原为全局码点下标。
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Text::wrap_lines(const std::string &text, const Font &f, float max_w, bool soft_wrap, int max_lines,
                      TextOverflow overflow, const render::TextLayoutOpts &opts) -> WrapResult {
    if (!soft_wrap || max_w <= 0.0f) {
        return finalize_lines({ text }, { 0u }, f, max_w, max_lines, overflow, opts);
    }

    // 逐词收集：记录每个「非空格词」在原始 text 中的字节范围 [start,end)。
    struct Word {
        size_t start;
        size_t end;
    };
    std::vector<Word> words;
    {
        size_t i = 0;
        while (i < text.size()) {
            const auto c = static_cast<unsigned char>(text[i]);
            const size_t cl = cp_len(c);
            if (c == ' ') {
                i += cl;
                continue;
            }
            const size_t ws = i;
            while (i < text.size()) {
                const auto d = static_cast<unsigned char>(text[i]);
                if (d == ' ') {
                    break;
                }
                i += cp_len(d);
            }
            words.push_back(Word{ .start = ws, .end = i });
        }
    }
    if (words.empty()) {
        return finalize_lines({ std::string{} }, { 0u }, f, max_w, max_lines, overflow, opts);
    }

    std::vector<std::string> lines;
    std::vector<size_t> starts;
    std::string line;                       ///< 当前行已累积的可见内容
    size_t line_start_off = words[0].start; ///< 当前行首字符在 text 中的字节偏移

    const auto measure = [&](const std::string &s) -> float { return render::FontEngine::measure_width(s, f, opts); };
    // 把当前行压入结果，并依据其首字节偏移记录全局码点下标。
    const auto push_line = [&]() -> void {
        lines.push_back(line);
        starts.push_back(cp_count(text.substr(0, line_start_off)));
    };
    // 超长词按字符折：除最后一块外，每块各成一行；最后一块成为新当前行。
    const auto char_split = [&](const std::string &word, size_t byte_off) -> void {
        std::string chunk;
        size_t chunk_off = byte_off;
        size_t i = 0;
        while (i < word.size()) {
            const size_t cl = cp_len(static_cast<unsigned char>(word[i]));
            const std::string ch = word.substr(i, cl);
            const size_t ch_off = byte_off + i;
            i += cl;
            const std::string ccand = chunk.empty() ? ch : chunk + ch;
            if (measure(ccand) <= max_w || chunk.empty()) {
                if (chunk.empty()) {
                    chunk_off = ch_off; // 记录本块首字符的字节偏移（仅首次）
                }
                chunk = ccand;
            } else {
                lines.push_back(chunk);
                starts.push_back(cp_count(text.substr(0, chunk_off)));
                chunk = ch;
                chunk_off = ch_off; // 新块首字符字节偏移
            }
        }
        line = chunk;
        line_start_off = chunk_off;
    };

    for (const Word &w : words) {
        const std::string word = text.substr(w.start, w.end - w.start);
        if (line.empty()) {
            if (measure(word) <= max_w) {
                line = word;
                line_start_off = w.start;
            } else {
                char_split(word, w.start);
            }
        } else {
            std::string cand = line;
            cand += ' ';
            cand += word;
            if (measure(cand) <= max_w) {
                line = cand;
            } else if (measure(word) <= max_w) {
                push_line();
                line = word;
                line_start_off = w.start;
            } else {
                push_line();
                char_split(word, w.start);
            }
        }
    }
    if (!line.empty()) {
        push_line();
    }
    return finalize_lines(std::move(lines), std::move(starts), f, max_w, max_lines, overflow, opts);
}

auto Text::effective_font(const Font &base) -> Font {
    Font f = base;
    if (f.size_pt <= 0.0f) {
        f.size_pt = 14.0f;
    }
    return f;
}

auto Text::cp_len(unsigned char c) -> size_t { return utf8_cp_len(c); }

auto Text::cp_count(const std::string &s) -> size_t { return utf8_cp_count(s); }

auto Text::cp_slice(const std::string &s, size_t start, size_t count) -> std::string {
    return utf8_cp_slice(s, start, count);
}

auto Text::selected_text() const -> std::string {
    if (!has_selection()) {
        return {};
    }
    const size_t a = std::min(m_sel_start, m_sel_end);
    const size_t b = std::max(m_sel_start, m_sel_end);
    return cp_slice(m_display_text, a, b - a + 1); // 含头含尾：[a, b] 共 (b-a+1) 个字符
}

auto Text::justify_layout(const std::string &line, const Font &f, const render::TextLayoutOpts &opts, float avail)
    -> std::vector<JustifiedWord> {
    // 逐词扫描：记录每词的行内码点区间（空格占 1 码点，不入词）。
    std::vector<JustifiedWord> words;
    size_t i = 0;
    size_t cp = 0;
    while (i < line.size()) {
        const auto c = static_cast<unsigned char>(line[i]);
        if (c == ' ') {
            i += cp_len(c);
            ++cp;
            continue;
        }
        const size_t bs = i;
        const size_t cps = cp;
        while (i < line.size() && line[i] != ' ') {
            i += cp_len(static_cast<unsigned char>(line[i]));
            ++cp;
        }
        words.push_back(
            JustifiedWord{ .text = line.substr(bs, i - bs), .cp_begin = cps, .cp_end = cp, .x = 0.0f, .w = 0.0f });
    }
    if (words.size() < 2) {
        return {}; // 不足两词不做两端对齐（与 on_paint 一致）
    }
    float sum_ww = 0.0f;
    for (auto &w : words) {
        w.w = render::FontEngine::measure_width(w.text, f, opts);
        sum_ww += w.w;
    }
    // 与 on_paint 的两端对齐同源：剩余宽度均分进 (n-1) 个词间隙。
    const float step = (avail - sum_ww) / static_cast<float>(words.size() - 1);
    float x = 0.0f;
    for (auto &w : words) {
        w.x = x;
        x += w.w + step;
    }
    return words;
}

auto Text::is_justified_line(size_t li) const -> bool {
    return text_align == TextAlign::Justify && m_lines.size() > 1 && (li + 1 < m_lines.size());
}

auto Text::line_caret_x(size_t li, size_t cp_in_line, const Font &f, const render::TextLayoutOpts &opts,
                        float avail) const -> float {
    const std::string &line = m_lines[li];
    if (is_justified_line(li)) {
        const auto words = justify_layout(line, f, opts, avail);
        if (!words.empty()) {
            for (size_t k = 0; k < words.size(); ++k) {
                const JustifiedWord &w = words[k];
                if (cp_in_line < w.cp_begin) {
                    // 落在词间空格：caret 取上一词右缘，使空格高亮覆盖整个拉伸间隙。
                    return (k == 0) ? 0.0f : words[k - 1].x + words[k - 1].w;
                }
                if (cp_in_line <= w.cp_end) {
                    return w.x + render::FontEngine::caret_x(w.text, cp_in_line - w.cp_begin, f, opts);
                }
            }
            return avail; // 行尾之后 → 行右缘（两端对齐铺满整行，修复行尾未高亮）
        }
    }
    // 非 Justify 行：实显 caret（物理 DPI 前缀 extent 折算回 dp，逐字符与实绘字形对齐；
    // scale=1 退化为 caret_x）——整行宽度比的线性近似仅行尾精确，行内相邻窄字符处
    // 会跨边界，造成「点第一个字符选中第二个」的 off-by-one。
    return render::FontEngine::display_caret_x(line, cp_in_line, f, opts, m_paint_scale);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Text::line_hit_test(size_t li, float x, const Font &f, const render::TextLayoutOpts &opts, float avail) const
    -> std::pair<size_t, size_t> {
    const std::string &line = m_lines[li];
    if (is_justified_line(li)) {
        const auto words = justify_layout(line, f, opts, avail);
        if (!words.empty()) {
            for (size_t k = 0; k < words.size(); ++k) {
                const JustifiedWord &w = words[k];
                if (x < w.x) {
                    // 词间拉伸间隙：整体归属其空格字符（含头含尾）；caret 按间隙中点取舍。
                    if (k == 0) {
                        return { 0u, 0u }; // 首词左侧（理论上 x<0）：夹到行首
                    }
                    const float left = words[k - 1].x + words[k - 1].w;
                    const size_t space_cp = words[k - 1].cp_end;
                    const size_t caret = (x < (left + w.x) * 0.5f) ? space_cp : w.cp_begin;
                    return { caret, space_cp };
                }
                if (x <= w.x + w.w) {
                    const float lx = x - w.x;
                    return { w.cp_begin + render::FontEngine::hit_test_char(w.text, lx, f, opts),
                             w.cp_begin + render::FontEngine::hit_test_char_inclusive(w.text, lx, f, opts) };
                }
            }
            const JustifiedWord &last = words.back();
            return { last.cp_end, last.cp_end == 0 ? 0 : last.cp_end - 1 }; // 行尾右侧 → 末字符
        }
    }
    // 非 Justify 行：实显命中（字符边界取物理 DPI 前缀 extent，与实绘字形逐字符对齐；
    // scale=1 退化后与自然命中完全等价）。
    return { render::FontEngine::display_hit_test_char(line, x, f, opts, m_paint_scale),
             render::FontEngine::display_hit_test_char_inclusive(line, x, f, opts, m_paint_scale) };
}

} // namespace aurora

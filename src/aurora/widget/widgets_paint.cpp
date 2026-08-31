#include <algorithm>

#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/widget/button.h"
#include "aurora/widget/text.h"

namespace aurora {

// NOLINTNEXTLINE(*-function-cognitive-complexity)
auto Text::on_paint(Painter &p, const Rect &bounds, const BuildContext &ctx) -> void {
    // resolved_text 缓存：若 on_layout 已计算则直接复用，否则回退计算
    if (m_resolved_dirty) {
        m_cached_resolved_text = resolved_text(ctx);
        m_resolved_dirty = false;
    }
    const std::string &s = m_cached_resolved_text;
    m_display_text = s; // 缓存显示文本供命中测试/选区
    const Font f = effective_font(font);
    const render::TextLayoutOpts opts{ .letter_spacing = letter_spacing,
                                       .word_spacing = word_spacing,
                                       .italic = (font_style == FontStyle::Italic) };
    const render::TextAAMode aa = text_aa_mode.has_value() ? *text_aa_mode : render::FontEngine::text_aa_mode();
    m_paint_scale = p.scale(); // 实显宽度校正与绘制同源：命中测试（无 Painter）复用最近一次绘制的像素比

    // 若布局阶段未产出行（极端情况），在此兜底计算
    float line_h = m_line_h;
    if (m_lines.empty() || line_h <= 0.0f) {
        line_h = render::FontEngine::measure_height(f) * std::max(0.1f, line_height);
        auto [lines, cp_start] = wrap_lines(s, f, bounds.size.width, soft_wrap, max_lines, overflow, opts);
        m_lines = std::move(lines);
        m_line_cp_start = std::move(cp_start);
        // 写回成员：即使未经过 on_layout（如独立文本/测试场景），后续命中测试
        // （on_pointer_event 依赖 m_line_h 定位可视行）也能拿到有效行高，避免早退。
        m_line_h = line_h;
    }

    const bool justify = (text_align == TextAlign::Justify);
    float y = bounds.origin.y;
    for (std::size_t li = 0; li < m_lines.size(); ++li) {
        const std::string &line = m_lines[li];
        const float line_w = render::FontEngine::measure_width(line, f, opts);
        float x = bounds.origin.x;
        switch (text_align) {
        case TextAlign::Right:
        case TextAlign::End: x = bounds.origin.x + (bounds.size.width - line_w); break;
        case TextAlign::Center: x = bounds.origin.x + ((bounds.size.width - line_w) * 0.5f); break;
        case TextAlign::Left:
        case TextAlign::Start:
        case TextAlign::Justify:
        default: x = bounds.origin.x; break;
        }

        // 文本底色
        if (background_color.m_a > 0) {
            p.fill_rect(Rect{ .origin = Point{ .x = bounds.origin.x, .y = y },
                              .size = Size{ .width = bounds.size.width, .height = line_h } },
                        background_color);
        }

        // 本行绘制范围（用于装饰线对齐）；Justify 时整行铺满。
        float line_left = x;
        float line_right = x + line_w;

        // 文本（含 opts：letter/word spacing & italic）。
        // justify 且为多行非末行 → 逐词均分剩余宽度做两端对齐（与 measure/caret 一致）。
        const bool justify_line = justify && m_lines.size() > 1 && (li + 1 < m_lines.size());
        if (justify_line) {
            auto words = split_words(line);
            if (words.size() >= 2) {
                float sum_ww = 0.0f;
                for (const auto &w : words) {
                    sum_ww += render::FontEngine::measure_width(w, f, opts);
                }
                const float avail = bounds.size.width;
                const float step = (avail - sum_ww) / static_cast<float>(words.size() - 1);
                float wx = bounds.origin.x;
                for (const auto &word : words) {
                    const float ww = render::FontEngine::measure_width(word, f, opts);
                    p.draw_text(
                        Rect{ .origin = Point{ .x = wx, .y = y }, .size = Size{ .width = ww, .height = line_h } }, word,
                        f, text_color, aa, opts);
                    wx += ww + step;
                }
                line_left = bounds.origin.x;
                line_right = bounds.origin.x + avail;
            } else {
                p.draw_text(
                    Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = line_w, .height = line_h } }, line,
                    f, text_color, aa, opts);
            }
        } else {
            p.draw_text(Rect{ .origin = Point{ .x = x, .y = y }, .size = Size{ .width = line_w, .height = line_h } },
                        line, f, text_color, aa, opts);
        }

        // 装饰线（上划线 / 下划线 / 删除线）按整行绘制。
        if (decoration != TextDecoration::None) {
            const Color dc = decoration_color.m_a > 0 ? decoration_color : text_color;
            const float thick = std::max(1.0f, line_h * 0.06f);
            if (decoration_has(decoration, TextDecoration::Underline)) {
                p.fill_rect(Rect{ .origin = Point{ .x = line_left, .y = y + (line_h * 0.82f) },
                                  .size = Size{ .width = line_right - line_left, .height = thick } },
                            dc);
            }
            if (decoration_has(decoration, TextDecoration::Overline)) {
                p.fill_rect(Rect{ .origin = Point{ .x = line_left, .y = y + (line_h * 0.12f) },
                                  .size = Size{ .width = line_right - line_left, .height = thick } },
                            dc);
            }
            if (decoration_has(decoration, TextDecoration::LineThrough)) {
                p.fill_rect(Rect{ .origin = Point{ .x = line_left, .y = y + (line_h * 0.5f) },
                                  .size = Size{ .width = line_right - line_left, .height = thick } },
                            dc);
            }
        }

        y += line_h;
    }

    // 选区高亮：必须画在文本「之后」（叠在文本上方），否则 ClearType 路径会以文本原点
    // 处像素为背景、把整个文本包围盒填成单一底色，覆盖掉先画的半透明高亮矩形。
    // 叠在上方的半透明高亮既不会被 ClearType 覆盖，选中文字也仍可读。
    // 注：Text 是只读显示控件，不绘制编辑光标（caret）——点击/获焦均不显示插入点；
    // 仅保留选区高亮（拖选 + Ctrl+A/Ctrl+C 复制），与「只读展示」语义一致。
    // 关键：高亮必须裁剪到本控件 bounds，否则当相邻控件之间仅余极小间隙时，
    // 半透明高亮矩形会渗入下一行相邻 Text，造成「选中一行却让邻行看起来被选中」的假象。
    // 只裁剪高亮、不裁剪文本，因此带 Modifier::scale/rotate 的 Text（如 demo 的缩放方块）
    // 的变换溢出绘制不受影响。
    // 选区高亮（含头含尾模型）：逐可视行绘制选区片段，确保多行/折行选区的高亮
    // 严格贴合字符几何，且端点字符（行首/行尾）始终被计入选区。
    // 无选区（m_sel_end == k_no_sel）时 has_selection() 为 false，不绘制任何高亮，
    // 避免旧模型下「m_sel_start != m_sel_end」在无选区时被误判为有选区而画出垃圾高亮。
    // push_clip(bounds) 作为兜底，确保任何几何取整/变换下高亮都不越出本控件盒子、
    // 也不会渗入下方相邻控件造成「邻行被选中」的假象。
    if (has_selection()) {
        const size_t a = std::min(m_sel_start, m_sel_end);
        const size_t b = std::max(m_sel_start, m_sel_end);
        p.push_clip(bounds);
        for (std::size_t li = 0; li < m_lines.size(); ++li) {
            const size_t line_cp0 = m_line_cp_start[li];
            const size_t line_n = cp_count(m_lines[li]);
            if (line_n == 0) {
                continue;
            }
            const size_t line_cp1 = line_cp0 + line_n; // 本行尾（不含）
            if (b < line_cp0 || a >= line_cp1) {
                continue; // 本行与选区无交叠
            }
            const size_t seg_a = std::max(a, line_cp0);
            const size_t seg_b = std::min(b, line_cp1 - 1); // 含头含尾的终点字符
            const float line_w = render::FontEngine::measure_width(m_lines[li], f, opts);
            float line_off = 0.0f;
            switch (text_align) {
            case TextAlign::Right:
            case TextAlign::End: line_off = bounds.size.width - line_w; break;
            case TextAlign::Center: line_off = (bounds.size.width - line_w) * 0.5f; break;
            default: line_off = 0.0f; break;
            }
            // 行内 x 经 line_caret_x 计算：Justify 行按逐词均分布局取词位（行尾 = 行右缘），
            // 与上方两端对齐绘制严格同源；非 Justify 行取实显 caret（物理 DPI 前缀 extent
            // 折算回 dp，逐字符与实绘字形对齐）——否则高亮按 96dp 自然测量计算，缩放屏下
            // hinting 取整不成比例，行尾欠高亮、行内字符高亮边界与实绘错位。
            const float seg_x0 =
                bounds.origin.x + line_off + line_caret_x(li, seg_a - line_cp0, f, opts, bounds.size.width);
            const float seg_x1 =
                bounds.origin.x + line_off + line_caret_x(li, seg_b - line_cp0 + 1, f, opts, bounds.size.width);
            const float yy = bounds.origin.y + (static_cast<float>(li) * line_h);
            p.fill_rect(Rect{ .origin = Point{ .x = seg_x0, .y = yy },
                              .size = Size{ .width = seg_x1 - seg_x0, .height = line_h } },
                        Color{ 80, 120, 220, 110 });
        }
        p.pop_clip();
    }
}

auto Button::paint_background(Painter &p, const Rect &bounds, Color bg) -> void {
    if (corner_radius > 0.0f) {
        p.fill_rounded_rect(bounds, corner_radius, bg);
    } else {
        p.fill_rect(bounds, bg);
    }
}

auto Button::paint_border(Painter &p, const Rect &bounds) -> void {
    if (border_width <= 0.0f || !border_color.has_value()) {
        return;
    }
    const Color bc = enabled ? *border_color : border_color->with_alpha(128); // 禁用态边框半透淡化
    if (corner_radius > 0.0f) {
        p.draw_rounded_border(bounds, corner_radius, border_width, bc);
    } else {
        p.draw_rect(bounds, bc);
    }
}

auto Button::paint_label(Painter &p, const Rect &bounds, Color text_color) -> void {
    const float fs = font.size_pt > 0.0f ? font.size_pt : 14.0f;
    const Font f{ .size_pt = fs };
    float tw = m_cached_text_width;
    float th = m_cached_text_height;
    if (tw <= 0.0f || th <= 0.0f) {
        tw = render::FontEngine::measure_width(label.get().text, f);
        th = render::FontEngine::measure_height(f);
        m_cached_text_width = tw;
        m_cached_text_height = th;
    }
    const float tx = bounds.origin.x + ((bounds.size.width - tw) * 0.5f);
    const float ty = bounds.origin.y + ((bounds.size.height - th) * 0.5f);
    p.draw_text(Rect{ .origin = Point{ .x = tx, .y = ty }, .size = Size{ .width = tw, .height = th } },
                label.get().text, font, text_color);
}

auto Button::on_paint(Painter &p, const Rect &bounds, const BuildContext & /*ctx*/) -> void {
    // 绘制分阶段（继承扩展点）：背景 → 边框 → 文字；状态色由 resolve_* 钩子解析。
    paint_background(p, bounds, resolve_background());
    paint_border(p, bounds);
    paint_label(p, bounds, resolve_text_color());
}

} // namespace aurora

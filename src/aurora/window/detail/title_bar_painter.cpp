// CSD 装饰绘制层实现：由 `WaylandSurface::Impl::draw_decoration` 原样搬移，仅把「读宿主状态」
// 换成读 `TitleBarPaintState`，绘制序列/配色/几何消费保持逐位一致（软件路径与 GPU 录制路径共用）。

#include "aurora/window/detail/title_bar_painter.h"

#include "aurora/core/font.h"
#include "aurora/render/painter.h"
#include "aurora/window/title_bar_geometry.h"

namespace aurora::csd {

auto paint_title_bar(Painter &p, const TitleBarPaintState &s) -> void {
    // 全屏默认隐藏标题栏；顶边悬停揭示由宿主置 fullscreen_bar_revealed 后重绘可见。
    if (!s.paints_anything()) {
        return;
    }
    const float W = s.width;
    // 几何单一来源：与命中测试共用同一纯函数，杜绝热区与绘制错位。
    const TitleBarGeometry g = title_bar_geometry(W, s.style, s.mode == WindowMode::Maximized, s.resizable);
    const Color bg = s.active ? s.style.bg_active : s.style.bg_inactive;
    const Color fg = s.active ? s.style.fg_active : s.style.fg_inactive;

    if (!s.title_bar) {
        // 可缩放边框：不画可见线（缩放由宿主边缘热区驱动，浅色背景上画线反而突兀）。
        return;
    }

    // 标题栏背景。
    p.fill_rect(Rect{Point{0.0F, 0.0F}, Size{W, s.style.height}}, bg);

    // 图标槽（set_title_bar_icon 注入后显示；无图标留白，几何预留位不变）。
    if (s.icon != nullptr && g.icon.size.width > 0.0F) {
        p.draw_image(*s.icon, g.icon);
    }

    // 标题文字（draw_text 缺字体时回退内置位图字体，不依赖 FontEngine）。
    if (s.style.show_title && !s.title.empty() && g.title.size.width > 0.0F) {
        Font f;
        f.size_pt = 13.0F;
        f.weight = 500;
        p.draw_text(g.title, s.title, f, fg);
    }

    // 悬停序号约定：0=minimize / 1=maximize / 2=close（宿主命中测试写入，与布局无关）。
    const int hb = s.hovered_button;

    auto center_of = [](const Rect &r) {
        return Point{r.origin.x + r.size.width * 0.5f, r.origin.y + r.size.height * 0.5f};
    };
    auto glyph_min = [&](const Rect &r, float lw, const Color &c) {
        const Point m = center_of(r);
        const float e = r.size.width / 3.0F;
        p.draw_line(Point{m.x - e, m.y}, Point{m.x + e, m.y}, lw, c);
    };
    auto glyph_max = [&](const Rect &r, float lw, const Color &c) {
        const Point m = center_of(r);
        const float e = r.size.width / 3.6f;
        if (s.mode != WindowMode::Maximized) {
            // □ 空心方框。
            p.draw_line(Point{m.x - e, m.y - e}, Point{m.x + e, m.y - e}, lw, c);
            p.draw_line(Point{m.x + e, m.y - e}, Point{m.x + e, m.y + e}, lw, c);
            p.draw_line(Point{m.x + e, m.y + e}, Point{m.x - e, m.y + e}, lw, c);
            p.draw_line(Point{m.x - e, m.y + e}, Point{m.x - e, m.y - e}, lw, c);
        } else {
            // ▯ 还原：前实框 + 右上错位背框（双框表达「已最大化，点击还原」）。
            const float o = e * 0.45f;
            p.draw_line(Point{m.x - e, m.y + o - e}, Point{m.x + e, m.y + o - e}, lw, c);
            p.draw_line(Point{m.x + e, m.y + o - e}, Point{m.x + e, m.y + o + e}, lw, c);
            p.draw_line(Point{m.x + e, m.y + o + e}, Point{m.x - e, m.y + o + e}, lw, c);
            p.draw_line(Point{m.x - e, m.y + o + e}, Point{m.x - e, m.y + o - e}, lw, c);
            p.draw_line(Point{m.x - e + o, m.y - e}, Point{m.x + e + o, m.y - e}, lw, c);
            p.draw_line(Point{m.x + e + o, m.y - e}, Point{m.x + e + o, m.y + e}, lw, c);
            p.draw_line(Point{m.x + e + o, m.y + e}, Point{m.x - e + o, m.y + e}, lw, c);
            p.draw_line(Point{m.x - e + o, m.y + e}, Point{m.x - e + o, m.y - e}, lw, c);
        }
    };
    auto glyph_close = [&](const Rect &r, float lw, const Color &c) {
        const Point m = center_of(r);
        const float e = r.size.width / 3.0F;
        p.draw_line(Point{m.x - e, m.y - e}, Point{m.x + e, m.y + e}, lw, c);
        p.draw_line(Point{m.x + e, m.y - e}, Point{m.x - e, m.y + e}, lw, c);
    };

    if (s.style.button_layout == TitleBarButtonLayout::Adwaita) {
        // Adwaita：扁平单色符号，悬停浮出圆形底（关闭钮红底为其视觉签名）。
        if (g.minimize.size.width > 0.0F) {
            if (hb == 0) {
                p.fill_rounded_rect(g.minimize, g.minimize.size.width * 0.5f, s.style.hover_tint);
            }
            glyph_min(g.minimize, 1.5f, fg);
        }
        if (g.maximize.size.width > 0.0F) {
            if (hb == 1) {
                p.fill_rounded_rect(g.maximize, g.maximize.size.width * 0.5f, s.style.hover_tint);
            }
            glyph_max(g.maximize, 1.5f, fg);
        }
        if (g.close.size.width > 0.0F) {
            if (hb == 2) {
                p.fill_rounded_rect(g.close, g.close.size.width * 0.5f, s.style.close_hover);
            }
            glyph_close(g.close, 1.5f, Color{255, 255, 255, 235});
        }
    } else if (s.style.button_layout == TitleBarButtonLayout::Windows) {
        // Windows：整高矩形热区，悬停整块填充。
        if (g.minimize.size.width > 0.0F) {
            if (hb == 0) {
                p.fill_rect(g.minimize, s.style.hover_tint);
            }
            glyph_min(g.minimize, 1.2f, fg);
        }
        if (g.maximize.size.width > 0.0F) {
            if (hb == 1) {
                p.fill_rect(g.maximize, s.style.hover_tint);
            }
            glyph_max(g.maximize, 1.2f, fg);
        }
        if (g.close.size.width > 0.0F) {
            if (hb == 2) {
                p.fill_rect(g.close, s.style.close_hover);
            }
            glyph_close(g.close, 1.2f, Color{255, 255, 255, 235});
        }
    } else {
        // macOS：常显三色圆点，悬停浮现深色符号（close/min/max 左→右序由几何层保证）。
        const Color sym{0x3D, 0x3D, 0x3D, 210};
        if (g.minimize.size.width > 0.0F) {
            p.fill_rounded_rect(g.minimize, g.minimize.size.width * 0.5f, Color{0xFE, 0xBC, 0x2E, 255});
            if (hb == 0) {
                glyph_min(g.minimize, 1.2f, sym);
            }
        }
        if (g.maximize.size.width > 0.0F) {
            p.fill_rounded_rect(g.maximize, g.maximize.size.width * 0.5f, Color{0x28, 0xC8, 0x40, 255});
            if (hb == 1) {
                glyph_max(g.maximize, 1.2f, sym);
            }
        }
        if (g.close.size.width > 0.0F) {
            p.fill_rounded_rect(g.close, g.close.size.width * 0.5f, Color{0xFF, 0x5F, 0x57, 255});
            if (hb == 2) {
                glyph_close(g.close, 1.2f, sym);
            }
        }
    }
}

}  // namespace aurora::csd

// CSD 装饰绘制层实现：由 `WaylandSurface::Impl::draw_decoration` 原样搬移，仅把「读宿主状态」
// 换成读 `TitleBarPaintState`，绘制序列/配色/几何消费保持逐位一致（软件路径与 GPU 录制路径共用）。

#include "aurora/window/detail/title_bar_painter.h"

#include "aurora/core/font.h"
#include "aurora/render/painter.h"
#include "aurora/window/detail/csd_geometry.h"
#include "aurora/window/title_bar_geometry.h"

namespace aurora::csd {

namespace {
/// @brief 把内容坐标域几何平移到整幅表面坐标域（内容原点 = 阴影边距）。
auto shift_to_surface(const Rect &r, float ox, float oy) -> Rect {
    return Rect{.origin = Point{.x = r.origin.x + ox, .y = r.origin.y + oy}, .size = r.size};
}
}  // namespace

auto paint_window_shadow(Painter &p, const TitleBarPaintState &s) -> void {
    if (s.shadow_margin <= 0.0F || s.width <= 0.0F || s.height <= 0.0F) {
        return;
    }
    // GPU/DL underlay 专用（软件 wl_shm 路径见 csd::compose_shadow_margins_bgra——软件
    // Painter 的「目标画布恒不透明」不变量使其无法直绘真 alpha 阴影）：形状 = 完整内容矩形
    // （表面坐标），零投影偏移；模糊半径 = margin−1，衰减在 buffer 外缘归零。视觉参数与
    // 软件合成器同源于 csd_geometry.h（AURORA_SHADOW_BASE_COLOR / AURORA_SHADOW_BLUR_INSET_PX /
    // shadow_attenuation），GPU SDF 无软件 Painter 的边界行接缝，故形状不收缩。
    const Rect shape{.origin = Point{.x = s.origin_x, .y = s.origin_y},
                     .size = Size{.width = s.width, .height = s.height}};
    p.draw_shadow(shape, 0.0F, 0.0F, s.shadow_margin - static_cast<float>(AURORA_SHADOW_BLUR_INSET_PX),
                  AURORA_SHADOW_BASE_COLOR);
}

auto paint_title_bar(Painter &p, const TitleBarPaintState &s) -> void {
    // 全屏默认隐藏标题栏；顶边悬停揭示由宿主置 fullscreen_bar_revealed 后重绘可见。
    if (!s.paints_title_bar()) {
        return;
    }
    const float w = s.width;
    // 几何单一来源：与命中测试共用同一纯函数，杜绝热区与绘制错位。
    const TitleBarGeometry gc = title_bar_geometry(w, s.style, s.mode == WindowMode::Maximized, s.resizable);
    const Color bg = s.active ? s.style.bg_active : s.style.bg_inactive;
    const Color fg = s.active ? s.style.fg_active : s.style.fg_inactive;
    // 内容几何 → 表面几何：标题栏画在内容区顶部，原点随阴影边距平移（塌缩态偏移 0）。
    const float ox = s.origin_x;
    const float oy = s.origin_y;
    // 指定初始化器须按 TitleBarGeometry 声明顺序（close/maximize/minimize/icon/title）。
    const TitleBarGeometry g{.close = shift_to_surface(gc.close, ox, oy),
                             .maximize = shift_to_surface(gc.maximize, ox, oy),
                             .minimize = shift_to_surface(gc.minimize, ox, oy),
                             .icon = shift_to_surface(gc.icon, ox, oy),
                             .title = shift_to_surface(gc.title, ox, oy)};

    // 标题栏背景（内容区整宽，顶部标题栏高度）。
    p.fill_rect(Rect{.origin = Point{.x = ox, .y = oy}, .size = Size{.width = w, .height = s.style.height}}, bg);

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
        return Point{.x = r.origin.x + (r.size.width * 0.5F), .y = r.origin.y + (r.size.height * 0.5F)};
    };
    auto glyph_min = [&](const Rect &r, float lw, const Color &c) {
        const Point m = center_of(r);
        const float e = r.size.width / 3.0F;
        p.draw_line(Point{.x = m.x - e, .y = m.y}, Point{.x = m.x + e, .y = m.y}, lw, c);
    };
    auto glyph_max = [&](const Rect &r, float lw, const Color &c) {
        const Point m = center_of(r);
        const float e = r.size.width / 3.6F;
        if (s.mode != WindowMode::Maximized) {
            // □ 空心方框。
            p.draw_line(Point{.x = m.x - e, .y = m.y - e}, Point{.x = m.x + e, .y = m.y - e}, lw, c);
            p.draw_line(Point{.x = m.x + e, .y = m.y - e}, Point{.x = m.x + e, .y = m.y + e}, lw, c);
            p.draw_line(Point{.x = m.x + e, .y = m.y + e}, Point{.x = m.x - e, .y = m.y + e}, lw, c);
            p.draw_line(Point{.x = m.x - e, .y = m.y + e}, Point{.x = m.x - e, .y = m.y - e}, lw, c);
        } else {
            // ▯ 还原：前实框 + 右上错位背框（双框表达「已最大化，点击还原」）。
            const float o = e * 0.45F;
            p.draw_line(Point{.x = m.x - e, .y = m.y + o - e}, Point{.x = m.x + e, .y = m.y + o - e}, lw, c);
            p.draw_line(Point{.x = m.x + e, .y = m.y + o - e}, Point{.x = m.x + e, .y = m.y + o + e}, lw, c);
            p.draw_line(Point{.x = m.x + e, .y = m.y + o + e}, Point{.x = m.x - e, .y = m.y + o + e}, lw, c);
            p.draw_line(Point{.x = m.x - e, .y = m.y + o + e}, Point{.x = m.x - e, .y = m.y + o - e}, lw, c);
            p.draw_line(Point{.x = m.x - e + o, .y = m.y - e}, Point{.x = m.x + e + o, .y = m.y - e}, lw, c);
            p.draw_line(Point{.x = m.x + e + o, .y = m.y - e}, Point{.x = m.x + e + o, .y = m.y + e}, lw, c);
            p.draw_line(Point{.x = m.x + e + o, .y = m.y + e}, Point{.x = m.x - e + o, .y = m.y + e}, lw, c);
            p.draw_line(Point{.x = m.x - e + o, .y = m.y + e}, Point{.x = m.x - e + o, .y = m.y - e}, lw, c);
        }
    };
    auto glyph_close = [&](const Rect &r, float lw, const Color &c) {
        const Point m = center_of(r);
        const float e = r.size.width / 3.0F;
        p.draw_line(Point{.x = m.x - e, .y = m.y - e}, Point{.x = m.x + e, .y = m.y + e}, lw, c);
        p.draw_line(Point{.x = m.x + e, .y = m.y - e}, Point{.x = m.x - e, .y = m.y + e}, lw, c);
    };

    if (s.style.button_layout == TitleBarButtonLayout::Adwaita) {
        // Adwaita：扁平单色符号，悬停浮出圆形底（关闭钮红底为其视觉签名）。
        if (g.minimize.size.width > 0.0F) {
            if (hb == 0) {
                p.fill_rounded_rect(g.minimize, g.minimize.size.width * 0.5F, s.style.hover_tint);
            }
            glyph_min(g.minimize, 1.5F, fg);
        }
        if (g.maximize.size.width > 0.0F) {
            if (hb == 1) {
                p.fill_rounded_rect(g.maximize, g.maximize.size.width * 0.5F, s.style.hover_tint);
            }
            glyph_max(g.maximize, 1.5F, fg);
        }
        if (g.close.size.width > 0.0F) {
            if (hb == 2) {
                p.fill_rounded_rect(g.close, g.close.size.width * 0.5F, s.style.close_hover);
            }
            glyph_close(g.close, 1.5F, Color{255, 255, 255, 235});
        }
    } else if (s.style.button_layout == TitleBarButtonLayout::Windows) {
        // Windows：整高矩形热区，悬停整块填充。
        if (g.minimize.size.width > 0.0F) {
            if (hb == 0) {
                p.fill_rect(g.minimize, s.style.hover_tint);
            }
            glyph_min(g.minimize, 1.2F, fg);
        }
        if (g.maximize.size.width > 0.0F) {
            if (hb == 1) {
                p.fill_rect(g.maximize, s.style.hover_tint);
            }
            glyph_max(g.maximize, 1.2F, fg);
        }
        if (g.close.size.width > 0.0F) {
            if (hb == 2) {
                p.fill_rect(g.close, s.style.close_hover);
            }
            glyph_close(g.close, 1.2F, Color{255, 255, 255, 235});
        }
    } else {
        // macOS：常显三色圆点，悬停浮现深色符号（close/min/max 左→右序由几何层保证）。
        const Color sym{0x3D, 0x3D, 0x3D, 210};
        if (g.minimize.size.width > 0.0F) {
            p.fill_rounded_rect(g.minimize, g.minimize.size.width * 0.5F, Color{0xFE, 0xBC, 0x2E, 255});
            if (hb == 0) {
                glyph_min(g.minimize, 1.2F, sym);
            }
        }
        if (g.maximize.size.width > 0.0F) {
            p.fill_rounded_rect(g.maximize, g.maximize.size.width * 0.5F, Color{0x28, 0xC8, 0x40, 255});
            if (hb == 1) {
                glyph_max(g.maximize, 1.2F, sym);
            }
        }
        if (g.close.size.width > 0.0F) {
            p.fill_rounded_rect(g.close, g.close.size.width * 0.5F, Color{0xFF, 0x5F, 0x57, 255});
            if (hb == 2) {
                glyph_close(g.close, 1.2F, sym);
            }
        }
    }
}

}  // namespace aurora::csd

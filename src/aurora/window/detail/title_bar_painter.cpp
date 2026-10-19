// CSD 装饰绘制层实现：由 `WaylandSurface::Impl::draw_decoration` 原样搬移，仅把「读宿主状态」
// 换成读 `TitleBarPaintState`，绘制序列/配色/几何消费保持逐位一致（软件路径与 GPU 录制路径共用）。

#include "aurora/window/detail/title_bar_painter.h"

#include <algorithm>

#include "aurora/core/font.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/window/detail/csd_geometry.h"
#include "aurora/window/title_bar_geometry.h"

namespace aurora::csd {

namespace {
/// @brief 把内容坐标域几何平移到整幅表面坐标域（内容原点 = 阴影边距）。
auto shift_to_surface(const Rect &r, float ox, float oy) -> Rect {
    return Rect{.origin = Point{.x = r.origin.x + ox, .y = r.origin.y + oy}, .size = r.size};
}

// 三套语言的标题栏字形一律走**固定设计网格**而非按钮盒比例：原生符号尺寸与按钮盒无关
// ——Windows 是 10×10 epx（Segoe Fluent Icons）、GNOME 是 16×16 symbolic 画布（四周各留 2dp，
// 符号实际占 12×12）、macOS 红绿灯内符号按真机 8px 跨度。早前统一按「盒宽 / 3」推导，使字形随
// 盒放大（Windows 档实测占盒宽 2/3、Mac 档占圆直径 2/3），与真机差距显著且相邻字形互相挤在
// 一起，故按各自的设计网格重写。
constexpr float AURORA_WIN_GLYPH_HALF_DP = 5.0F;  // 设计网格半宽（10×10 格）
constexpr float AURORA_WIN_GLYPH_STROKE_DP = 1.0F;  // 单线线宽（真机 1px @100% DPI）
constexpr float AURORA_ADWAITA_GLYPH_HALF_DP = 6.0F;  // 16×16 symbolic 画布内 12×12 符号的半宽
constexpr float AURORA_ADWAITA_GLYPH_STROKE_DP = 1.5F;  // symbolic 单线线宽
// libadwaita 的悬停圆底画在 `windowcontrols > button > image` 上，恒为「16dp 图标 + 3dp 内边距」
// = 22dp，**与按钮盒宽解耦**（盒在 `.default-decoration` 下是 30dp，圆的直径仍是 22dp）。
constexpr float AURORA_ADWAITA_DISC_DP = 22.0F;
constexpr float AURORA_MAC_GLYPH_MIN_HALF_DP = 4.0F;  // 真机最小化横线 8px 宽
constexpr float AURORA_MAC_GLYPH_CLOSE_HALF_DP = 2.8F;  // 真机 ✕ 臂长 8px ⇒ 投影半宽 8/√2≈2.83
constexpr float AURORA_MAC_GLYPH_FULLSCREEN_HALF_DP = 4.0F;  // 真机全屏双向箭头跨度 8px
constexpr float AURORA_MAC_GLYPH_STROKE_DP = 1.0F;  // 圆内符号线宽
constexpr float AURORA_MAC_BORDER_DP = 1.0F;  // 红绿灯描边宽（真机 1px）

// macOS 红绿灯取色（真机取色表：`macos-traffic-light-buttons-as-SVG` 的 README；填色另参 Apple
// 语义色 `#FF5F57`/`#FEBC2E`/`#28C840`）。每枚圆点都有一圈比填色深一档的描边——这圈描边是圆点
// 立体感的来源，早前画的是纯色圆盘，看上去是「三个扁平色块」。
constexpr Color AURORA_MAC_CLOSE_FILL{0xFF, 0x5F, 0x57, 255};
constexpr Color AURORA_MAC_CLOSE_BORDER{0xE2, 0x4B, 0x41, 255};
constexpr Color AURORA_MAC_CLOSE_SYMBOL{0x46, 0x08, 0x04, 255};
constexpr Color AURORA_MAC_CLOSE_FILL_PRESSED{0xB1, 0x50, 0x48, 255};
constexpr Color AURORA_MAC_CLOSE_BORDER_PRESSED{0xA1, 0x42, 0x39, 255};
constexpr Color AURORA_MAC_CLOSE_SYMBOL_PRESSED{0x17, 0x01, 0x01, 255};
constexpr Color AURORA_MAC_MIN_FILL{0xFE, 0xBC, 0x2E, 255};
constexpr Color AURORA_MAC_MIN_BORDER{0xE1, 0xA7, 0x3E, 255};
constexpr Color AURORA_MAC_MIN_SYMBOL{0x90, 0x59, 0x1D, 255};
constexpr Color AURORA_MAC_MIN_FILL_PRESSED{0xB8, 0x92, 0x3B, 255};
constexpr Color AURORA_MAC_MIN_BORDER_PRESSED{0xA6, 0x7F, 0x36, 255};
constexpr Color AURORA_MAC_MIN_SYMBOL_PRESSED{0x53, 0x2A, 0x0A, 255};
constexpr Color AURORA_MAC_MAX_FILL{0x28, 0xC8, 0x40, 255};
constexpr Color AURORA_MAC_MAX_BORDER{0x2D, 0xAC, 0x2F, 255};
constexpr Color AURORA_MAC_MAX_SYMBOL{0x2A, 0x62, 0x18, 255};
constexpr Color AURORA_MAC_MAX_FILL_PRESSED{0x4A, 0x97, 0x41, 255};
constexpr Color AURORA_MAC_MAX_BORDER_PRESSED{0x42, 0x82, 0x34, 255};
constexpr Color AURORA_MAC_MAX_SYMBOL_PRESSED{0x11, 0x31, 0x07, 255};
// 失焦（窗口非活动）：真机三枚统一变浅灰且不显符号。
constexpr Color AURORA_MAC_INACTIVE_FILL{0xDD, 0xDD, 0xDD, 255};
constexpr Color AURORA_MAC_INACTIVE_BORDER{0xD1, 0xD0, 0xD2, 255};
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
    // 模糊半径与基色由 `WindowFrameStyle` 装配（默认值 == 既有常量，迁移零视觉变化），
    // 折算规则见 csd::frame_blur_dp / csd::frame_margin_dp。
    // `shadow_blur <= 0` 时的回落不是冗余分支：直接构造的 `TitleBarPaintState`（测试与
    // 非经 `make_paint_state` 的调用方）不会带折算值，此时须沿用既有推导式「margin − 1」，
    // 否则「新增可配模糊」这一件事本身就会把默认观感改成零衰减。
    const float blur = s.shadow_blur > 0.0F
                           ? s.shadow_blur
                           : std::max(0.0F, s.shadow_margin - static_cast<float>(AURORA_SHADOW_BLUR_INSET_PX));
    p.draw_shadow(shape, 0.0F, 0.0F, blur, s.shadow_color);
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
    // corner_radius > 0 时**只圆下两角**：窗口顶角归窗框/阴影外轮廓，标题栏顶边须与之上沿齐平
    // （否则圆角处会透出窗框底色）。实现为「整圆角矩形 + 补回顶部两角」两步，省去按角拆路径。
    const Rect bg_rect{.origin = Point{.x = ox, .y = oy}, .size = Size{.width = w, .height = s.style.height}};
    const float cr = std::clamp(s.style.corner_radius, 0.0F, std::min(w, s.style.height) * 0.5F);
    if (cr > 0.0F) {
        p.fill_rounded_rect(bg_rect, cr, bg);
        p.fill_rect(Rect{.origin = Point{.x = ox, .y = oy}, .size = Size{.width = cr, .height = cr}}, bg);
        p.fill_rect(Rect{.origin = Point{.x = ox + w - cr, .y = oy}, .size = Size{.width = cr, .height = cr}}, bg);
    } else {
        p.fill_rect(bg_rect, bg);
    }

    // 底缘分隔线（libadwaita `headerbar` 的 `box-shadow: inset 0 -1px var(--headerbar-shade-color)`；
    // 亮色 = 12% 黑、暗色 = 36% 黑，预设里存的是已叠加的实色）。只有 Adwaita 两档给值——
    // Win11 与 macOS 的标题栏底缘都没有这条线，`separator` 全透明时整段跳过。
    if (s.style.separator.a > 0) {
        p.fill_rect(
            Rect{.origin = Point{.x = ox, .y = oy + s.style.height - 1.0F}, .size = Size{.width = w, .height = 1.0F}},
            s.style.separator);
    }

    // 图标槽（set_title_bar_icon 注入后显示；无图标留白，几何预留位不变）。
    if (s.icon != nullptr && g.icon.size.width > 0.0F) {
        p.draw_image(*s.icon, g.icon);
    }

    // 标题文字（draw_text 缺字体时回退内置位图字体，不依赖 FontEngine）。
    if (s.style.show_title && !s.title.empty() && g.title.size.width > 0.0F) {
        Font f;
        f.size_pt = s.style.metrics.title_font_size_pt;
        f.weight = s.style.metrics.title_font_weight;
        // center_title：几何层给的是**整宽带**（不扣按钮组），居中由绘制层按实测量宽完成
        // ——macOS 窗口标题即居中，WinUI 左对齐。偏移后右缘不动，故超宽标题的截断口径与
        // 左对齐时一致，不会因居中而少截/多截。
        const float tw = render::FontEngine::measure_width(s.title, f);
        const float off = s.style.center_title ? std::max(0.0F, (g.title.size.width - tw) * 0.5F) : 0.0F;
        // 垂直居中：`draw_text` 的 y 是**行盒顶**（GDI TA_TOP 语义，基线 = 顶 + ascent），把标题
        // 带原样传下去等于把文字顶到栏的上缘——实测墨迹中心比栏高中心偏上 Adwaita 6dp、
        // Windows 8.5dp、Mac 1.5dp。三套语言的标题在栏内都是垂直居中的，故按字体行盒高把带顶
        // 下移半程（行盒 = ascent+descent，与绘制侧同一 `measure_height` 口径；行盒含下伸部，
        // 纯大写标题的视觉中心因此略低于几何中心 1~2dp，属字体度量固有偏差，不另加补偿常数）。
        const float dy = std::max(0.0F, (g.title.size.height - render::FontEngine::measure_height(f)) * 0.5F);
        const Rect band{.origin = Point{.x = g.title.origin.x + off, .y = g.title.origin.y + dy},
                        .size = Size{.width = g.title.size.width - off, .height = g.title.size.height - dy}};
        p.draw_text(band, s.title, f, fg);
    }

    // 悬停序号约定：0=minimize / 1=maximize / 2=close（宿主命中测试写入，与布局无关）。
    const int hb = s.hovered_button;

    // 按钮填色优先级：**按压 > 悬停 > 不填**。按压必须压过悬停——按下时指针必然也在该钮上，
    // 若悬停优先则按压态永远被遮住，用户得不到「已按下」的反馈。
    const auto button_fill = [&](int ordinal, const Color &hover, const Color &pressed, Color &out) -> bool {
        if (ordinal == s.pressed_button) {
            out = pressed;
            return true;
        }
        if (ordinal == hb) {
            out = hover;
            return true;
        }
        return false;
    };

    auto center_of = [](const Rect &r) {
        return Point{.x = r.origin.x + (r.size.width * 0.5F), .y = r.origin.y + (r.size.height * 0.5F)};
    };
    // 线段原语（三套语言共用）：字形一律由「横线 / 方框或双框 / 对角 ✕」组合而成。
    const auto stroke = [&](float x1, float y1, float x2, float y2, float lw, const Color &c) -> void {
        p.draw_line(Point{.x = x1, .y = y1}, Point{.x = x2, .y = y2}, lw, c);
    };
    // 形状三件套按调用方给定的**设计网格半宽** `half` 与线宽 `lw` 居中绘制——盒再宽也不放大
    // （各语言的 half 常量见文件顶部）。
    const auto glyph_min_at = [&](const Rect &r, float half, float lw, const Color &c) -> void {
        const Point m = center_of(r);
        stroke(m.x - half, m.y, m.x + half, m.y, lw, c);  // 与真机一致：横线过按钮盒垂直中心
    };
    const auto glyph_max_at = [&](const Rect &r, float half, float lw, const Color &c) -> void {
        const Point m = center_of(r);
        if (s.mode != WindowMode::Maximized) {
            // □ 空心方框。
            stroke(m.x - half, m.y - half, m.x + half, m.y - half, lw, c);
            stroke(m.x + half, m.y - half, m.x + half, m.y + half, lw, c);
            stroke(m.x + half, m.y + half, m.x - half, m.y + half, lw, c);
            stroke(m.x - half, m.y + half, m.x - half, m.y - half, lw, c);
        } else {
            // ▯ 还原：前实框 + 右上错位背框（双框表达「已最大化，点击还原」）。
            const float o = half * 0.45F;
            stroke(m.x - half, m.y + o - half, m.x + half, m.y + o - half, lw, c);
            stroke(m.x + half, m.y + o - half, m.x + half, m.y + o + half, lw, c);
            stroke(m.x + half, m.y + o + half, m.x - half, m.y + o + half, lw, c);
            stroke(m.x - half, m.y + o + half, m.x - half, m.y + o - half, lw, c);
            stroke(m.x - half + o, m.y - half, m.x + half + o, m.y - half, lw, c);
            stroke(m.x + half + o, m.y - half, m.x + half + o, m.y + half, lw, c);
            stroke(m.x + half + o, m.y + half, m.x - half + o, m.y + half, lw, c);
            stroke(m.x - half + o, m.y + half, m.x - half + o, m.y - half, lw, c);
        }
    };
    const auto glyph_close_at = [&](const Rect &r, float half, float lw, const Color &c) -> void {
        const Point m = center_of(r);
        stroke(m.x - half, m.y - half, m.x + half, m.y + half, lw, c);
        stroke(m.x + half, m.y - half, m.x - half, m.y + half, lw, c);
    };
    // macOS 绿钮（zoom / 全屏）符号：真机是**双向对角箭头**——未最大化时箭头朝外（进入全屏），
    // 已最大化时朝内（退出全屏），而不是通用的空心方框（方框既不还原观感，也与绿钮的语义不符）。
    // 每条箭头 = 一条对角干 + 尖端两条沿轴回折的短须（12pt 圆内的箭头无须三角填充）。
    const auto glyph_fullscreen_at = [&](const Rect &r, float half, float lw, const Color &c, bool inward) -> void {
        const Point m = center_of(r);
        const float head = half * 0.55F;  // 短须长
        const float tip = inward ? half * 0.35F : half;  // 箭尖（对角干的末端）
        const float tail = inward ? half : half * 0.35F;  // 对角干的起点
        const auto arrow = [&](float sx, float sy) -> void {
            stroke(m.x + (sx * tail), m.y + (sy * tail), m.x + (sx * tip), m.y + (sy * tip), lw, c);
            stroke(m.x + (sx * tip), m.y + (sy * tip), m.x + (sx * tip) - (sx * head), m.y + (sy * tip), lw, c);
            stroke(m.x + (sx * tip), m.y + (sy * tip), m.x + (sx * tip), m.y + (sy * tip) - (sy * head), lw, c);
        };
        arrow(1.0F, 1.0F);
        arrow(-1.0F, -1.0F);
    };
    // Adwaita 的悬停圆底：直径恒 22dp（16dp symbolic 图标 + 3dp 内边距），**与按钮盒宽解耦**
    // ——盒被覆写成任意宽时圆不变、仍居中。早前直接取「盒内切圆」，盒 26dp 时圆即 26dp，偏大。
    const auto adwaita_disc = [&](const Rect &r, const Color &c) -> void {
        const Point m = center_of(r);
        const float h = AURORA_ADWAITA_DISC_DP * 0.5F;
        p.fill_rounded_rect(Rect{.origin = Point{.x = m.x - h, .y = m.y - h},
                                 .size = Size{.width = AURORA_ADWAITA_DISC_DP, .height = AURORA_ADWAITA_DISC_DP}},
                            h, c);
    };
    // 红绿灯圆盘：填色 + 一圈比填色深一档的 1px 描边；失焦整组变灰。
    const auto mac_dot = [&](const Rect &r, bool pressed, const Color &fill, const Color &border,
                             const Color &fill_pressed, const Color &border_pressed) -> void {
        if (!s.active) {
            p.fill_rounded_rect(r, r.size.width * 0.5F, AURORA_MAC_INACTIVE_FILL);
            p.draw_rounded_border(r, r.size.width * 0.5F, AURORA_MAC_BORDER_DP, AURORA_MAC_INACTIVE_BORDER);
            return;
        }
        p.fill_rounded_rect(r, r.size.width * 0.5F, pressed ? fill_pressed : fill);
        p.draw_rounded_border(r, r.size.width * 0.5F, AURORA_MAC_BORDER_DP, pressed ? border_pressed : border);
    };

    // 各语言的「半宽 / 线宽」常量对（取值与理由见文件顶部）。
    constexpr float wh = AURORA_WIN_GLYPH_HALF_DP;
    constexpr float wlw = AURORA_WIN_GLYPH_STROKE_DP;
    constexpr float ah = AURORA_ADWAITA_GLYPH_HALF_DP;
    constexpr float alw = AURORA_ADWAITA_GLYPH_STROKE_DP;
    constexpr float mnh = AURORA_MAC_GLYPH_MIN_HALF_DP;
    constexpr float mch = AURORA_MAC_GLYPH_CLOSE_HALF_DP;
    constexpr float mfh = AURORA_MAC_GLYPH_FULLSCREEN_HALF_DP;
    constexpr float mlw = AURORA_MAC_GLYPH_STROKE_DP;
    // Windows 的还原态与另两套不同：真机是「8×8 主框居左下 + 副框只露顶边与右边」，不是通用
    // 双框，故单列一份（最大化态仍与另两套同形）。
    const auto win_max = [&](const Rect &r, const Color &c) -> void {
        const Point m = center_of(r);
        if (s.mode != WindowMode::Maximized) {
            stroke(m.x - wh, m.y - wh, m.x + wh, m.y - wh, wlw, c);  // □ 满格空心方框
            stroke(m.x + wh, m.y - wh, m.x + wh, m.y + wh, wlw, c);
            stroke(m.x + wh, m.y + wh, m.x - wh, m.y + wh, wlw, c);
            stroke(m.x - wh, m.y + wh, m.x - wh, m.y - wh, wlw, c);
        } else {
            // ▯ 还原：8×8 主框居左下（半宽 0.8 格、上沿抬 0.6 格），副框向右上错位 0.4 格后
            // 只画落在格内的顶边与右边——这就是真机上「双层叠框」的可见部分。
            constexpr float fw = wh * 0.8F;
            constexpr float ft = wh * 0.6F;
            constexpr float off = wh * 0.4F;
            stroke(m.x - fw, m.y - ft, m.x + fw, m.y - ft, wlw, c);
            stroke(m.x + fw, m.y - ft, m.x + fw, m.y + wh, wlw, c);
            stroke(m.x + fw, m.y + wh, m.x - fw, m.y + wh, wlw, c);
            stroke(m.x - fw, m.y + wh, m.x - fw, m.y - ft, wlw, c);
            stroke(m.x - off, m.y - wh, m.x + wh, m.y - wh, wlw, c);
            stroke(m.x + wh, m.y - wh, m.x + wh, m.y + ft, wlw, c);
        }
    };

    // 关闭钮字形取色判据（Adwaita / Windows 两档同一条，Mac 档恒为彩底 + 深色符号不适用）：
    // 由**底色是否被填充**决定——亮色档底色近白，恒白字形实测在按钮盒内检不到任何墨迹
    // （等于关闭按钮画不出来），故常态须随前景色；底色换成悬停 / 按压的特征红后则须转白，
    // 红底上的深色符号同样不可读。两侧是同一个判据。
    if (s.style.button_layout == TitleBarButtonLayout::Adwaita) {
        // Adwaita：扁平单色符号（16×16 symbolic 画布内 12×12），圆底**常态就有**——libadwaita
        // 把圆画在 `windowcontrols > button > image` 上且 `background-color: $button_color`
        // （currentColor 10%），悬停 15% / 按压 30% 只是把它加深，不是「悬停才浮出」。
        // 按压态此前未被消费（只判 hovered_button），按下时没有任何反馈，故统一走 button_fill。
        if (g.minimize.size.width > 0.0F) {
            Color fill = s.style.button_tint;
            button_fill(0, s.style.hover_tint, s.style.pressed_tint, fill);
            if (fill.a > 0) {
                adwaita_disc(g.minimize, fill);
            }
            glyph_min_at(g.minimize, ah, alw, fg);
        }
        if (g.maximize.size.width > 0.0F) {
            Color fill = s.style.button_tint;
            button_fill(1, s.style.hover_tint, s.style.pressed_tint, fill);
            if (fill.a > 0) {
                adwaita_disc(g.maximize, fill);
            }
            glyph_max_at(g.maximize, ah, alw, fg);
        }
        if (g.close.size.width > 0.0F) {
            Color fill = s.style.button_tint;
            const bool filled = button_fill(2, s.style.close_hover, s.style.close_pressed, fill);
            if (fill.a > 0) {
                adwaita_disc(g.close, fill);
            }
            glyph_close_at(g.close, ah, alw, filled ? Color{255, 255, 255, 235} : fg);
        }
    } else if (s.style.button_layout == TitleBarButtonLayout::Windows) {
        // Windows：整高矩形热区，悬停整块填充；字形按 10×10dp 设计网格（见文件顶部常量）。
        if (g.minimize.size.width > 0.0F) {
            Color fill;
            if (button_fill(0, s.style.hover_tint, s.style.pressed_tint, fill)) {
                p.fill_rect(g.minimize, fill);
            }
            glyph_min_at(g.minimize, wh, wlw, fg);
        }
        if (g.maximize.size.width > 0.0F) {
            Color fill;
            if (button_fill(1, s.style.hover_tint, s.style.pressed_tint, fill)) {
                p.fill_rect(g.maximize, fill);
            }
            win_max(g.maximize, fg);
        }
        if (g.close.size.width > 0.0F) {
            Color fill;
            const bool filled = button_fill(2, s.style.close_hover, s.style.close_pressed, fill);
            if (filled) {
                p.fill_rect(g.close, fill);
            }
            glyph_close_at(g.close, wh, wlw, filled ? Color{255, 255, 255, 235} : fg);
        }
    } else {
        // macOS 红绿灯：三枚圆点常显且不依赖悬停；符号则是**整组同时浮现**（`NSWindow` 的 group
        // hover 语义——指针进入红绿灯组即三枚符号一起出现，而不是只显被指到的那一枚）；
        // 窗口失焦时三枚统一变浅灰且不显任何符号；按下即整枚（填色 + 描边 + 符号）一起加深，
        // 真机并不叠白罩。绿钮是「进入全屏」，符号为双向对角箭头（最大化态转内向）。
        const bool show_symbols = s.active && ((hb >= 0) || (s.pressed_button >= 0));
        if (g.close.size.width > 0.0F) {
            const bool pressed = s.pressed_button == 2;
            mac_dot(g.close, pressed, AURORA_MAC_CLOSE_FILL, AURORA_MAC_CLOSE_BORDER, AURORA_MAC_CLOSE_FILL_PRESSED,
                    AURORA_MAC_CLOSE_BORDER_PRESSED);
            if (show_symbols) {
                glyph_close_at(g.close, mch, mlw, pressed ? AURORA_MAC_CLOSE_SYMBOL_PRESSED : AURORA_MAC_CLOSE_SYMBOL);
            }
        }
        if (g.minimize.size.width > 0.0F) {
            const bool pressed = s.pressed_button == 0;
            mac_dot(g.minimize, pressed, AURORA_MAC_MIN_FILL, AURORA_MAC_MIN_BORDER, AURORA_MAC_MIN_FILL_PRESSED,
                    AURORA_MAC_MIN_BORDER_PRESSED);
            if (show_symbols) {
                glyph_min_at(g.minimize, mnh, mlw, pressed ? AURORA_MAC_MIN_SYMBOL_PRESSED : AURORA_MAC_MIN_SYMBOL);
            }
        }
        if (g.maximize.size.width > 0.0F) {
            const bool pressed = s.pressed_button == 1;
            mac_dot(g.maximize, pressed, AURORA_MAC_MAX_FILL, AURORA_MAC_MAX_BORDER, AURORA_MAC_MAX_FILL_PRESSED,
                    AURORA_MAC_MAX_BORDER_PRESSED);
            if (show_symbols) {
                glyph_fullscreen_at(g.maximize, mfh, mlw,
                                    pressed ? AURORA_MAC_MAX_SYMBOL_PRESSED : AURORA_MAC_MAX_SYMBOL,
                                    s.mode == WindowMode::Maximized);
            }
        }
    }
}

}  // namespace aurora::csd

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "aurora/animation/animator.h"
#include "aurora/app/scheduler.h"
#include "aurora/aurora.h"
#include "aurora/modifier/modifier.h"
#include "aurora/navigation/navigator_host.h"
#include "aurora/navigation/route.h"
#include "aurora/render/font_engine.h"
#include "aurora/render/painter.h"
#include "aurora/state/reactive.h"
#include "aurora/widget/bottom_nav_bar.h"
#include "aurora/widget/button.h"
#include "aurora/widget/canvas.h"
#include "aurora/widget/containers.h"
#include "aurora/widget/grid_view.h"
#include "aurora/widget/image_widget.h"
#include "aurora/widget/lazy_row.h"
#include "aurora/widget/scroll.h"
#include "aurora/widget/text.h"
#include "aurora/widget/text_input.h"

#include "google_play_data.h"

namespace au = aurora;

namespace gp::ui {

// ---- 双主题调色板 ----
struct GPTheme {
    Color bg;               // 页面背景
    Color surface;          // 卡片表面
    Color surface_2;        // 次级表面（搜索框/芯片底）
    Color surface_3;        // 顶栏 / 底栏
    Color text;             // 主文本
    Color text_secondary;   // 次级文本
    Color divider;          // 分隔线
    Color primary;          // 品牌主色
    Color on_primary;       // 主色上的前景
    Color chip_bg;          // 未选芯片底
    Color chip_text;        // 未选芯片字
    Color chip_active_bg;   // 选中芯片底
    Color chip_active_text; // 选中芯片字
    Color nav_pill;         // 底栏选中药丸
    Color shadow;           // 卡片投影
    Color rating_bg;        // 评分绿徽章底
    Color rating_text;      // 评分徽章字
    Color banner_text;      // 横幅前景
    Color section_icon;     // 章节图标
    Color search_bg;        // 搜索框底
};

inline auto gp_theme(bool dark) -> GPTheme {
    if (dark) {
        return GPTheme{
            .bg = au::Color{ 0x12, 0x12, 0x14, 0xFF },
            .surface = au::Color{ 0x1E, 0x1E, 0x21, 0xFF },
            .surface_2 = au::Color{ 0x2A, 0x2A, 0x2E, 0xFF },
            .surface_3 = au::Color{ 0x18, 0x18, 0x1B, 0xFF },
            .text = au::Color{ 0xF1, 0xF3, 0xF4, 0xFF },
            .text_secondary = au::Color{ 0x9A, 0x9C, 0xA1, 0xFF },
            .divider = au::Color{ 0x2C, 0x2C, 0x30, 0xFF },
            .primary = au::Color{ 0x8A, 0xB4, 0xF8, 0xFF },
            .on_primary = au::Color{ 0x12, 0x12, 0x14, 0xFF },
            .chip_bg = au::Color{ 0x2A, 0x2A, 0x2E, 0xFF },
            .chip_text = au::Color{ 0xD0, 0xD2, 0xD6, 0xFF },
            .chip_active_bg = au::Color{ 0x8A, 0xB4, 0xF8, 0xFF },
            .chip_active_text = au::Color{ 0x12, 0x12, 0x14, 0xFF },
            .nav_pill = au::Color{ 0x2A, 0x33, 0x47, 0xFF },
            .shadow = au::Color{ 0x00, 0x00, 0x00, 0x55 },
            .rating_bg = au::Color{ 0x0F, 0x9D, 0x58, 0xFF },
            .rating_text = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
            .banner_text = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
            .section_icon = au::Color{ 0x8A, 0xB4, 0xF8, 0xFF },
            .search_bg = au::Color{ 0x2A, 0x2A, 0x2E, 0xFF },
        };
    }
    return GPTheme{
        .bg = au::Color{ 0xF1, 0xF3, 0xF4, 0xFF },
        .surface = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
        .surface_2 = au::Color{ 0xEA, 0xEC, 0xEF, 0xFF },
        .surface_3 = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
        .text = au::Color{ 0x20, 0x21, 0x24, 0xFF },
        .text_secondary = au::Color{ 0x5F, 0x63, 0x68, 0xFF },
        .divider = au::Color{ 0xE3, 0xE5, 0xE8, 0xFF },
        .primary = au::Color{ 0x1A, 0x73, 0xE8, 0xFF },
        .on_primary = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
        .chip_bg = au::Color{ 0xEA, 0xEC, 0xEF, 0xFF },
        .chip_text = au::Color{ 0x3C, 0x40, 0x44, 0xFF },
        .chip_active_bg = au::Color{ 0x1A, 0x73, 0xE8, 0xFF },
        .chip_active_text = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
        .nav_pill = au::Color{ 0xE8, 0xF0, 0xFE, 0xFF },
        .shadow = au::Color{ 0x20, 0x21, 0x24, 0x2E },
        .rating_bg = au::Color{ 0x0F, 0x9D, 0x58, 0xFF },
        .rating_text = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
        .banner_text = au::Color{ 0xFF, 0xFF, 0xFF, 0xFF },
        .section_icon = au::Color{ 0x1A, 0x73, 0xE8, 0xFF },
        .search_bg = au::Color{ 0xEA, 0xEC, 0xEF, 0xFF },
    };
}

// ---- 类别映射 ----
inline auto tab_category(int tab) -> std::string_view {
    switch (tab) {
    case 1: return "games";
    case 2: return "movies";
    case 3: return "books";
    default: return "apps";
    }
}

inline auto tab_label(int tab) -> std::string_view {
    switch (tab) {
    case 1: return "Games";
    case 2: return "Movies & Music";
    case 3: return "Books";
    default: return "Apps";
    }
}

// 类别字符串 → 图标索引（apps/games/movies/books → 0/1/2/3）。
inline auto cat_icon(const std::string &cat) -> int {
    if (cat == "games") {
        return 1;
    }
    if (cat == "movies") {
        return 2;
    }
    if (cat == "books") {
        return 3;
    }
    return 0;
}

// ---- 绘图辅助 ----
constexpr float AURORA_PI = std::numbers::pi_v<float>;

// 自驱动动画用的稳定时钟与缓动（规避 Animator 悬垂指针风险）
using GpClock = std::chrono::steady_clock;

inline auto ease_out(float t) -> float {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - ((1.0f - t) * (1.0f - t));
}

// 微光占位块：底色 + 随时间左→右扫过的高光
inline void paint_shimmer_block(au::Painter &p, const au::Rect &rect, const GPTheme &th, float phase) {
    p.fill_rounded_rect(rect, 16.0f, th.surface_2);
    p.push_clip_rounded(rect, 16.0f);
    const float w = rect.size.width;
    const float off = -w + (phase * (2.0f * w));
    const au::Rect sheen{ .origin = au::Point{ .x = rect.origin.x + off, .y = rect.origin.y },
                          .size = au::Size{ .width = w * 0.45f, .height = rect.size.height } };
    p.draw_linear_gradient(sheen, sheen.origin,
                           au::Point{ .x = sheen.origin.x + sheen.size.width, .y = sheen.origin.y },
                           { au::Color{ 0xFF, 0xFF, 0xFF, 0x00 }, au::Color{ 0xFF, 0xFF, 0xFF, 0x55 },
                             au::Color{ 0xFF, 0xFF, 0xFF, 0x00 } },
                           { 0.0f, 0.5f, 1.0f });
    p.pop_clip();
}

inline auto fmt_rating(float r) -> std::string {
    const int v = static_cast<int>(std::round(r * 2.0f));
    return std::to_string(v / 2) + "." + std::to_string((v % 2) * 5);
}

inline void paint_stars(au::Painter &p, const au::Rect &b, float rating) {
    const int full = std::clamp(static_cast<int>(std::round(rating)), 0, 5);
    std::string s;
    s.reserve(5);
    for (int i = 0; i < 5; ++i) {
        s += (i < full) ? "★" : "☆";
    }
    const au::Font f{ .size_pt = 13.0f };
    p.draw_text(au::Rect{ .origin = b.origin, .size = b.size }, s, f, au::Color{ 0xFB, 0xBC, 0x04, 0xFF });
}

inline void draw_icon_image(au::Painter &p, const au::Rect &b, const gp::Image &img, float radius) {
    if (img.width <= 0 || img.height <= 0) {
        return;
    }
    p.push_clip_rounded(b, radius);
    p.draw_image(img, b);
    p.pop_clip();
}

inline void draw_star(au::Painter &p, au::Point center, float r, au::Color c, float lw = 1.4f) {
    std::vector<au::Point> pts;
    pts.reserve(10);
    for (int i = 0; i < 10; ++i) {
        const float ang = (-AURORA_PI / 2.0f) + (static_cast<float>(i) * AURORA_PI / 5.0f);
        const float rad = (i % 2 == 0) ? r : r * 0.45f;
        pts.emplace_back(au::Point{ .x = center.x + (std::cos(ang) * rad), .y = center.y + (std::sin(ang) * rad) });
    }
    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1) % pts.size();
        p.draw_line(pts[i], pts[j], lw, c);
    }
}

// ---- 通用文本节点 ----
inline auto text_node(const std::string &s, float size, int weight, au::Color color, int max_lines = 1) -> au::Node {
    return au::Node{ std::make_shared<au::Text>(au::TextProps{
        .content = s,
        .font = au::Font{ .size_pt = size, .weight = weight },
        .text_color = color,
        .max_lines = max_lines,
        .soft_wrap = true,
    }) };
}

// ---- 类别 / 通用图标（矢量，主题化）----
inline void draw_gp_icon(int idx, au::Painter &p, const au::Rect &b, bool selected, au::Color primary, au::Color sub) {
    const au::Color c = selected ? primary : sub;
    const float cx = b.origin.x + (b.size.width * 0.5f);
    const float cy = b.origin.y + (b.size.height * 0.5f);
    const float e = std::min(b.size.width, b.size.height) * 0.40f;
    if (idx == 0) { // 应用：2x2 网格
        const float cs = e * 0.84f;
        const float gap = e * 0.32f;
        const float off = (cs + gap) * 0.5f;
        const float rad = cs * 0.25f;
        for (int r = 0; r < 2; ++r) {
            for (int col = 0; col < 2; ++col) {
                au::Rect qr{ .origin = au::Point{ .x = cx - off + (static_cast<float>(col) * (cs + gap)),
                                                  .y = cy - off + (static_cast<float>(r) * (cs + gap)) },
                             .size = au::Size{ .width = cs, .height = cs } };
                p.fill_rounded_rect(qr, rad, c);
            }
        }
    } else if (idx == 1) { // 游戏：播放三角
        au::Point a{ .x = cx - (e * 0.55f), .y = cy - (e * 0.85f) };
        au::Point b2{ .x = cx - (e * 0.55f), .y = cy + (e * 0.85f) };
        au::Point c2{ .x = cx + (e * 0.90f), .y = cy };
        const float lw = e * 0.60f;
        p.draw_line(a, b2, lw, c);
        p.draw_line(b2, c2, lw, c);
        p.draw_line(c2, a, lw, c);
    } else if (idx == 2) { // 影音：胶片
        const float w = e * 1.90f;
        const float h = e * 1.40f;
        p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = cx - (w * 0.5f), .y = cy - (h * 0.5f) },
                                      .size = au::Size{ .width = w, .height = h } },
                            e * 0.20f, c);
        p.draw_line(au::Point{ .x = cx - (w * 0.5f), .y = cy }, au::Point{ .x = cx + (w * 0.5f), .y = cy }, e * 0.14f,
                    sub);
    } else { // 图书：双页
        const float pg_w = e * 0.90f;
        const float pg_h = e * 1.50f;
        p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = cx - e, .y = cy - (pg_h * 0.5f) },
                                      .size = au::Size{ .width = pg_w, .height = pg_h } },
                            e * 0.14f, c);
        p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = cx + e - pg_w, .y = cy - (pg_h * 0.5f) },
                                      .size = au::Size{ .width = pg_w, .height = pg_h } },
                            e * 0.14f, c);
    }
}

inline void draw_search_icon(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5f);
    const float cy = b.origin.y + (b.size.height * 0.5f);
    const float r = std::min(b.size.width, b.size.height) * 0.30f;
    p.draw_line(au::Point{ .x = cx - r, .y = cy - r }, au::Point{ .x = cx + r, .y = cy + r }, 2.0f, c);
    p.draw_line(au::Point{ .x = cx - r, .y = cy - r }, au::Point{ .x = cx + (r * 0.5f), .y = cy - (r * 0.2f) },
                r * 0.9f, c);
    p.draw_line(au::Point{ .x = cx - r, .y = cy - r }, au::Point{ .x = cx - (r * 0.2f), .y = cy + (r * 0.5f) },
                r * 0.9f, c);
    p.draw_line(au::Point{ .x = cx + (r * 0.5f), .y = cy - (r * 0.2f) },
                au::Point{ .x = cx - (r * 0.2f), .y = cy + (r * 0.5f) }, r * 0.9f, c);
}

inline void draw_menu_icon(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5f);
    const float cy = b.origin.y + (b.size.height * 0.5f);
    for (int i = -1; i <= 1; ++i) {
        p.draw_line(au::Point{ .x = cx - 8.0f, .y = cy + (static_cast<float>(i) * 6.0f) },
                    au::Point{ .x = cx + 8.0f, .y = cy + (static_cast<float>(i) * 6.0f) }, 2.0f, c);
    }
}

inline void draw_back_icon(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5f);
    const float cy = b.origin.y + (b.size.height * 0.5f);
    p.draw_line(au::Point{ .x = cx + 5.0f, .y = cy - 7.0f }, au::Point{ .x = cx - 5.0f, .y = cy }, 2.5f, c);
    p.draw_line(au::Point{ .x = cx - 5.0f, .y = cy }, au::Point{ .x = cx + 5.0f, .y = cy + 7.0f }, 2.5f, c);
}

inline void draw_chevron_right(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5f);
    const float cy = b.origin.y + (b.size.height * 0.5f);
    p.draw_line(au::Point{ .x = cx - 4.0f, .y = cy - 6.0f }, au::Point{ .x = cx + 4.0f, .y = cy }, 2.2f, c);
    p.draw_line(au::Point{ .x = cx + 4.0f, .y = cy }, au::Point{ .x = cx - 4.0f, .y = cy + 6.0f }, 2.2f, c);
}

inline void draw_theme_icon(au::Painter &p, const au::Rect &b, bool dark) {
    const float cx = b.origin.x + (b.size.width * 0.5f);
    const float cy = b.origin.y + (b.size.height * 0.5f);
    const float r = std::min(b.size.width, b.size.height) * 0.34f;
    const au::Color c = dark ? au::Color{ 0xFB, 0xBC, 0x04, 0xFF } : au::Color{ 0x1A, 0x73, 0xE8, 0xFF };
    if (dark) { // 月亮
        p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = cx - (r * 0.2f), .y = cy - r },
                                      .size = au::Size{ .width = r * 1.2f, .height = r * 2.0f } },
                            r * 0.6f, c);
        p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = cx - (r * 0.55f), .y = cy - (r * 0.9f) },
                                      .size = au::Size{ .width = r * 1.2f, .height = r * 1.8f } },
                            r * 0.6f, gp_theme(true).surface_3);
    } else { // 太阳
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * AURORA_PI / 4.0f;
            p.draw_line(au::Point{ .x = cx + (std::cos(a) * (r * 1.25f)), .y = cy + (std::sin(a) * (r * 1.25f)) },
                        au::Point{ .x = cx + (std::cos(a) * (r * 1.6f)), .y = cy + (std::sin(a) * (r * 1.6f)) }, 2.0f,
                        c);
        }
        p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = cx - r, .y = cy - r },
                                      .size = au::Size{ .width = r * 2.0f, .height = r * 2.0f } },
                            r * 0.7f, c);
    }
}

// 评分绿色徽章（含小星 + 数值）
inline void draw_rating_chip(au::Painter &p, const au::Rect &b, float rating, const GPTheme &th) {
    p.fill_rounded_rect(b, b.size.height * 0.5f, th.rating_bg);
    const float r = b.size.height * 0.30f;
    draw_star(p, au::Point{ .x = b.origin.x + (b.size.height * 0.42f), .y = b.origin.y + (b.size.height * 0.5f) }, r,
              th.rating_text, 1.2f);
    const float tx = b.origin.x + (b.size.height * 0.85f);
    p.draw_text(
        au::Rect{ .origin = au::Point{ .x = tx, .y = b.origin.y },
                  .size = au::Size{ .width = b.size.width - (tx - b.origin.x) - 6.0f, .height = b.size.height } },
        fmt_rating(rating), au::Font{ .size_pt = 12.0f, .weight = 700 }, th.rating_text);
}

// ---- 应用网格单元（GridView 用）----
class AppCell : public au::LeafWidget {
  public:
    // 仅持有指向数据源元素的指针（由 GridView 工厂闭包保活的 shared_ptr<vector> 提供），
    // 不按值拷贝整个 AppItem（含 144KB 程序化图标）。指针在 cell 存活期间始终有效，
    // 从而省去每格一份图标副本（稳态约 7MB）。
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    const gp::AppItem *m_item = nullptr;
    std::function<void(const std::string &)> on_open;
    au::Reactive<bool> *m_dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "AppCell"; }
    // 出场动画期间（alpha≈0 每帧变化）禁止 Display List 缓存，否则缓存回放会冻结首帧使卡片
    // 不可见；动画结束后内容稳定（t=1、无自驱动），恢复可缓存。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return m_appear_complete; }

    auto on_hover_change(bool entered) -> void override {
        m_hot = entered;
        m_pixel_valid = false; // 悬停态变化：像素缓存需重渲
        mark_needs_paint();
    }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            m_press = true;
            m_pixel_valid = false;
            mark_needs_paint();
            if (on_open) {
                on_open(m_item->id);
            }
            e.handled = true;
        } else if (e.action == au::MouseAction::Release) {
            m_press = false;
            m_pixel_valid = false;
            mark_needs_paint();
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = c.max.width, .height = 140.0f });
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        if (!m_appear_started) {
            m_appear_started = true;
            m_appear_start = GpClock::now();
        }
        const float t = ease_out(
            static_cast<float>(std::chrono::duration<double>(GpClock::now() - m_appear_start).count() / 0.32f));
        if (t < 1.0f) {
            m_appear_complete = false;
            mark_needs_paint();
            draw_cell(p, b, th, t); // 动画期间直接画（偏移上浮 + 淡入）
            return;
        }
        m_appear_complete = true;
        // 稳态：像素离屏缓存。阴影/圆角/文本是昂贵绘制原语，Scroll 整页重录离屏缓冲时若每帧
        // 重画全部单元格（30 格×blur10 阴影 SDF）可达数十毫秒；缓存后每帧仅一次 blit。
        // 缓存仅需容纳边框（AppCell 无阴影，避免相邻卡片投影叠加形成黑边）。
        constexpr float k_shad = 2.0f;
        const bool dark = (m_dark != nullptr && m_dark->get());
        if (!m_pixel || !m_pixel_valid || m_pixel_w != b.size.width || m_pixel_h != b.size.height ||
            m_pixel_dark != dark) {
            if (!m_pixel) {
                m_pixel = std::make_unique<au::Painter>();
            }
            m_pixel->set_scale(p.scale());
            m_pixel->begin(static_cast<int>(b.size.width + (2.0f * k_shad)),
                           static_cast<int>(b.size.height + (2.0f * k_shad)));
            draw_cell(*m_pixel, au::Rect{ .origin = au::Point{ .x = k_shad, .y = k_shad }, .size = b.size }, th, 1.0f);
            m_pixel_w = b.size.width;
            m_pixel_h = b.size.height;
            m_pixel_dark = dark;
            m_pixel_valid = true;
        }
        // 离屏缓存 composite 位置 snap 到整数物理像素，避免半像素偏移让缓存内已清晰的
        // 文本再次发虚（125%/175% DPI 下列宽非整数时尤其明显）。
        const float sx = std::floor(((b.origin.x - k_shad) * p.scale()) + 0.5f) / p.scale();
        const float sy = std::floor(((b.origin.y - k_shad) * p.scale()) + 0.5f) / p.scale();
        p.composite(*m_pixel, au::Matrix2D::from_translate(sx, sy));
    }

  private:
    auto draw_cell(au::Painter &p, const au::Rect &r, const GPTheme &th, float alpha) const -> void {
        p.set_alpha(alpha);
        constexpr float radius = 16.0f;
        p.fill_rounded_rect(r, radius, th.surface);
        if (m_hot) {
            p.draw_rounded_border(r, radius, 1.5f, th.primary);
        } else if (m_press) {
            p.draw_rounded_border(r, radius, 1.5f, th.primary.with_alpha(120));
        } else {
            p.draw_rounded_border(r, radius, 1.0f, th.divider);
        }

        constexpr float pad = 12.0f;
        constexpr float icon = 64.0f;
        draw_icon_image(p,
                        au::Rect{ .origin = r.origin + au::Point{ .x = pad, .y = pad },
                                  .size = au::Size{ .width = icon, .height = icon } },
                        m_item->icon, 14.0f);

        const float tx = r.origin.x + pad;
        float ty = r.origin.y + pad + icon + 8.0f;
        const float tw = r.size.width - (2.0f * pad);
        p.draw_text(au::Rect{ .origin = au::Point{ .x = tx, .y = ty }, .size = au::Size{ .width = tw, .height = 18 } },
                    m_item->name, au::Font{ .size_pt = 14.0f, .weight = 600 }, th.text);
        ty += 20.0f;
        p.draw_text(au::Rect{ .origin = au::Point{ .x = tx, .y = ty }, .size = au::Size{ .width = tw, .height = 16 } },
                    m_item->developer, au::Font{ .size_pt = 12.0f }, th.text_secondary);
        ty += 18.0f;
        draw_rating_chip(
            p, au::Rect{ .origin = au::Point{ .x = tx, .y = ty }, .size = au::Size{ .width = 54.0f, .height = 18.0f } },
            m_item->rating, th);
        p.set_alpha(1.0);
    }

    bool m_hot = false;
    bool m_press = false;
    bool m_appear_started = false;
    bool m_appear_complete = false;
    GpClock::time_point m_appear_start;
    std::unique_ptr<au::Painter> m_pixel;
    bool m_pixel_valid = false;
    float m_pixel_w = 0.0f;
    float m_pixel_h = 0.0f;
    bool m_pixel_dark = false;
};

// ---- 推荐行单元（LazyRow 用，方形）----
class RecoCell : public au::LeafWidget {
  public:
    // 同 AppCell：引用数据源元素，避免按值拷贝图标（稳态约 7MB）。
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    const gp::AppItem *m_item = nullptr;
    std::function<void(const std::string &)> on_open;
    au::Reactive<bool> *m_dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "RecoCell"; }
    // 同 AppCell：出场动画期间禁止缓存，动画结束后恢复缓存（避免 Scroll 整页重录每帧重画）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return m_appear_complete; }

    auto on_hover_change(bool entered) -> void override {
        m_hot = entered;
        m_pixel_valid = false;
        mark_needs_paint();
    }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            m_press = true;
            m_pixel_valid = false;
            mark_needs_paint();
            if (on_open) {
                on_open(m_item->id);
            }
            e.handled = true;
        } else if (e.action == au::MouseAction::Release) {
            m_press = false;
            m_pixel_valid = false;
            mark_needs_paint();
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = c.max.width, .height = c.max.height });
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        if (!m_appear_started) {
            m_appear_started = true;
            m_appear_start = GpClock::now();
        }
        const float t = ease_out(
            static_cast<float>(std::chrono::duration<double>(GpClock::now() - m_appear_start).count() / 0.32f));
        if (t < 1.0f) {
            m_appear_complete = false;
            mark_needs_paint();
            draw_cell(p, b, th, t);
            return;
        }
        m_appear_complete = true;
        // 稳态：像素离屏缓存（同 AppCell；RecoCell 无阴影，缓存仅需容纳边框）。
        constexpr float k_shad = 2.0f;
        const bool dark = (m_dark != nullptr && m_dark->get());
        if (!m_pixel || !m_pixel_valid || m_pixel_w != b.size.width || m_pixel_h != b.size.height ||
            m_pixel_dark != dark) {
            if (!m_pixel) {
                m_pixel = std::make_unique<au::Painter>();
            }
            m_pixel->set_scale(p.scale());
            m_pixel->begin(static_cast<int>(b.size.width + (2.0f * k_shad)),
                           static_cast<int>(b.size.height + (2.0f * k_shad)));
            draw_cell(*m_pixel, au::Rect{ .origin = au::Point{ .x = k_shad, .y = k_shad }, .size = b.size }, th, 1.0f);
            m_pixel_w = b.size.width;
            m_pixel_h = b.size.height;
            m_pixel_dark = dark;
            m_pixel_valid = true;
        }
        // 离屏缓存 composite 位置 snap 到整数物理像素，避免半像素偏移让缓存内已清晰的
        // 文本再次发虚（125%/175% DPI 下列宽非整数时尤其明显）。
        const float sx = std::floor(((b.origin.x - k_shad) * p.scale()) + 0.5f) / p.scale();
        const float sy = std::floor(((b.origin.y - k_shad) * p.scale()) + 0.5f) / p.scale();
        p.composite(*m_pixel, au::Matrix2D::from_translate(sx, sy));
    }

  private:
    auto draw_cell(au::Painter &p, const au::Rect &r, const GPTheme &th, float alpha) const -> void {
        p.set_alpha(alpha);
        constexpr float radius = 18.0f;
        p.fill_rounded_rect(r, radius, th.surface);
        if (m_hot) {
            p.draw_rounded_border(r, radius, 1.5f, th.primary);
        } else if (m_press) {
            p.draw_rounded_border(r, radius, 1.5f, th.primary.with_alpha(120));
        } else {
            p.draw_rounded_border(r, radius, 1.0f, th.divider);
        }

        constexpr float pad = 12.0f;
        const float icon = std::min(r.size.width, r.size.height) - (2.0f * pad);
        draw_icon_image(p,
                        au::Rect{ .origin = r.origin + au::Point{ .x = pad, .y = pad },
                                  .size = au::Size{ .width = icon, .height = icon } },
                        m_item->icon, 16.0f);
        const float tx = r.origin.x + pad;
        float ty = r.origin.y + pad + icon + 8.0f;
        const float tw = r.size.width - (2.0f * pad);
        p.draw_text(au::Rect{ .origin = au::Point{ .x = tx, .y = ty }, .size = au::Size{ .width = tw, .height = 18 } },
                    m_item->name, au::Font{ .size_pt = 14.0f, .weight = 600 }, th.text);
        ty += 20.0f;
        draw_rating_chip(
            p, au::Rect{ .origin = au::Point{ .x = tx, .y = ty }, .size = au::Size{ .width = 54.0f, .height = 18.0f } },
            m_item->rating, th);
        p.set_alpha(1.0);
    }

    bool m_hot = false;
    bool m_press = false;
    bool m_appear_started = false;
    bool m_appear_complete = false;
    GpClock::time_point m_appear_start;
    std::unique_ptr<au::Painter> m_pixel;
    bool m_pixel_valid = false;
    float m_pixel_w = 0.0f;
    float m_pixel_h = 0.0f;
    bool m_pixel_dark = false;
};

// ---- 精品横幅轮播（圆点 + 点击切换，渐变卡片）----
class BannerCarousel : public au::LeafWidget {
  public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    std::shared_ptr<std::vector<gp::AppItem>> m_items;
    std::function<void(const std::string &)> on_open;
    au::Reactive<bool> *m_dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "BannerCarousel"; }
    // banner 每帧整体平移（m_offset）+ 入场淡入，内容本身（渐变/图标/文本）在滑动期间不变：
    // 故不复用 Display List 缓存（缓存会冻结平移与 alpha 首帧），改为把卡片一次性渲染进离屏层、
    // 每帧仅平移合成（见 on_paint），把每帧成本从「重录重放整棵子树」压到一次 composite。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        const float w = size().width;
        if (e.action == au::MouseAction::Press) {
            const float lx = e.local_position.x;
            const float ly = e.local_position.y;
            // 圆点区域
            if (ly > size().height - 22.0f) {
                const int n = static_cast<int>(m_items->size());
                const float total = static_cast<float>(n) * 14.0f;
                const float sx = (w - total) * 0.5f;
                for (int i = 0; i < n; ++i) {
                    if (lx >= sx + (static_cast<float>(i) * 14.0f) &&
                        lx <= sx + ((static_cast<float>(i) + 1.0f) * 14.0f)) {
                        jump_to(i);
                        e.handled = true;
                        return;
                    }
                }
                return;
            }
            // 横幅命中
            const int n = static_cast<int>(m_items->size());
            for (int i = 0; i < n; ++i) {
                const float x = m_left + (static_cast<float>(i) * m_step) + m_offset;
                if (lx >= x && lx <= x + m_banner_w && ly >= 0.0f && ly <= size().height - 24.0f) {
                    if (on_open) {
                        on_open((*m_items)[i].id); // NOLINT(*-redundant-parentheses)
                    }
                    e.handled = true;
                    return;
                }
            }
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        const float w = c.max.width;
        m_banner_w = w * 0.86f;
        m_step = m_banner_w + 16.0f;
        m_left = (w - m_banner_w) * 0.5f;
        return c.constrain(au::Size{ .width = w, .height = 196.0f });
    }
    // NOLINTNEXTLINE(readability-function-cognitive-complexity) 演示绘制逻辑，分支密度高属固有复杂度
    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        const int n = static_cast<int>(m_items->size());

        // 自驱动：平滑滑动由 on_paint 每帧推进；目标切换由 Scheduler 定时任务（on_mount 注册，
        // 见 schedule_next）周期性触发，仅在真实过渡时 mark_needs_paint。静止等待态不再自标脏保活——
        // 旧逻辑每 0.5s 假标脏会骗过 Scroll 把整块离屏缓冲重录，拖垮帧率。
        const auto now = GpClock::now();
        if (!m_clock_started) {
            m_clock_started = true;
            m_last = now;
        }
        float dt = std::chrono::duration<float>(now - m_last).count();
        m_last = now;
        dt = std::min(dt, 0.1f);
        if (n > 1) {
            const float target_off = -static_cast<float>(m_target) * m_step;
            const float diff = target_off - m_offset;
            if (std::fabs(diff) > 0.5f) {
                const float speed = m_step / 0.45f; // 约 0.45s 滑到位
                const float step = std::clamp(speed * dt, 0.0f, std::fabs(diff));
                m_offset += (diff > 0.0f ? 1.0f : -1.0f) * step;
                mark_needs_paint(); // 滑动中每帧驱动，过渡结束自然停止
            } else {
                m_offset = target_off;
                m_index = m_target;
            }
        }

        // 入场淡入
        if (!m_appear_started) {
            m_appear_started = true;
            m_appear_start = now;
        }
        const float ent = ease_out(std::chrono::duration<float>(now - m_appear_start).count() / 0.35f);
        if (ent < 1.0f) {
            mark_needs_paint();
        }
        p.set_alpha(static_cast<double>(ent));

        // 离屏层缓存：横幅卡片（渐变+图标+文本）内容在滑动/轮播期间每帧不变，仅整体平移（m_offset）
        // 与入场淡入（alpha）。若每帧直绘，这些命令被并入祖先 Display List，随祖先每帧重录
        // （见 Widget::invalidate_display_list_up 对不可缓存后代的上溯失效）产生 ~8ms 级 glue；
        // 改为一次性渲染到离屏层、每帧仅按 m_offset 平移合成，把每帧成本压到一次 composite + 几个圆点。
        m_card_h = b.size.height - 24.0f;
        if (n > 0 && m_banner_w > 0.0f) {
            const float lw = static_cast<float>(n) * m_step;
            const bool dark = m_dark != nullptr && m_dark->get();
            if (!m_layer_valid || std::fabs(m_layer_w - lw) > 0.5f || std::fabs(m_layer_h - m_card_h) > 0.5f ||
                m_layer_dark != dark) {
                m_layer = std::make_unique<au::Painter>();
                m_layer->set_scale(p.scale());
                m_layer->begin(static_cast<int>(std::ceil(lw)), static_cast<int>(std::ceil(m_card_h)));
                render_banner_layer(*m_layer, th, n);
                m_layer_w = lw;
                m_layer_h = m_card_h;
                m_layer_dark = dark;
                m_layer_valid = true;
            }
        }

        p.push_clip(b);
        p.set_alpha(static_cast<double>(ent));
        if (m_layer) {
            // 整条卡片带一次性渲染进离屏层（offset=0，层局部坐标卡片 i 位于 x=i*m_step），每帧按当前
            // m_offset 平移合成到 banner 视口；裁剪保证仅可见窗口参与合成。
            p.composite(*m_layer, au::Matrix2D::from_translate(b.origin.x + m_left + m_offset, b.origin.y));
        } else {
            draw_banner_direct(p, th, n, b); // 兜底：首帧 layout 前尺寸未就绪时直绘（与旧逻辑一致）
        }
        if (n > 1) {
            const float total = static_cast<float>(n) * 14.0f;
            const float sx = b.origin.x + ((b.size.width - total) * 0.5f);
            const float cy = b.origin.y + b.size.height - 12.0f;
            for (int i = 0; i < n; ++i) {
                const bool active = (i == m_target);
                const float dot_w = active ? 18.0f : 6.0f;
                const float dx = sx + (static_cast<float>(i) * 14.0f) + ((14.0f - dot_w) * 0.5f);
                p.fill_rounded_rect(au::Rect{ .origin = au::Point{ .x = dx, .y = cy - 3.0f },
                                              .size = au::Size{ .width = dot_w, .height = 6.0f } },
                                    3.0f, active ? th.primary : th.text_secondary.with_alpha(120));
            }
        }
        p.pop_clip();
        p.set_alpha(1.0);
    }

    // 在给定 Painter 上绘制第 i 张卡片（卡片左上角 (x,y)）；卡片内容静态，离屏层与兜底直绘共用。
    auto draw_card(au::Painter &p, const GPTheme &th, int i, float x, float y) const -> void {
        const au::Rect card{ .origin = au::Point{ .x = x, .y = y },
                             .size = au::Size{ .width = m_banner_w, .height = m_card_h } };
        p.push_clip_rounded(card, 20.0f);
        const AppItem &a = (*m_items)[i]; // NOLINT(*-redundant-parentheses)
        p.draw_linear_gradient(card, card.origin,
                               au::Point{ .x = card.origin.x, .y = card.origin.y + card.size.height },
                               { a.color_a, a.color_b }, { 0.0f, 1.0f });
        p.pop_clip();
        constexpr float icon = 80.0f;
        draw_icon_image(p,
                        au::Rect{ .origin = card.origin + au::Point{ .x = 18.0f, .y = 18.0f },
                                  .size = au::Size{ .width = icon, .height = icon } },
                        a.icon, 18.0f);
        float ty = card.origin.y + 18.0f + icon + 10.0f;
        p.draw_text(au::Rect{ .origin = au::Point{ .x = card.origin.x + 18.0f, .y = ty },
                              .size = au::Size{ .width = card.size.width - 36.0f, .height = 22 } },
                    a.name, au::Font{ .size_pt = 18.0f, .weight = 700 }, th.banner_text);
        ty += 24.0f;
        p.draw_text(au::Rect{ .origin = au::Point{ .x = card.origin.x + 18.0f, .y = ty },
                              .size = au::Size{ .width = card.size.width - 36.0f, .height = 18 } },
                    a.developer, au::Font{ .size_pt = 13.0f }, th.banner_text.with_alpha(230));
    }

    // 一次性把全部卡片渲染进离屏层（offset=0，层局部坐标：卡片 i 位于 x=i*m_step，y=0）。
    auto render_banner_layer(au::Painter &lp, const GPTheme &th, int n) const -> void {
        for (int i = 0; i < n; ++i) {
            draw_card(lp, th, i, static_cast<float>(i) * m_step, 0.0f);
        }
    }

    // 兜底：首帧 layout 前尺寸未就绪时直绘（与旧逻辑一致，按当前 m_offset 摆放）。
    auto draw_banner_direct(au::Painter &p, const GPTheme &th, int n, const au::Rect &b) const -> void {
        for (int i = 0; i < n; ++i) {
            const float x = b.origin.x + m_left + (static_cast<float>(i) * m_step) + m_offset;
            if (x > b.origin.x + b.size.width || x + m_banner_w < b.origin.x) {
                continue;
            }
            draw_card(p, th, i, x, b.origin.y);
        }
    }

    auto jump_to(int i) -> void {
        const int n = static_cast<int>(m_items->size());
        if (n <= 0) {
            return;
        }
        m_target = std::clamp(i, 0, n - 1);
        mark_needs_paint();
    }

    auto on_mount(const au::BuildContext & /*ctx*/) -> void override { schedule_next(); }

    /// @brief 注册一次性自动轮播定时任务：到点切换目标并标脏驱动滑动；用弱引用守卫，
    /// 控件销毁后定时器自动失效，避免悬空回调。无运行中 App（如无头基准）时 Scheduler 为空则跳过。
    auto schedule_next() -> void {
        auto *sch = au::Scheduler::current();
        if (sch == nullptr) {
            return;
        }
        const int n = static_cast<int>(m_items ? m_items->size() : 0);
        if (n <= 1) {
            return;
        }
        std::weak_ptr<au::Widget> self = weak_from_this();
        m_timer = sch->set_timeout(std::chrono::milliseconds(3600), [self, this]() -> void {
            if (self.lock() == nullptr) {
                return; // 控件已销毁：停止调度
            }
            const int cnt = static_cast<int>(m_items->size());
            if (cnt > 1) {
                m_target = (m_target + 1) % cnt;
                mark_needs_paint();
            }
            schedule_next(); // 重排下一轮
        });
    }

  private:
    float m_card_h = 0.0f;                // 卡片高（离屏层与直绘共用）
    std::unique_ptr<au::Painter> m_layer; // 横幅卡片离屏层（一次性渲染，每帧平移合成）
    bool m_layer_valid = false;
    float m_layer_w = 0.0f, m_layer_h = 0.0f;
    bool m_layer_dark = false; // 主题（明暗）变化需重建离屏层
    float m_offset = 0.0f;
    float m_step = 0.0f;
    float m_banner_w = 0.0f;
    float m_left = 0.0f;
    int m_index = 0;
    int m_target = 0;
    bool m_clock_started = false;
    GpClock::time_point m_last;
    bool m_appear_started = false;
    GpClock::time_point m_appear_start;
    au::TimerHandle m_timer; ///< 自动轮播定时任务句柄（on_mount 注册，控件销毁后由弱引用守卫失效）
};

// ---- 类目筛选 Chip ----
class FilterChip : public au::LeafWidget {
  public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    std::string label;
    bool selected = false;
    std::function<void()> on_tap;
    au::Reactive<bool> *m_dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FilterChip"; }

    auto on_hover_change(bool entered) -> void override {
        m_hot = entered;
        mark_needs_paint();
    }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            if (on_tap) {
                on_tap();
            }
            e.handled = true;
        }
    }

  protected:
    auto on_layout(const au::Constraints & /*c*/, const au::BuildContext & /*ctx*/) -> au::Size override {
        const au::Font f{ .size_pt = 13.0f };
        m_w = au::render::FontEngine::measure_width(label, f) + 28.0f;
        return au::Size{ .width = m_w, .height = 34.0f };
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        au::Color bg = th.chip_bg;
        if (selected) {
            bg = th.chip_active_bg;
        } else if (m_hot) {
            bg = th.chip_bg.shaded(0.96f);
        }
        p.fill_rounded_rect(b, 17.0f, bg);
        const au::Color tc = selected ? th.chip_active_text : th.chip_text;
        const au::Font f{ .size_pt = 13.0f, .weight = selected ? 600 : 400 };
        p.draw_text(au::Rect{ .origin = au::Point{ .x = b.origin.x + 14.0f, .y = b.origin.y },
                              .size = au::Size{ .width = b.size.width - 28.0f, .height = b.size.height } },
                    label, f, tc);
    }

  private:
    float m_w = 60.0f;
    bool m_hot = false;
};

// ---- 章节标题（图标 + 标题 + 查看全部）----
class SectionHeader : public au::LeafWidget {
  public:
    std::string title;
    int icon = 0;
    au::Reactive<bool> *m_dark = nullptr;

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SectionHeader"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = c.max.width, .height = 28.0f });
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        constexpr float s = 20.0f;
        draw_gp_icon(icon, p, au::Rect{ .origin = b.origin, .size = au::Size{ .width = s, .height = s } }, true,
                     th.primary, th.primary);
        p.draw_text(au::Rect{ .origin = au::Point{ .x = b.origin.x + s + 8.0f, .y = b.origin.y },
                              .size = au::Size{ .width = b.size.width - s - 80.0f, .height = b.size.height } },
                    title, au::Font{ .size_pt = 17.0f, .weight = 700 }, th.text);
        const std::string more = "See all";
        const float mw = au::render::FontEngine::measure_width(more, au::Font{ .size_pt = 13.0f });
        draw_chevron_right(
            p,
            au::Rect{ .origin = au::Point{ .x = b.origin.x + b.size.width - 16.0f, .y = b.origin.y + 4.0f },
                      .size = au::Size{ .width = 16.0f, .height = 20.0f } },
            th.text_secondary);
        p.draw_text(au::Rect{ .origin = au::Point{ .x = b.origin.x + b.size.width - 22.0f - mw, .y = b.origin.y },
                              .size = au::Size{ .width = mw, .height = b.size.height } },
                    more, au::Font{ .size_pt = 13.0f }, th.text_secondary);
    }
};

// ---- 底部导航（主题化，选中药丸）----
class GpBottomNav : public au::LeafWidget {
  public:
    std::vector<au::BottomNavItem> items;
    int selected_index = 0;
    std::function<void(int)> on_select;
    au::Reactive<bool> *m_dark = nullptr;

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "GpBottomNav"; }

    // 自管按下/抬起命中分发的叶控件：无 Clickable 修饰，必须显式声明为点击目标，
    // 否则 hit_test_chain 中 self_hit==false，整棵控件被排除在命中链外、on_pointer_event
    // 永不触发（底部「游戏」等图标无反应）。
    [[nodiscard]] auto wants_click() const -> bool override { return true; }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            const int n = static_cast<int>(items.size());
            const float w = size().width / static_cast<float>(n);
            const int idx = std::clamp(static_cast<int>(e.local_position.x / w), 0, n - 1);
            if (on_select) {
                on_select(idx);
            }
            e.handled = true;
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = c.max.width, .height = 64.0f });
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        p.fill_rect(au::Rect{ .origin = b.origin, .size = au::Size{ .width = b.size.width, .height = 1.0f } },
                    th.divider);
        p.fill_rect(au::Rect{ .origin = au::Point{ .x = b.origin.x, .y = b.origin.y + 1.0f },
                              .size = au::Size{ .width = b.size.width, .height = b.size.height - 1.0f } },
                    th.surface_3);
        const int n = static_cast<int>(items.size());
        const float w = b.size.width / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            const bool sel = (i == selected_index);
            const au::Rect cell{ .origin = au::Point{ .x = b.origin.x + (static_cast<float>(i) * w), .y = b.origin.y },
                                 .size = au::Size{ .width = w, .height = b.size.height } };
            constexpr float pw = 56.0f;
            constexpr float ph = 36.0f;
            const au::Rect pill{ .origin =
                                     au::Point{ .x = cell.origin.x + ((w - pw) * 0.5f), .y = cell.origin.y + 6.0f },
                                 .size = au::Size{ .width = pw, .height = ph } };
            if (sel) {
                p.fill_rounded_rect(pill, 18.0f, th.nav_pill);
            }
            // 图标水平居中于单元格，并在 pill 内垂直居中
            constexpr float icon_size = 24.0f;
            const au::Rect icon_box{ .origin = au::Point{ .x = cell.origin.x + ((w - icon_size) * 0.5f),
                                                          .y = cell.origin.y + 6.0f + ((ph - icon_size) * 0.5f) },
                                     .size = au::Size{ .width = icon_size, .height = icon_size } };
            items[i].icon(p, icon_box, sel);
            // 文字水平居中于单元格（draw_text 默认左对齐，需按测量宽度定位）
            const au::Font label_font{ .size_pt = 11.0f, .weight = sel ? 600 : 400 };
            const float tw = au::render::FontEngine::measure_width(items[i].label, label_font);
            const float tx = cell.origin.x + ((w - tw) * 0.5f);
            p.draw_text(au::Rect{ .origin = au::Point{ .x = tx, .y = cell.origin.y + 34.0f },
                                  .size = au::Size{ .width = tw, .height = 16.0f } },
                        items[i].label, label_font, sel ? th.primary : th.text_secondary);
        }
    }
};

// ---- 主体内容（按状态重建）----
// ---- 首屏骨架屏（微光占位，自驱动）----
class SkeletonScreen : public au::LeafWidget {
  public:
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes) 演示字段，工厂直接赋值
    au::Reactive<bool> *m_dark = nullptr;

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SkeletonScreen"; }
    // 微光骨架屏每帧 mark_needs_paint 自驱动：禁止 Display List 缓存，否则回放冻结首帧。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{ .width = c.max.width, .height = 760.0f });
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        const auto now = GpClock::now();
        if (!m_started) {
            m_started = true;
            m_start = now;
        }
        const float t = std::fmod(std::chrono::duration<float>(now - m_start).count() * 0.8f, 1.0f);
        mark_needs_paint();

        constexpr float pad_x = 16.0f;
        const float w = b.size.width;
        const float cw = w - (2.0f * pad_x);
        float y = 0.0f;
        const float ox = b.origin.x;
        const float oy = b.origin.y;
        // 横幅占位
        paint_shimmer_block(p,
                            au::Rect{ .origin = au::Point{ .x = ox + pad_x, .y = oy + y },
                                      .size = au::Size{ .width = cw, .height = 196.0f } },
                            th, t);
        y += 196.0f + 14.0f;
        // 标题占位
        paint_shimmer_block(p,
                            au::Rect{ .origin = au::Point{ .x = ox + pad_x, .y = oy + y },
                                      .size = au::Size{ .width = 150.0f, .height = 24.0f } },
                            th, t);
        y += 24.0f + 14.0f;
        // 推荐行：4 个方块
        constexpr float sq = 150.0f;
        for (int i = 0; i < 4; ++i) {
            const float sx = ox + pad_x + (static_cast<float>(i) * (sq + 12.0f));
            paint_shimmer_block(
                p,
                au::Rect{ .origin = au::Point{ .x = sx, .y = oy + y }, .size = au::Size{ .width = sq, .height = sq } },
                th, std::fmod(t + (0.15f * static_cast<float>(i)), 1.0f));
        }
        y += sq + 14.0f;
        // 网格：3 列 x 2 行
        const float g = (cw - (2.0f * 12.0f)) / 3.0f;
        for (int r = 0; r < 2; ++r) {
            for (int col = 0; col < 3; ++col) {
                const float gx = ox + pad_x + (static_cast<float>(col) * (g + 12.0f));
                const float gy = oy + y + (static_cast<float>(r) * (140.0f + 12.0f));
                paint_shimmer_block(p,
                                    au::Rect{ .origin = au::Point{ .x = gx, .y = gy },
                                              .size = au::Size{ .width = g, .height = 140.0f } },
                                    th, std::fmod(t + (0.1f * static_cast<float>((r * 3) + col)), 1.0f));
            }
        }
    }

  private:
    bool m_started = false;
    GpClock::time_point m_start;
};

class BodyView : public au::Container {
  public:
    BodyView(au::Reactive<int> *tab, au::Reactive<std::string> *subcat, au::Reactive<std::string> *search,
             gp::PlayRepository *repo, std::function<void(const std::string &)> on_open, au::Reactive<bool> *dark)
        : m_tab(tab), m_subcat(subcat), m_search(search), m_repo(repo), m_on_open(std::move(on_open)), m_dark(dark),
          m_skeleton_until(GpClock::now() + std::chrono::milliseconds(700)), m_skeleton_active(true) {}

    auto collect_signals(std::vector<au::SignalViewBase *> &out) -> void override {
        out.push_back(m_tab);
        out.push_back(m_subcat);
        out.push_back(m_search);
        if (m_dark != nullptr) {
            out.push_back(m_dark);
        }
    }
    [[nodiscard]] auto type_name() const -> const char * override { return "BodyView"; }
    // 首屏骨架期间 on_paint 内 mark_needs_paint 自驱动，且随 tab/subcat 重建子节点：内容每帧
    // 变化，禁止 Display List 缓存，否则骨架微光冻结、子节点切换不重绘。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }
    // 首屏骨架→真实内容的切换发生在 on_layout 内、且依赖时间（m_skeleton_until）：
    // 若允许布局缓存，约束不变时会跳过 on_layout，使切换永不触发（白屏 / Path B）。
    // 故禁用布局缓存，保证每个布局 pass 都重跑 on_layout 以拾取状态变化（与 can_cache_display_list
    // 对称：绘制每帧变动→禁 DL 缓存，布局非纯函数→禁布局缓存）。
    [[nodiscard]] auto can_cache_layout() const -> bool override { return false; }

    auto set_viewport_width(float w) -> void { m_vp_width = w; }

  protected:
    // 首屏骨架屏：加载期显示微光占位，超时后重建真实内容
    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) -> void override {
        const auto now = GpClock::now();
        if (now < m_skeleton_until) {
            mark_needs_paint();
        } else if (m_skeleton_active) {
            m_skeleton_active = false;
            mark_needs_layout();
        }
        Container::on_paint(p, b, ctx);
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity) 演示布局重建逻辑，分支密度高属固有复杂度
    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> au::Size override {
        float w = 360.0f;
        if (m_vp_width > 0.0f) {
            w = m_vp_width;
        } else if (c.max.width < au::Size::infinity().width) {
            w = c.max.width;
        }
        m_children.clear();
        float y = 0.0f;
        constexpr float gap = 14.0f;
        constexpr float pad_x = 16.0f;

        auto add = [&](au::Node n) -> void {
            if (!n) {
                return;
            }
            const bool is_grid = std::string{ n.widget().type_name() } == "GridView";
            const float hh = is_grid ? 480.0f : au::Size::infinity().height;
            auto mod = n.widget().modifier.get();
            mod = mod.fill_max_width().padding(
                au::EdgeInsets{ .left = pad_x, .top = 0.0f, .right = pad_x, .bottom = 0.0f });
            n.widget().modifier = mod;
            n.widget().mount(ctx);
            n.widget().layout(au::Constraints{ .min = au::Size{ .width = 0.0f, .height = 0.0f },
                                               .max = au::Size{ .width = w, .height = hh } },
                              ctx);
            const float used = is_grid ? 480.0f : n.widget().size().height;
            n.set_bounds(
                au::Rect{ .origin = au::Point{ .x = 0.0f, .y = y }, .size = au::Size{ .width = w, .height = used } });
            y += used + gap;
            m_children.push_back(std::move(n));
        };

        // 首屏骨架屏：加载期用微光占位替代真实内容
        if (GpClock::now() < m_skeleton_until) {
            const auto sk = std::make_shared<SkeletonScreen>();
            sk->m_dark = m_dark;
            add(au::Node{ sk });
            return c.constrain(au::Size{ .width = w, .height = 760.0f });
        }

        const int tab = m_tab->get();
        const std::string sub = m_subcat->get();
        const std::string q = m_search->get();

        if (!q.empty()) {
            add(make_header("Search", 0));
            const auto res = m_repo->search(q);
            if (res.empty()) {
                const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
                const auto empty =
                    std::make_shared<au::Canvas>(300.0f, 120.0f, [th](au::Painter &p, const au::Rect &b) -> void {
                        p.draw_text(au::Rect{ .origin = b.origin, .size = b.size }, "No related apps found",
                                    au::Font{ .size_pt = 15.0f }, th.text_secondary);
                    });
                add(au::Node{ empty });
            } else {
                add(make_grid(res, (w > 760.0f) ? 4 : 3));
            }
        } else {
            const auto cat = std::string(tab_category(tab));
            add(make_header("Featured", cat_icon(cat)));
            auto feat = m_repo->featured();
            std::vector<gp::AppItem> fcat;
            for (const auto &a : feat) {
                if (a.category == cat) {
                    fcat.push_back(a);
                }
            }
            if (fcat.empty()) {
                fcat = std::move(feat);
            }
            add(make_banner_carousel(fcat));

            add(make_header("Recommended for you", cat_icon(cat)));
            const auto rec = m_repo->list_by_category(cat);
            add(make_reco_row(rec));

            add(make_filter_chips(cat, sub));

            const auto grid_items = sub.empty() ? m_repo->list_by_category(cat) : m_repo->list_by_subcategory(cat, sub);
            add(make_grid(grid_items, (w > 760.0f) ? 4 : 3));
        }
        return c.constrain(au::Size{ .width = w, .height = y });
    }

  private:
    auto make_header(const std::string &title, int icon) const -> au::Node {
        const auto h = std::make_shared<SectionHeader>();
        h->title = title;
        h->icon = icon;
        h->m_dark = m_dark;
        return au::Node{ h };
    }

    auto make_grid(const std::vector<gp::AppItem> &items, int cols) const -> au::Node {
        auto ptr = std::make_shared<std::vector<gp::AppItem>>(items);
        const auto g = std::make_shared<au::GridView>(
            static_cast<int>(ptr->size()), cols,
            [ptr, this](int i) -> au::Node {
                const auto cell = std::make_shared<AppCell>();
                cell->m_item = &ptr->at(i);
                cell->on_open = m_on_open;
                cell->m_dark = m_dark;
                return au::Node{ cell };
            },
            140.0f);
        g->set_cache_extent(300.0f);
        return au::Node{ g };
    }

    auto make_banner_carousel(const std::vector<gp::AppItem> &items) const -> au::Node {
        auto ptr = std::make_shared<std::vector<gp::AppItem>>(items);
        const auto carousel = std::make_shared<BannerCarousel>();
        carousel->m_items = std::move(ptr);
        carousel->on_open = m_on_open;
        carousel->m_dark = m_dark;
        return au::Node{ carousel };
    }

    auto make_reco_row(const std::vector<gp::AppItem> &items) const -> au::Node {
        auto ptr = std::make_shared<std::vector<gp::AppItem>>(items);
        auto row = std::make_shared<au::LazyRow>(
            static_cast<int>(ptr->size()),
            [ptr, this](int i) -> au::Node {
                auto cell = std::make_shared<RecoCell>();
                cell->m_item = &ptr->at(i);
                cell->on_open = m_on_open;
                cell->m_dark = m_dark;
                return au::Node{ cell };
            },
            150.0f);
        row->set_padding(au::EdgeInsets{ .left = 0.0f, .top = 0.0f, .right = 8.0f, .bottom = 0.0f });
        row->set_cache_extent(200.0f);
        row->set_on_item_click([ptr, this](int i) -> void {
            if (ptr != nullptr && i >= 0 && std::cmp_less(i, ptr->size())) {
                m_on_open((*ptr)[i].id); // NOLINT(*-redundant-parentheses)
            }
        });
        return au::Node{ row };
    }

    auto make_filter_chips(const std::string &cat, const std::string &sub) const -> au::Node {
        auto row = std::make_shared<au::Row>();
        row->modifier =
            au::Modifier{}.padding(au::EdgeInsets{ .left = 0.0f, .top = 0.0f, .right = 4.0f, .bottom = 4.0f });
        const auto subs = gp::subcategories_of(cat);
        bool first = true;
        for (const auto &s : subs) {
            if (!first) {
                auto sp = std::make_shared<au::Canvas>(8.0f, 34.0f, [](au::Painter &, const au::Rect &) -> void {});
                row->add(au::Node{ sp });
            }
            first = false;
            auto chip = std::make_shared<FilterChip>();
            chip->label = s;
            chip->selected = s == sub;
            chip->on_tap = [this, s]() -> void { m_subcat->set(s); };
            chip->m_dark = m_dark;
            row->add(au::Node{ chip });
        }
        return au::Node{ row };
    }

    au::Reactive<int> *m_tab;
    au::Reactive<std::string> *m_subcat;
    au::Reactive<std::string> *m_search;
    gp::PlayRepository *m_repo;
    std::function<void(const std::string &)> m_on_open;
    au::Reactive<bool> *m_dark;
    float m_vp_width = 0.0f;
    GpClock::time_point m_skeleton_until;
    bool m_skeleton_active = false;
};

// ---- 应用外壳（顶栏 + 内容 + 底栏）----
class AppShell : public au::Container {
  public:
    AppShell(PlayRepository *repo, std::function<void(const std::string &)> on_open, au::Reactive<bool> *dark)
        : m_repo(repo), m_on_open(std::move(on_open)), m_dark(dark) {}

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    au::Reactive<int> m_tab{ 0 };
    au::Reactive<std::string> m_subcat{ "" };
    au::Reactive<std::string> m_search{ "" };
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> &out) -> void override {
        out.push_back(&m_tab);
        out.push_back(&m_subcat);
        out.push_back(&m_search);
        if (m_dark != nullptr) {
            out.push_back(m_dark);
        }
    }
    [[nodiscard]] auto type_name() const -> const char * override { return "AppShell"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> au::Size override {
        const float w = c.max.width;
        const float h = c.max.height;
        // 读取安全区内边距（Wayland CSD 标题栏/边框占用区经 content_inset → MediaQuery.padding 并入）。
        // 子树据此下沉，避开自绘装饰（对齐 Flutter SafeArea 范式）。
        const auto safe = au::MediaQuery::of(ctx).padding;
        const float safe_top = safe.top; // CSD 标题栏高度（GNOME 下 ≈32px；KDE/其他下 = 0）
        const bool d = m_dark != nullptr && m_dark->get();
        if (!m_built || d != m_rendered_dark) {
            m_children.clear();
            auto top = build_top_bar(ctx);
            auto bv = std::make_shared<BodyView>(&m_tab, &m_subcat, &m_search, m_repo, m_on_open, m_dark);
            m_body_view = bv;
            auto body = au::Node{ std::make_shared<au::Scroll>(au::ScrollProps{ .child = au::Node{ std::move(bv) } }) };
            auto nav = std::make_shared<GpBottomNav>();
            nav->items = nav_items();
            nav->selected_index = m_tab.get();
            nav->on_select = [this](int idx) -> void {
                m_tab.set(idx);
                m_subcat.set("");
            };
            nav->m_dark = m_dark;
            m_nav_ptr = nav;
            m_children.push_back(std::move(top));
            m_children.push_back(std::move(body));
            m_children.emplace_back(std::move(nav));
            for (auto &n : m_children) {
                n.widget().mount(ctx);
            }
            for (auto &n : m_children) {
                n.widget().set_layout_parent(this);
            }
            m_rendered_dark = d;
            m_built = true;
        }
        if (m_body_view) {
            m_body_view->set_viewport_width(w);
        }
        // selected_index 由 AppShell 经 m_tab 驱动，但 GpBottomNav 未对它做响应式订阅；
        // 直接赋值不会使其显示列表失效，导致命中链/DL 复用旧帧（选中药丸不动 = 视觉「无反应」）。
        // 故在值真正变化时标脏，使导航栏重绘、选中药丸随 tab 移动。
        if (m_nav_ptr) {
            const int t = m_tab.get();
            if (m_nav_ptr->selected_index != t) {
                m_nav_ptr->selected_index = t;
                m_nav_ptr->mark_needs_paint();
            }
        }

        constexpr float top_h = 56.0f;
        constexpr float nav_h = 64.0f;
        // 安全区下沉：所有子节点 Y 偏移 safe_top，可用高度相应缩减。
        m_children[0].widget().layout(au::Constraints{ .min = au::Size{ .width = w, .height = top_h },
                                                       .max = au::Size{ .width = w, .height = top_h } },
                                      ctx);
        m_children[0].set_bounds(au::Rect{ .origin = au::Point{ .x = 0.0f, .y = safe_top },
                                           .size = au::Size{ .width = w, .height = top_h } });

        const float body_h = std::max(0.0f, h - safe_top - top_h - nav_h);
        m_children[1].widget().layout(au::Constraints{ .min = au::Size{ .width = w, .height = body_h },
                                                       .max = au::Size{ .width = w, .height = body_h } },
                                      ctx);
        m_children[1].set_bounds(au::Rect{ .origin = au::Point{ .x = 0.0f, .y = safe_top + top_h },
                                           .size = au::Size{ .width = w, .height = body_h } });

        m_children[2].widget().layout(au::Constraints{ .min = au::Size{ .width = w, .height = nav_h },
                                                       .max = au::Size{ .width = w, .height = nav_h } },
                                      ctx);
        m_children[2].set_bounds(au::Rect{ .origin = au::Point{ .x = 0.0f, .y = h - nav_h },
                                           .size = au::Size{ .width = w, .height = nav_h } });

        return c.constrain(au::Size{ .width = w, .height = h });
    }

  private:
    auto nav_items() const -> std::vector<au::BottomNavItem> {
        const bool d = m_dark != nullptr && m_dark->get();
        const GPTheme th = gp_theme(d);
        std::vector<au::BottomNavItem> items;
        for (int i = 0; i < 4; ++i) {
            const int idx = i;
            items.push_back(au::BottomNavItem{
                .icon = [idx, th](au::Painter &p, const au::Rect &b, bool sel) -> void {
                    draw_gp_icon(idx, p, b, sel, th.primary, th.text_secondary);
                },
                .label = std::string(tab_label(i)),
            });
        }
        return items;
    }

    auto build_top_bar(const au::BuildContext &ctx) -> au::Node {
        (void)ctx;
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        const auto row = std::make_shared<au::Row>();
        row->modifier = au::Modifier{}.background(th.surface_3).border(1.0f, th.divider);

        const auto menu = std::make_shared<au::Canvas>(
            40.0f, 40.0f, [th](au::Painter &p, const au::Rect &b) -> void { draw_menu_icon(p, b, th.text_secondary); });
        row->add(au::Node{ menu });

        const auto title = std::make_shared<au::Text>(au::TextProps{
            .content = "Google Play",
            .font = au::Font{ .size_pt = 20.0f, .weight = 700 },
            .text_color = th.primary,
        });
        title->modifier =
            au::Modifier{}.padding(au::EdgeInsets{ .left = 0.0f, .top = 8.0f, .right = 0.0f, .bottom = 8.0f });
        row->add(au::Node{ title });

        const auto search_row = std::make_shared<au::Row>();
        search_row->modifier =
            au::Modifier{}
                .expand()
                .background(th.search_bg)
                .clip_rounded(22.0f)
                .padding(au::EdgeInsets{ .left = 8.0f, .top = 12.0f, .right = 8.0f, .bottom = 12.0f });
        const auto search_icon =
            std::make_shared<au::Canvas>(20.0f, 20.0f, [th](au::Painter &p, const au::Rect &b) -> void {
                draw_search_icon(p, b, th.text_secondary);
            });
        search_row->add(au::Node{ search_icon });
        const auto input =
            std::make_shared<au::TextInput>(au::TextInputProps{ .placeholder = "Search apps and games" });
        input->set_on_changed([this](const std::string &t) -> void { m_search.set(t); });
        input->modifier = au::Modifier{}.expand();
        search_row->add(au::Node{ input });
        row->add(au::Node{ search_row });

        const auto theme_btn =
            std::make_shared<au::Canvas>(40.0f, 40.0f, [this](au::Painter &p, const au::Rect &b) -> void {
                const bool dark = m_dark != nullptr && m_dark->get();
                draw_theme_icon(p, b, dark);
            });
        theme_btn->modifier = au::Modifier{}.clickable([this]() -> void {
            if (m_dark != nullptr) {
                m_dark->set(!m_dark->get());
            }
        });
        row->add(au::Node{ theme_btn });

        return au::Node{ row };
    }

    gp::PlayRepository *m_repo;
    std::function<void(const std::string &)> m_on_open;
    au::Reactive<bool> *m_dark;
    std::shared_ptr<BodyView> m_body_view;
    std::shared_ptr<GpBottomNav> m_nav_ptr;
    bool m_built = false;
    bool m_rendered_dark = false;
};

// ---- 详情页 ----
class DetailPage : public au::Container {
  public:
    DetailPage(gp::PlayRepository *repo, std::string app_id, std::function<void()> on_back, au::Reactive<bool> *dark)
        : m_repo(repo), m_app_id(std::move(app_id)), m_on_back(std::move(on_back)), m_dark(dark) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "DetailPage"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> au::Size override {
        if (!m_built) {
            build(ctx);
            m_built = true;
        }
        const float w = c.max.width;
        const float h = c.max.height;
        constexpr float top_h = 56.0f;
        m_children[0].widget().layout(au::Constraints{ .min = au::Size{ .width = w, .height = top_h },
                                                       .max = au::Size{ .width = w, .height = top_h } },
                                      ctx);
        m_children[0].set_bounds(
            au::Rect{ .origin = au::Point{ .x = 0.0f, .y = 0.0f }, .size = au::Size{ .width = w, .height = top_h } });
        const float body_h = std::max(0.0f, h - top_h);
        m_children[1].widget().layout(au::Constraints{ .min = au::Size{ .width = w, .height = body_h },
                                                       .max = au::Size{ .width = w, .height = body_h } },
                                      ctx);
        m_children[1].set_bounds(
            au::Rect{ .origin = au::Point{ .x = 0.0f, .y = top_h }, .size = au::Size{ .width = w, .height = body_h } });
        return c.constrain(au::Size{ .width = w, .height = h });
    }

  private:
    auto build(const au::BuildContext &ctx) -> void {
        const GPTheme th = gp_theme(m_dark != nullptr && m_dark->get());
        const auto app = m_repo->detail(m_app_id);

        // 顶栏
        auto bar = std::make_shared<au::Row>();
        bar->modifier = au::Modifier{}.background(th.surface_3).border(1.0f, th.divider);
        auto back = std::make_shared<au::Canvas>(
            40.0f, 40.0f, [th](au::Painter &p, const au::Rect &b) -> void { draw_back_icon(p, b, th.primary); });
        back->modifier = au::Modifier{}.clickable(m_on_back);
        bar->add(au::Node{ back });
        auto t = std::make_shared<au::Text>(au::TextProps{
            .content = app.name,
            .font = au::Font{ .size_pt = 18.0f, .weight = 700 },
            .text_color = th.text,
        });
        t->modifier = au::Modifier{}.expand();
        bar->add(au::Node{ t });
        m_children.emplace_back(bar);

        // 内容列
        auto col = std::make_shared<au::Column>();

        // 渐变头图
        auto hero = std::make_shared<au::Row>();
        hero->modifier = au::Modifier{}.fill_max_width().padding(
            au::EdgeInsets{ .left = 16.0f, .top = 16.0f, .right = 8.0f, .bottom = 16.0f });
        auto icon = std::make_shared<au::ImageView>(app.icon);
        icon->modifier = au::Modifier{}.size(96.0f, 96.0f).clip_rounded(20.0f);
        hero->add(au::Node{ icon });

        auto info = std::make_shared<au::Column>();
        info->modifier = au::Modifier{}.expand().padding(
            au::EdgeInsets{ .left = 0.0f, .top = 12.0f, .right = 0.0f, .bottom = 0.0f });
        info->add(text_node(app.name, 20.0f, 700, th.text));
        info->add(text_node(app.developer, 14.0f, 400, th.text_secondary));
        auto stars = std::make_shared<au::Canvas>(
            110.0f, 16.0f, [app](au::Painter &p, const au::Rect &b) -> void { paint_stars(p, b, app.rating); });
        info->add(au::Node{ stars });

        auto btn = std::make_shared<au::Button>("Install");
        btn->background(th.primary);
        btn->text_color(th.on_primary);
        btn->set_corner_radius(20.0f);
        btn->set_min_size(120.0f, 40.0f);
        btn->set_on_click([this, btn, th]() -> void {
            const bool now = !m_installed.get();
            m_installed.set(now);
            if (now) {
                btn->set_label("Open");
                btn->background(au::Color{ 0x0F, 0x9D, 0x58, 0xFF });
            } else {
                btn->set_label("Install");
                btn->background(th.primary);
            }
        });
        info->add(au::Node{ btn });
        hero->add(au::Node{ info });
        col->add(au::Node{ hero });

        // 评分概览卡片
        auto rating_card = std::make_shared<au::Row>();
        rating_card->modifier =
            au::Modifier{}
                .background(th.surface)
                .border(1.0f, th.divider)
                .clip_rounded(16.0f)
                .padding(au::EdgeInsets{ .left = 14.0f, .top = 16.0f, .right = 14.0f, .bottom = 16.0f })
                .fill_max_width();
        rating_card->add(text_node(fmt_rating(app.rating), 26.0f, 700, th.text));
        auto right = std::make_shared<au::Column>();
        right->modifier = au::Modifier{}.expand().padding(
            au::EdgeInsets{ .left = 0.0f, .top = 12.0f, .right = 0.0f, .bottom = 0.0f });
        right->add(au::Node{ std::make_shared<au::Canvas>(
            120.0f, 16.0f, [app](au::Painter &p, const au::Rect &b) -> void { paint_stars(p, b, app.rating); }) });
        right->add(text_node(app.downloads + " downloads", 12.0f, 400, th.text_secondary));
        rating_card->add(au::Node{ right });
        col->add(au::Node{ rating_card });

        // 截图轮播
        auto shots_ptr = std::make_shared<std::vector<gp::Image>>(m_repo->screenshots_for(m_app_id));
        auto shot_row = std::make_shared<au::LazyRow>(
            static_cast<int>(shots_ptr->size()),
            [shots_ptr, th](int i) -> au::Node {
                const auto c = std::make_shared<au::Canvas>(
                    250.0f, 140.0f, [shots_ptr, th, i](au::Painter &p, const au::Rect &b) -> void {
                        p.draw_shadow(b, 0.0f, 3.0f, 8.0f, th.shadow);
                        p.fill_rounded_rect(b, 12.0f, th.surface);
                        if (static_cast<size_t>(i) < shots_ptr->size()) {
                            draw_icon_image(
                                p,
                                au::Rect{
                                    .origin = au::Point{ .x = b.origin.x + 4.0f, .y = b.origin.y + 4.0f },
                                    .size = au::Size{ .width = b.size.width - 8.0f, .height = b.size.height - 8.0f } },
                                (*shots_ptr)[i], 10.0f); // NOLINT(*-redundant-parentheses)
                        }
                    });
                return au::Node{ c };
            },
            250.0f);
        shot_row->set_padding(au::EdgeInsets{ .left = 16.0f, .top = 0.0f, .right = 16.0f, .bottom = 16.0f });
        shot_row->set_cache_extent(300.0f);
        col->add(au::Node{ shot_row });

        // 描述
        auto desc = text_node(app.description, 14.0f, 400, th.text, 0);
        desc.widget().modifier = au::Modifier{}.fill_max_width().padding(
            au::EdgeInsets{ .left = 8.0f, .top = 16.0f, .right = 8.0f, .bottom = 16.0f });
        col->add(desc);

        // 关于
        auto about = std::make_shared<au::Column>();
        about->modifier = au::Modifier{}
                              .background(th.surface)
                              .border(1.0f, th.divider)
                              .clip_rounded(16.0f)
                              .padding(au::EdgeInsets{ .left = 12.0f, .top = 12.0f, .right = 12.0f, .bottom = 12.0f });
        auto add_row = [&](const std::string &k, const std::string &v) -> void {
            const auto r = std::make_shared<au::Row>();
            r->add(text_node(k, 13.0f, 400, th.text_secondary));
            auto val = text_node(v, 13.0f, 600, th.text);
            val.widget().modifier = au::Modifier{}.expand();
            r->add(val);
            about->add(au::Node{ r });
        };
        add_row("Size", std::to_string(static_cast<int>(app.size_mb)) + " MB");
        add_row("Downloads", app.downloads);
        add_row("Version", app.version);
        add_row("Updated", app.updated);
        auto about_wrap = au::Node{ about };
        about_wrap.widget().modifier = au::Modifier{}.fill_max_width().padding(
            au::EdgeInsets{ .left = 8.0f, .top = 16.0f, .right = 8.0f, .bottom = 16.0f });
        col->add(about_wrap);

        // 评价
        {
            auto hd = text_node("Reviews", 16.0f, 700, th.text);
            hd.widget().modifier = au::Modifier{}.fill_max_width().padding(
                au::EdgeInsets{ .left = 8.0f, .top = 16.0f, .right = 4.0f, .bottom = 16.0f });
            col->add(hd);
        }
        for (const auto &rv : m_repo->reviews(m_app_id)) {
            auto card = std::make_shared<au::Column>();
            card->modifier =
                au::Modifier{}
                    .background(th.surface)
                    .border(1.0f, th.divider)
                    .clip_rounded(12.0f)
                    .padding(au::EdgeInsets{ .left = 10.0f, .top = 12.0f, .right = 10.0f, .bottom = 12.0f });
            auto head = std::make_shared<au::Row>();
            head->add(text_node(rv.user, 14.0f, 600, th.text));
            auto rating = std::make_shared<au::Canvas>(
                90.0f, 16.0f, [rv](au::Painter &p, const au::Rect &b) -> void { paint_stars(p, b, rv.rating); });
            head->add(au::Node{ rating });
            card->add(au::Node{ head });
            card->add(text_node(rv.text, 13.0f, 400, th.text_secondary, 0));
            auto cw = au::Node{ card };
            cw.widget().modifier = au::Modifier{}.fill_max_width().padding(
                au::EdgeInsets{ .left = 4.0f, .top = 16.0f, .right = 4.0f, .bottom = 16.0f });
            col->add(cw);
        }

        m_children.emplace_back(std::make_shared<au::Scroll>(au::ScrollProps{ .child = au::Node{ col } }));
        m_children[0].widget().mount(ctx);
        m_children[1].widget().mount(ctx);
        for (auto &n : m_children) {
            n.widget().set_layout_parent(this);
        }
    }

    gp::PlayRepository *m_repo;
    std::string m_app_id;
    std::function<void()> m_on_back;
    au::Reactive<bool> *m_dark;
    au::Reactive<bool> m_installed{ false };
    bool m_built = false;
};

// ---- 入口装配 ----
inline auto make_google_play_host(au::Animator &anim, gp::PlayRepository *repo,
                                  const std::function<void(const std::string &)> &on_open,
                                  au::Reactive<bool> *dark = nullptr) -> std::shared_ptr<au::NavigatorHost> {
    const auto host = std::make_shared<au::NavigatorHost>(anim);
    host->push(au::Route{ au::Node{ std::make_shared<AppShell>(repo, on_open, dark) }, "home", {} });
    return host;
}

inline void push_detail_route(const std::shared_ptr<au::NavigatorHost> &host, PlayRepository *repo,
                              const std::string &app_id, std::function<void()> on_back,
                              au::Reactive<bool> *dark = nullptr) {
    host->push(au::Route{
        au::Node{ std::make_shared<DetailPage>(repo, app_id, std::move(on_back), dark) }, "detail:" + app_id,
        au::RouteTransition{ .animated = true, .kind = au::TransitionKind::Slide, .duration_seconds = 0.28 } });
}

} // namespace gp::ui

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
    Color bg;  // 页面背景
    Color surface;  // 卡片表面
    Color surface_2;  // 次级表面（搜索框/芯片底）
    Color surface_3;  // 顶栏 / 底栏
    Color text;  // 主文本
    Color text_secondary;  // 次级文本
    Color divider;  // 分隔线
    Color primary;  // 品牌主色
    Color on_primary;  // 主色上的前景
    Color chip_bg;  // 未选芯片底
    Color chip_text;  // 未选芯片字
    Color chip_active_bg;  // 选中芯片底
    Color chip_active_text;  // 选中芯片字
    Color nav_pill;  // 底栏选中药丸
    Color shadow;  // 卡片投影
    Color rating_bg;  // 评分绿徽章底
    Color rating_text;  // 评分徽章字
    Color banner_text;  // 横幅前景
    Color section_icon;  // 章节图标
    Color search_bg;  // 搜索框底
};

inline auto gp_theme(bool dark) -> GPTheme {
    if (dark) {
        return GPTheme{
            .bg = au::Color{0x12, 0x12, 0x14, 0xFF},
            .surface = au::Color{0x1E, 0x1E, 0x21, 0xFF},
            .surface_2 = au::Color{0x2A, 0x2A, 0x2E, 0xFF},
            .surface_3 = au::Color{0x18, 0x18, 0x1B, 0xFF},
            .text = au::Color{0xF1, 0xF3, 0xF4, 0xFF},
            .text_secondary = au::Color{0x9A, 0x9C, 0xA1, 0xFF},
            .divider = au::Color{0x2C, 0x2C, 0x30, 0xFF},
            .primary = au::Color{0x8A, 0xB4, 0xF8, 0xFF},
            .on_primary = au::Color{0x12, 0x12, 0x14, 0xFF},
            .chip_bg = au::Color{0x2A, 0x2A, 0x2E, 0xFF},
            .chip_text = au::Color{0xD0, 0xD2, 0xD6, 0xFF},
            .chip_active_bg = au::Color{0x8A, 0xB4, 0xF8, 0xFF},
            .chip_active_text = au::Color{0x12, 0x12, 0x14, 0xFF},
            .nav_pill = au::Color{0x2A, 0x33, 0x47, 0xFF},
            .shadow = au::Color{0x00, 0x00, 0x00, 0x55},
            .rating_bg = au::Color{0x0F, 0x9D, 0x58, 0xFF},
            .rating_text = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
            .banner_text = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
            .section_icon = au::Color{0x8A, 0xB4, 0xF8, 0xFF},
            .search_bg = au::Color{0x2A, 0x2A, 0x2E, 0xFF},
        };
    }
    return GPTheme{
        .bg = au::Color{0xF1, 0xF3, 0xF4, 0xFF},
        .surface = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
        .surface_2 = au::Color{0xEA, 0xEC, 0xEF, 0xFF},
        .surface_3 = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
        .text = au::Color{0x20, 0x21, 0x24, 0xFF},
        .text_secondary = au::Color{0x5F, 0x63, 0x68, 0xFF},
        .divider = au::Color{0xE3, 0xE5, 0xE8, 0xFF},
        .primary = au::Color{0x1A, 0x73, 0xE8, 0xFF},
        .on_primary = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
        .chip_bg = au::Color{0xEA, 0xEC, 0xEF, 0xFF},
        .chip_text = au::Color{0x3C, 0x40, 0x44, 0xFF},
        .chip_active_bg = au::Color{0x1A, 0x73, 0xE8, 0xFF},
        .chip_active_text = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
        .nav_pill = au::Color{0xE8, 0xF0, 0xFE, 0xFF},
        .shadow = au::Color{0x20, 0x21, 0x24, 0x2E},
        .rating_bg = au::Color{0x0F, 0x9D, 0x58, 0xFF},
        .rating_text = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
        .banner_text = au::Color{0xFF, 0xFF, 0xFF, 0xFF},
        .section_icon = au::Color{0x1A, 0x73, 0xE8, 0xFF},
        .search_bg = au::Color{0xEA, 0xEC, 0xEF, 0xFF},
    };
}

// ---- 类别映射 ----
inline auto tab_category(int tab) -> std::string_view {
    switch (tab) {
        case 1:
            return "games";
        case 2:
            return "movies";
        case 3:
            return "books";
        default:
            return "apps";
    }
}

inline auto tab_label(int tab) -> std::string_view {
    switch (tab) {
        case 1:
            return "Games";
        case 2:
            return "Movies & Music";
        case 3:
            return "Books";
        default:
            return "Apps";
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
    t = std::clamp(t, 0.0F, 1.0F);
    return 1.0F - ((1.0F - t) * (1.0F - t));
}

// 微光占位块：底色 + 随时间左→右扫过的高光
inline void paint_shimmer_block(au::Painter &p, const au::Rect &rect, const GPTheme &th, float phase) {
    p.fill_rounded_rect(rect, 16.0F, th.surface_2);
    p.push_clip_rounded(rect, 16.0F);
    const float w = rect.size.width;
    const float off = -w + (phase * (2.0F * w));
    const au::Rect sheen{.origin = au::Point{.x = rect.origin.x + off, .y = rect.origin.y},
                         .size = au::Size{.width = w * 0.45F, .height = rect.size.height}};
    p.draw_linear_gradient(
        sheen, sheen.origin, au::Point{.x = sheen.origin.x + sheen.size.width, .y = sheen.origin.y},
        {au::Color{0xFF, 0xFF, 0xFF, 0x00}, au::Color{0xFF, 0xFF, 0xFF, 0x55}, au::Color{0xFF, 0xFF, 0xFF, 0x00}},
        {0.0F, 0.5F, 1.0F});
    p.pop_clip();
}

inline auto fmt_rating(float r) -> std::string {
    const int v = static_cast<int>(std::round(r * 2.0F));
    return std::to_string(v / 2) + "." + std::to_string((v % 2) * 5);
}

inline void paint_stars(au::Painter &p, const au::Rect &b, float rating) {
    const int full = std::clamp(static_cast<int>(std::round(rating)), 0, 5);
    std::string s;
    s.reserve(5);
    for (int i = 0; i < 5; ++i) {
        s += (i < full) ? "★" : "☆";
    }
    const au::Font f{.size_pt = 13.0F};
    p.draw_text(au::Rect{.origin = b.origin, .size = b.size}, s, f, au::Color{0xFB, 0xBC, 0x04, 0xFF});
}

inline void draw_icon_image(au::Painter &p, const au::Rect &b, const gp::Image &img, float radius) {
    if (img.width <= 0 || img.height <= 0) {
        return;
    }
    p.push_clip_rounded(b, radius);
    p.draw_image(img, b);
    p.pop_clip();
}

inline void draw_star(au::Painter &p, au::Point center, float r, au::Color c, float lw = 1.4F) {
    std::vector<au::Point> pts;
    pts.reserve(10);
    for (int i = 0; i < 10; ++i) {
        const float ang = (-AURORA_PI / 2.0F) + (static_cast<float>(i) * AURORA_PI / 5.0F);
        const float rad = (i % 2 == 0) ? r : r * 0.45F;
        pts.emplace_back(au::Point{.x = center.x + (std::cos(ang) * rad), .y = center.y + (std::sin(ang) * rad)});
    }
    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1) % pts.size();
        p.draw_line(pts[i], pts[j], lw, c);
    }
}

// ---- 通用文本节点 ----
inline auto text_node(const std::string &s, float size, int weight, au::Color color, int max_lines = 1) -> au::Node {
    return au::Node{std::make_shared<au::Text>(au::TextProps{
        .content = s,
        .font = au::Font{.size_pt = size, .weight = weight},
        .text_color = color,
        .max_lines = max_lines,
        .soft_wrap = true,
    })};
}

// ---- 类别 / 通用图标（矢量，主题化）----
inline void draw_gp_icon(int idx, au::Painter &p, const au::Rect &b, bool selected, au::Color primary, au::Color sub) {
    const au::Color c = selected ? primary : sub;
    const float cx = b.origin.x + (b.size.width * 0.5F);
    const float cy = b.origin.y + (b.size.height * 0.5F);
    const float e = std::min(b.size.width, b.size.height) * 0.40F;
    if (idx == 0) {  // 应用：2x2 网格
        const float cs = e * 0.84F;
        const float gap = e * 0.32F;
        const float off = (cs + gap) * 0.5F;
        const float rad = cs * 0.25F;
        for (int r = 0; r < 2; ++r) {
            for (int col = 0; col < 2; ++col) {
                au::Rect qr{.origin = au::Point{.x = cx - off + (static_cast<float>(col) * (cs + gap)),
                                                .y = cy - off + (static_cast<float>(r) * (cs + gap))},
                            .size = au::Size{.width = cs, .height = cs}};
                p.fill_rounded_rect(qr, rad, c);
            }
        }
    } else if (idx == 1) {  // 游戏：播放三角
        au::Point a{.x = cx - (e * 0.55F), .y = cy - (e * 0.85F)};
        au::Point b2{.x = cx - (e * 0.55F), .y = cy + (e * 0.85F)};
        au::Point c2{.x = cx + (e * 0.90F), .y = cy};
        const float lw = e * 0.60F;
        p.draw_line(a, b2, lw, c);
        p.draw_line(b2, c2, lw, c);
        p.draw_line(c2, a, lw, c);
    } else if (idx == 2) {  // 影音：胶片
        const float w = e * 1.90F;
        const float h = e * 1.40F;
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = cx - (w * 0.5F), .y = cy - (h * 0.5F)},
                                     .size = au::Size{.width = w, .height = h}},
                            e * 0.20F, c);
        p.draw_line(au::Point{.x = cx - (w * 0.5F), .y = cy}, au::Point{.x = cx + (w * 0.5F), .y = cy}, e * 0.14F, sub);
    } else {  // 图书：双页
        const float pg_w = e * 0.90F;
        const float pg_h = e * 1.50F;
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = cx - e, .y = cy - (pg_h * 0.5F)},
                                     .size = au::Size{.width = pg_w, .height = pg_h}},
                            e * 0.14F, c);
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = cx + e - pg_w, .y = cy - (pg_h * 0.5F)},
                                     .size = au::Size{.width = pg_w, .height = pg_h}},
                            e * 0.14F, c);
    }
}

inline void draw_search_icon(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5F);
    const float cy = b.origin.y + (b.size.height * 0.5F);
    const float r = std::min(b.size.width, b.size.height) * 0.30F;
    p.draw_line(au::Point{.x = cx - r, .y = cy - r}, au::Point{.x = cx + r, .y = cy + r}, 2.0F, c);
    p.draw_line(au::Point{.x = cx - r, .y = cy - r}, au::Point{.x = cx + (r * 0.5F), .y = cy - (r * 0.2F)}, r * 0.9F,
                c);
    p.draw_line(au::Point{.x = cx - r, .y = cy - r}, au::Point{.x = cx - (r * 0.2F), .y = cy + (r * 0.5F)}, r * 0.9F,
                c);
    p.draw_line(au::Point{.x = cx + (r * 0.5F), .y = cy - (r * 0.2F)},
                au::Point{.x = cx - (r * 0.2F), .y = cy + (r * 0.5F)}, r * 0.9F, c);
}

inline void draw_menu_icon(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5F);
    const float cy = b.origin.y + (b.size.height * 0.5F);
    for (int i = -1; i <= 1; ++i) {
        p.draw_line(au::Point{.x = cx - 8.0F, .y = cy + (static_cast<float>(i) * 6.0F)},
                    au::Point{.x = cx + 8.0F, .y = cy + (static_cast<float>(i) * 6.0F)}, 2.0F, c);
    }
}

inline void draw_back_icon(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5F);
    const float cy = b.origin.y + (b.size.height * 0.5F);
    p.draw_line(au::Point{.x = cx + 5.0F, .y = cy - 7.0F}, au::Point{.x = cx - 5.0F, .y = cy}, 2.5F, c);
    p.draw_line(au::Point{.x = cx - 5.0F, .y = cy}, au::Point{.x = cx + 5.0F, .y = cy + 7.0F}, 2.5F, c);
}

inline void draw_chevron_right(au::Painter &p, const au::Rect &b, au::Color c) {
    const float cx = b.origin.x + (b.size.width * 0.5F);
    const float cy = b.origin.y + (b.size.height * 0.5F);
    p.draw_line(au::Point{.x = cx - 4.0F, .y = cy - 6.0F}, au::Point{.x = cx + 4.0F, .y = cy}, 2.2F, c);
    p.draw_line(au::Point{.x = cx + 4.0F, .y = cy}, au::Point{.x = cx - 4.0F, .y = cy + 6.0F}, 2.2F, c);
}

inline void draw_theme_icon(au::Painter &p, const au::Rect &b, bool dark) {
    const float cx = b.origin.x + (b.size.width * 0.5F);
    const float cy = b.origin.y + (b.size.height * 0.5F);
    const float r = std::min(b.size.width, b.size.height) * 0.34F;
    const au::Color c = dark ? au::Color{0xFB, 0xBC, 0x04, 0xFF} : au::Color{0x1A, 0x73, 0xE8, 0xFF};
    if (dark) {  // 月亮
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = cx - (r * 0.2F), .y = cy - r},
                                     .size = au::Size{.width = r * 1.2F, .height = r * 2.0F}},
                            r * 0.6F, c);
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = cx - (r * 0.55F), .y = cy - (r * 0.9F)},
                                     .size = au::Size{.width = r * 1.2F, .height = r * 1.8F}},
                            r * 0.6F, gp_theme(true).surface_3);
    } else {  // 太阳
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * AURORA_PI / 4.0F;
            p.draw_line(au::Point{.x = cx + (std::cos(a) * (r * 1.25F)), .y = cy + (std::sin(a) * (r * 1.25F))},
                        au::Point{.x = cx + (std::cos(a) * (r * 1.6F)), .y = cy + (std::sin(a) * (r * 1.6F))}, 2.0F, c);
        }
        p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = cx - r, .y = cy - r},
                                     .size = au::Size{.width = r * 2.0F, .height = r * 2.0F}},
                            r * 0.7F, c);
    }
}

// 评分绿色徽章（含小星 + 数值）
inline void draw_rating_chip(au::Painter &p, const au::Rect &b, float rating, const GPTheme &th) {
    p.fill_rounded_rect(b, b.size.height * 0.5F, th.rating_bg);
    const float r = b.size.height * 0.30F;
    draw_star(p, au::Point{.x = b.origin.x + (b.size.height * 0.42F), .y = b.origin.y + (b.size.height * 0.5F)}, r,
              th.rating_text, 1.2F);
    const float tx = b.origin.x + (b.size.height * 0.85F);
    p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = b.origin.y},
                         .size = au::Size{.width = b.size.width - (tx - b.origin.x) - 6.0F, .height = b.size.height}},
                fmt_rating(rating), au::Font{.size_pt = 12.0F, .weight = 700}, th.rating_text);
}

// ---- 应用网格单元（GridView 用）----
class AppCell : public au::LeafWidget {
  public:
    // 仅持有指向数据源元素的指针（由 GridView 工厂闭包保活的 shared_ptr<vector> 提供），
    // 不按值拷贝整个 AppItem（含 144KB 程序化图标）。指针在 cell 存活期间始终有效，
    // 从而省去每格一份图标副本（稳态约 7MB）。
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    const gp::AppItem *item = nullptr;
    std::function<void(const std::string &)> on_open;
    au::Reactive<bool> *dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "AppCell"; }
    // 出场动画期间（alpha≈0 每帧变化）禁止 Display List 缓存，否则缓存回放会冻结首帧使卡片
    // 不可见；动画结束后内容稳定（t=1、无自驱动），恢复可缓存。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return appear_complete_; }

    auto on_hover_change(bool entered) -> void override {
        hot_ = entered;
        pixel_valid_ = false;  // 悬停态变化：像素缓存需重渲
        mark_needs_paint();
    }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            press_ = true;
            pixel_valid_ = false;
            mark_needs_paint();
            if (on_open) {
                on_open(item->id);
            }
            e.is_handled = true;
        } else if (e.action == au::MouseAction::Release) {
            press_ = false;
            pixel_valid_ = false;
            mark_needs_paint();
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = 140.0F});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        if (!appear_started_) {
            appear_started_ = true;
            appear_start_ = GpClock::now();
        }
        const float t =
            ease_out(static_cast<float>(std::chrono::duration<double>(GpClock::now() - appear_start_).count() / 0.32F));
        if (t < 1.0F) {
            appear_complete_ = false;
            mark_needs_paint();
            draw_cell(p, b, th, t);  // 动画期间直接画（偏移上浮 + 淡入）
            return;
        }
        appear_complete_ = true;
        // 稳态：像素离屏缓存。阴影/圆角/文本是昂贵绘制原语，Scroll 整页重录离屏缓冲时若每帧
        // 重画全部单元格（30 格×blur10 阴影 SDF）可达数十毫秒；缓存后每帧仅一次 blit。
        // 缓存仅需容纳边框（AppCell 无阴影，避免相邻卡片投影叠加形成黑边）。
        constexpr float k_shad = 2.0F;
        const bool is_dark = (dark != nullptr && dark->get());
        if (!pixel_ || !pixel_valid_ || pixel_w_ != b.size.width || pixel_h_ != b.size.height ||
            pixel_dark_ != dark->get()) {
            if (!pixel_) {
                pixel_ = std::make_unique<au::Painter>();
            }
            pixel_->set_scale(p.scale());
            pixel_->begin(static_cast<int>(b.size.width + (2.0F * k_shad)),
                          static_cast<int>(b.size.height + (2.0F * k_shad)));
            draw_cell(*pixel_, au::Rect{.origin = au::Point{.x = k_shad, .y = k_shad}, .size = b.size}, th, 1.0F);
            pixel_w_ = b.size.width;
            pixel_h_ = b.size.height;
            pixel_dark_ = is_dark;
            pixel_valid_ = true;
        }
        // 离屏缓存 composite 位置 snap 到整数物理像素，避免半像素偏移让缓存内已清晰的
        // 文本再次发虚（125%/175% DPI 下列宽非整数时尤其明显）。
        const float sx = std::floor(((b.origin.x - k_shad) * p.scale()) + 0.5F) / p.scale();
        const float sy = std::floor(((b.origin.y - k_shad) * p.scale()) + 0.5F) / p.scale();
        p.composite(*pixel_, au::Matrix2D::from_translate(sx, sy));
    }

  private:
    auto draw_cell(au::Painter &p, const au::Rect &r, const GPTheme &th, float alpha) const -> void {
        p.set_alpha(alpha);
        constexpr float radius = 16.0F;
        p.fill_rounded_rect(r, radius, th.surface);
        if (hot_) {
            p.draw_rounded_border(r, radius, 1.5F, th.primary);
        } else if (press_) {
            p.draw_rounded_border(r, radius, 1.5F, th.primary.with_alpha(120));
        } else {
            p.draw_rounded_border(r, radius, 1.0F, th.divider);
        }

        constexpr float pad = 12.0F;
        constexpr float icon = 64.0F;
        draw_icon_image(p,
                        au::Rect{.origin = r.origin + au::Point{.x = pad, .y = pad},
                                 .size = au::Size{.width = icon, .height = icon}},
                        item->icon, 14.0F);

        const float tx = r.origin.x + pad;
        float ty = r.origin.y + pad + icon + 8.0F;
        const float tw = r.size.width - (2.0F * pad);
        p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = ty}, .size = au::Size{.width = tw, .height = 18}},
                    item->name, au::Font{.size_pt = 14.0F, .weight = 600}, th.text);
        ty += 20.0F;
        p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = ty}, .size = au::Size{.width = tw, .height = 16}},
                    item->developer, au::Font{.size_pt = 12.0F}, th.text_secondary);
        ty += 18.0F;
        draw_rating_chip(
            p, au::Rect{.origin = au::Point{.x = tx, .y = ty}, .size = au::Size{.width = 54.0F, .height = 18.0F}},
            item->rating, th);
        p.set_alpha(1.0);
    }

    bool hot_ = false;
    bool press_ = false;
    bool appear_started_ = false;
    bool appear_complete_ = false;
    GpClock::time_point appear_start_;
    std::unique_ptr<au::Painter> pixel_;
    bool pixel_valid_ = false;
    float pixel_w_ = 0.0F;
    float pixel_h_ = 0.0F;
    bool pixel_dark_ = false;
};

// ---- 推荐行单元（LazyRow 用，方形）----
class RecoCell : public au::LeafWidget {
  public:
    // 同 AppCell：引用数据源元素，避免按值拷贝图标（稳态约 7MB）。
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    const gp::AppItem *item = nullptr;
    std::function<void(const std::string &)> on_open;
    au::Reactive<bool> *dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "RecoCell"; }
    // 同 AppCell：出场动画期间禁止缓存，动画结束后恢复缓存（避免 Scroll 整页重录每帧重画）。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return appear_complete_; }

    auto on_hover_change(bool entered) -> void override {
        hot_ = entered;
        pixel_valid_ = false;
        mark_needs_paint();
    }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            press_ = true;
            pixel_valid_ = false;
            mark_needs_paint();
            if (on_open) {
                on_open(item->id);
            }
            e.is_handled = true;
        } else if (e.action == au::MouseAction::Release) {
            press_ = false;
            pixel_valid_ = false;
            mark_needs_paint();
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = c.max.height});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        if (!appear_started_) {
            appear_started_ = true;
            appear_start_ = GpClock::now();
        }
        const float t =
            ease_out(static_cast<float>(std::chrono::duration<double>(GpClock::now() - appear_start_).count() / 0.32F));
        if (t < 1.0F) {
            appear_complete_ = false;
            mark_needs_paint();
            draw_cell(p, b, th, t);
            return;
        }
        appear_complete_ = true;
        // 稳态：像素离屏缓存（同 AppCell；RecoCell 无阴影，缓存仅需容纳边框）。
        constexpr float k_shad = 2.0F;
        const bool is_dark = (dark != nullptr && dark->get());
        if (!pixel_ || !pixel_valid_ || pixel_w_ != b.size.width || pixel_h_ != b.size.height ||
            pixel_dark_ != dark->get()) {
            if (!pixel_) {
                pixel_ = std::make_unique<au::Painter>();
            }
            pixel_->set_scale(p.scale());
            pixel_->begin(static_cast<int>(b.size.width + (2.0F * k_shad)),
                          static_cast<int>(b.size.height + (2.0F * k_shad)));
            draw_cell(*pixel_, au::Rect{.origin = au::Point{.x = k_shad, .y = k_shad}, .size = b.size}, th, 1.0F);
            pixel_w_ = b.size.width;
            pixel_h_ = b.size.height;
            pixel_dark_ = is_dark;
            pixel_valid_ = true;
        }
        // 离屏缓存 composite 位置 snap 到整数物理像素，避免半像素偏移让缓存内已清晰的
        // 文本再次发虚（125%/175% DPI 下列宽非整数时尤其明显）。
        const float sx = std::floor(((b.origin.x - k_shad) * p.scale()) + 0.5F) / p.scale();
        const float sy = std::floor(((b.origin.y - k_shad) * p.scale()) + 0.5F) / p.scale();
        p.composite(*pixel_, au::Matrix2D::from_translate(sx, sy));
    }

  private:
    auto draw_cell(au::Painter &p, const au::Rect &r, const GPTheme &th, float alpha) const -> void {
        p.set_alpha(alpha);
        constexpr float radius = 18.0F;
        p.fill_rounded_rect(r, radius, th.surface);
        if (hot_) {
            p.draw_rounded_border(r, radius, 1.5F, th.primary);
        } else if (press_) {
            p.draw_rounded_border(r, radius, 1.5F, th.primary.with_alpha(120));
        } else {
            p.draw_rounded_border(r, radius, 1.0F, th.divider);
        }

        constexpr float pad = 12.0F;
        const float icon = std::min(r.size.width, r.size.height) - (2.0F * pad);
        draw_icon_image(p,
                        au::Rect{.origin = r.origin + au::Point{.x = pad, .y = pad},
                                 .size = au::Size{.width = icon, .height = icon}},
                        item->icon, 16.0F);
        const float tx = r.origin.x + pad;
        float ty = r.origin.y + pad + icon + 8.0F;
        const float tw = r.size.width - (2.0F * pad);
        p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = ty}, .size = au::Size{.width = tw, .height = 18}},
                    item->name, au::Font{.size_pt = 14.0F, .weight = 600}, th.text);
        ty += 20.0F;
        draw_rating_chip(
            p, au::Rect{.origin = au::Point{.x = tx, .y = ty}, .size = au::Size{.width = 54.0F, .height = 18.0F}},
            item->rating, th);
        p.set_alpha(1.0);
    }

    bool hot_ = false;
    bool press_ = false;
    bool appear_started_ = false;
    bool appear_complete_ = false;
    GpClock::time_point appear_start_;
    std::unique_ptr<au::Painter> pixel_;
    bool pixel_valid_ = false;
    float pixel_w_ = 0.0F;
    float pixel_h_ = 0.0F;
    bool pixel_dark_ = false;
};

// ---- 精品横幅轮播（圆点 + 点击切换，渐变卡片）----
class BannerCarousel : public au::LeafWidget {
  public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    std::shared_ptr<std::vector<gp::AppItem>> items;
    std::function<void(const std::string &)> on_open;
    au::Reactive<bool> *dark = nullptr;
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
            if (ly > size().height - 22.0F) {
                const int n = static_cast<int>(items->size());
                const float total = static_cast<float>(n) * 14.0F;
                const float sx = (w - total) * 0.5F;
                for (int i = 0; i < n; ++i) {
                    if (lx >= sx + (static_cast<float>(i) * 14.0F) &&
                        lx <= sx + ((static_cast<float>(i) + 1.0F) * 14.0F)) {
                        jump_to(i);
                        e.is_handled = true;
                        return;
                    }
                }
                return;
            }
            // 横幅命中
            const int n = static_cast<int>(items->size());
            for (int i = 0; i < n; ++i) {
                const float x = left_ + (static_cast<float>(i) * step_) + offset_;
                if (lx >= x && lx <= x + banner_w_ && ly >= 0.0F && ly <= size().height - 24.0F) {
                    if (on_open) {
                        on_open((*items)[i].id);  // NOLINT(*-redundant-parentheses)
                    }
                    e.is_handled = true;
                    return;
                }
            }
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        const float w = c.max.width;
        banner_w_ = w * 0.86F;
        step_ = banner_w_ + 16.0F;
        left_ = (w - banner_w_) * 0.5F;
        return c.constrain(au::Size{.width = w, .height = 196.0F});
    }
    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        const int n = static_cast<int>(items->size());

        // 自驱动：平滑滑动由 on_paint 每帧推进；目标切换由 Scheduler 定时任务（on_mount 注册，
        // 见 schedule_next）周期性触发，仅在真实过渡时 mark_needs_paint。静止等待态不再自标脏保活——
        // 旧逻辑每 0.5s 假标脏会骗过 Scroll 把整块离屏缓冲重录，拖垮帧率。
        const auto now = GpClock::now();
        if (!clock_started_) {
            clock_started_ = true;
            last_ = now;
        }
        float dt = std::chrono::duration<float>(now - last_).count();
        last_ = now;
        dt = std::min(dt, 0.1F);
        if (n > 1) {
            const float target_off = -static_cast<float>(target_) * step_;
            const float diff = target_off - offset_;
            if (std::fabs(diff) > 0.5F) {
                const float speed = step_ / 0.45F;  // 约 0.45s 滑到位
                const float step = std::clamp(speed * dt, 0.0F, std::fabs(diff));
                offset_ += (diff > 0.0F ? 1.0F : -1.0F) * step;
                mark_needs_paint();  // 滑动中每帧驱动，过渡结束自然停止
            } else {
                offset_ = target_off;
                index_ = target_;
            }
        }

        // 入场淡入
        if (!appear_started_) {
            appear_started_ = true;
            appear_start_ = now;
        }
        const float ent = ease_out(std::chrono::duration<float>(now - appear_start_).count() / 0.35F);
        if (ent < 1.0F) {
            mark_needs_paint();
        }
        p.set_alpha(static_cast<double>(ent));

        // 离屏层缓存：横幅卡片（渐变+图标+文本）内容在滑动/轮播期间每帧不变，仅整体平移（m_offset）
        // 与入场淡入（alpha）。若每帧直绘，这些命令被并入祖先 Display List，随祖先每帧重录
        // （见 Widget::invalidate_display_list_up 对不可缓存后代的上溯失效）产生 ~8ms 级 glue；
        // 改为一次性渲染到离屏层、每帧仅按 m_offset 平移合成，把每帧成本压到一次 composite + 几个圆点。
        card_h_ = b.size.height - 24.0F;
        if (n > 0 && banner_w_ > 0.0F) {
            const float lw = static_cast<float>(n) * step_;
            const bool is_dark = dark != nullptr && dark->get();
            if (!layer_valid_ || std::fabs(layer_w_ - lw) > 0.5F || std::fabs(layer_h_ - card_h_) > 0.5F ||
                layer_dark_ != dark->get()) {
                layer_ = std::make_unique<au::Painter>();
                layer_->set_scale(p.scale());
                layer_->begin(static_cast<int>(std::ceil(lw)), static_cast<int>(std::ceil(card_h_)));
                render_banner_layer(*layer_, th, n);
                layer_w_ = lw;
                layer_h_ = card_h_;
                layer_dark_ = is_dark;
                layer_valid_ = true;
            }
        }

        p.push_clip(b);
        p.set_alpha(static_cast<double>(ent));
        if (layer_) {
            // 整条卡片带一次性渲染进离屏层（offset=0，层局部坐标卡片 i 位于 x=i*m_step），每帧按当前
            // m_offset 平移合成到 banner 视口；裁剪保证仅可见窗口参与合成。
            p.composite(*layer_, au::Matrix2D::from_translate(b.origin.x + left_ + offset_, b.origin.y));
        } else {
            draw_banner_direct(p, th, n, b);  // 兜底：首帧 layout 前尺寸未就绪时直绘（与旧逻辑一致）
        }
        if (n > 1) {
            const float total = static_cast<float>(n) * 14.0F;
            const float sx = b.origin.x + ((b.size.width - total) * 0.5F);
            const float cy = b.origin.y + b.size.height - 12.0F;
            for (int i = 0; i < n; ++i) {
                const bool active = (i == target_);
                const float dot_w = active ? 18.0F : 6.0F;
                const float dx = sx + (static_cast<float>(i) * 14.0F) + ((14.0F - dot_w) * 0.5F);
                p.fill_rounded_rect(au::Rect{.origin = au::Point{.x = dx, .y = cy - 3.0F},
                                             .size = au::Size{.width = dot_w, .height = 6.0F}},
                                    3.0F, active ? th.primary : th.text_secondary.with_alpha(120));
            }
        }
        p.pop_clip();
        p.set_alpha(1.0);
    }

    // 在给定 Painter 上绘制第 i 张卡片（卡片左上角 (x,y)）；卡片内容静态，离屏层与兜底直绘共用。
    auto draw_card(au::Painter &p, const GPTheme &th, int i, float x, float y) const -> void {
        const au::Rect card{.origin = au::Point{.x = x, .y = y},
                            .size = au::Size{.width = banner_w_, .height = card_h_}};
        p.push_clip_rounded(card, 20.0F);
        const AppItem &a = (*items)[i];  // NOLINT(*-redundant-parentheses)
        p.draw_linear_gradient(card, card.origin, au::Point{.x = card.origin.x, .y = card.origin.y + card.size.height},
                               {a.color_a, a.color_b}, {0.0F, 1.0F});
        p.pop_clip();
        constexpr float icon = 80.0F;
        draw_icon_image(p,
                        au::Rect{.origin = card.origin + au::Point{.x = 18.0F, .y = 18.0F},
                                 .size = au::Size{.width = icon, .height = icon}},
                        a.icon, 18.0F);
        float ty = card.origin.y + 18.0F + icon + 10.0F;
        p.draw_text(au::Rect{.origin = au::Point{.x = card.origin.x + 18.0F, .y = ty},
                             .size = au::Size{.width = card.size.width - 36.0F, .height = 22}},
                    a.name, au::Font{.size_pt = 18.0F, .weight = 700}, th.banner_text);
        ty += 24.0F;
        p.draw_text(au::Rect{.origin = au::Point{.x = card.origin.x + 18.0F, .y = ty},
                             .size = au::Size{.width = card.size.width - 36.0F, .height = 18}},
                    a.developer, au::Font{.size_pt = 13.0F}, th.banner_text.with_alpha(230));
    }

    // 一次性把全部卡片渲染进离屏层（offset=0，层局部坐标：卡片 i 位于 x=i*m_step，y=0）。
    auto render_banner_layer(au::Painter &lp, const GPTheme &th, int n) const -> void {
        for (int i = 0; i < n; ++i) {
            draw_card(lp, th, i, static_cast<float>(i) * step_, 0.0F);
        }
    }

    // 兜底：首帧 layout 前尺寸未就绪时直绘（与旧逻辑一致，按当前 m_offset 摆放）。
    auto draw_banner_direct(au::Painter &p, const GPTheme &th, int n, const au::Rect &b) const -> void {
        for (int i = 0; i < n; ++i) {
            const float x = b.origin.x + left_ + (static_cast<float>(i) * step_) + offset_;
            if (x > b.origin.x + b.size.width || x + banner_w_ < b.origin.x) {
                continue;
            }
            draw_card(p, th, i, x, b.origin.y);
        }
    }

    auto jump_to(int i) -> void {
        const int n = static_cast<int>(items->size());
        if (n <= 0) {
            return;
        }
        target_ = std::clamp(i, 0, n - 1);
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
        const int n = static_cast<int>(items ? items->size() : 0);
        if (n <= 1) {
            return;
        }
        std::weak_ptr<au::Widget> self = weak_from_this();
        timer_ = sch->set_timeout(std::chrono::milliseconds(3600), [self, this]() -> void {
            if (self.lock() == nullptr) {
                return;  // 控件已销毁：停止调度
            }
            const int cnt = static_cast<int>(items->size());
            if (cnt > 1) {
                target_ = (target_ + 1) % cnt;
                mark_needs_paint();
            }
            schedule_next();  // 重排下一轮
        });
    }

  private:
    float card_h_ = 0.0F;  // 卡片高（离屏层与直绘共用）
    std::unique_ptr<au::Painter> layer_;  // 横幅卡片离屏层（一次性渲染，每帧平移合成）
    bool layer_valid_ = false;
    float layer_w_ = 0.0F, layer_h_ = 0.0F;
    bool layer_dark_ = false;  // 主题（明暗）变化需重建离屏层
    float offset_ = 0.0F;
    float step_ = 0.0F;
    float banner_w_ = 0.0F;
    float left_ = 0.0F;
    int index_ = 0;
    int target_ = 0;
    bool clock_started_ = false;
    GpClock::time_point last_;
    bool appear_started_ = false;
    GpClock::time_point appear_start_;
    au::TimerHandle timer_;  ///< 自动轮播定时任务句柄（on_mount 注册，控件销毁后由弱引用守卫失效）
};

// ---- 类目筛选 Chip ----
class FilterChip : public au::LeafWidget {
  public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    std::string label;
    bool selected = false;
    std::function<void()> on_tap;
    au::Reactive<bool> *dark = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "FilterChip"; }

    auto on_hover_change(bool entered) -> void override {
        hot_ = entered;
        mark_needs_paint();
    }

    auto on_pointer_event(au::MouseEvent &e) -> void override {
        if (e.action == au::MouseAction::Press) {
            if (on_tap) {
                on_tap();
            }
            e.is_handled = true;
        }
    }

  protected:
    auto on_layout(const au::Constraints & /*c*/, const au::BuildContext & /*ctx*/) -> au::Size override {
        const au::Font f{.size_pt = 13.0F};
        w_ = au::render::FontEngine::measure_width(label, f) + 28.0F;
        return au::Size{.width = w_, .height = 34.0F};
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        au::Color bg = th.chip_bg;
        if (selected) {
            bg = th.chip_active_bg;
        } else if (hot_) {
            bg = th.chip_bg.shaded(0.96F);
        }
        p.fill_rounded_rect(b, 17.0F, bg);
        const au::Color tc = selected ? th.chip_active_text : th.chip_text;
        const au::Font f{.size_pt = 13.0F, .weight = selected ? 600 : 400};
        p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + 14.0F, .y = b.origin.y},
                             .size = au::Size{.width = b.size.width - 28.0F, .height = b.size.height}},
                    label, f, tc);
    }

  private:
    float w_ = 60.0F;
    bool hot_ = false;
};

// ---- 章节标题（图标 + 标题 + 查看全部）----
class SectionHeader : public au::LeafWidget {
  public:
    std::string title;
    int icon = 0;
    au::Reactive<bool> *dark = nullptr;

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SectionHeader"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = 28.0F});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        constexpr float s = 20.0F;
        draw_gp_icon(icon, p, au::Rect{.origin = b.origin, .size = au::Size{.width = s, .height = s}}, true, th.primary,
                     th.primary);
        p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + s + 8.0F, .y = b.origin.y},
                             .size = au::Size{.width = b.size.width - s - 80.0F, .height = b.size.height}},
                    title, au::Font{.size_pt = 17.0F, .weight = 700}, th.text);
        const std::string more = "See all";
        const float mw = au::render::FontEngine::measure_width(more, au::Font{.size_pt = 13.0F});
        draw_chevron_right(p,
                           au::Rect{.origin = au::Point{.x = b.origin.x + b.size.width - 16.0F, .y = b.origin.y + 4.0F},
                                    .size = au::Size{.width = 16.0F, .height = 20.0F}},
                           th.text_secondary);
        p.draw_text(au::Rect{.origin = au::Point{.x = b.origin.x + b.size.width - 22.0F - mw, .y = b.origin.y},
                             .size = au::Size{.width = mw, .height = b.size.height}},
                    more, au::Font{.size_pt = 13.0F}, th.text_secondary);
    }
};

// ---- 底部导航（主题化，选中药丸）----
class GpBottomNav : public au::LeafWidget {
  public:
    std::vector<au::BottomNavItem> items;
    int selected_index = 0;
    std::function<void(int)> on_select;
    au::Reactive<bool> *dark = nullptr;

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
            e.is_handled = true;
        }
    }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = 64.0F});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        p.fill_rect(au::Rect{.origin = b.origin, .size = au::Size{.width = b.size.width, .height = 1.0F}}, th.divider);
        p.fill_rect(au::Rect{.origin = au::Point{.x = b.origin.x, .y = b.origin.y + 1.0F},
                             .size = au::Size{.width = b.size.width, .height = b.size.height - 1.0F}},
                    th.surface_3);
        const int n = static_cast<int>(items.size());
        const float w = b.size.width / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            const bool sel = (i == selected_index);
            const au::Rect cell{.origin = au::Point{.x = b.origin.x + (static_cast<float>(i) * w), .y = b.origin.y},
                                .size = au::Size{.width = w, .height = b.size.height}};
            constexpr float pw = 56.0F;
            constexpr float ph = 36.0F;
            const au::Rect pill{.origin = au::Point{.x = cell.origin.x + ((w - pw) * 0.5F), .y = cell.origin.y + 6.0F},
                                .size = au::Size{.width = pw, .height = ph}};
            if (sel) {
                p.fill_rounded_rect(pill, 18.0F, th.nav_pill);
            }
            // 图标水平居中于单元格，并在 pill 内垂直居中
            constexpr float icon_size = 24.0F;
            const au::Rect icon_box{.origin = au::Point{.x = cell.origin.x + ((w - icon_size) * 0.5F),
                                                        .y = cell.origin.y + 6.0F + ((ph - icon_size) * 0.5F)},
                                    .size = au::Size{.width = icon_size, .height = icon_size}};
            items[i].icon(p, icon_box, sel);
            // 文字水平居中于单元格（draw_text 默认左对齐，需按测量宽度定位）
            const au::Font label_font{.size_pt = 11.0F, .weight = sel ? 600 : 400};
            const float tw = au::render::FontEngine::measure_width(items[i].label, label_font);
            const float tx = cell.origin.x + ((w - tw) * 0.5F);
            p.draw_text(au::Rect{.origin = au::Point{.x = tx, .y = cell.origin.y + 34.0F},
                                 .size = au::Size{.width = tw, .height = 16.0F}},
                        items[i].label, label_font, sel ? th.primary : th.text_secondary);
        }
    }
};

// ---- 主体内容（按状态重建）----
// ---- 首屏骨架屏（微光占位，自驱动）----
class SkeletonScreen : public au::LeafWidget {
  public:
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes) 演示字段，工厂直接赋值
    au::Reactive<bool> *dark = nullptr;

    auto collect_signals(std::vector<au::SignalViewBase *> & /*out*/) -> void override {}
    [[nodiscard]] auto type_name() const -> const char * override { return "SkeletonScreen"; }
    // 微光骨架屏每帧 mark_needs_paint 自驱动：禁止 Display List 缓存，否则回放冻结首帧。
    [[nodiscard]] auto can_cache_display_list() const -> bool override { return false; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext & /*ctx*/) -> au::Size override {
        return c.constrain(au::Size{.width = c.max.width, .height = 760.0F});
    }

    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext & /*ctx*/) -> void override {
        const GPTheme th = gp_theme(dark != nullptr && dark->get());
        const auto now = GpClock::now();
        if (!started_) {
            started_ = true;
            start_ = now;
        }
        const float t = std::fmod(std::chrono::duration<float>(now - start_).count() * 0.8F, 1.0F);
        mark_needs_paint();

        constexpr float pad_x = 16.0F;
        const float w = b.size.width;
        const float cw = w - (2.0F * pad_x);
        float y = 0.0F;
        const float ox = b.origin.x;
        const float oy = b.origin.y;
        // 横幅占位
        paint_shimmer_block(p,
                            au::Rect{.origin = au::Point{.x = ox + pad_x, .y = oy + y},
                                     .size = au::Size{.width = cw, .height = 196.0F}},
                            th, t);
        y += 196.0F + 14.0F;
        // 标题占位
        paint_shimmer_block(p,
                            au::Rect{.origin = au::Point{.x = ox + pad_x, .y = oy + y},
                                     .size = au::Size{.width = 150.0F, .height = 24.0F}},
                            th, t);
        y += 24.0F + 14.0F;
        // 推荐行：4 个方块
        constexpr float sq = 150.0F;
        for (int i = 0; i < 4; ++i) {
            const float sx = ox + pad_x + (static_cast<float>(i) * (sq + 12.0F));
            paint_shimmer_block(
                p, au::Rect{.origin = au::Point{.x = sx, .y = oy + y}, .size = au::Size{.width = sq, .height = sq}}, th,
                std::fmod(t + (0.15F * static_cast<float>(i)), 1.0F));
        }
        y += sq + 14.0F;
        // 网格：3 列 x 2 行
        const float g = (cw - (2.0F * 12.0F)) / 3.0F;
        for (int r = 0; r < 2; ++r) {
            for (int col = 0; col < 3; ++col) {
                const float gx = ox + pad_x + (static_cast<float>(col) * (g + 12.0F));
                const float gy = oy + y + (static_cast<float>(r) * (140.0F + 12.0F));
                paint_shimmer_block(
                    p, au::Rect{.origin = au::Point{.x = gx, .y = gy}, .size = au::Size{.width = g, .height = 140.0F}},
                    th, std::fmod(t + (0.1F * static_cast<float>((r * 3) + col)), 1.0F));
            }
        }
    }

  private:
    bool started_ = false;
    GpClock::time_point start_;
};

class BodyView : public au::Container {
  public:
    BodyView(au::Reactive<int> *tab, au::Reactive<std::string> *subcat, au::Reactive<std::string> *search,
             gp::PlayRepository *repo, std::function<void(const std::string &)> on_open, au::Reactive<bool> *dark)
        : tab_(tab), subcat_(subcat), search_(search), repo_(repo), on_open_(std::move(on_open)), dark_(dark),
          skeleton_until_(GpClock::now() + std::chrono::milliseconds(700)), skeleton_active_(true) {}

    auto collect_signals(std::vector<au::SignalViewBase *> &out) -> void override {
        out.push_back(tab_);
        out.push_back(subcat_);
        out.push_back(search_);
        if (dark_ != nullptr) {
            out.push_back(dark_);
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

    auto set_viewport_width(float w) -> void { vp_width_ = w; }

  protected:
    // 首屏骨架屏：加载期显示微光占位，超时后重建真实内容
    auto on_paint(au::Painter &p, const au::Rect &b, const au::BuildContext &ctx) -> void override {
        const auto now = GpClock::now();
        if (now < skeleton_until_) {
            mark_needs_paint();
        } else if (skeleton_active_) {
            skeleton_active_ = false;
            mark_needs_layout();
        }
        Container::on_paint(p, b, ctx);
    }

    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> au::Size override {
        float w = 360.0F;
        if (vp_width_ > 0.0F) {
            w = vp_width_;
        } else if (c.max.width < au::Size::infinity().width) {
            w = c.max.width;
        }
        children_.clear();
        float y = 0.0F;
        constexpr float gap = 14.0F;
        constexpr float pad_x = 16.0F;

        auto add = [&](au::Node n) -> void {
            if (!n) {
                return;
            }
            const bool is_grid = std::string{n.widget().type_name()} == "GridView";
            const float hh = is_grid ? 480.0F : au::Size::infinity().height;
            auto mod = n.widget().modifier.get();
            mod = mod.fill_max_width().padding(
                au::EdgeInsets{.left = pad_x, .top = 0.0F, .right = pad_x, .bottom = 0.0F});
            n.widget().modifier = mod;
            n.widget().mount(ctx);
            n.widget().layout(au::Constraints{.min = au::Size{.width = 0.0F, .height = 0.0F},
                                              .max = au::Size{.width = w, .height = hh}},
                              ctx);
            const float used = is_grid ? 480.0F : n.widget().size().height;
            n.set_bounds(
                au::Rect{.origin = au::Point{.x = 0.0F, .y = y}, .size = au::Size{.width = w, .height = used}});
            y += used + gap;
            children_.push_back(std::move(n));
        };

        // 首屏骨架屏：加载期用微光占位替代真实内容
        if (GpClock::now() < skeleton_until_) {
            const auto sk = std::make_shared<SkeletonScreen>();
            sk->dark = dark_;
            add(au::Node{sk});
            return c.constrain(au::Size{.width = w, .height = 760.0F});
        }

        const int tab = tab_->get();
        const std::string sub = subcat_->get();
        const std::string q = search_->get();

        if (!q.empty()) {
            add(make_header("Search", 0));
            const auto res = repo_->search(q);
            if (res.empty()) {
                const GPTheme th = gp_theme(dark_ != nullptr && dark_->get());
                const auto empty =
                    std::make_shared<au::Canvas>(300.0F, 120.0F, [th](au::Painter &p, const au::Rect &b) -> void {
                        p.draw_text(au::Rect{.origin = b.origin, .size = b.size}, "No related apps found",
                                    au::Font{.size_pt = 15.0F}, th.text_secondary);
                    });
                add(au::Node{empty});
            } else {
                add(make_grid(res, (w > 760.0F) ? 4 : 3));
            }
        } else {
            const auto cat = std::string(tab_category(tab));
            add(make_header("Featured", cat_icon(cat)));
            auto feat = repo_->featured();
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
            const auto rec = repo_->list_by_category(cat);
            add(make_reco_row(rec));

            add(make_filter_chips(cat, sub));

            const auto grid_items = sub.empty() ? repo_->list_by_category(cat) : repo_->list_by_subcategory(cat, sub);
            add(make_grid(grid_items, (w > 760.0F) ? 4 : 3));
        }
        return c.constrain(au::Size{.width = w, .height = y});
    }

  private:
    auto make_header(const std::string &title, int icon) const -> au::Node {
        const auto h = std::make_shared<SectionHeader>();
        h->title = title;
        h->icon = icon;
        h->dark = dark_;
        return au::Node{h};
    }

    auto make_grid(const std::vector<gp::AppItem> &items, int cols) const -> au::Node {
        auto ptr = std::make_shared<std::vector<gp::AppItem>>(items);
        const auto g = std::make_shared<au::GridView>(
            static_cast<int>(ptr->size()), cols,
            [ptr, this](int i) -> au::Node {
                const auto cell = std::make_shared<AppCell>();
                cell->item = &ptr->at(i);
                cell->on_open = on_open_;
                cell->dark = dark_;
                return au::Node{cell};
            },
            140.0F);
        g->set_cache_extent(300.0F);
        return au::Node{g};
    }

    auto make_banner_carousel(const std::vector<gp::AppItem> &items) const -> au::Node {
        auto ptr = std::make_shared<std::vector<gp::AppItem>>(items);
        const auto carousel = std::make_shared<BannerCarousel>();
        carousel->items = std::move(ptr);
        carousel->on_open = on_open_;
        carousel->dark = dark_;
        return au::Node{carousel};
    }

    auto make_reco_row(const std::vector<gp::AppItem> &items) const -> au::Node {
        auto ptr = std::make_shared<std::vector<gp::AppItem>>(items);
        auto row = std::make_shared<au::LazyRow>(
            static_cast<int>(ptr->size()),
            [ptr, this](int i) -> au::Node {
                auto cell = std::make_shared<RecoCell>();
                cell->item = &ptr->at(i);
                cell->on_open = on_open_;
                cell->dark = dark_;
                return au::Node{cell};
            },
            150.0F);
        row->set_padding(au::EdgeInsets{.left = 0.0F, .top = 0.0F, .right = 8.0F, .bottom = 0.0F});
        row->set_cache_extent(200.0F);
        row->set_on_item_click([ptr, this](int i) -> void {
            if (ptr != nullptr && i >= 0 && std::cmp_less(i, ptr->size())) {
                on_open_((*ptr)[i].id);  // NOLINT(*-redundant-parentheses)
            }
        });
        return au::Node{row};
    }

    auto make_filter_chips(const std::string &cat, const std::string &sub) const -> au::Node {
        auto row = std::make_shared<au::Row>();
        row->modifier =
            au::Modifier{}.padding(au::EdgeInsets{.left = 0.0F, .top = 0.0F, .right = 4.0F, .bottom = 4.0F});
        const auto subs = gp::subcategories_of(cat);
        bool first = true;
        for (const auto &s : subs) {
            if (!first) {
                auto sp = std::make_shared<au::Canvas>(8.0F, 34.0F, [](au::Painter &, const au::Rect &) -> void {});
                row->add(au::Node{sp});
            }
            first = false;
            auto chip = std::make_shared<FilterChip>();
            chip->label = s;
            chip->selected = s == sub;
            chip->on_tap = [this, s]() -> void { subcat_->set(s); };
            chip->dark = dark_;
            row->add(au::Node{chip});
        }
        return au::Node{row};
    }

    au::Reactive<int> *tab_;
    au::Reactive<std::string> *subcat_;
    au::Reactive<std::string> *search_;
    gp::PlayRepository *repo_;
    std::function<void(const std::string &)> on_open_;
    au::Reactive<bool> *dark_;
    float vp_width_ = 0.0F;
    GpClock::time_point skeleton_until_;
    bool skeleton_active_ = false;
};

// ---- 应用外壳（顶栏 + 内容 + 底栏）----
class AppShell : public au::Container {
  public:
    AppShell(PlayRepository *repo, std::function<void(const std::string &)> on_open, au::Reactive<bool> *dark)
        : repo_(repo), on_open_(std::move(on_open)), dark_(dark) {}

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    // 演示叶控件的数据/回调字段由 BodyView/AppShell 工厂直接赋值（数据载体式），刻意公开。
    au::Reactive<int> tab{0};
    au::Reactive<std::string> subcat{""};
    au::Reactive<std::string> search{""};
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    auto collect_signals(std::vector<au::SignalViewBase *> &out) -> void override {
        out.push_back(&tab);
        out.push_back(&subcat);
        out.push_back(&search);
        if (dark_ != nullptr) {
            out.push_back(dark_);
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
        const float safe_top = safe.top;  // CSD 标题栏高度（GNOME 下 ≈32px；KDE/其他下 = 0）
        const bool d = dark_ != nullptr && dark_->get();
        if (!built_ || d != rendered_dark_) {
            children_.clear();
            auto top = build_top_bar(ctx);
            auto bv = std::make_shared<BodyView>(&tab, &subcat, &search, repo_, on_open_, dark_);
            body_view_ = bv;
            auto body = au::Node{std::make_shared<au::Scroll>(au::ScrollProps{.child = au::Node{std::move(bv)}})};
            auto nav = std::make_shared<GpBottomNav>();
            nav->items = nav_items();
            nav->selected_index = tab.get();
            nav->on_select = [this](int idx) -> void {
                tab.set(idx);
                subcat.set("");
            };
            nav->dark = dark_;
            nav_ptr_ = nav;
            children_.push_back(std::move(top));
            children_.push_back(std::move(body));
            children_.emplace_back(std::move(nav));
            for (auto &n : children_) {
                n.widget().mount(ctx);
            }
            for (auto &n : children_) {
                n.widget().set_layout_parent(this);
            }
            rendered_dark_ = d;
            built_ = true;
        }
        if (body_view_) {
            body_view_->set_viewport_width(w);
        }
        // selected_index 由 AppShell 经 m_tab 驱动，但 GpBottomNav 未对它做响应式订阅；
        // 直接赋值不会使其显示列表失效，导致命中链/DL 复用旧帧（选中药丸不动 = 视觉「无反应」）。
        // 故在值真正变化时标脏，使导航栏重绘、选中药丸随 tab 移动。
        if (nav_ptr_) {
            const int t = tab.get();
            if (nav_ptr_->selected_index != t) {
                nav_ptr_->selected_index = t;
                nav_ptr_->mark_needs_paint();
            }
        }

        constexpr float top_h = 56.0F;
        constexpr float nav_h = 64.0F;
        // 安全区下沉：所有子节点 Y 偏移 safe_top，可用高度相应缩减。
        children_[0].widget().layout(
            au::Constraints{.min = au::Size{.width = w, .height = top_h}, .max = au::Size{.width = w, .height = top_h}},
            ctx);
        children_[0].set_bounds(
            au::Rect{.origin = au::Point{.x = 0.0F, .y = safe_top}, .size = au::Size{.width = w, .height = top_h}});

        const float body_h = std::max(0.0F, h - safe_top - top_h - nav_h);
        children_[1].widget().layout(au::Constraints{.min = au::Size{.width = w, .height = body_h},
                                                     .max = au::Size{.width = w, .height = body_h}},
                                     ctx);
        children_[1].set_bounds(au::Rect{.origin = au::Point{.x = 0.0F, .y = safe_top + top_h},
                                         .size = au::Size{.width = w, .height = body_h}});

        children_[2].widget().layout(
            au::Constraints{.min = au::Size{.width = w, .height = nav_h}, .max = au::Size{.width = w, .height = nav_h}},
            ctx);
        children_[2].set_bounds(
            au::Rect{.origin = au::Point{.x = 0.0F, .y = h - nav_h}, .size = au::Size{.width = w, .height = nav_h}});

        return c.constrain(au::Size{.width = w, .height = h});
    }

  private:
    auto nav_items() const -> std::vector<au::BottomNavItem> {
        const bool d = dark_ != nullptr && dark_->get();
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
        const GPTheme th = gp_theme(dark_ != nullptr && dark_->get());
        const auto row = std::make_shared<au::Row>();
        row->modifier = au::Modifier{}.background(th.surface_3).border(1.0F, th.divider);

        const auto menu = std::make_shared<au::Canvas>(
            40.0F, 40.0F, [th](au::Painter &p, const au::Rect &b) -> void { draw_menu_icon(p, b, th.text_secondary); });
        row->add(au::Node{menu});

        const auto title = std::make_shared<au::Text>(au::TextProps{
            .content = "Google Play",
            .font = au::Font{.size_pt = 20.0F, .weight = 700},
            .text_color = th.primary,
        });
        title->modifier =
            au::Modifier{}.padding(au::EdgeInsets{.left = 0.0F, .top = 8.0F, .right = 0.0F, .bottom = 8.0F});
        row->add(au::Node{title});

        const auto search_row = std::make_shared<au::Row>();
        search_row->modifier = au::Modifier{}
                                   .expand()
                                   .background(th.search_bg)
                                   .clip_rounded(22.0F)
                                   .padding(au::EdgeInsets{.left = 8.0F, .top = 12.0F, .right = 8.0F, .bottom = 12.0F});
        const auto search_icon = std::make_shared<au::Canvas>(
            20.0F, 20.0F,
            [th](au::Painter &p, const au::Rect &b) -> void { draw_search_icon(p, b, th.text_secondary); });
        search_row->add(au::Node{search_icon});
        const auto input = std::make_shared<au::TextInput>(au::TextInputProps{.placeholder = "Search apps and games"});
        input->set_on_changed([this](const std::string &t) -> void { search.set(t); });
        input->modifier = au::Modifier{}.expand();
        search_row->add(au::Node{input});
        row->add(au::Node{search_row});

        const auto theme_btn =
            std::make_shared<au::Canvas>(40.0F, 40.0F, [this](au::Painter &p, const au::Rect &b) -> void {
                const bool dark = dark_ != nullptr && dark_->get();
                draw_theme_icon(p, b, dark);
            });
        theme_btn->modifier = au::Modifier{}.clickable([this]() -> void {
            if (dark_ != nullptr) {
                dark_->set(!dark_->get());
            }
        });
        row->add(au::Node{theme_btn});

        return au::Node{row};
    }

    gp::PlayRepository *repo_;
    std::function<void(const std::string &)> on_open_;
    au::Reactive<bool> *dark_;
    std::shared_ptr<BodyView> body_view_;
    std::shared_ptr<GpBottomNav> nav_ptr_;
    bool built_ = false;
    bool rendered_dark_ = false;
};

// ---- 详情页 ----
class DetailPage : public au::Container {
  public:
    DetailPage(gp::PlayRepository *repo, std::string app_id, std::function<void()> on_back, au::Reactive<bool> *dark)
        : repo_(repo), app_id_(std::move(app_id)), on_back_(std::move(on_back)), dark_(dark) {}

    [[nodiscard]] auto type_name() const -> const char * override { return "DetailPage"; }

  protected:
    auto on_layout(const au::Constraints &c, const au::BuildContext &ctx) -> au::Size override {
        if (!built_) {
            build(ctx);
            built_ = true;
        }
        const float w = c.max.width;
        const float h = c.max.height;
        constexpr float top_h = 56.0F;
        children_[0].widget().layout(
            au::Constraints{.min = au::Size{.width = w, .height = top_h}, .max = au::Size{.width = w, .height = top_h}},
            ctx);
        children_[0].set_bounds(
            au::Rect{.origin = au::Point{.x = 0.0F, .y = 0.0F}, .size = au::Size{.width = w, .height = top_h}});
        const float body_h = std::max(0.0F, h - top_h);
        children_[1].widget().layout(au::Constraints{.min = au::Size{.width = w, .height = body_h},
                                                     .max = au::Size{.width = w, .height = body_h}},
                                     ctx);
        children_[1].set_bounds(
            au::Rect{.origin = au::Point{.x = 0.0F, .y = top_h}, .size = au::Size{.width = w, .height = body_h}});
        return c.constrain(au::Size{.width = w, .height = h});
    }

  private:
    auto build(const au::BuildContext &ctx) -> void {
        const GPTheme th = gp_theme(dark_ != nullptr && dark_->get());
        const auto app = repo_->detail(app_id_);

        // 顶栏
        auto bar = std::make_shared<au::Row>();
        bar->modifier = au::Modifier{}.background(th.surface_3).border(1.0F, th.divider);
        auto back = std::make_shared<au::Canvas>(
            40.0F, 40.0F, [th](au::Painter &p, const au::Rect &b) -> void { draw_back_icon(p, b, th.primary); });
        back->modifier = au::Modifier{}.clickable(on_back_);
        bar->add(au::Node{back});
        auto t = std::make_shared<au::Text>(au::TextProps{
            .content = app.name,
            .font = au::Font{.size_pt = 18.0F, .weight = 700},
            .text_color = th.text,
        });
        t->modifier = au::Modifier{}.expand();
        bar->add(au::Node{t});
        children_.emplace_back(bar);

        // 内容列
        auto col = std::make_shared<au::Column>();

        // 渐变头图
        auto hero = std::make_shared<au::Row>();
        hero->modifier = au::Modifier{}.fill_max_width().padding(
            au::EdgeInsets{.left = 16.0F, .top = 16.0F, .right = 8.0F, .bottom = 16.0F});
        auto icon = std::make_shared<au::ImageView>(app.icon);
        icon->modifier = au::Modifier{}.size(96.0F, 96.0F).clip_rounded(20.0F);
        hero->add(au::Node{icon});

        auto info = std::make_shared<au::Column>();
        info->modifier =
            au::Modifier{}.expand().padding(au::EdgeInsets{.left = 0.0F, .top = 12.0F, .right = 0.0F, .bottom = 0.0F});
        info->add(text_node(app.name, 20.0F, 700, th.text));
        info->add(text_node(app.developer, 14.0F, 400, th.text_secondary));
        auto stars = std::make_shared<au::Canvas>(
            110.0F, 16.0F, [app](au::Painter &p, const au::Rect &b) -> void { paint_stars(p, b, app.rating); });
        info->add(au::Node{stars});

        auto btn = std::make_shared<au::Button>("Install");
        btn->background(th.primary);
        btn->text_color(th.on_primary);
        btn->set_corner_radius(20.0F);
        btn->set_min_size(120.0F, 40.0F);
        btn->set_on_click([this, btn, th]() -> void {
            const bool now = !installed_.get();
            installed_.set(now);
            if (now) {
                btn->set_label("Open");
                btn->background(au::Color{0x0F, 0x9D, 0x58, 0xFF});
            } else {
                btn->set_label("Install");
                btn->background(th.primary);
            }
        });
        info->add(au::Node{btn});
        hero->add(au::Node{info});
        col->add(au::Node{hero});

        // 评分概览卡片
        auto rating_card = std::make_shared<au::Row>();
        rating_card->modifier =
            au::Modifier{}
                .background(th.surface)
                .border(1.0F, th.divider)
                .clip_rounded(16.0F)
                .padding(au::EdgeInsets{.left = 14.0F, .top = 16.0F, .right = 14.0F, .bottom = 16.0F})
                .fill_max_width();
        rating_card->add(text_node(fmt_rating(app.rating), 26.0F, 700, th.text));
        auto right = std::make_shared<au::Column>();
        right->modifier =
            au::Modifier{}.expand().padding(au::EdgeInsets{.left = 0.0F, .top = 12.0F, .right = 0.0F, .bottom = 0.0F});
        right->add(au::Node{std::make_shared<au::Canvas>(
            120.0F, 16.0F, [app](au::Painter &p, const au::Rect &b) -> void { paint_stars(p, b, app.rating); })});
        right->add(text_node(app.downloads + " downloads", 12.0F, 400, th.text_secondary));
        rating_card->add(au::Node{right});
        col->add(au::Node{rating_card});

        // 截图轮播
        auto shots_ptr = std::make_shared<std::vector<gp::Image>>(repo_->screenshots_for(app_id_));
        auto shot_row = std::make_shared<au::LazyRow>(
            static_cast<int>(shots_ptr->size()),
            [shots_ptr, th](int i) -> au::Node {
                const auto c = std::make_shared<au::Canvas>(
                    250.0F, 140.0F, [shots_ptr, th, i](au::Painter &p, const au::Rect &b) -> void {
                        p.draw_shadow(b, 0.0F, 3.0F, 8.0F, th.shadow);
                        p.fill_rounded_rect(b, 12.0F, th.surface);
                        if (static_cast<size_t>(i) < shots_ptr->size()) {
                            draw_icon_image(
                                p,
                                au::Rect{
                                    .origin = au::Point{.x = b.origin.x + 4.0F, .y = b.origin.y + 4.0F},
                                    .size = au::Size{.width = b.size.width - 8.0F, .height = b.size.height - 8.0F}},
                                (*shots_ptr)[i], 10.0F);  // NOLINT(*-redundant-parentheses)
                        }
                    });
                return au::Node{c};
            },
            250.0F);
        shot_row->set_padding(au::EdgeInsets{.left = 16.0F, .top = 0.0F, .right = 16.0F, .bottom = 16.0F});
        shot_row->set_cache_extent(300.0F);
        col->add(au::Node{shot_row});

        // 描述
        auto desc = text_node(app.description, 14.0F, 400, th.text, 0);
        desc.widget().modifier = au::Modifier{}.fill_max_width().padding(
            au::EdgeInsets{.left = 8.0F, .top = 16.0F, .right = 8.0F, .bottom = 16.0F});
        col->add(desc);

        // 关于
        auto about = std::make_shared<au::Column>();
        about->modifier = au::Modifier{}
                              .background(th.surface)
                              .border(1.0F, th.divider)
                              .clip_rounded(16.0F)
                              .padding(au::EdgeInsets{.left = 12.0F, .top = 12.0F, .right = 12.0F, .bottom = 12.0F});
        auto add_row = [&](const std::string &k, const std::string &v) -> void {
            const auto r = std::make_shared<au::Row>();
            r->add(text_node(k, 13.0F, 400, th.text_secondary));
            auto val = text_node(v, 13.0F, 600, th.text);
            val.widget().modifier = au::Modifier{}.expand();
            r->add(val);
            about->add(au::Node{r});
        };
        add_row("Size", std::to_string(static_cast<int>(app.size_mb)) + " MB");
        add_row("Downloads", app.downloads);
        add_row("Version", app.version);
        add_row("Updated", app.updated);
        auto about_wrap = au::Node{about};
        about_wrap.widget().modifier = au::Modifier{}.fill_max_width().padding(
            au::EdgeInsets{.left = 8.0F, .top = 16.0F, .right = 8.0F, .bottom = 16.0F});
        col->add(about_wrap);

        // 评价
        {
            auto hd = text_node("Reviews", 16.0F, 700, th.text);
            hd.widget().modifier = au::Modifier{}.fill_max_width().padding(
                au::EdgeInsets{.left = 8.0F, .top = 16.0F, .right = 4.0F, .bottom = 16.0F});
            col->add(hd);
        }
        for (const auto &rv : repo_->reviews(app_id_)) {
            auto card = std::make_shared<au::Column>();
            card->modifier = au::Modifier{}
                                 .background(th.surface)
                                 .border(1.0F, th.divider)
                                 .clip_rounded(12.0F)
                                 .padding(au::EdgeInsets{.left = 10.0F, .top = 12.0F, .right = 10.0F, .bottom = 12.0F});
            auto head = std::make_shared<au::Row>();
            head->add(text_node(rv.user, 14.0F, 600, th.text));
            auto rating = std::make_shared<au::Canvas>(
                90.0F, 16.0F, [rv](au::Painter &p, const au::Rect &b) -> void { paint_stars(p, b, rv.rating); });
            head->add(au::Node{rating});
            card->add(au::Node{head});
            card->add(text_node(rv.text, 13.0F, 400, th.text_secondary, 0));
            auto cw = au::Node{card};
            cw.widget().modifier = au::Modifier{}.fill_max_width().padding(
                au::EdgeInsets{.left = 4.0F, .top = 16.0F, .right = 4.0F, .bottom = 16.0F});
            col->add(cw);
        }

        children_.emplace_back(std::make_shared<au::Scroll>(au::ScrollProps{.child = au::Node{col}}));
        children_[0].widget().mount(ctx);
        children_[1].widget().mount(ctx);
        for (auto &n : children_) {
            n.widget().set_layout_parent(this);
        }
    }

    gp::PlayRepository *repo_;
    std::string app_id_;
    std::function<void()> on_back_;
    au::Reactive<bool> *dark_;
    au::Reactive<bool> installed_{false};
    bool built_ = false;
};

// ---- 入口装配 ----
inline auto make_google_play_host(au::Animator &anim, gp::PlayRepository *repo,
                                  const std::function<void(const std::string &)> &on_open,
                                  au::Reactive<bool> *dark = nullptr) -> std::shared_ptr<au::NavigatorHost> {
    const auto host = std::make_shared<au::NavigatorHost>(anim);
    host->push(au::Route{au::Node{std::make_shared<AppShell>(repo, on_open, dark)}, "home", {}});
    return host;
}

inline void push_detail_route(const std::shared_ptr<au::NavigatorHost> &host, PlayRepository *repo,
                              const std::string &app_id, std::function<void()> on_back,
                              au::Reactive<bool> *dark = nullptr) {
    host->push(
        au::Route{au::Node{std::make_shared<DetailPage>(repo, app_id, std::move(on_back), dark)}, "detail:" + app_id,
                  au::RouteTransition{.animated = true, .kind = au::TransitionKind::Slide, .duration_seconds = 0.28}});
}

}  // namespace gp::ui

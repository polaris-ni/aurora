/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/title_bar_painter.h + src/aurora/window/detail/title_bar_painter.cpp
/// 测试说明: 覆盖 CSD 装饰绘制层——「直接光栅」与「录制→回放进 Painter」两条上屏路径逐位等价
/// （wl_shm 软件宿主与 wgpu GPU 宿主共用同一实现、不得各自漂移的核心不变量，图像/文本/圆角/
/// 线段原语全部走一遍）、显隐门控（无 CSD 标题栏、全屏未揭示顶边条 → 零像素）、三种按钮布局的
/// 常态与悬停取色、失焦配色切换、隐藏按钮与不可缩放窗口在该槽不留任何像素。
/// 无平台依赖（纯 Painter 画布），故 Windows/Linux/macOS 与无头 CI 均可跑。

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "aurora/core/color.h"
#include "aurora/core/image.h"
#include "aurora/core/types.h"
#include "aurora/render/display_list.h"
#include "aurora/render/painter.h"
#include "aurora/window/detail/title_bar_painter.h"
#include "aurora/window/title_bar_geometry.h"
#include "aurora/window/window_state.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_title_bar_painter {

namespace {

constexpr Color AURORA_BASE{10, 20, 30, 255};  ///< 画布底色：装饰未覆盖处必须仍是它
constexpr Color AURORA_MAC_RED{0xFF, 0x5F, 0x57, 255};
constexpr Color AURORA_MAC_YELLOW{0xFE, 0xBC, 0x2E, 255};
constexpr Color AURORA_MAC_GREEN{0x28, 0xC8, 0x40, 255};

[[nodiscard]] auto center_of(const Rect &r) -> Point {
    return Point{.x = r.origin.x + (r.size.width * 0.5F), .y = r.origin.y + (r.size.height * 0.5F)};
}

[[nodiscard]] auto px(const Painter &p, const Point &pt) -> Color {
    return p.get_pixel(static_cast<int>(pt.x), static_cast<int>(pt.y));
}

/// @brief 装配一帧装饰状态（400 宽、Adwaita 暗色默认样式、无标题文字与图标——颜色断言取的是
/// 纯底色区，文字与图标会盖掉它）。
[[nodiscard]] auto plain_state() -> csd::TitleBarPaintState {
    csd::TitleBarPaintState s;
    s.width = 400.0F;
    s.title_bar = true;
    s.active = true;
    s.resizable = true;
    s.hovered_button = -1;
    return s;
}

auto raster_direct(Painter &p, const csd::TitleBarPaintState &s) -> void {
    p.begin(400, 120);
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 400.0F, .height = 120.0F}},
                AURORA_BASE);
    csd::paint_title_bar(p, s);
}

/// @brief 录制为 DisplayList 再回放进同一画布——GPU 宿主（WgpuWaylandSurface）走的就是这条。
auto raster_recorded(Painter &p, const csd::TitleBarPaintState &s) -> void {
    p.begin(400, 120);
    p.fill_rect(Rect{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 400.0F, .height = 120.0F}},
                AURORA_BASE);
    DisplayList dl;
    p.record(dl);
    csd::paint_title_bar(p, s);
    p.stop();
    dl.replay(p);
}

// ── 阴影边距（shadow margin）专用夹具：画布 = 整幅表面（内容 + 2×margin）────────
// 注意：Painter 的全局不变量是「目标画布不透明」（混合恒写 alpha=255），本夹具只验证
// GPU/DL underlay 的命令序列在直绘与录制/回放两路逐位一致；真 alpha 衰减的像素断言在
// utest_csd_shadow_compose（软件 wl_shm 上屏的 BGRA 合成器）里。
constexpr int AURORA_MARGIN = 10;  ///< 与 csd::AURORA_SHADOW_MARGIN_DP 同值（阴影带厚度）
constexpr int AURORA_CONTENT_W = 400;
constexpr int AURORA_CONTENT_H = 100;
constexpr int AURORA_SURF_W = AURORA_CONTENT_W + (2 * AURORA_MARGIN);  // 420
constexpr int AURORA_SURF_H = AURORA_CONTENT_H + (2 * AURORA_MARGIN);  // 120

/// @brief Borderless 风格的阴影状态（无标题栏，纯阴影 underlay，便于对 margin 像素做断言）。
[[nodiscard]] auto shadow_state(bool margin_on) -> csd::TitleBarPaintState {
    csd::TitleBarPaintState s;
    s.width = static_cast<float>(AURORA_CONTENT_W);
    s.height = static_cast<float>(AURORA_CONTENT_H);
    s.title_bar = false;  // 不画标题栏，只验证阴影 underlay
    s.active = true;
    s.resizable = true;
    if (margin_on) {
        s.origin_x = s.origin_y = static_cast<float>(AURORA_MARGIN);
        s.shadow_margin = static_cast<float>(AURORA_MARGIN);
    }
    return s;
}

/// @brief GPU 帧在软件画布上的同构序列：不透明基底 → 阴影 underlay → 内容矩形底色二次覆盖。
auto raster_shadow_direct(Painter &p, const csd::TitleBarPaintState &s) -> void {
    p.begin(AURORA_SURF_W, AURORA_SURF_H);
    p.fill_rect(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
             .size = Size{.width = static_cast<float>(AURORA_SURF_W), .height = static_cast<float>(AURORA_SURF_H)}},
        AURORA_BASE);
    csd::paint_window_shadow(p, s);
    p.fill_rect(
        Rect{.origin = Point{.x = s.origin_x, .y = s.origin_y}, .size = Size{.width = s.width, .height = s.height}},
        AURORA_BASE);
}

/// @brief 同序列录进 DL 再回放（GPU 宿主 Sink::begin_frame underlay + app 帧底色的录制口径）。
auto raster_shadow_recorded(Painter &p, const csd::TitleBarPaintState &s) -> void {
    p.begin(AURORA_SURF_W, AURORA_SURF_H);
    p.fill_rect(
        Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
             .size = Size{.width = static_cast<float>(AURORA_SURF_W), .height = static_cast<float>(AURORA_SURF_H)}},
        AURORA_BASE);
    DisplayList dl;
    p.record(dl);
    csd::paint_window_shadow(p, s);
    p.fill_rect(
        Rect{.origin = Point{.x = s.origin_x, .y = s.origin_y}, .size = Size{.width = s.width, .height = s.height}},
        AURORA_BASE);
    p.stop();
    dl.replay(p);
}

/// @brief 阴影序列直绘 vs 录制/回放逐字节一致（软件 begin_frame 与 GPU underlay 的核心不变量）。
auto check_shadow_bit_identical(const csd::TitleBarPaintState &s) -> void {
    Painter direct;
    raster_shadow_direct(direct, s);
    Painter replayed;
    raster_shadow_recorded(replayed, s);
    AURORA_TEST_REQUIRE(direct.data() != nullptr);
    AURORA_TEST_REQUIRE(replayed.data() != nullptr);
    AURORA_TEST_REQUIRE_EQ(direct.width(), replayed.width());
    AURORA_TEST_REQUIRE_EQ(direct.height(), replayed.height());
    const std::size_t n = static_cast<std::size_t>(direct.width()) * static_cast<std::size_t>(direct.height()) * 4U;
    std::size_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 逐字节比较两帧缓冲，下标即字节偏移
        if (direct.data()[i] != replayed.data()[i]) {
            ++diff;
        }
    }
    AURORA_TEST_CHECK_EQ(diff, 0U);
}

[[nodiscard]] auto solid_icon(int side, Color c) -> std::shared_ptr<Image> {
    auto img = std::make_shared<Image>();
    img->width = side;
    img->height = side;
    img->pixels.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4U, 0U);
    for (int i = 0; i < side * side; ++i) {
        const std::size_t o = static_cast<std::size_t>(i) * 4U;
        img->pixels[o + 0U] = c.r;
        img->pixels[o + 1U] = c.g;
        img->pixels[o + 2U] = c.b;
        img->pixels[o + 3U] = c.a;
    }
    img->invalidate_content_hash();
    return img;
}

/// @brief 两条路径的整幅像素必须逐字节相同（差异像素数断言为 0）。
auto check_paths_bit_identical(const csd::TitleBarPaintState &s) -> void {
    Painter direct;
    raster_direct(direct, s);
    Painter replayed;
    raster_recorded(replayed, s);
    AURORA_TEST_REQUIRE(direct.data() != nullptr);
    AURORA_TEST_REQUIRE(replayed.data() != nullptr);
    AURORA_TEST_REQUIRE_EQ(direct.width(), replayed.width());
    AURORA_TEST_REQUIRE_EQ(direct.height(), replayed.height());
    const std::size_t n = static_cast<std::size_t>(direct.width()) * static_cast<std::size_t>(direct.height()) * 4U;
    std::size_t diff = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 逐字节比较两帧缓冲，下标即字节偏移
        if (direct.data()[i] != replayed.data()[i]) {
            ++diff;
        }
    }
    AURORA_TEST_CHECK_EQ(diff, 0U);
}

/// @brief 盒内「非底色墨迹」像素的外接框（整数像素域）。
struct InkBox {
    int min_x = 0;  ///< 最左墨迹像素 x
    int max_x = -1;  ///< 最右墨迹像素 x（max_x < min_x 即无墨迹）
    int min_y = 0;  ///< 最上墨迹像素 y
    int max_y = -1;  ///< 最下墨迹像素 y

    [[nodiscard]] auto empty() const -> bool { return max_x < min_x; }
    [[nodiscard]] auto width() const -> float { return empty() ? 0.0F : static_cast<float>(max_x - min_x); }
    [[nodiscard]] auto center_x() const -> float { return empty() ? -1.0F : static_cast<float>(min_x + max_x) * 0.5F; }
    [[nodiscard]] auto center_y() const -> float { return empty() ? -1.0F : static_cast<float>(min_y + max_y) * 0.5F; }
};

/// @brief 在盒内扫描「非底色墨迹」像素，返回其外接框。
/// 底色判据：与 bg 任一通道差 > 30（抗抗锯齿半透明边）；字形为对比色，中心像素为纯前景色。
/// 用于实测字形是否「撑满盒宽」或「与盒宽解耦」、以及文字是否居中——不依赖绘制实现细节。
/// Mac 档把「底色」传成圆点色即可在圆内单独量出悬停符号。
[[nodiscard]] auto ink_bbox(const Painter &p, const Rect &box, const Color &bg) -> InkBox {
    const int x0 = static_cast<int>(std::floor(box.origin.x));
    const int y0 = static_cast<int>(std::floor(box.origin.y));
    const int x1 = static_cast<int>(std::ceil(box.origin.x + box.size.width));
    const int y1 = static_cast<int>(std::ceil(box.origin.y + box.size.height));
    InkBox out{.min_x = x1, .max_x = x0 - 1, .min_y = y1, .max_y = y0 - 1};
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Color c = p.get_pixel(x, y);
            const int dr = std::abs(c.r - bg.r);
            const int dg = std::abs(c.g - bg.g);
            const int db = std::abs(c.b - bg.b);
            const int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            if (d > 30) {
                out.min_x = std::min(x, out.min_x);
                out.max_x = std::max(x, out.max_x);
                out.min_y = std::min(y, out.min_y);
                out.max_y = std::max(y, out.max_y);
            }
        }
    }
    return out;
}

}  // namespace

AURORA_TEST_CASE(direct_raster_and_record_replay_are_bit_identical) {
    // 主不变量：软件路径（present 里直绘进帧缓冲）与 GPU 路径（录成 DL 追加回放进当帧）必须
    // 画出同一份像素。状态刻意含齐四类原语：图标（DrawImage）、标题文字（DrawText）、悬停圆底
    // （RoundedRect）、按钮符号（DrawLine）——任一类在录制/回放口径上分叉，本用例即红灯。
    csd::TitleBarPaintState s = plain_state();
    s.title = "Aa 01 窗口";  // CJK-LITERAL: cjk-fixture - mixed ASCII/CJK glyph run, painted not printed
    s.icon = solid_icon(16, Color{0x35, 0x84, 0xE4, 255});
    s.hovered_button = 2;
    check_paths_bit_identical(s);

    // 另两种按钮布局的分支（Windows 整高矩形罩、Mac 常显圆点）同样须逐位一致。
    s.style.button_layout = TitleBarButtonLayout::Windows;
    s.hovered_button = 1;
    check_paths_bit_identical(s);
    s.style.button_layout = TitleBarButtonLayout::Mac;
    s.hovered_button = 0;
    check_paths_bit_identical(s);
}

AURORA_TEST_CASE(gated_states_emit_no_pixels) {
    csd::TitleBarPaintState s = plain_state();
    const Point mid_band{.x = 200.0F, .y = s.style.height * 0.5F};

    // 无 CSD 标题栏（合成器提供 SSD / 无边框策略）：整幅仍是底色 → GPU 宿主据此跳过回放。
    s.title_bar = false;
    AURORA_TEST_CHECK_FALSE(s.paints_title_bar());
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, mid_band) == AURORA_BASE);
    }

    // 全屏默认隐藏标题栏；顶边悬停揭示后必须重新可见（覆盖层语义，不改窗口尺寸）。
    s.title_bar = true;
    s.mode = WindowMode::FullScreen;
    AURORA_TEST_CHECK_FALSE(s.paints_title_bar());
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, mid_band) == AURORA_BASE);
    }
    s.fullscreen_bar_revealed = true;
    AURORA_TEST_CHECK_TRUE(s.paints_title_bar());
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, mid_band) == s.style.bg_active);
    }
}

AURORA_TEST_CASE(adwaita_hover_paints_circle_tint_and_close_red) {
    const TitleBarGeometry g = title_bar_geometry(400.0F, TitleBarStyle{}, false, true);
    csd::TitleBarPaintState s = plain_state();

    // 常态：关闭钮槽中心是白色符号，既非特征红也非底色。
    {
        Painter p;
        raster_direct(p, s);
        const Color c = px(p, center_of(g.close));
        AURORA_TEST_CHECK(c != s.style.close_hover);
        AURORA_TEST_CHECK(c != AURORA_BASE);
        // 装饰只占顶部 height：其下仍是底色。
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height + 4.0F}) == AURORA_BASE);
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height * 0.5F}) == s.style.bg_active);
    }
    // 悬停关闭钮（序号 2）：圆底铺满为 close_hover 特征红。取圆心上方一点——圆心本身被
    // 白色 ✕ 字形穿过，那里只能断言「不是红」。
    s.hovered_button = 2;
    {
        Painter p;
        raster_direct(p, s);
        const Point above_centre{.x = center_of(g.close).x, .y = g.close.origin.y + 3.0F};
        AURORA_TEST_CHECK(px(p, above_centre) == s.style.close_hover);
    }
    // 悬停最小化钮（序号 0）：半透明白罩，既非底色也非红。
    s.hovered_button = 0;
    {
        Painter p;
        raster_direct(p, s);
        const Color c = px(p, center_of(g.minimize));
        AURORA_TEST_CHECK(c != AURORA_BASE);
        AURORA_TEST_CHECK(c != s.style.close_hover);
    }
}

AURORA_TEST_CASE(windows_layout_hovers_full_height_rect) {
    constexpr TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
    csd::TitleBarPaintState s = plain_state();
    s.style = style;
    s.hovered_button = 2;
    Painter p;
    raster_direct(p, s);
    const float cx = center_of(g.close).x;
    // 整高矩形热区：上下缘内侧仍为红底（Adwaita 的圆底在同样位置会露出底色）。
    AURORA_TEST_CHECK(px(p, Point{cx, 2.0F}) == style.close_hover);
    AURORA_TEST_CHECK(px(p, Point{cx, style.height - 3.0F}) == style.close_hover);
}

AURORA_TEST_CASE(windows_light_close_glyph_uses_fg_ink_and_leaves_padding) {
    // 验收判据 1：WindowsLight 下关闭钮盒内必须出现前景色墨迹（常态无悬停），且字形不撑满盒宽
    // —— 盒内两侧留白须仍是底色。旧实现恒写白色 α235，亮色档底色近白时关闭钮画不出来（功能性缺陷），
    // 故这项断言直接钉住「关闭钮可见」这个修复。
    constexpr TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::WindowsLight);
    const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
    csd::TitleBarPaintState s = plain_state();
    s.style = style;
    s.active = true;
    s.hovered_button = -1;  // 常态无悬停：字形应随前景色（黑）而非恒白
    Painter p;
    raster_direct(p, s);
    constexpr Color bg = style.bg_active;  // #F3F3F3
    const float cw = ink_bbox(p, g.close, bg).width();
    // ① 盒内确有墨迹（关闭钮画出来了）。
    AURORA_TEST_CHECK_GT(cw, 0.0F);
    // ② 字形外接框宽 ≤ 半个盒宽（10dp 设计网格居中，两侧留白可见——未撑满盒）。
    AURORA_TEST_CHECK_LE(cw, (g.close.size.width * 0.5F) + 1.0F);
    // ③ 盒内两侧留白确为底色（取垂直中心、距左缘 3px 的点，远在 10dp 字形之外）。
    const Point pad{.x = g.close.origin.x + 3.0F, .y = g.close.origin.y + (g.close.size.height * 0.5F)};
    AURORA_TEST_CHECK(px(p, pad) == bg);
}

AURORA_TEST_CASE(windows_min_glyph_box_width_independent_of_button_size) {
    // 验收判据 2：Windows 字形走固定 10×10dp 设计网格，与按钮盒宽解耦。换两组不同的
    // metrics.button_size 渲染，最小化字形外接框宽应恒定 ≈10dp（不随盒宽放大）。两条渲染
    // 共用同一画布/底色，仅盒宽不同，故「外接框宽差≈0」直接证明解耦不变量。
    const auto measure_min_glyph = [&](float button_size) -> float {
        TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
        style.metrics.button_size = button_size;  // 显式覆盖盒宽（0 = 走栏高推导）
        const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
        csd::TitleBarPaintState s = plain_state();
        s.style = style;
        s.active = true;
        s.hovered_button = -1;
        Painter p;
        raster_direct(p, s);
        return ink_bbox(p, g.minimize, style.bg_active).width();
    };
    const float w46 = measure_min_glyph(46.0F);
    const float w60 = measure_min_glyph(60.0F);
    // 设计网格 10dp（±抗锯齿余量）；两条盒宽下字形外接框宽恒定 —— 解耦不变量。
    AURORA_TEST_CHECK_NEAR(w46, 10.0F, 2.5F);
    AURORA_TEST_CHECK_NEAR(w60, 10.0F, 2.5F);
    AURORA_TEST_CHECK_NEAR(w46, w60, 1.0F);
}

AURORA_TEST_CASE(adwaita_light_close_glyph_uses_fg_ink_and_leaves_padding) {
    // AdwaitaLight 与 WindowsLight 是同一个缺陷：关闭钮字形恒写白色 α235，而亮色档常态下
    // 关闭钮**没有**底色（只有悬停才铺红），白字形落在 `#EBEBEB` 上等于画不出来。
    // 判据与 Windows 档同构：常态下盒内必须出现前景色墨迹。
    constexpr TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::AdwaitaLight);
    const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
    csd::TitleBarPaintState s = plain_state();
    s.style = style;
    s.active = true;
    s.hovered_button = -1;  // 常态无悬停：字形应随前景色（黑）而非恒白
    Painter p;
    raster_direct(p, s);
    constexpr Color bg = style.bg_active;  // #EBEBEB
    const float cw = ink_bbox(p, g.close, bg).width();
    // ① 盒内确有墨迹（关闭钮画出来了）。
    AURORA_TEST_CHECK_GT(cw, 0.0F);
    // ② 16dp symbolic 网格（±抗锯齿余量）：盒 26dp ⇒ 两侧各留 5dp。
    AURORA_TEST_CHECK_NEAR(cw, 16.0F, 3.0F);
    // ③ 盒内左侧留白确为底色（距左缘 2px，远在 16dp 字形之外）。
    const Point pad{.x = g.close.origin.x + 2.0F, .y = g.close.origin.y + (g.close.size.height * 0.5F)};
    AURORA_TEST_CHECK(px(p, pad) == bg);
}

AURORA_TEST_CASE(adwaita_min_glyph_box_width_independent_of_button_size) {
    // Adwaita 字形同样走固定 16×16dp symbolic 网格（GNOME 的 `windowcontrols` 按钮 26dp 里
    // 装的就是 16dp 图标），与按钮盒宽解耦：换两组盒宽渲染，最小化字形外接框宽恒定 ≈16dp。
    const auto measure_min_glyph = [&](float button_size) -> float {
        TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
        style.metrics.button_size = button_size;  // 显式覆盖盒宽（0 = 走栏高推导）
        const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
        csd::TitleBarPaintState s = plain_state();
        s.style = style;
        s.active = true;
        s.hovered_button = -1;
        Painter p;
        raster_direct(p, s);
        return ink_bbox(p, g.minimize, style.bg_active).width();
    };
    const float w26 = measure_min_glyph(26.0F);
    const float w34 = measure_min_glyph(34.0F);
    AURORA_TEST_CHECK_NEAR(w26, 16.0F, 3.0F);
    AURORA_TEST_CHECK_NEAR(w34, 16.0F, 3.0F);
    AURORA_TEST_CHECK_NEAR(w26, w34, 1.0F);
}

AURORA_TEST_CASE(mac_glyph_box_width_independent_of_button_size) {
    // macOS 红绿灯内符号按 6pt 设计网格绘制（恒 12pt 的圆里占一半），同样与盒解耦。
    // 量法：把「底色」传成圆点色，只扫圆心水平带（避开圆外的标题栏底色），即得符号自身的外接框。
    const auto measure_min_glyph = [&](float button_size) -> float {
        TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::MacDark);
        style.metrics.button_size = button_size;
        const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
        csd::TitleBarPaintState s = plain_state();
        s.style = style;
        s.active = true;
        s.hovered_button = 0;  // macOS 符号仅在悬停 / 按压时浮现
        Painter p;
        raster_direct(p, s);
        const Point c = center_of(g.minimize);
        // 只扫**圆心那一行**、横向取盒宽 80%（半宽 0.4×直径，恒落在圆内）—— 若扫多行，
        // 带角会滑出圆外，圆外的标题栏底色与圆点色差远大于阈值，会被误计成墨迹。
        const Rect row{.origin = Point{.x = c.x - (g.minimize.size.width * 0.4F), .y = c.y},
                       .size = Size{.width = g.minimize.size.width * 0.8F, .height = 1.0F}};
        return ink_bbox(p, row, AURORA_MAC_YELLOW).width();
    };
    const float w12 = measure_min_glyph(12.0F);
    const float w20 = measure_min_glyph(20.0F);
    AURORA_TEST_CHECK_NEAR(w12, 6.0F, 3.0F);
    AURORA_TEST_CHECK_NEAR(w20, 6.0F, 3.0F);
    AURORA_TEST_CHECK_NEAR(w12, w20, 1.0F);
}

AURORA_TEST_CASE(mac_preset_title_is_centered_in_full_width_band) {
    // macOS 窗口标题居中：几何层在 center_title=true 时给的是**整宽带**（不扣按钮组），
    // 居中由绘制层按实测量宽完成。判据 = 标题墨迹外接框中心 ≈ 窗口水平中心；
    // 对照组（center_title=false）须明显偏左——否则「居中」只是把带宽放开而没真居中。
    constexpr TitleBarStyle preset = TitleBarStyle::from_preset(CsdPreset::MacDark);
    const auto measure_title_centre = [&](bool centered) -> float {
        TitleBarStyle style = preset;
        style.center_title = centered;
        csd::TitleBarPaintState s = plain_state();
        s.style = style;
        s.title = "Centered";  // 纯 ASCII：任何字体面都能出字，判据不依赖具体字体
        s.active = true;
        Painter p;
        raster_direct(p, s);
        // 只扫 x≥100 的区（避开左侧红绿灯），墨迹即标题本身。
        const Rect band{.origin = Point{.x = 100.0F, .y = 0.0F}, .size = Size{.width = 300.0F, .height = style.height}};
        return ink_bbox(p, band, style.bg_active).center_x();
    };
    const float mid = measure_title_centre(true);
    const float left = measure_title_centre(false);
    AURORA_TEST_CHECK_GT(mid, 0.0F);  // 标题确实画出来了（-1 表示无墨迹）
    AURORA_TEST_CHECK_GT(left, 0.0F);
    AURORA_TEST_CHECK_NEAR(mid, 200.0F, 12.0F);  // 整宽带中心 = 400 / 2
    AURORA_TEST_CHECK_LT(left, 180.0F);  // 左对齐对照组：明显不在中点
}

AURORA_TEST_CASE(title_is_vertically_centered_in_every_language) {
    // 标题在栏内**垂直**居中：`draw_text` 的 y 是行盒顶（基线 = 顶 + ascent），绘制层须按字体
    // 行盒高把带顶下移半程，否则文字贴栏的上缘（修复前实测墨迹中心比栏高中心偏上：Adwaita
    // 6dp / Windows 8.5dp / Mac 1.5dp）。判据 = 墨迹外接框中心与栏高中心的偏差 < 2.5dp——
    // 容差取自「行盒含下伸部，纯大写标题的视觉中心本就比几何中心低 1~2dp」。三档全过才算绿：
    // 敏感侧在 Adwaita / Windows（修复前偏差 6 / 8.5 远超阈值），Mac 档修复前只偏 1.5，单看它
    // 不足以区分，故判据以三档整体为准。
    const auto deviation = [&](CsdPreset preset) -> float {
        TitleBarStyle style = TitleBarStyle::from_preset(preset);
        style.show_minimize = false;  // 隐去三枚按钮：带内只剩标题墨迹，扫描不必避开按钮区
        style.show_maximize = false;
        style.show_close = false;
        csd::TitleBarPaintState s = plain_state();
        s.style = style;
        s.title = "Title";
        s.active = true;
        Painter p;
        raster_direct(p, s);
        const Rect band{.origin = Point{.x = 0.0F, .y = 0.0F}, .size = Size{.width = 400.0F, .height = style.height}};
        const InkBox k = ink_bbox(p, band, style.bg_active);
        if (k.empty()) {
            return 1e9F;  // 一个字都没画出来：判据直接转红（不静默当成「居中」）
        }
        return std::fabs(k.center_y() - (style.height * 0.5F));
    };
    AURORA_TEST_CHECK_LT(deviation(CsdPreset::AdwaitaDark), 2.5F);
    AURORA_TEST_CHECK_LT(deviation(CsdPreset::WindowsDark), 2.5F);
    AURORA_TEST_CHECK_LT(deviation(CsdPreset::MacDark), 2.5F);
}

AURORA_TEST_CASE(adwaita_preset_title_is_centered_in_full_width_band) {
    // libadwaita `AdwHeaderBar` 的标题居中 ⇒ Adwaita 档 `center_title` 默认 true，几何层给整宽带。
    // 判据与 Mac 档同构：墨迹中心 ≈ 窗口水平中心；对照组（显式 false）须明显偏左。
    const auto measure_title_centre = [&](bool centered) -> float {
        TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::AdwaitaDark);
        style.center_title = centered;
        csd::TitleBarPaintState s = plain_state();
        s.style = style;
        s.title = "Centered";
        s.active = true;
        Painter p;
        raster_direct(p, s);
        // 只扫 x∈[30,290] 的区：右侧避让按钮组（首枚 minimize 起于 304——扫到它会被当成标题
        // 墨迹把中心拽向右）、左侧仍容纳左对齐对照组的文字（起于 36）。图标槽为空槽，不产墨迹。
        const Rect band{.origin = Point{.x = 30.0F, .y = 0.0F}, .size = Size{.width = 260.0F, .height = style.height}};
        return ink_bbox(p, band, style.bg_active).center_x();
    };
    const float mid = measure_title_centre(true);
    const float left = measure_title_centre(false);
    AURORA_TEST_CHECK_GT(mid, 0.0F);  // 标题确实画出来了（-1 = 无墨迹）
    AURORA_TEST_CHECK_GT(left, 0.0F);
    AURORA_TEST_CHECK_NEAR(mid, 200.0F, 12.0F);
    AURORA_TEST_CHECK_LT(left, 180.0F);
}

AURORA_TEST_CASE(mac_layout_shows_traffic_lights_without_hover) {
    TitleBarStyle style;
    style.button_layout = TitleBarButtonLayout::Mac;
    const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
    csd::TitleBarPaintState s = plain_state();
    s.style = style;
    Painter p;
    raster_direct(p, s);
    // 三枚圆点常显（macOS 视觉签名），不依赖悬停态。
    AURORA_TEST_CHECK(px(p, center_of(g.close)) == AURORA_MAC_RED);
    AURORA_TEST_CHECK(px(p, center_of(g.minimize)) == AURORA_MAC_YELLOW);
    AURORA_TEST_CHECK(px(p, center_of(g.maximize)) == AURORA_MAC_GREEN);
}

AURORA_TEST_CASE(inactive_palette_and_hidden_slots_leave_only_bg) {
    const TitleBarGeometry shown = title_bar_geometry(400.0F, TitleBarStyle{}, false, true);
    const Point min_c = center_of(shown.minimize);
    const Point max_c = center_of(shown.maximize);

    // 失焦：底色整条切到失焦值。
    csd::TitleBarPaintState s = plain_state();
    s.active = false;
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height * 0.5F}) == s.style.bg_inactive);
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height + 4.0F}) == AURORA_BASE);
    }
    // show_minimize=false：几何给空盒、绘制层跳过，该槽不留任何像素。
    // （隐藏按钮不占位，故取样点取「按最宽布局算出的原槽位」——右移补位的按钮不会落到那里。）
    s = plain_state();
    s.style.show_minimize = false;
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, min_c) == s.style.bg_active);
    }
    // resizable=false：最大化钮同样退化为空盒（与命中测试同口径）。再隐去最小化钮，
    // 使最大化原槽位两侧都不被补位按钮占用，取样才只反映「最大化是否画了东西」。
    s = plain_state();
    s.resizable = false;
    s.style.show_minimize = false;
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, max_c) == s.style.bg_active);
    }
    // show_title=false：文字区只剩底色（内置位图字体的白像素不应出现）。
    s = plain_state();
    s.title = "Aa 01";
    s.style.show_title = false;
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height * 0.5F}) == s.style.bg_active);
    }
}

AURORA_TEST_CASE(shadow_underlay_paints_margin_and_restores_content) {
    // paint_window_shadow 是 GPU/DL underlay 源：本用例在「不透明画布」不变量下验证——
    // ① 直绘与录制/回放逐位一致；② 阴影落在内容矩形外的 margin 环（环像素变暗）；
    // ③ 内容矩形被随后的不透明底色完整恢复；④ 远外缘（衰减已归零）保持基底不被触碰。
    // 真 alpha 数值（外缘字=0x0、渐变预乘）由 utest_csd_shadow_compose 对 BGRA 合成器断言。
    const csd::TitleBarPaintState s = shadow_state(true);
    check_shadow_bit_identical(s);

    Painter p;
    raster_shadow_direct(p, s);

    // 左/上 margin 带中点：处于衰减环内，黑色阴影在不透明基底上使像素变暗。
    AURORA_TEST_CHECK_LT(px(p, Point{5.0F, 60.0F}).r, AURORA_BASE.r);
    AURORA_TEST_CHECK_LT(px(p, Point{210.0F, 5.0F}).g, AURORA_BASE.g);
    // 对侧环同样闭合（取避开 Painter 边界行口径的一列）。
    AURORA_TEST_CHECK_LT(px(p, Point{415.0F, 60.0F}).b, AURORA_BASE.b);
    AURORA_TEST_CHECK_LT(px(p, Point{210.0F, 115.0F}).r, AURORA_BASE.r);

    // 四向缓冲外缘：距离 ≥ blur=margin−1，衰减环不覆盖（软件画布上保持基底；真上屏缓冲
    // 这些字由 BGRA 合成器覆写为 0x0，见 utest_csd_shadow_compose）。
    AURORA_TEST_CHECK(px(p, Point{0.0F, 60.0F}) == AURORA_BASE);
    AURORA_TEST_CHECK(px(p, Point{210.0F, 0.0F}) == AURORA_BASE);

    // 内容矩形被不透明底色二次覆盖完整恢复（阴影形状内部实心填充不向内渗漏）。
    AURORA_TEST_CHECK(px(p, Point{200.0F, 60.0F}) == AURORA_BASE);
    AURORA_TEST_CHECK(px(p, Point{11.0F, 11.0F}) == AURORA_BASE);  // 内容左上角内点
}

AURORA_TEST_CASE(shadow_collapses_when_margin_zero_and_borderless_has_no_overlay) {
    // 塌缩态（margin=0：最大化/全屏/平铺/非 CSD）：paint_window_shadow 无操作。
    const csd::TitleBarPaintState s = shadow_state(false);
    check_shadow_bit_identical(s);
    {
        Painter p;
        raster_shadow_direct(p, s);
        AURORA_TEST_CHECK(px(p, Point{3.0F, 60.0F}) == AURORA_BASE);
        AURORA_TEST_CHECK(px(p, Point{419.0F, 119.0F}) == AURORA_BASE);
    }

    // Borderless：有阴影 underlay 但无标题栏 overlay——两个门控相互独立，GPU 宿主据此
    // 分别决定 underlay/overlay DL 是否回放。
    AURORA_TEST_CHECK_FALSE(s.paints_title_bar());
    const csd::TitleBarPaintState borderless = shadow_state(true);
    AURORA_TEST_CHECK_FALSE(borderless.paints_title_bar());
    AURORA_TEST_CHECK(borderless.shadow_margin > 0.0F);
    {
        // Borderless 阴影帧的内容区顶部不得出现标题栏背景填充。
        Painter p;
        raster_shadow_direct(p, borderless);
        AURORA_TEST_CHECK(px(p, Point{210.0F, 15.0F}) == AURORA_BASE);  // 内容顶部 = 底色非标题色
    }

    // 全屏揭示条只放行 overlay，不复活阴影（全屏 margin 恒塌缩）。
    csd::TitleBarPaintState fs = shadow_state(false);
    fs.title_bar = true;
    fs.mode = WindowMode::FullScreen;
    AURORA_TEST_CHECK_FALSE(fs.paints_title_bar());
    fs.fullscreen_bar_revealed = true;
    AURORA_TEST_CHECK_TRUE(fs.paints_title_bar());
    AURORA_TEST_CHECK_EQ(fs.shadow_margin, 0.0F);
}

}  // namespace aurora::test_cases::utest_title_bar_painter

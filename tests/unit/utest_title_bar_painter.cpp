/// 测试类型: unit
/// 目标单元: src/aurora/window/detail/title_bar_painter.h + src/aurora/window/detail/title_bar_painter.cpp
/// 测试说明: 覆盖 CSD 装饰绘制层——「直接光栅」与「录制→回放进 Painter」两条上屏路径逐位等价
/// （wl_shm 软件宿主与 wgpu GPU 宿主共用同一实现、不得各自漂移的核心不变量，图像/文本/圆角/
/// 线段原语全部走一遍）、显隐门控（无 CSD 标题栏、全屏未揭示顶边条 → 零像素）、三种按钮布局的
/// 常态与悬停取色、失焦配色切换、隐藏按钮与不可缩放窗口在该槽不留任何像素。
/// 无平台依赖（纯 Painter 画布），故 Windows/Linux/macOS 与无头 CI 均可跑。

#include <cstddef>
#include <cstdint>
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

constexpr Color kBase{10, 20, 30, 255};  ///< 画布底色：装饰未覆盖处必须仍是它
constexpr Color kMacRed{0xFF, 0x5F, 0x57, 255};
constexpr Color kMacYellow{0xFE, 0xBC, 0x2E, 255};
constexpr Color kMacGreen{0x28, 0xC8, 0x40, 255};

[[nodiscard]] auto center_of(const Rect &r) -> Point {
    return Point{r.origin.x + r.size.width * 0.5F, r.origin.y + r.size.height * 0.5F};
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
    p.fill_rect(Rect{Point{0.0F, 0.0F}, Size{400.0F, 120.0F}}, kBase);
    csd::paint_title_bar(p, s);
}

/// @brief 录制为 DisplayList 再回放进同一画布——GPU 宿主（WgpuWaylandSurface）走的就是这条。
auto raster_recorded(Painter &p, const csd::TitleBarPaintState &s) -> void {
    p.begin(400, 120);
    p.fill_rect(Rect{Point{0.0F, 0.0F}, Size{400.0F, 120.0F}}, kBase);
    DisplayList dl;
    p.record(dl);
    csd::paint_title_bar(p, s);
    p.stop();
    dl.replay(p);
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
        if (direct.data()[i] != replayed.data()[i]) {
            ++diff;
        }
    }
    AURORA_TEST_CHECK_EQ(diff, 0U);
}

}  // namespace

AURORA_TEST_CASE(direct_raster_and_record_replay_are_bit_identical) {
    // 主不变量：软件路径（present 里直绘进帧缓冲）与 GPU 路径（录成 DL 追加回放进当帧）必须
    // 画出同一份像素。状态刻意含齐四类原语：图标（DrawImage）、标题文字（DrawText）、悬停圆底
    // （RoundedRect）、按钮符号（DrawLine）——任一类在录制/回放口径上分叉，本用例即红灯。
    csd::TitleBarPaintState s = plain_state();
    s.title = "Aa 01 窗口";
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
    const Point mid_band{200.0F, s.style.height * 0.5F};

    // 无 CSD 标题栏（合成器提供 SSD / 无边框策略）：整幅仍是底色 → GPU 宿主据此跳过回放。
    s.title_bar = false;
    AURORA_TEST_CHECK_FALSE(s.paints_anything());
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, mid_band) == kBase);
    }

    // 全屏默认隐藏标题栏；顶边悬停揭示后必须重新可见（覆盖层语义，不改窗口尺寸）。
    s.title_bar = true;
    s.mode = WindowMode::FullScreen;
    AURORA_TEST_CHECK_FALSE(s.paints_anything());
    {
        Painter p;
        raster_direct(p, s);
        AURORA_TEST_CHECK(px(p, mid_band) == kBase);
    }
    s.fullscreen_bar_revealed = true;
    AURORA_TEST_CHECK_TRUE(s.paints_anything());
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
        AURORA_TEST_CHECK(c != kBase);
        // 装饰只占顶部 height：其下仍是底色。
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height + 4.0F}) == kBase);
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height * 0.5F}) == s.style.bg_active);
    }
    // 悬停关闭钮（序号 2）：圆底铺满为 close_hover 特征红。取圆心上方一点——圆心本身被
    // 白色 ✕ 字形穿过，那里只能断言「不是红」。
    s.hovered_button = 2;
    {
        Painter p;
        raster_direct(p, s);
        const Point above_centre{center_of(g.close).x, g.close.origin.y + 3.0F};
        AURORA_TEST_CHECK(px(p, above_centre) == s.style.close_hover);
    }
    // 悬停最小化钮（序号 0）：半透明白罩，既非底色也非红。
    s.hovered_button = 0;
    {
        Painter p;
        raster_direct(p, s);
        const Color c = px(p, center_of(g.minimize));
        AURORA_TEST_CHECK(c != kBase);
        AURORA_TEST_CHECK(c != s.style.close_hover);
    }
}

AURORA_TEST_CASE(windows_layout_hovers_full_height_rect) {
    const TitleBarStyle style = TitleBarStyle::windows_dark();
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

AURORA_TEST_CASE(mac_layout_shows_traffic_lights_without_hover) {
    TitleBarStyle style;
    style.button_layout = TitleBarButtonLayout::Mac;
    const TitleBarGeometry g = title_bar_geometry(400.0F, style, false, true);
    csd::TitleBarPaintState s = plain_state();
    s.style = style;
    Painter p;
    raster_direct(p, s);
    // 三枚圆点常显（macOS 视觉签名），不依赖悬停态。
    AURORA_TEST_CHECK(px(p, center_of(g.close)) == kMacRed);
    AURORA_TEST_CHECK(px(p, center_of(g.minimize)) == kMacYellow);
    AURORA_TEST_CHECK(px(p, center_of(g.maximize)) == kMacGreen);
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
        AURORA_TEST_CHECK(px(p, Point{40.0F, s.style.height + 4.0F}) == kBase);
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

}  // namespace aurora::test_cases::utest_title_bar_painter

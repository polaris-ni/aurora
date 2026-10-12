/// 测试类型: unit
/// 目标单元: include/aurora/window/title_bar_geometry.h
/// 测试说明: 覆盖 title_bar_geometry 纯几何计算——Adwaita/Windows/Mac 三种按钮布局、
/// 图标与标题分区、隐藏按钮不占位、resizable/退化宽度/居中标题边界行为

#include "aurora/window/title_bar_geometry.h"
#include "framework/aurora_test.h"

namespace aurora::test_cases::utest_title_bar_geometry {

namespace {

/// @brief 四元组近似断言（浮点几何统一 1e-4 容差）。
auto near_rect(const Rect &r, float x, float y, float w, float h) -> void {
    AURORA_TEST_CHECK_NEAR(r.origin.x, x, 1e-4F);
    AURORA_TEST_CHECK_NEAR(r.origin.y, y, 1e-4F);
    AURORA_TEST_CHECK_NEAR(r.size.width, w, 1e-4F);
    AURORA_TEST_CHECK_NEAR(r.size.height, h, 1e-4F);
}

}  // namespace

AURORA_TEST_CASE(adwaita_buttons_stack_right_to_left) {
    // 默认 Adwaita：正方形边长 clamp(36-10,22,30)=26、间距 6、右缘外边距 6（libadwaita
    // `.toolbar` 的 6px margins and spacing 口径）；自右向左 close→max→min，垂直居中。
    constexpr TitleBarStyle style;
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    near_rect(g.close, 768.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g.maximize, 736.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g.minimize, 704.0F, 5.0F, 26.0F, 26.0F);
}

AURORA_TEST_CASE(adwaita_icon_and_title_bands) {
    // 图标槽：边长 min(16, 36-20)=16、x=12、垂直居中；标题区为**整宽带**（Adwaita 档
    // `center_title` 为默认 true，libadwaita `AdwHeaderBar` 标题即居中），居中由绘制层按
    // 实测量宽完成，垂直方向同样由绘制层按行盒高居中。
    constexpr TitleBarStyle style;
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    near_rect(g.icon, 12.0F, 10.0F, 16.0F, 16.0F);
    near_rect(g.title, 0.0F, 0.0F, 800.0F, 36.0F);
}

AURORA_TEST_CASE(windows_buttons_seamless_full_height) {
    // WinUI 布局（真机 DWM 口径）：按钮盒 46×32（预设显式钉值）、整高、自右上角无缝向左排
    // （无边距无间距）；图标槽显式 16dp —— 栏高 32 时按栏高推导的 min(16, h-20) 会退化为 12，
    // 故预设必须钉住这一项。
    constexpr TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    near_rect(g.close, 754.0F, 0.0F, 46.0F, 32.0F);
    near_rect(g.maximize, 708.0F, 0.0F, 46.0F, 32.0F);
    near_rect(g.minimize, 662.0F, 0.0F, 46.0F, 32.0F);
    // 相邻按钮右缘相接（无缝）。
    AURORA_TEST_CHECK_NEAR(g.maximize.right(), g.close.origin.x, 1e-4F);
    AURORA_TEST_CHECK_NEAR(g.minimize.right(), g.maximize.origin.x, 1e-4F);
    // 图标槽 16dp 垂直居中：(32-16)/2 = 8。
    near_rect(g.icon, 12.0F, 8.0F, 16.0F, 16.0F);
    // 标题区从图标右侧 +8 起到按钮组左侧 -8，垂直全高。
    near_rect(g.title, 36.0F, 0.0F, 618.0F, 32.0F);
}

AURORA_TEST_CASE(mac_traffic_lights_left_to_right) {
    // macOS 布局（**未钉值**的回落口径）：圆直径 clamp(36*0.4,11,14)=14、左缘外边距 8、间距 8，
    // 自左向右 close→min→max（macOS 顺序）；图标排按钮组右侧 +12。
    // ⚠️ Mac 两档预设不走这组推导式（已钉 12 / 8 / 14），见下一条用例。
    TitleBarStyle style;
    style.button_layout = TitleBarButtonLayout::Mac;
    style.center_title = false;  // 本用例测左对齐口径的标题镜像区（默认 true 会给整宽带）
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    near_rect(g.close, 8.0F, 11.0F, 14.0F, 14.0F);
    near_rect(g.minimize, 30.0F, 11.0F, 14.0F, 14.0F);
    near_rect(g.maximize, 52.0F, 11.0F, 14.0F, 14.0F);
    near_rect(g.icon, 78.0F, 10.0F, 16.0F, 16.0F);
    // 标题区镜像：左至按钮组/图标右侧，右到 width-8。
    near_rect(g.title, 102.0F, 0.0F, 690.0F, 36.0F);
}

AURORA_TEST_CASE(mac_preset_pins_native_traffic_light_metrics) {
    // Mac 预设把红绿灯几何钉到真机口径（栏高 28 / 直径 12 / 圆心距 20 ⇒ 间距 8、首枚圆心距
    // 左缘 20 ⇒ 外边距 14），与上面那条「未钉值回落」用例刻意并存——两组数字不同正是钉值生效
    // 的证据：沿用推导式在栏高 28 时会给出直径 clamp(11.2,11,14)=11.2、外边距 8，与真机差 43%。
    constexpr TitleBarStyle style = TitleBarStyle::from_preset(CsdPreset::MacDark);
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    near_rect(g.close, 14.0F, 8.0F, 12.0F, 12.0F);
    near_rect(g.minimize, 34.0F, 8.0F, 12.0F, 12.0F);
    near_rect(g.maximize, 54.0F, 8.0F, 12.0F, 12.0F);
    // 图标槽同样钉 16dp（栏高 28 时推导式 min(16, 8) 会退化成 8）。
    near_rect(g.icon, 78.0F, 6.0F, 16.0F, 16.0F);
    // Mac 预设置 center_title=true（macOS 窗口标题居中）⇒ 标题区是整宽带，居中由绘制层完成。
    near_rect(g.title, 0.0F, 0.0F, 800.0F, 28.0F);
}

AURORA_TEST_CASE(hidden_buttons_do_not_reserve_space) {
    // 显式 show_*=false 的按钮返回空盒且不占位，其余按钮位置不变（右侧系自贴边排布，
    // 藏最左的最小化钮不影响 close/max；藏 close 则后继按钮向贴边侧收缩补位）。
    TitleBarStyle style;
    style.show_minimize = false;
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, true);
    AURORA_TEST_CHECK_EQ(g.minimize.size.width, 0.0F);
    near_rect(g.close, 768.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g.maximize, 736.0F, 5.0F, 26.0F, 26.0F);

    TitleBarStyle no_close;
    no_close.show_close = false;
    const TitleBarGeometry g2 = title_bar_geometry(800.0F, no_close, false, true);
    AURORA_TEST_CHECK_EQ(g2.close.size.width, 0.0F);
    near_rect(g2.maximize, 768.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g2.minimize, 736.0F, 5.0F, 26.0F, 26.0F);
}

AURORA_TEST_CASE(non_resizable_hides_maximize_and_shifts_minimize) {
    // resizable=false：maximize 盒为空（自动隐藏），min 收缩进其让出的槽位。
    constexpr TitleBarStyle style;
    const TitleBarGeometry g = title_bar_geometry(800.0F, style, false, false);
    AURORA_TEST_CHECK_NEAR(g.maximize.size.width, 0.0F, 1e-4F);
    near_rect(g.close, 768.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g.minimize, 736.0F, 5.0F, 26.0F, 26.0F);
}

AURORA_TEST_CASE(maximized_flag_does_not_change_geometry) {
    // 头文件契约：maximized 只决定最大化/还原图标字形（绘制层消费），不影响任何按钮盒。
    constexpr TitleBarStyle style;
    const TitleBarGeometry normal = title_bar_geometry(800.0F, style, false, true);
    const TitleBarGeometry maximized = title_bar_geometry(800.0F, style, true, true);
    AURORA_TEST_CHECK_TRUE(normal.close == maximized.close);
    AURORA_TEST_CHECK_TRUE(normal.maximize == maximized.maximize);
    AURORA_TEST_CHECK_TRUE(normal.minimize == maximized.minimize);
    AURORA_TEST_CHECK_TRUE(normal.icon == maximized.icon);
    AURORA_TEST_CHECK_TRUE(normal.title == maximized.title);
}

AURORA_TEST_CASE(degenerate_and_squeezed_widths_yield_empty_bands) {
    // 退化输入：width<=0 全空几何（空盒约定 = width==0）。
    // 左对齐口径（`center_title` 现默认为 true，居中档给整宽带、不参与「挤压退化」判据）。
    TitleBarStyle style;
    style.center_title = false;
    const TitleBarGeometry zero = title_bar_geometry(0.0F, style, false, true);
    AURORA_TEST_CHECK_NEAR(zero.close.size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(zero.maximize.size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(zero.minimize.size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(zero.icon.size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(zero.title.size.width, 0.0F, 1e-4F);
    const TitleBarGeometry negative = title_bar_geometry(-10.0F, style, false, true);
    AURORA_TEST_CHECK_NEAR(negative.title.size.width, 0.0F, 1e-4F);
    // 窄窗挤压：标题区 left>right 时退化为空盒，按钮盒仍按规则计算。
    const TitleBarGeometry narrow = title_bar_geometry(60.0F, style, false, true);
    AURORA_TEST_CHECK_NEAR(narrow.title.size.width, 0.0F, 1e-4F);
    AURORA_TEST_CHECK_NEAR(narrow.close.origin.x, 28.0F, 1e-4F);
}

AURORA_TEST_CASE(center_title_uses_full_width_band) {
    // center_title=true：标题区整宽（0..width）供绘制层水平居中；按钮盒不受影响。
    TitleBarStyle style;
    style.center_title = true;
    const TitleBarGeometry g = title_bar_geometry(500.0F, style, false, true);
    near_rect(g.title, 0.0F, 0.0F, 500.0F, 36.0F);
    near_rect(g.close, 468.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g.maximize, 436.0F, 5.0F, 26.0F, 26.0F);
    near_rect(g.minimize, 404.0F, 5.0F, 26.0F, 26.0F);
}

AURORA_TEST_CASE(button_side_follows_height_clamps) {
    // 高度驱动的尺寸规则：Adwaita 边长 clamp(h-10,22,30)；Windows 宽 round(h*1.44)；
    // Mac 直径 clamp(h*0.4,11,14)；图标槽 min(16, h-20)。
    TitleBarStyle adwaita;
    adwaita.height = 30.0F;
    const TitleBarGeometry small = title_bar_geometry(800.0F, adwaita, false, true);
    near_rect(small.close, 772.0F, 4.0F, 22.0F, 22.0F);  // clamp(20,22,30)=22，右缘外边距 6
    near_rect(small.icon, 12.0F, 10.0F, 10.0F, 10.0F);  // min(16, 30-20)=10

    adwaita.height = 50.0F;
    const TitleBarGeometry large = title_bar_geometry(800.0F, adwaita, false, true);
    near_rect(large.close, 764.0F, 10.0F, 30.0F, 30.0F);  // clamp(40,22,30)=30

    TitleBarStyle windows = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    windows.height = 40.0F;
    windows.metrics.button_size = 0.0F;  // 归零覆盖值 → 走栏高推导路径（Windows 宽 = round(h*1.44)）
    const TitleBarGeometry win = title_bar_geometry(800.0F, windows, false, true);
    near_rect(win.close, 742.0F, 0.0F, 58.0F, 40.0F);  // round(57.6)=58

    // 显式覆盖优先于栏高推导：预设已钉 button_size=46，即便 height=40 也保持 46（不复随栏高漂移）。
    TitleBarStyle win_ov = TitleBarStyle::from_preset(CsdPreset::WindowsDark);
    win_ov.height = 40.0F;  // 不归零 button_size（预设已置 46）
    const TitleBarGeometry win_override = title_bar_geometry(800.0F, win_ov, false, true);
    near_rect(win_override.close, 754.0F, 0.0F, 46.0F, 40.0F);

    TitleBarStyle mac;
    mac.height = 20.0F;
    mac.button_layout = TitleBarButtonLayout::Mac;
    const TitleBarGeometry lights = title_bar_geometry(800.0F, mac, false, true);
    near_rect(lights.close, 8.0F, 4.5F, 11.0F, 11.0F);  // clamp(8,11,14)=11
}

}  // namespace aurora::test_cases::utest_title_bar_geometry

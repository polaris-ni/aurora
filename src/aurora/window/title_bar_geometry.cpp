#include "aurora/window/title_bar_geometry.h"

#include <algorithm>
#include <cmath>

namespace aurora {

// ============================================================================
// 标题栏尺寸规则（唯一权威来源；绘制层与命中测试层一律消费
// title_bar_geometry() 的输出，不得各自再推几何）：
//
// - Adwaita：按钮为正方形，边长 btn = clamp(height-6, 26, 34)，垂直居中；
//   间距 3；右缘外边距 6。从右往左顺序 close→max→min。
//   盒宽 30（栏高 36 时）= libadwaita `.default-decoration` 的「22dp 内容 + 4dp 外边距」：
//   `windowcontrols > button { min-width: 22px; padding: 4px }`，而内容即 `> image`
//   （16dp symbolic 图标 + 3dp 内边距 = 22dp）。间距 3 出自 `windowcontrols { border-spacing: 3px }`。
//   ⚠️ 悬停圆**不是**盒的内切圆（那会给 30dp）：圆画在 image 上，恒 22dp 居中，由绘制层按
//   语言常量给出，与盒宽解耦（盒被覆写时圆不变）。
// - Windows：按钮矩形宽 = round(height*1.44)、高 = height 整高，
//   从右上角无缝向左排（无外边距、无间距），顺序同上。
// - Mac：圆形直径 d = clamp(height*0.4, 11, 14)，位于左侧，左缘外边距 8，
//   间距 8，从左往右 close→min→max（macOS 顺序）。
//   ⚠️ 本推导只是「未钉值时的回落」：Mac 两档预设已把直径 / 间距 / 外边距钉为
//   12 / 8 / 14（真机口径），故经 `from_preset` 取到的样式不走这组推导式。
// - icon：存在时边长 = min(16, height-20)，x=12（Mac 布局时排在按钮组右侧 +12），
//   垂直居中。
// - title：x 从 icon 右侧+8 起（无图标则 12；Mac 布局无图标则按钮组右侧+8；
//   center_title 时整宽居中），右界到按钮组左侧-8（Mac 镜像为 width-8）；
//   垂直全高（文字由绘制层居中）。
// - 隐藏按钮（显式 show_* = false，或 maximize 因 resizable=false 自动隐藏）
//   不占位，其余按钮向贴边侧收缩补位；空 Rect 表示法 = Size{0,0}。
//
// 度量覆写（`TitleBarStyle::metrics`）：上表每个推导式都可被显式覆写，判据是「非 0 即覆写」
// ——0 恒表示「沿用本表按栏高联动的推导结果」。这样既有三套预设的观感不会被一次性改坏，
// 且 `TitleBarStyle{}` 的默认值使本文件行为与引入覆写前逐值相同。
// ⚠️ 已知边界：Adwaita 的 button_gap / Mac 的 gap 与 margin 推导值非零，故**无法**用 0 显式
// 表达「零间距」（0 会被解读为「走推导」）。需要无缝排布时请改用 `TitleBarButtonLayout::Windows`
// （其推导值本就是 0），这是视觉语言层面的选择，语义上更贴切。
// ============================================================================

namespace {

/// @brief 在高度 bar 内垂直居中的正方形盒（x 为盒左缘）。
constexpr auto v_centered_square(float x, float bar_height, float side) noexcept -> Rect {
    return Rect{.origin = Point{.x = x, .y = (bar_height - side) * 0.5F}, .size = Size{.width = side, .height = side}};
}

/// @brief 右侧系按钮布局（Adwaita / Windows）：自右向左 close→max→min。
/// @return 按钮组朝标题一侧的边缘 x 坐标。
auto layout_right_buttons(const TitleBarStyle &style, float width, bool resizable, TitleBarGeometry &g) -> float {
    const bool windows = style.button_layout == TitleBarButtonLayout::Windows;
    const float side = style.metrics.button_size > 0.0F ? style.metrics.button_size
                                                        : (windows ? std::round(style.height * 1.44F)
                                                                   : std::clamp(style.height - 6.0F, 26.0F, 34.0F));
    // 未钉值时 Adwaita 的间距为 3dp（libadwaita `windowcontrols { border-spacing: 3px }`）、
    // 右缘外边距 6dp（见文件头注释块）；Windows 恒 0（无缝整高排布）。
    const float gap = style.metrics.button_gap > 0.0F ? style.metrics.button_gap : (windows ? 0.0F : 3.0F);
    const float margin = style.metrics.button_margin > 0.0F ? style.metrics.button_margin : (windows ? 0.0F : 6.0F);
    float cursor = width - margin;  // 下一个按钮的贴边侧外缘
    float group_inner_edge = width - margin;
    const auto place = [&](Rect &dst) -> void {
        cursor -= side;  // 先落盒再扣间距：首个按钮不吃前导间距
        dst = windows
                  ? Rect{.origin = Point{.x = cursor, .y = 0.0F}, .size = Size{.width = side, .height = style.height}}
                  : v_centered_square(cursor, style.height, side);
        group_inner_edge = cursor;
        cursor -= gap;
    };
    if (style.show_close) {
        place(g.close);
    }
    if (style.show_maximize && resizable) {  // resizable=false 时 maximize 盒为空
        place(g.maximize);
    }
    if (style.show_minimize) {
        place(g.minimize);
    }
    return group_inner_edge;
}

/// @brief Mac 左侧系按钮布局：自左向右 close→min→max。
/// @return 按钮组朝标题一侧的边缘 x 坐标。
auto layout_left_buttons(const TitleBarStyle &style, bool resizable, TitleBarGeometry &g) -> float {
    const float d =
        style.metrics.button_size > 0.0F ? style.metrics.button_size : std::clamp(style.height * 0.4F, 11.0F, 14.0F);
    const float gap = style.metrics.button_gap > 0.0F ? style.metrics.button_gap : 8.0F;
    const float margin = style.metrics.button_margin > 0.0F ? style.metrics.button_margin : 8.0F;
    float cursor = margin;
    float group_inner_edge = margin;  // 无可见按钮时退化为左边距本身
    const auto place = [&](Rect &dst) -> void {
        dst = v_centered_square(cursor, style.height, d);
        cursor += d + gap;
        group_inner_edge = cursor - gap;  // 最近一个圆的右缘
    };
    if (style.show_close) {
        place(g.close);
    }
    if (style.show_minimize) {
        place(g.minimize);
    }
    if (style.show_maximize && resizable) {
        place(g.maximize);
    }
    return group_inner_edge;
}

/// @brief 图标槽（16×16 或 height-20 取小者；Mac 排在按钮组右侧 +12）。
auto layout_icon(const TitleBarStyle &style, bool mac, float group_inner_edge, TitleBarGeometry &g) -> void {
    const float icon_side =
        style.metrics.icon_size > 0.0F ? style.metrics.icon_size : std::min(16.0F, style.height - 20.0F);
    if (icon_side > 0.0F) {
        const float icon_x = mac ? group_inner_edge + style.metrics.content_padding : style.metrics.content_padding;
        g.icon = v_centered_square(icon_x, style.height, icon_side);
    }
}

/// @brief 标题文字可用区（垂直全高；窄窗挤压 left>right 时保持空盒）。
auto layout_title(const TitleBarStyle &style, float width, bool mac, float group_inner_edge, TitleBarGeometry &g)
    -> void {
    if (style.center_title) {
        g.title = Rect{.origin = Point{.x = 0.0F, .y = 0.0F},
                       .size = Size{.width = width, .height = style.height}};  // 整宽居中，文字由绘制层水平居中
        return;
    }
    float left = style.metrics.content_padding;
    if (g.icon.size.width > 0.0F) {
        left = g.icon.right() + 8.0F;
    } else if (mac) {
        left = group_inner_edge + 8.0F;
    }
    const float right = mac ? width - 8.0F : group_inner_edge - 8.0F;  // 到按钮组一侧留 8
    if (left <= right) {
        g.title =
            Rect{.origin = Point{.x = left, .y = 0.0F}, .size = Size{.width = right - left, .height = style.height}};
    }
}

}  // namespace

auto title_bar_geometry(float width, const TitleBarStyle &style, bool maximized, bool resizable) -> TitleBarGeometry {
    (void)maximized;  // 按钮盒尺寸不随最大化变化；最大化⇄还原图标由绘制层据该参数切换字形。

    TitleBarGeometry g;
    if (!(width > 0.0F)) {
        return g;  // 退化输入：全空几何（各分区保持默认空盒 Size{0,0}）
    }
    const bool mac = style.button_layout == TitleBarButtonLayout::Mac;

    const float group_inner_edge =
        mac ? layout_left_buttons(style, resizable, g) : layout_right_buttons(style, width, resizable, g);
    layout_icon(style, mac, group_inner_edge, g);
    layout_title(style, width, mac, group_inner_edge, g);

    return g;
}

}  // namespace aurora

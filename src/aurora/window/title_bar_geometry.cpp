#include "aurora/window/title_bar_geometry.h"

#include <algorithm>
#include <cmath>

namespace aurora {

// ============================================================================
// 标题栏尺寸规则（唯一权威来源；绘制层与命中测试层一律消费
// title_bar_geometry() 的输出，不得各自再推几何）：
//
// - Adwaita：按钮为正方形，边长 btn = clamp(height-10, 22, 30)，垂直居中；
//   间距 4；右缘外边距 8。从右往左顺序 close→max→min。
//   悬停圆即该正方形的内切圆（绘制层取盒即可）。
// - Windows：按钮矩形宽 = round(height*1.44)、高 = height 整高，
//   从右上角无缝向左排（无外边距、无间距），顺序同上。
// - Mac：圆形直径 d = clamp(height*0.4, 11, 14)，位于左侧，左缘外边距 8，
//   间距 8，从左往右 close→min→max（macOS 顺序）。
// - icon：存在时边长 = min(16, height-20)，x=12（Mac 布局时排在按钮组右侧 +12），
//   垂直居中。
// - title：x 从 icon 右侧+8 起（无图标则 12；Mac 布局无图标则按钮组右侧+8；
//   center_title 时整宽居中），右界到按钮组左侧-8（Mac 镜像为 width-8）；
//   垂直全高（文字由绘制层居中）。
// - 隐藏按钮（显式 show_* = false，或 maximize 因 resizable=false 自动隐藏）
//   不占位，其余按钮向贴边侧收缩补位；空 Rect 表示法 = Size{0,0}。
// ============================================================================

namespace {

/// @brief 在高度 bar 内垂直居中的正方形盒（x 为盒左缘）。
constexpr auto v_centered_square(float x, float bar_height, float side) noexcept -> Rect {
    return Rect{ .origin = Point{ .x = x, .y = (bar_height - side) * 0.5f },
                 .size = Size{ .width = side, .height = side } };
}

/// @brief 右侧系按钮布局（Adwaita / Windows）：自右向左 close→max→min。
/// @return 按钮组朝标题一侧的边缘 x 坐标。
auto layout_right_buttons(const TitleBarStyle &style, float width, bool resizable, TitleBarGeometry &g) -> float {
    const bool windows = style.button_layout == TitleBarButtonLayout::Windows;
    const float side = windows ? std::round(style.height * 1.44f) : std::clamp(style.height - 10.0f, 22.0f, 30.0f);
    const float gap = windows ? 0.0f : 4.0f;    // Windows 无缝排布，间距 0
    const float margin = windows ? 0.0f : 8.0f; // Windows 贴右上角，无边距
    float cursor = width - margin;              // 下一个按钮的贴边侧外缘
    float group_inner_edge = width - margin;
    const auto place = [&](Rect &dst) -> void {
        cursor -= side; // 先落盒再扣间距：首个按钮不吃前导间距
        dst = windows ? Rect{ .origin = Point{ .x = cursor, .y = 0.0f },
                              .size = Size{ .width = side, .height = style.height } }
                      : v_centered_square(cursor, style.height, side);
        group_inner_edge = cursor;
        cursor -= gap;
    };
    if (style.show_close) {
        place(g.close);
    }
    if (style.show_maximize && resizable) { // resizable=false 时 maximize 盒为空
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
    const float d = std::clamp(style.height * 0.4f, 11.0f, 14.0f);
    float cursor = 8.0f;           // 左缘外边距 8
    float group_inner_edge = 8.0f; // 无可见按钮时退化为左边距本身
    const auto place = [&](Rect &dst) -> void {
        dst = v_centered_square(cursor, style.height, d);
        cursor += d + 8.0f;               // 直径 + 间距 8
        group_inner_edge = cursor - 8.0f; // 最近一个圆的右缘
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
    const float icon_side = std::min(16.0f, style.height - 20.0f);
    if (icon_side > 0.0f) {
        const float icon_x = mac ? group_inner_edge + 12.0f : 12.0f;
        g.icon = v_centered_square(icon_x, style.height, icon_side);
    }
}

/// @brief 标题文字可用区（垂直全高；窄窗挤压 left>right 时保持空盒）。
auto layout_title(const TitleBarStyle &style, float width, bool mac, float group_inner_edge, TitleBarGeometry &g)
    -> void {
    if (style.center_title) {
        g.title = Rect{ .origin = Point{ .x = 0.0f, .y = 0.0f },
                        .size = Size{ .width = width, .height = style.height } }; // 整宽居中，文字由绘制层水平居中
        return;
    }
    float left = 12.0f;
    if (g.icon.size.width > 0.0f) {
        left = g.icon.right() + 8.0f;
    } else if (mac) {
        left = group_inner_edge + 8.0f;
    }
    const float right = mac ? width - 8.0f : group_inner_edge - 8.0f; // 到按钮组一侧留 8
    if (left <= right) {
        g.title = Rect{ .origin = Point{ .x = left, .y = 0.0f },
                        .size = Size{ .width = right - left, .height = style.height } };
    }
}

} // namespace

auto title_bar_geometry(float width, const TitleBarStyle &style, bool maximized, bool resizable) -> TitleBarGeometry {
    (void)maximized; // 按钮盒尺寸不随最大化变化；最大化⇄还原图标由绘制层据该参数切换字形。

    TitleBarGeometry g;
    if (!(width > 0.0f)) {
        return g; // 退化输入：全空几何（各分区保持默认空盒 Size{0,0}）
    }
    const bool mac = style.button_layout == TitleBarButtonLayout::Mac;

    const float group_inner_edge =
        mac ? layout_left_buttons(style, resizable, g) : layout_right_buttons(style, width, resizable, g);
    layout_icon(style, mac, group_inner_edge, g);
    layout_title(style, width, mac, group_inner_edge, g);

    return g;
}

} // namespace aurora

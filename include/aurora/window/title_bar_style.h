#pragma once

#include "aurora/core/color.h"

namespace aurora {

/// @brief 标题栏按钮视觉语言（参考业界：libadwaita/WinUI/macOS）。
/// 决定窗口控制按钮（关闭/最大化/最小化）贴边侧、顺序与几何形状；
/// 几何解释统一收敛在 `title_bar_geometry()`（绘制与命中测试共用的单一来源）。
enum class TitleBarButtonLayout : std::uint8_t {
    Adwaita,  ///< GNOME/libadwaita：正方形按钮右缘内收 8dp 垂直居中，右→左 close→max→min；悬停高亮为按钮内切圆。
    Windows,  ///< WinUI/Windows 11：整高宽矩形自右上角无缝向左排（无外边距、无间距）；顺序同 Adwaita。
    Mac,  ///< macOS「红绿灯」：圆形按钮左缘外收 8dp 垂直居中，左→右 close→min→max（macOS 惯例顺序）。
};

/// @brief 标题栏度量覆写值（纯数据聚合；**缺省即沿用各视觉语言的推导规则**，故默认零改动）。
///
/// 为什么每个分量都有「0 = 自动」：三种按钮语言各自的盒尺寸、间距、贴边外边距原本是与栏高联动的
/// 推导式（见 `src/aurora/window/title_bar_geometry.cpp` 顶部注释块）。直接把这些式子换成常量会一
/// 次性破坏三套既有预设的观感，故改以 0 表示「我没意见，按语言规则推」，非 0 才是用户显式覆写。
struct TitleBarMetrics {
    float button_size = 0.0F;  ///< 按钮盒边长（逻辑 dp）；0 = 按语言规则由栏高推导
    float button_gap = 0.0F;  ///< 相邻按钮间距（逻辑 dp）；0 = 按语言规则（Adwaita 4 / Windows 0 / Mac 8）
    float button_margin = 0.0F;  ///< 按钮组贴边侧外边距（逻辑 dp）；0 = 按语言规则（Adwaita 8 / Windows 0 / Mac 8）
    float icon_size = 0.0F;  ///< 图标槽边长（逻辑 dp）；0 = 按规则取 min(16, 栏高 − 20)
    float content_padding = 12.0F;  ///< 标题侧安全内边距（逻辑 dp，同时决定图标槽与首枚标题 x）
    float title_font_size_pt = 13.0F;  ///< 标题字号（磅）；沿用绘制层既有默认 13pt
    int title_font_weight = 500;  ///< 标题字重（CSS 100..900 口径）；沿用绘制层既有默认 500
};

/// @brief CSD 自绘标题栏样式值类型（纯数据聚合，可直接 `TitleBarStyle{...}` 指定成员初始化）。
/// 颜色按激活/失焦成对提供，由绘制层依据窗口焦点状态切换（失焦自动变暗）。
/// 默认成员值 = `adwaita_dark()` 预设：`TitleBarStyle{}` 即得一套可直接使用的 GNOME 暗色 header bar。
/// 数值出处：libadwaita/GNOME 暗色 header bar（bg `#303030`/`#242424`、close_hover red3 `#E01B24`）、
/// WinUI/Windows 11 暗色标题栏（bg ≈`#202020`、close_hover `#C42B1C`），个别 alpha 为取整微调。
struct TitleBarStyle {
    float height = 36.0F;  ///< 标题栏高度（逻辑 dp）
    Color bg_active{0x30, 0x30, 0x30};  ///< 激活态背景（GNOME 暗色 header bar `#303030`）
    Color bg_inactive{0x24, 0x24, 0x24};  ///< 失焦背景（自动变暗用；GNOME 暗色 `#242424`）
    Color fg_active{255, 255, 255};  ///< 标题文字/符号前景色（激活）
    Color fg_inactive{0x9A, 0x99, 0x96};  ///< 失焦前景（GTK 暗色 insensitive 前景近似值）
    Color hover_tint{255, 255, 255, 32};  ///< 按钮（非关闭）悬停底色（白色 α≈0.125）
    Color close_hover{0xE0, 0x1B, 0x24};  ///< 关闭钮悬停底色（Adwaita 特征红；GNOME red3 `#E01B24`）
    TitleBarButtonLayout button_layout = TitleBarButtonLayout::Adwaita;  ///< 按钮视觉语言
    bool show_minimize = true;  ///< 是否绘制/命中最小化按钮（false 时几何层返回空盒）
    bool show_maximize = true;  ///< 显式开关；resizable=false 时绘制/命中层仍会自动隐藏
    bool show_close = true;  ///< 是否绘制/命中关闭按钮
    bool show_title = true;  ///< 是否绘制标题文字（不影响几何分区划分）
    bool center_title = false;  ///< true=标题居中（Adwaita/WinUI 风格）；false=左对齐（现状兼容）

    TitleBarMetrics metrics{};  ///< 度量覆写（各分量 0 = 沿用视觉语言的推导规则，非 0 才是显式覆写）
    float corner_radius = 0.0F;  ///< 标题栏背景**下角**圆角半径（逻辑 dp）；0 = 直角（现状兼容）
    Color pressed_tint{255, 255, 255, 56};  ///< 非关闭钮按压底色（白色 α≈0.22，强于悬停的 α32）
    Color close_pressed{0xC0, 0x1B, 0x28};  ///< 关闭钮按压底色（Adwaita red3 按下态，比悬停态更深）

    /// @brief GNOME/libadwaita 暗色 header bar 预设（bg≈`#303030`/`#242424`、fg=白、close_hover=`#E01B24`）。
    /// @return 默认构造的 TitleBarStyle——其默认成员值即本暗色预设。
    [[nodiscard]] static constexpr auto adwaita_dark() noexcept -> TitleBarStyle { return TitleBarStyle{}; }
    /// @brief GNOME/libadwaita 亮色 header bar 预设（bg≈`#EBEBEB`/`#E0E0E0`、fg=黑、close_hover=`#E01B24`）。
    /// @return 亮色配色覆盖后的 TitleBarStyle（按钮布局仍为 Adwaita）。
    [[nodiscard]] static constexpr auto adwaita_light() noexcept -> TitleBarStyle {
        // 亮色预设的可变暂存对象：自默认成员值（暗色预设）起步，下方逐字段覆盖为亮色配色。
        TitleBarStyle s;
        s.bg_active = Color{0xEB, 0xEB, 0xEB};
        s.bg_inactive = Color{0xE0, 0xE0, 0xE0};
        s.fg_active = Color::black();
        s.fg_inactive = Color{0x92, 0x95, 0x95};  // GTK 亮色 insensitive 前景近似值
        s.hover_tint = Color{0, 0, 0, 32};  // 与暗色白 α32 镜像的黑 α≈0.125
        s.close_hover = Color{0xE0, 0x1B, 0x24};
        return s;
    }
    /// @brief WinUI/Windows 11 暗色标题栏预设（bg≈`#202020`、close_hover=`#C42B1C`、Windows 按钮布局）。
    /// @return 覆盖 bg/hover/close_hover 并切换 button_layout 为 Windows 的 TitleBarStyle。
    [[nodiscard]] static constexpr auto windows_dark() noexcept -> TitleBarStyle {
        TitleBarStyle s;
        s.bg_active = Color{0x20, 0x20, 0x20};
        s.bg_inactive = Color{0x19, 0x19, 0x19};
        s.fg_inactive = Color{0xA0, 0xA0, 0xA0};  // WinUI 失焦标题文字灰
        s.hover_tint = Color{255, 255, 255, 26};  // Win11 悬停极淡白罩（微调值）
        s.close_hover = Color{0xC4, 0x2B, 0x1C};  // Windows 关闭钮悬停特征红
        s.button_layout = TitleBarButtonLayout::Windows;
        return s;
    }
};

}  // namespace aurora

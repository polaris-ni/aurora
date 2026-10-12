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

/// @brief CSD 标题栏预设（三套视觉语言 × 明暗两档 = 六套）。
///
/// 经 `TitleBarStyle::from_preset(CsdPreset)` 取值；预设只产 `TitleBarStyle`（颜色 / 布局语言 /
/// 度量基线），显隐开关全部保持开启——显隐与几何细节属调用方的定制面，不属预设。
enum class CsdPreset : std::uint8_t {
    AdwaitaLight,  ///< GNOME/libadwaita 亮色 header bar（bg≈`#EBEBEB`、fg=黑、close_hover=`#E01B24`）
    AdwaitaDark,  ///< GNOME/libadwaita 暗色 header bar（bg≈`#303030`、fg=白、close_hover=`#E01B24`）；亦是
                  ///< `TitleBarStyle{}` 的默认成员值
    WindowsLight,  ///< WinUI/Windows 11 亮色标题栏（bg≈`#F3F3F3`、fg=黑、close_hover=`#C42B1C`）
    WindowsDark,  ///< WinUI/Windows 11 暗色标题栏（bg≈`#202020`、fg=白、close_hover=`#C42B1C`）
    MacLight,  ///< macOS 亮色「红绿灯」标题栏（bg≈`#ECECEC`、fg=黑；红绿灯圆形底色由绘制层按语言固定）
    MacDark,  ///< macOS 暗色「红绿灯」标题栏（bg≈`#323232`、fg=白；同上）
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
/// 默认成员值 = `from_preset(CsdPreset::AdwaitaDark)`：`TitleBarStyle{}` 即得一套可直接使用的 GNOME 暗色 header bar。
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

    /// @brief 按预设取一套标题栏样式。
    /// @param p 预设枚举值（三套视觉语言 × 明暗两档）。
    /// @return 对应预设的 `TitleBarStyle`；`CsdPreset::AdwaitaDark` 与默认构造逐值相同。
    /// @note 预设数值出处（均为业界近似取整）：libadwaita header bar（暗 `#303030`/`#242424`、
    ///     亮 `#EBEBEB`/`#E0E0E0`、close_hover red3 `#E01B24`）；WinUI/Win11（暗 `#202020`/`#191919`、
    ///     亮 `#F3F3F3`、close_hover `#C42B1C`）；macOS（暗 `#323232`、亮 `#ECECEC`）。按压底色取
    ///     「悬停罩加深一档」的近似值；Mac 布局的圆形红绿灯底色由绘制层按语言固定，预设不覆盖。
    [[nodiscard]] static constexpr auto from_preset(CsdPreset p) noexcept -> TitleBarStyle {
        TitleBarStyle s;  // 基准 = AdwaitaDark（默认成员值）
        switch (p) {
            case CsdPreset::AdwaitaDark:
                break;
            case CsdPreset::AdwaitaLight:
                s.bg_active = Color{0xEB, 0xEB, 0xEB};
                s.bg_inactive = Color{0xE0, 0xE0, 0xE0};
                s.fg_active = Color::black();
                s.fg_inactive = Color{0x92, 0x95, 0x95};  // GTK 亮色 insensitive 前景近似值
                s.hover_tint = Color{0, 0, 0, 32};  // 暗色白 α32 的镜像
                s.pressed_tint = Color{0, 0, 0, 56};
                break;
            case CsdPreset::WindowsDark:
                s.bg_active = Color{0x20, 0x20, 0x20};
                s.bg_inactive = Color{0x19, 0x19, 0x19};
                s.fg_inactive = Color{0xA0, 0xA0, 0xA0};  // WinUI 失焦标题文字灰
                s.hover_tint = Color{255, 255, 255, 26};  // Win11 悬停极淡白罩
                s.close_hover = Color{0xC4, 0x2B, 0x1C};  // Windows 关闭钮悬停特征红
                s.pressed_tint = Color{255, 255, 255, 52};
                s.close_pressed = Color{0x99, 0x1D, 0x11};  // 悬停特征红的加深近似
                s.button_layout = TitleBarButtonLayout::Windows;
                break;
            case CsdPreset::WindowsLight:
                s.bg_active = Color{0xF3, 0xF3, 0xF3};
                s.bg_inactive = Color{0xEE, 0xEE, 0xEE};
                s.fg_active = Color::black();
                s.fg_inactive = Color{0x8A, 0x8A, 0x8A};
                s.hover_tint = Color{0, 0, 0, 26};  // 暗色白 α26 的镜像
                s.close_hover = Color{0xC4, 0x2B, 0x1C};
                s.pressed_tint = Color{0, 0, 0, 52};
                s.close_pressed = Color{0x99, 0x1D, 0x11};
                s.button_layout = TitleBarButtonLayout::Windows;
                break;
            case CsdPreset::MacDark:
                s.bg_active = Color{0x32, 0x32, 0x32};
                s.bg_inactive = Color{0x28, 0x28, 0x28};
                s.fg_inactive = Color{0x98, 0x98, 0x98};
                s.hover_tint = Color{255, 255, 255, 32};
                s.close_hover = Color{255, 255, 255, 32};  // macOS 红绿灯不换底色，悬停仅加淡罩
                s.pressed_tint = Color{255, 255, 255, 56};
                s.close_pressed = Color{255, 255, 255, 56};
                s.button_layout = TitleBarButtonLayout::Mac;
                break;
            case CsdPreset::MacLight:
                s.bg_active = Color{0xEC, 0xEC, 0xEC};
                s.bg_inactive = Color{0xE2, 0xE2, 0xE2};
                s.fg_active = Color::black();
                s.fg_inactive = Color{0x8C, 0x8C, 0x8C};
                s.hover_tint = Color{0, 0, 0, 32};
                s.close_hover = Color{0, 0, 0, 32};
                s.pressed_tint = Color{0, 0, 0, 56};
                s.close_pressed = Color{0, 0, 0, 56};
                s.button_layout = TitleBarButtonLayout::Mac;
                break;
        }
        return s;
    }
};

}  // namespace aurora

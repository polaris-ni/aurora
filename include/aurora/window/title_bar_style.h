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
/// `TitleBarStyle{}` 的默认成员值即 `CsdPreset::AdwaitaDark` 一档。
/// 三套语言的**度量口径**分治：Adwaita 两档沿用栏高推导式（按钮盒 `clamp(h-6,26,34)`=30dp、
/// 间距 3dp、贴边外边距 6dp，取 libadwaita `.default-decoration` 的「22dp 内容 + 4dp 外边距」
/// 与 `windowcontrols { border-spacing: 3px }` 口径）；Windows 两档按 Win11 真机钉值（栏高 32dp /
/// 按钮盒 46dp / 图标槽 16dp）；Mac 两档按 macOS 标准窗口钉值（栏高 28dp / 红绿灯直径 12dp /
/// 首枚圆心距左缘 20dp / 标题 13pt 居中）。字形尺寸与度量无关，一律由绘制层按各语言的固定设计
/// 网格绘制（见 `title_bar_painter.cpp`）。
enum class CsdPreset : std::uint8_t {
    AdwaitaLight,  ///< GNOME/libadwaita 亮色 header bar（bg=`#FFFFFF`、fg=`#333334`、close_hover=`#E01B24`）
    AdwaitaDark,  ///< GNOME/libadwaita 暗色 header bar（bg=`#2E2E32`、fg=白、close_hover=`#E01B24`）
    WindowsLight,  ///< Win11 亮色标题栏（bg≈`#F3F3F3`、fg=黑、close_hover=`#C42B1C`；栏高 32dp / 按钮盒 46dp）
    WindowsDark,  ///< WinUI/Windows 11 暗色标题栏（bg≈`#202020`、fg=白、close_hover=`#C42B1C`；栏高与按钮盒同亮色档）
    MacLight,  ///< macOS 亮色「红绿灯」标题栏（bg≈`#ECECEC`、fg=黑；圆形底色由绘制层按语言固定）
    MacDark,  ///< macOS 暗色「红绿灯」标题栏（bg≈`#323232`、fg=白；度量同亮色档）
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
    int title_font_weight =
        700;  ///< 标题字重（CSS 100..900）；默认取 libadwaita `.title` 的 bold（=700），Win / Mac 预设钉 400
};

/// @brief CSD 自绘标题栏样式值类型（纯数据聚合，可直接 `TitleBarStyle{...}` 指定成员初始化）。
/// 颜色按激活/失焦成对提供，由绘制层依据窗口焦点状态切换（失焦自动变暗）。
/// 默认成员值 = `from_preset(CsdPreset::AdwaitaDark)`：`TitleBarStyle{}` 即得一套可直接使用的 GNOME 暗色 header bar。
/// 数值出处：libadwaita 现行 header bar（暗 `#2E2E32`/`#222226`、亮 `#FFFFFF`/`#FAFAFB`、
/// close_hover red3 `#E01B24`）、WinUI/Windows 11 暗色标题栏（bg ≈`#202020`、close_hover
/// `#C42B1C`），个别 alpha 为取整微调。
struct TitleBarStyle {
    float height = 36.0F;  ///< 标题栏高度（逻辑 dp）
    Color bg_active{0x2E, 0x2E, 0x32};  ///< 激活态背景（libadwaita 暗色 header bar `#2E2E32`）
    Color bg_inactive{0x22, 0x22, 0x26};  ///< 失焦背景（自动变暗用；libadwaita 暗色 `#222226`）
    Color fg_active{255, 255, 255};  ///< 标题文字/符号前景色（激活）
    Color fg_inactive{0x91, 0x91, 0x93};  ///< 失焦前景（backdrop 半透明叠加后的实色近似值）
    Color hover_tint{255, 255, 255, 38};  ///< 按钮（非关闭）悬停底色（白色 α≈0.15；libadwaita `$button_hover_color`）
    Color button_tint{255, 255, 255, 26};  ///< 窗口控制圆底的**常态**底色（当前色 α≈0.10）；alpha=0 即常态不画圆底
    Color separator{0x1D, 0x1D, 0x20};  ///< 标题栏底缘分隔线颜色（逻辑 dp 1px）；alpha=0 即不画该线
    Color close_hover{0xE0, 0x1B, 0x24};  ///< 关闭钮悬停底色（Adwaita 特征红；GNOME red3 `#E01B24`）
    TitleBarButtonLayout button_layout = TitleBarButtonLayout::Adwaita;  ///< 按钮视觉语言
    bool show_minimize = true;  ///< 是否绘制/命中最小化按钮（false 时几何层返回空盒）
    bool show_maximize = true;  ///< 显式开关；resizable=false 时绘制/命中层仍会自动隐藏
    bool show_close = true;  ///< 是否绘制/命中关闭按钮
    bool show_title = true;  ///< 是否绘制标题文字（不影响几何分区划分）
    bool center_title = true;  ///< true=标题居中（默认档即 Adwaita / macOS 口径）；false=左对齐（WinUI 口径）

    TitleBarMetrics metrics{};  ///< 度量覆写（各分量 0 = 沿用视觉语言的推导规则，非 0 才是显式覆写）
    float corner_radius = 0.0F;  ///< 标题栏背景**下角**圆角半径（逻辑 dp）；0 = 直角（现状兼容）
    Color pressed_tint{255, 255, 255, 77};  ///< 非关闭钮按压底色（白色 α≈0.30，强于悬停的 α38）
    Color close_pressed{0xC0, 0x1B, 0x28};  ///< 关闭钮按压底色（Adwaita red3 按下态，比悬停态更深）

    /// @brief 按预设取一套标题栏样式。
    /// @param p 预设枚举值（三套视觉语言 × 明暗两档）。
    /// @return 对应预设的 `TitleBarStyle`；`CsdPreset::AdwaitaDark` 与默认构造逐值相同。
    /// @note 预设数值出处（均为业界近似取整）：libadwaita 现行 header bar（暗 `#2E2E32`/`#222226`、
    ///     亮 `#FFFFFF`/`#FAFAFB`、close_hover red3 `#E01B24`、底缘 shade 线 = 12%（亮）/ 36%（暗）
    ///     黑叠加后的实色）；WinUI/Win11（暗 `#202020`/`#191919`、亮 `#F3F3F3`、
    ///     close_hover `#C42B1C`）；macOS（暗 `#323232`、亮 `#ECECEC`）。按压底色取「悬停罩加深一档」
    ///     的近似值；Mac 布局的红绿灯填色 / 描边 / 符号色 / 按下色由绘制层按语言固定，预设不覆盖。
    /// @note `separator` 是标题栏底缘那条 1px 分隔线（libadwaita `headerbar` 的
    ///     `box-shadow: inset 0 -1px var(--headerbar-shade-color)`）。只有 Adwaita 两档有：
    ///     Win11 与 macOS 的标题栏底缘均无此线，故四档预设一律置全透明。alpha=0 即不画该线。
    /// @note `button_tint` 是窗口控制圆底的**常态**底色——libadwaita 的圆底画在 `> image` 上且
    ///     常态就有（`background-color: $button_color` = currentColor 10%），并非只在悬停时出现；
    ///     悬停 15% / 按压 30% 仍走 `hover_tint` / `pressed_tint`。Windows 与 Mac 两档常态无圆底，
    ///     故置全透明。
    /// @note Windows 两档的**度量**按真机 DWM 标题栏实测口径给出（非按栏高推导）：栏高 32dp、
    ///     按钮盒 46dp、图标槽 16dp、标题 9pt / 字重 400；字形本身由绘制层按 10×10dp 设计网格
    ///     绘制（见 `title_bar_painter.cpp`），故按钮盒再宽也不放大字形——这是 Win11 的实际行为。
    ///     非关闭钮的悬停 / 按压遮罩取 WinUI 标准口径（α≈0.0605 / α≈0.1）。
    /// @note Mac 两档的**度量**同上按 macOS 标准窗口口径钉值（栏高 28dp / 红绿灯 12dp / 圆心距
    ///     20dp / 首枚圆心距左缘 20dp / 标题 13pt-Regular 且居中）；两条推导式（直径
    ///     `clamp(h*0.4,11,14)`、外边距 8）在栏高 36 时分别给出 14 / 8，与真机差 17% / 43%，
    ///     故必须显式钉住而不能沿用。Adwaita 两档**不钉度量**：其推导式 `clamp(h-6,26,34)` 在栏高
    ///     36 时给 30dp，正是 libadwaita `.default-decoration` 的「22dp 内容 + 4dp 外边距」，间距
    ///     3dp 出自 `windowcontrols { border-spacing: 3px }`——钉值反而会破坏「`TitleBarStyle{}`
    ///     默认成员值即 AdwaitaDark」这条契约。
    /// @note 三套语言的**字形**一律由绘制层按固定设计网格绘制，与按钮盒尺寸解耦（Windows
    ///     10×10dp / Adwaita 16×16dp / macOS 6pt），详见 `title_bar_painter.cpp`。
    /// @note **标题对齐**（两个方向各自成规）：水平方向 Adwaita / Mac 两档居中（libadwaita
    ///     `AdwHeaderBar` 与 macOS 窗口标题均居中），Windows 两档左对齐（WinUI 即左对齐）——
    ///     默认成员值 true 就是 Adwaita 档的口径，故只有 Windows 两档显式置 false。垂直方向
    ///     三档一律居中：绘制层按 `FontEngine::measure_height` 把行盒摆到栏高的中点
    ///     （`draw_text` 的 y 是**行盒顶**，把标题带原样传下去会让文字贴栏的上缘）。
    [[nodiscard]] static constexpr auto from_preset(CsdPreset p) noexcept -> TitleBarStyle {
        TitleBarStyle s;  // 基准 = AdwaitaDark（默认成员值）
        switch (p) {
            case CsdPreset::AdwaitaDark:
                break;
            case CsdPreset::AdwaitaLight:
                s.bg_active = Color{0xFF, 0xFF, 0xFF};  // libadwaita `--headerbar-bg-color`
                s.bg_inactive = Color{0xFA, 0xFA, 0xFB};  // `--headerbar-backdrop-color` = window_bg
                s.fg_active = Color{0x33, 0x33, 0x34};  // `--headerbar-fg-color` = RGB(0 0 6 / 80%) 叠白
                s.fg_inactive = Color{0x7D, 0x7D, 0x80};  // backdrop 半透明叠加后的实色近似值
                s.hover_tint = Color{0, 0, 0, 38};  // `$button_hover_color` = currentColor 15%
                s.button_tint = Color{0, 0, 0, 26};  // `$button_color` = currentColor 10%
                s.pressed_tint = Color{0, 0, 0, 77};  // `$button_active_color` = currentColor 30%
                // `--headerbar-shade-color` = RGB(0 0 6 / 12%) 叠在白上的实色。
                s.separator = Color{0xE0, 0xE0, 0xE0};
                break;
            case CsdPreset::WindowsDark:
                s.height = 32.0F;  // Win11 标题栏高（真机实测口径，非 Adwaita 基准 36）
                s.bg_active = Color{0x20, 0x20, 0x20};
                s.bg_inactive = Color{0x19, 0x19, 0x19};
                s.fg_inactive = Color{0xA0, 0xA0, 0xA0};  // WinUI 失焦标题文字灰
                s.hover_tint = Color{255, 255, 255, 15};  // WinUI 悬停遮罩（白 α≈0.0605）
                s.button_tint = Color::transparent();  // Win11 标题栏按钮常态无底色
                s.separator = Color::transparent();  // Win11 标题栏底缘无分隔线
                s.close_hover = Color{0xC4, 0x2B, 0x1C};  // Windows 关闭钮悬停特征红
                s.pressed_tint = Color{255, 255, 255, 25};  // WinUI 按压遮罩（白 α≈0.1）
                s.close_pressed = Color{0x99, 0x1D, 0x11};  // 悬停特征红的加深近似
                s.button_layout = TitleBarButtonLayout::Windows;
                s.center_title = false;  // WinUI 标题左对齐（默认 true 是 Adwaita/Mac 口径）
                s.metrics.button_size = 46.0F;  // 真机 caption 按钮盒宽
                s.metrics.icon_size = 16.0F;  // 图标槽：显式钉住（否则栏高 32 时推导退化为 12）
                s.metrics.title_font_size_pt = 9.0F;  // 原生标题 12px
                s.metrics.title_font_weight = 400;  // 原生 Regular
                break;
            case CsdPreset::WindowsLight:
                s.height = 32.0F;
                s.bg_active = Color{0xF3, 0xF3, 0xF3};
                s.bg_inactive = Color{0xEE, 0xEE, 0xEE};
                s.fg_active = Color::black();
                s.fg_inactive = Color{0x8A, 0x8A, 0x8A};
                s.hover_tint = Color{0, 0, 0, 15};  // 暗色白 α15 的镜像
                s.button_tint = Color::transparent();
                s.separator = Color::transparent();
                s.close_hover = Color{0xC4, 0x2B, 0x1C};
                s.pressed_tint = Color{0, 0, 0, 25};
                s.close_pressed = Color{0x99, 0x1D, 0x11};
                s.button_layout = TitleBarButtonLayout::Windows;
                s.center_title = false;
                s.metrics.button_size = 46.0F;
                s.metrics.icon_size = 16.0F;
                s.metrics.title_font_size_pt = 9.0F;
                s.metrics.title_font_weight = 400;
                break;
            case CsdPreset::MacDark:
                // macOS 标准窗口（无 toolbar）标题栏几何：栏高 28pt、红绿灯直径 12pt、相邻圆心
                // 距 20pt（即边缘间距 8pt）、首枚圆心距窗口左缘 20pt（即左外边距 14pt）。
                s.height = 28.0F;
                s.bg_active = Color{0x32, 0x32, 0x32};
                s.bg_inactive = Color{0x28, 0x28, 0x28};
                s.fg_inactive = Color{0x98, 0x98, 0x98};
                // macOS 红绿灯的底色与符号色由绘制层按语言固定（三色填色 + 描边 + 按下加深），
                // 预设的 tint / hover / pressed 三支不适用于该语言，一律置透明表示「不参与」。
                s.hover_tint = Color::transparent();
                s.button_tint = Color::transparent();
                s.separator = Color::transparent();  // macOS 标题栏底缘无分隔线
                s.close_hover = Color::transparent();
                s.pressed_tint = Color::transparent();
                s.close_pressed = Color::transparent();
                s.button_layout = TitleBarButtonLayout::Mac;
                s.center_title = true;  // macOS 窗口标题居中
                s.metrics.button_size = 12.0F;  // 红绿灯直径
                s.metrics.button_gap = 8.0F;  // 圆心距 20 − 直径 12
                s.metrics.button_margin = 14.0F;  // 圆心距左缘 20 − 半径 6
                s.metrics.icon_size = 16.0F;  // 同上：栏高 28 时推导式会退化，须钉住
                s.metrics.title_font_size_pt = 13.0F;  // NSWindow 标题字号
                s.metrics.title_font_weight = 400;  // SF 系标题为 Regular
                break;
            case CsdPreset::MacLight:
                s.height = 28.0F;
                s.bg_active = Color{0xEC, 0xEC, 0xEC};
                s.bg_inactive = Color{0xE2, 0xE2, 0xE2};
                s.fg_active = Color::black();
                s.fg_inactive = Color{0x8C, 0x8C, 0x8C};
                s.hover_tint = Color::transparent();
                s.button_tint = Color::transparent();
                s.separator = Color::transparent();
                s.close_hover = Color::transparent();
                s.pressed_tint = Color::transparent();
                s.close_pressed = Color::transparent();
                s.button_layout = TitleBarButtonLayout::Mac;
                s.center_title = true;
                s.metrics.button_size = 12.0F;
                s.metrics.button_gap = 8.0F;
                s.metrics.button_margin = 14.0F;
                s.metrics.icon_size = 16.0F;
                s.metrics.title_font_size_pt = 13.0F;
                s.metrics.title_font_weight = 400;
                break;
        }
        return s;
    }
};

}  // namespace aurora

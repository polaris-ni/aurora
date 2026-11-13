// ============================================================================
// theming/theme_presets.cpp - 「风格 × 明暗」两轴的标定预设表
// ----------------------------------------------------------------------------
// 本文件是 `Theme::from_style` 的实现体，也是全库唯一存放跨平台观感标定值的地方。
//
// 取值纪律（与 specification/07-environment-modifier.md 的口径一致）：
//   1. **只覆盖有出处的角色与度量**，其余逐字段沿用 Material 基线（`ColorRoles::light()` /
//      `dark()` 与四组令牌的声明默认值）。没有出处就不写值——臆造的数值一旦进表，下游再也
//      分不清它是标定还是猜测。
//   2. 每个覆写值就地带分级：`官方` = 上游一手（仓库源文件里的字面量或厂商规范表里的数）；
//      `实测` = 第三方真机 dump；`派生` = 由官方公式合成、非官方直接给出的数；`未取证` = 本轮
//      没拿到一手出处，因此**不写值**（只在注释里留名，供下一轮取证）。
//   3. 半透明层色（Fluent 的 `#E4000000` 一类、Adwaita 的 `color-mix`）在静态预设里无法保留
//      「叠加于任意底色」的语义，一律按其官方公式合成到该档的页面底色上，落成不透明近似，
//      并把算式随值写明——这类值分级为 `派生`。
//   4. 上游自身的强调色配对不达 WCAG AA 4.5:1 时（Breeze / Aqua 确有其事），照抄上游配对并
//      就地记豁免与实测比值；上游调色板里有达标档的（Adwaita 有 blue_4、Fluent 有变体阶梯），
//      取达标档而不是豁免。
//
// 引用口径：上游路径写「仓库 + 仓内相对路径」，不写行号，也不写本机绝对路径；分级为 `实测`
// 的第三方真机 dump 与其可达性说明，随对应档位段落就地写明。
// ============================================================================

#include <array>
#include <string_view>

#include "aurora/core/color.h"
#include "aurora/theming/color_roles.h"
#include "aurora/theming/color_scheme.h"
#include "aurora/theming/csd_roles.h"
#include "aurora/theming/layout_conventions.h"
#include "aurora/theming/theme.h"
#include "aurora/theming/theme_style.h"
#include "aurora/theming/type_scale.h"
#include "aurora/window/title_bar_style.h"

namespace aurora {
namespace {

// Material 档正文基准（`TypeScale::body_medium` 的声明默认值），字阶重锚的比值分母。
constexpr float AURORA_MATERIAL_BODY_PT = 14.0F;

// 1pt = 4/3px 的逆向换算：上游给的像素基准折回本库的磅值口径。
constexpr auto points_from_pixels(float px) noexcept -> float { return px * 0.75F; }

// 字阶十五档的成员指针清单：**整族变换的唯一遍历面**（风格档重锚与字号缩放共用一份）。
//
// 为什么需要这张表而不是逐档手写：`TypeScale` 的两处整族变换（`retarget_type_scale` 换基准、
// `with_accessibility` 按 `font_scale` 增长）各写一遍十五档就是两份名单，加一档字阶时漏改其中
// 一处不会编译红，只会让那一档悄悄不受缩放——正是本批要消灭的「绕过 `TypeScale` 的通道」。
// 本组**不是**令牌登记表（`TypeScale` 的值是 `Font` 而非标量档，不满足属性回落点的同型要求，
// 故不进 `theme_tokens`，理由见 specification/07-environment-modifier.md §5.1），因此没有门禁
// 拿它比对字段；穷尽性改由下面那条 `sizeof` 守卫机检——`TypeScale` 全字段皆 `Font` 且无 padding，
// 清单少列一档时乘积即不等于结构体大小，当场编译红。
constexpr std::array<Font TypeScale::*, 15> AURORA_TYPE_SCALE_SLOTS = {
    &TypeScale::display_large,   &TypeScale::display_medium, &TypeScale::display_small, &TypeScale::headline_large,
    &TypeScale::headline_medium, &TypeScale::headline_small, &TypeScale::title_large,   &TypeScale::title_medium,
    &TypeScale::title_small,     &TypeScale::body_large,     &TypeScale::body_medium,   &TypeScale::body_small,
    &TypeScale::label_large,     &TypeScale::label_medium,   &TypeScale::label_small};

static_assert(AURORA_TYPE_SCALE_SLOTS.size() * sizeof(Font) == sizeof(TypeScale),
              "AURORA_TYPE_SCALE_SLOTS must list every TypeScale tier: a missing tier stops scaling silently");

// 字阶重锚：族名整组替换，字号按「平台正文基准 / Material 正文基准」等比缩放。
//
// 为什么是等比缩放而不是照抄上游的 15 档字阶表：四套语言各自的字阶名与本库的 Material
// 语义名（display / headline / title / body / label）不是一一对应，且本轮只取到各平台的
// **正文基准**一手值（Windows 14px、macOS 13pt、GNOME 11pt、KDE 10pt），上游完整阶梯要么
// 不存在（Adwaita 官方只给族名不给尺寸）要么与本库档位名不可映射。等比缩放保留本库字阶
// 形状、只换基准，分级为 `派生`。
auto retarget_type_scale(std::string_view family, float body_pt) -> TypeScale {
    TypeScale scale;
    const float factor = body_pt / AURORA_MATERIAL_BODY_PT;
    for (Font TypeScale::*slot : AURORA_TYPE_SCALE_SLOTS) {
        Font &font = scale.*slot;
        font.family = std::string(family);
        font.size_pt = font.size_pt * factor;
    }
    return scale;
}

// 标题栏预设 → CSD 配色角色组：八支配色逐字段搬运，投影色不在预设面内故保留基线。
//
// 这条通道让「风格档的 CSD 配色」与「`TitleBarStyle::from_preset` 的真机标定值」只有唯一
// 一份数——两处的既有契约（`CsdRoles::light()` ≡ `AdwaitaLight`、`dark()` ≡ `AdwaitaDark`）
// 因此由构造保证而非重复声明。
constexpr auto csd_from_preset(CsdPreset preset) noexcept -> CsdRoles {
    const TitleBarStyle bar = TitleBarStyle::from_preset(preset);
    CsdRoles roles = CsdRoles::light();
    roles.title_bar_bg_active = bar.bg_active;
    roles.title_bar_bg_inactive = bar.bg_inactive;
    roles.title_bar_fg_active = bar.fg_active;
    roles.title_bar_fg_inactive = bar.fg_inactive;
    roles.title_bar_hover_tint = bar.hover_tint;
    roles.title_bar_pressed_tint = bar.pressed_tint;
    roles.title_bar_close_hover = bar.close_hover;
    return roles;
}

// ---------------------------------------------------------------------------
// Windows / Fluent（Windows 11）
// ---------------------------------------------------------------------------
// 中性面取自 microsoft/microsoft-ui-xaml 的 controls/dev/CommonStyles/
// Common_themeresources_any.xaml，Light 块与 Default(=Dark) 块的同名键：
// `SolidBackgroundFillColorBase`、`TextFillColorPrimary`、`TextFillColorSecondary`、
// `ControlStrokeColorDefault`、`CardBackgroundFillColorDefault`。其中四支是带 alpha 的层色，
// 按纪律第 3 条合成到本档页面底色（`#F3F3F3` / `#202020`）后落成不透明近似。
// 强调色：上游只做 `{ThemeResource SystemAccentColor}` 引用（该键由用户选定），仓内唯一的
// 字面量是 dxaml/xcp/components/theminginterop/SystemThemingInterop.cpp 里测试覆写路径的
// 变体阶梯（Dark1/Dark2/Dark3/Light1/Light2/Light3）。基准色 `#0078D4` 未取到一手出处，
// 故本档取该官方阶梯中的两档作强调色对，阶梯取值官方、到角色的对应由本文件裁决。
constexpr auto windows_roles(bool dark) noexcept -> ColorRoles {
    ColorRoles roles = dark ? ColorRoles::dark() : ColorRoles::light();
    if (dark) {
        roles.primary = Color{0x76, 0xB9, 0xED};  // 官方 SystemAccentColorLight2
        roles.on_primary = Color{0x00, 0x42, 0x75};  // 官方 SystemAccentColorDark2（4.88:1）
        roles.primary_container = Color{0x00, 0x42, 0x75};  // 官方 SystemAccentColorDark2
        roles.on_primary_container = Color{0xA6, 0xD8, 0xFF};  // 官方 SystemAccentColorLight3（6.82:1）
        roles.surface = Color{0x20, 0x20, 0x20};  // 官方 SolidBackgroundFillColorBase
        roles.surface_container = Color{0x2B, 0x2B, 0x2B};  // 派生 0DFFFFFF 叠 #202020
        roles.on_surface = Color{0xFF, 0xFF, 0xFF};  // 官方 TextFillColorPrimary
        roles.on_surface_variant = Color{0xCC, 0xCC, 0xCC};  // 派生 C5FFFFFF 叠 #202020
        roles.outline_variant = Color{0x30, 0x30, 0x30};  // 派生 12FFFFFF 叠 #202020
        roles.error = Color{0xC4, 0x2B, 0x1C};  // 实测 复用标题栏关闭钮悬停红（5.66:1）
        roles.focus_ring = Color{0xFF, 0xFF, 0xFF};  // 官方 TextFillColorPrimary(Dark)
    } else {
        roles.primary = Color{0x00, 0x5A, 0x9E};  // 官方 SystemAccentColorDark1（配白 7.10:1）
        roles.on_primary = Color{0xFF, 0xFF, 0xFF};  // 官方 强调色上的前景取白
        roles.primary_container = Color{0xA6, 0xD8, 0xFF};  // 官方 SystemAccentColorLight3
        roles.on_primary_container = Color{0x00, 0x26, 0x42};  // 官方 SystemAccentColorDark3（10.24:1）
        roles.surface = Color{0xF3, 0xF3, 0xF3};  // 官方 SolidBackgroundFillColorBase
        roles.surface_container = Color{0xFB, 0xFB, 0xFB};  // 派生 B3FFFFFF 叠 #F3F3F3
        roles.on_surface = Color{0x1A, 0x1A, 0x1A};  // 派生 E4000000 叠 #F3F3F3
        roles.on_surface_variant = Color{0x5C, 0x5C, 0x5C};  // 派生 9E000000 叠 #F3F3F3
        roles.outline_variant = Color{0xE5, 0xE5, 0xE5};  // 派生 0F000000 叠 #F3F3F3
        roles.error = Color{0xC4, 0x2B, 0x1C};  // 实测 同上
        roles.focus_ring = Color{0x1A, 0x1A, 0x1A};  // 派生 外环 E4000000 叠页面底
    }
    return roles;
}

// Fluent 焦点视觉是双环（外环 E4000000 / 内环 B3FFFFFF，Light 档；Dark 档白 / B3000000，
// 出处同上仓 Focus_solid_themeresources 一族），本库的 `focus_ring` 只有一支色位、
// `FocusRingStyle` 也只有 Outline / OuterGlow / None 三档，故本批只落外环。内层对比环属
// 计划/待建面（要它就得给 `FocusRingStyle` 加一档，是公共 API 增量，不在本批）。
auto apply_windows(Theme &t, bool dark) -> void {
    t.roles = windows_roles(dark);
    t.csd = csd_from_preset(dark ? CsdPreset::WindowsDark : CsdPreset::WindowsLight);
    t.type_scale = retarget_type_scale("Segoe UI Variable Text", points_from_pixels(14.0F));
    t.shapes.small = 4.0F;  // 官方 ControlCornerRadius（Common_themeresources 的 CornerRadius 族）
    t.shapes.medium = 8.0F;  // 官方 OverlayCornerRadius
    t.controls.control_height = 32.0F;  // 派生 Body 行高 20px + ButtonPadding 垂直 5+6 + 1px 描边
    // conventions 不覆写：对话框接受在前、标题左对齐、窗口内菜单栏、经典滚动条、描边焦点环
    // 五档与 Material 默认逐值相同，写出来反而制造「已按 Fluent 单独标定」的错觉。
    // 未取证：控件最小高（仓内 Button 无 MinHeight setter）、滚动条宽、菜单行高、
    // 悬停/按压状态层的 SubtleFillColor 族、`SystemAccentColor` 基准字面量。
}

// ---------------------------------------------------------------------------
// macOS / Aqua
// ---------------------------------------------------------------------------
// 本档全部中性面分级为 `实测`（第三方真机 dump：Monterey 12.0.1 / Big Sur 11.2.3 的
// windowBackground / controlBackground / label / separator 读数）与 `派生`（label 与
// separator 是带 alpha 的系统色，按纪律第 3 条合成到本档页面底色）。Apple HIG 明写
// 「勿硬编码系统色值、数值随版本浮动」，故整档标注 **待复测**：真机 macOS 26 重测后须逐值
// 校准，未重测前不得当作 Apple 的承诺值使用。
// 强调色两处冲突：HIG 的 Blue 行给 (0,136,255)，AppKit 实测 controlAccentColor 给
// (0,122,255)。取实测值——它是用户机器上真正出现的填充色。
constexpr auto macos_roles(bool dark) noexcept -> ColorRoles {
    ColorRoles roles = dark ? ColorRoles::dark() : ColorRoles::light();
    if (dark) {
        roles.primary = Color{0x00, 0x7A, 0xFF};  // 实测 controlAccentColor（待复测）
        roles.on_primary = Color{0xFF, 0xFF, 0xFF};  // 实测 系统强调色配对前景（豁免见下）
        roles.surface = Color{0x32, 0x32, 0x32};  // 实测 windowBackground
        roles.surface_container = Color{0x1E, 0x1E, 0x1E};  // 实测 controlBackground
        roles.on_surface = Color{0xE0, 0xE0, 0xE0};  // 派生 label(白色 a.847) 叠 #323232（9.71:1）
        roles.outline_variant = Color{0x46, 0x46, 0x46};  // 派生 separator(a.098) 叠 #323232
    } else {
        roles.primary = Color{0x00, 0x7A, 0xFF};  // 实测 同上（HIG Blue 行为 (0,136,255)，冲突取实测）
        roles.on_primary = Color{0xFF, 0xFF, 0xFF};  // 实测 同上
        roles.surface = Color{0xEC, 0xEC, 0xEC};  // 实测 windowBackground
        roles.surface_container = Color{0xFF, 0xFF, 0xFF};  // 实测 controlBackground
        roles.on_surface = Color{0x24, 0x24, 0x24};  // 派生 label(黑色 a.847) 叠 #ECECEC（13.14:1）
        roles.outline_variant = Color{0xD5, 0xD5, 0xD5};  // 派生 separator(a.098) 叠 #ECECEC
    }
    // 豁免（不变量：每对 on_* 与其底色 ≥ WCAG AA 4.5:1）：#007AFF 配白实测 4.02:1，达
    // AA Large（3:1）未达普通文本线。不取更深的蓝是因为 Apple 没有给——HIG 的 Blue 只有
    // 一行，且真机 controlAccentColor 就是这个值；改成自造深蓝等于把「Aqua 档」换成猜测。
    return roles;
}

auto apply_macos(Theme &t, bool dark) -> void {
    t.roles = macos_roles(dark);
    t.csd = csd_from_preset(dark ? CsdPreset::MacDark : CsdPreset::MacLight);
    t.type_scale = retarget_type_scale("SF Pro Text", 13.0F);  // 官方 HIG 平台默认正文 13pt
    // conventions 取 macOS 三大惯例分叉：动作钮竖排右对齐且接受在上（Qt MacLayout 的
    // 垂直序 Yes/Accept 先于 No/Reject 佐证）、菜单栏优先系统原生、覆盖式滚动条、外扩发光焦点环。
    t.conventions.dialog_button_order = DialogButtonOrder::VerticalRightAligned;
    t.conventions.menu_bar_placement = MenuBarPlacement::NativePreferred;
    t.conventions.scrollbar_mode = ScrollbarMode::Overlay;
    t.conventions.focus_ring_style = FocusRingStyle::OuterGlow;
    // 未取证：控件基准高（候选 28dp 无一手出处）、圆角档位、系统红/绿的 HIG 字面量、
    // secondaryLabel 与 tertiaryLabel 的 alpha。全部留 Material 基线，待 macOS 26 复测批补。
}

// ---------------------------------------------------------------------------
// GNOME / Adwaita
// ---------------------------------------------------------------------------
// 全部取自 gitlab.gnome.org/GNOME/libadwaita（本机经其 GitHub 镜像核对）的
// src/stylesheet/_colors.scss、_palette.scss、_common.scss、widgets/_buttons.scss。
// 强调色：Adwaita 的 blue_3 `#3584E4` 是默认 accent_bg_color，但配官方前景白实测只有
// 3.77:1，不达本库不变量。同一官方调色板里的 blue_4 `#1C71D8` 配白 4.77:1 达标，且仍是
// Adwaita 蓝，故取 blue_4——这是「改取值」而不是「记豁免」，两档明暗同值。
constexpr auto gnome_roles(bool dark) noexcept -> ColorRoles {
    ColorRoles roles = dark ? ColorRoles::dark() : ColorRoles::light();
    roles.primary = Color{0x1C, 0x71, 0xD8};  // 官方 blue_4（blue_3 配白 3.77:1 不达，改取本档）
    if (dark) {
        roles.on_primary = Color{0xFF, 0xFF, 0xFF};  // 官方 accent_fg_color（4.77:1）
        roles.primary_container = Color{0x1A, 0x5F, 0xB4};  // 官方 blue_5
        roles.on_primary_container = Color{0xFF, 0xFF, 0xFF};
        roles.surface = Color{0x22, 0x22, 0x26};  // 官方 window_bg_color
        roles.surface_dim = Color{0x1D, 0x1D, 0x20};  // 官方 view_bg_color（本档最深的面）
        roles.surface_bright = Color{0x36, 0x36, 0x3A};  // 官方 dialog_bg_color / popover_bg_color
        roles.surface_container_lowest = Color{0x1D, 0x1D, 0x20};  // 官方 view_bg_color
        roles.surface_container_low = Color{0x22, 0x22, 0x26};  // 官方 window_bg_color
        roles.surface_container = Color{0x28, 0x28, 0x2C};  // 官方 secondary_sidebar_backdrop_color
        roles.surface_container_high = Color{0x2E, 0x2E, 0x32};  // 官方 sidebar_bg_color / headerbar_bg_color
        roles.surface_container_highest = Color{0x36, 0x36, 0x3A};  // 官方 dialog_bg_color / popover_bg_color
        roles.on_surface = Color{0xFF, 0xFF, 0xFF};  // 官方 window_fg_color(dark)
        roles.outline_variant = Color{0x4D, 0x4D, 0x51};  // 派生 15% 白（--border-opacity）叠 headerbar_bg_color
        roles.error = Color{0xC0, 0x1C, 0x28};  // 官方 red_4 = 暗档 destructive_bg_color（6.11:1）
        roles.focus_ring = Color{0x1C, 0x71, 0xD8, 0x80};  // 派生 accent-color 的 focus_border_opacity 50%
        roles.hover = Color{255, 255, 255, 18};  // 派生 $hover_color = currentColor 7%
        roles.pressed = Color{255, 255, 255, 41};  // 派生 $active_color = currentColor 16%
    } else {
        roles.on_primary = Color{0xFF, 0xFF, 0xFF};  // 官方 accent_fg_color
        roles.primary_container = Color{0x99, 0xC1, 0xF1};  // 官方 blue_1
        roles.on_primary_container = Color{0x1F, 0x27, 0x35};  // 派生 window_fg 叠 blue_1（8.04:1）
        roles.surface = Color{0xFA, 0xFA, 0xFB};  // 官方 window_bg_color
        roles.surface_dim = Color{0xEB, 0xEB, 0xED};  // 官方 sidebar_bg_color（本档最深的面）
        roles.surface_bright = Color{0xFF, 0xFF, 0xFF};  // 官方 view_bg_color / card_bg_color
        roles.surface_container_lowest = Color{0xFF, 0xFF, 0xFF};  // 官方 view_bg_color / card_bg_color
        roles.surface_container_low = Color{0xFA, 0xFA, 0xFB};  // 官方 window_bg_color / dialog_bg_color
        roles.surface_container = Color{0xF3, 0xF3, 0xF5};  // 官方 secondary_sidebar_bg_color
        roles.surface_container_high = Color{0xEB, 0xEB, 0xED};  // 官方 sidebar_bg_color
        // surface_container_highest 留基线：Adwaita 亮档只有四层中性面，第五档无出处。
        roles.on_surface = Color{0x32, 0x32, 0x37};  // 派生 window_fg RGB(0 0 6 / 80%) 叠 window_bg
        roles.outline_variant = Color{0xE0, 0xE0, 0xE1};  // 派生 15%（--border-opacity）叠 view_bg_color
        roles.error = Color{0xE0, 0x1B, 0x24};  // 官方 red_3 = 亮档 destructive_bg_color（4.83:1）
        roles.focus_ring = Color{0x1C, 0x71, 0xD8, 0x80};  // 派生 同暗档
        roles.hover = Color{0x32, 0x32, 0x37, 18};  // 派生 currentColor 7%
        roles.pressed = Color{0x32, 0x32, 0x37, 41};  // 派生 currentColor 16%
    }
    return roles;
}

auto apply_gnome(Theme &t, bool dark) -> void {
    t.roles = gnome_roles(dark);
    // Gnome 档的 CSD 基座就是 Adwaita 两档，本行取值与不覆写逐值相同；写出来是为了让
    // 「四套风格都走同一条 csd 通道」这件事在本文件里可见（不变量由 utest_theme_presets 钉）。
    t.csd = csd_from_preset(dark ? CsdPreset::AdwaitaDark : CsdPreset::AdwaitaLight);
    // 官方 gsettings-desktop-schemas 的 font-name 默认值：GNOME 48 起为 'Adwaita Sans 11'，
    // 47.x 及以前是 'Cantarell 11'。静态档取现行名；族名是偏好而非保证，缺字体时按
    // core/font.h 的既有回退语义降级，不报错。
    t.type_scale = retarget_type_scale("Adwaita Sans", 11.0F);
    t.shapes.small = 9.0F;  // 官方 $button_radius
    t.shapes.large = 12.0F;  // 官方 $card_radius
    t.shapes.extra_large = 15.0F;  // 派生 $dialog_radius = $button_radius + 6
    t.controls.control_height = 34.0F;  // 派生 buttons.scss 的 min-height 24 + 垂直 padding 5×2
    t.conventions.dialog_button_order = DialogButtonOrder::AcceptLast;  // 官方 Adwaita 对话框排布：[取消] 左、[确认] 右
    t.conventions.scrollbar_mode = ScrollbarMode::Overlay;
    // 未取证：正文 11pt 之外的字阶尺寸（Adwaita 官方不给字号）、`on_surface_variant` 的
    // 独立档（Adwaita 只有 window_fg 一支）、outline 主档（描边只给 --border-opacity 公式）。
}

// ---------------------------------------------------------------------------
// KDE / Plasma 6 Breeze
// ---------------------------------------------------------------------------
// 中性面与强调色取自 KDE/breeze 的 colors/BreezeLight.colors 与 BreezeDark.colors
// （`[Colors:Window|View|Button|Header|Tooltip|Selection|Complementary]` 各段）。
// 版本陷阱：暗档中性面在 Plasma 6.4 前后整体改暗（v6.3.0 是 Window 42,46,50 / View 27,30,32 /
// Button 49,54,59），本表取 master 与 v6.8.2 的现行值，勿按 v6.3.0 复核。
// 强调色 #3DAEE9 明暗同值（两档各 24 处 61,174,233），同时推翻「Plasma 6 已换默认强调色」的传闻。
constexpr auto kde_roles(bool dark) noexcept -> ColorRoles {
    ColorRoles roles = dark ? ColorRoles::dark() : ColorRoles::light();
    roles.primary = Color{0x3D, 0xAE, 0xE9};  // 官方 ForegroundActive / Selection BackgroundNormal
    if (dark) {
        roles.on_primary = Color{0xFC, 0xFC, 0xFC};  // 官方 Selection ForegroundNormal(dark)
        roles.primary_container = Color{0x1E, 0x57, 0x74};  // 官方 Selection BackgroundAlternate(dark)
        roles.on_primary_container = Color{0xFC, 0xFC, 0xFC};  // 7.66:1
        roles.surface = Color{0x20, 0x23, 0x26};  // 官方 Window BackgroundNormal
        roles.surface_container_lowest = Color{0x14, 0x16, 0x18};  // 官方 View BackgroundNormal
        roles.surface_container_low = Color{0x1D, 0x1F, 0x22};  // 官方 View BackgroundAlternate
        roles.surface_container = Color{0x20, 0x23, 0x26};  // 官方 Window BackgroundNormal
        roles.surface_container_high = Color{0x29, 0x2C, 0x30};  // 官方 Button / Header / Tooltip BackgroundNormal
        roles.surface_container_highest = Color{0x2A, 0x2E, 0x32};  // 官方 Complementary BackgroundNormal
        roles.on_surface = Color{0xFC, 0xFC, 0xFC};  // 官方 ForegroundNormal
        roles.on_surface_variant = Color{0xA1, 0xA9, 0xB1};  // 官方 ForegroundInactive（底色上 5.89:1 起）
        roles.error = Color{0xDA, 0x44, 0x53};  // 官方 ForegroundNegative（明暗同值）
        roles.focus_ring = Color{0x3D, 0xAE, 0xE9};  // 官方 DecorationFocus
    } else {
        roles.on_primary = Color{0xFF, 0xFF, 0xFF};  // 官方 Selection ForegroundNormal
        roles.primary_container = Color{0xA3, 0xD4, 0xFA};  // 官方 Selection BackgroundAlternate
        roles.on_primary_container = Color{0x23, 0x26, 0x29};  // 官方 ForegroundNormal（9.67:1）
        roles.surface = Color{0xEF, 0xF0, 0xF1};  // 官方 Window BackgroundNormal
        roles.surface_container_lowest = Color{0xFF, 0xFF, 0xFF};  // 官方 View BackgroundNormal
        roles.surface_container_low = Color{0xFC, 0xFC, 0xFC};  // 官方 Button BackgroundNormal
        roles.surface_container = Color{0xF7, 0xF7, 0xF7};  // 官方 Tooltip / View BackgroundAlternate
        roles.surface_container_high = Color{0xEF, 0xF0, 0xF1};  // 官方 Window BackgroundNormal
        roles.surface_container_highest = Color{0xDE, 0xE0, 0xE2};  // 官方 Header BackgroundNormal
        roles.on_surface = Color{0x23, 0x26, 0x29};  // 官方 ForegroundNormal
        roles.on_surface_variant = Color{0x70, 0x7D, 0x8A};  // 官方 ForegroundInactive（见下豁免）
        roles.error = Color{0xDA, 0x44, 0x53};  // 官方 ForegroundNegative
        roles.focus_ring = Color{0x3D, 0xAE, 0xE9};  // 官方 DecorationFocus
    }
    // 三处照抄上游并记豁免（本库不变量要求 ≥4.5:1，Breeze 官方配对本身不达）：
    //   primary #3DAEE9 配白 2.49:1 —— 这是 KDE 选中态的真实观感，其调色板无达标深蓝档；
    //   on_surface_variant #707D8A 在亮档中性面上 3.69:1 —— 官方 ForegroundInactive 即此值；
    //   error #DA4453 配白 4.26:1 —— 官方 ForegroundNegative 即明暗同值的一支。
    // 与 macOS 档同理：改成自造的深一档蓝等于把「Breeze 档」换成猜测，取舍偏袒取证事实。
    return roles;
}

auto apply_kde(Theme &t, bool dark) -> void {
    t.roles = kde_roles(dark);
    t.csd = csd_from_preset(dark ? CsdPreset::BreezeDark : CsdPreset::BreezeLight);
    t.type_scale = retarget_type_scale("Noto Sans", 10.0F);  // 官方 plasma-integration 的 General 字体默认
    t.shapes.small = 5.0F;  // 官方 $Metrics::Frame_FrameRadius（绘制半径 4.5 = 5 - 0.5×描边）
    t.controls.control_height = 36.0F;  // 派生 文字高 + 2×Button_MarginWidth(6) + 2×Frame_FrameWidth(2)
    t.controls.scrollbar_thickness = 8.0F;  // 官方 $Metrics::ScrollBar_SliderWidth（21dp 轨道内的滑块宽）
    // KDE 标题栏的按钮序与 Adwaita 同侧同为「关在右」（KWin 默认 ButtonsOnRight=HIAX，
    // 即 ContextHelp/Minimize/Maximize/Close），标题居中（默认 AlignCenterFullWidth），
    // 故 button_layout 复用 TitleBarButtonLayout::Adwaita、栏高沿用其推导式：Breeze 的栏高
    // 由字体与 gridUnit 运行时推导（max(文字高, gridUnit×2) + smallSpacing×4），没有独立
    // 钉值出处，写死常量反而比推导更不像真机。
    t.conventions.dialog_button_order = DialogButtonOrder::AcceptFirst;  // 官方 Qt KdeLayout：Accept 在 Reject 左
    t.conventions.scrollbar_mode = ScrollbarMode::Classic;  // 官方 Breeze 小部件层只有经典占位滚动条
    // 未取证：KDE HIG 对按钮序无明文（develop-kde-org 的 hig 九篇全 grep 零命中），本档取值
    // 来自 Qt 的 KdeLayout 表 + plasma-integration 的显式设键；焦点环的「内嵌虚线」形态在
    // 本库 `FocusRingStyle` 三档里没有对应档，与 Windows 双环同属计划/待建面。
}

}  // namespace

auto Theme::from_style(ThemeStyle style, ColorScheme scheme, const AccessibilitySettings &accessibility) -> Theme {
    // `Unspecified` ≡ 明色列：与 `resolve_color_scheme` 及 CSD 桥的口径一致，三态不塌——
    // 取明色列只是为了「必须有值才能出主题」，`scheme` 字段本身仍原样回传，交由优先链下层解析。
    const bool is_dark = (scheme == ColorScheme::Dark);
    Theme themed = is_dark ? dark() : light();
    themed.style = style;
    themed.scheme = scheme;
    switch (style) {
        case ThemeStyle::Material:
            break;  // 基线档：除 style / scheme 外逐字段不动，与 light() / dark() / {} 全等
        case ThemeStyle::Windows:
            apply_windows(themed, is_dark);
            break;
        case ThemeStyle::MacOs:
            apply_macos(themed, is_dark);
            break;
        case ThemeStyle::Gnome:
            apply_gnome(themed, is_dark);
            break;
        case ThemeStyle::Kde:
            apply_kde(themed, is_dark);
            break;
    }
    // 无障碍开关在这里作为**输入**折进快照，且只经 `with_accessibility` 一处实现（两轴解算与开关折入
    // 因此是同一个交点，读取通路不看开关）。默认实参全关 ⇒ 本行等价于原样返回。
    return themed.with_accessibility(accessibility);
}

auto Theme::with_accessibility(const AccessibilitySettings &accessibility) const -> Theme {
    Theme folded = *this;
    // 全关 / 不缩放时逐字段原样返回：默认档与开关接入前逐位一致（golden 判据）。
    if (accessibility.high_contrast) {
        // 高对比不另打第三列常量：整族调 tone 的派生，判据即 detail::AURORA_CONTRAST_AUDITS 那一张表。
        // 覆盖面边界：窗口装饰配色（`CsdRoles`）不在该登记表内，故本批不随高对比变形（待登记后再接）。
        folded.roles = folded.roles.with_high_contrast();
    }
    // 字号缩放作用于 `TypeScale` 解析，并**带动非文本度量**：字阶十五档、间距七档、形状六档同倍增长。
    // 只在这一处乘倍率 ⇒ 读字阶的四枚图表与读正文档的文本控件走的是同一个数，缩放倍率在控件侧不再
    // 有第二次乘法（Apple `@ScaledMetric` / Compose `sp` 的同一条教训：度量不跟着长就是溢出硬失败）。
    // 显式设值仍压过主题：本函数只改令牌，表态过的属性行读自己的字段、不读这里。
    if (const float scale = accessibility.resolved_font_scale(); scale != 1.0F) {
        for (Font TypeScale::*slot : AURORA_TYPE_SCALE_SLOTS) {
            // 括号必要：`a.*p.m` 按 `a.*(p.m)` 解析（成员指针解引用的优先级低于成员访问）。
            (folded.type_scale.*slot).size_pt *= scale;
        }
        // 间距与形状档自带登记表（`check_theme_token_registry` 的 T1 保证表 == 全部公开标量字段），
        // 故整族变换直接遍历登记表，不再手写第二份名单。
        for (const auto &[slot, name] : SpacingScale::AURORA_TOKENS) {
            static_cast<void>(name);
            folded.spacing.*slot *= scale;
        }
        for (const auto &[slot, name] : ShapeScale::AURORA_TOKENS) {
            static_cast<void>(name);
            folded.shapes.*slot *= scale;
        }
    }
    // 减动效是时长档的短路门（取零档），不是倍率：整族归零后动画侧一步落端点。
    if (accessibility.reduce_motion) {
        const Duration zero{};
        for (const auto &[slot, name] : MotionScale::AURORA_TOKENS) {
            static_cast<void>(name);
            folded.motion.*slot = zero;
        }
    }
    return folded;
}

}  // namespace aurora

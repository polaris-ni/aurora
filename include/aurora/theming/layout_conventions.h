#pragma once

#include <cstdint>

namespace aurora {

/// @brief 对话框动作按钮的排布惯例（同一组按钮，谁在前、怎么对齐）。
///
/// 只表排布，不表文案、不表语义角色——「接受」指结果为正的那个按钮（如 OK / Yes / 确定），
/// 「拒绝」指取消侧。三档覆盖四大设计语言的现实分歧：Windows 与 KDE 把接受钮排在拒绝钮之前
/// （Qt 动作按钮序表 `buttonRoleLayouts` 的 `WinLayout` / `KdeLayout` 两支均为 Yes→No、
/// Accept→Reject）；GNOME/Adwaita 的对话框把接受钮排在行尾（右）并以 `suggested-action`
/// 填强调色（即 Adwaita 警报对话框里 [取消] 在左、[确定] 在右的实测排布）；
/// macOS 把按钮竖排在右下角且接受钮在上。
enum class DialogButtonOrder : std::uint8_t {
    AcceptFirst,  ///< 水平行内接受钮在前（左），拒绝钮在后。
    AcceptLast,  ///< 水平行内接受钮在后（右），拒绝钮在前。
    VerticalRightAligned,  ///< 竖排、整体右对齐，接受钮在上（macOS 口径）。
};

/// @brief 对话框标题的水平对齐惯例。
enum class DialogTitleAlignment : std::uint8_t {
    Leading,  ///< 与正文同侧起排（左对齐，随书写方向）。
    Centered,  ///< 相对对话框居中。
};

/// @brief 应用级菜单栏的位置惯例。
///
/// 与 `PlatformCapabilities::native_menu_bar` 的关系是「惯例优先、能力兜底」：本档位表达
/// 设计语言倾向，原生菜单栏不可用（或应用显式覆写）时仍在窗口内自绘。
enum class MenuBarPlacement : std::uint8_t {
    InWindow,  ///< 窗口内自绘菜单栏（GNOME / KDE / Windows 应用内菜单常见）。
    NativePreferred,  ///< 优先使用系统原生菜单栏（macOS 的唯一位置）。
};

/// @brief 滚动条的占位模式。
enum class ScrollbarMode : std::uint8_t {
    Classic,  ///< 经典占位：滚动条计入布局尺寸，内容区随之收窄。
    Overlay,  ///< 覆盖式：滚动条浮在内容之上、不占布局（空闲时可隐藏）。
};

/// @brief 焦点环的呈现方式。
///
/// 只表形状语言，颜色仍由 `ColorRoles::focus_ring` 给（极性色口径不变）。
enum class FocusRingStyle : std::uint8_t {
    Outline,  ///< 沿控件外缘描边。
    OuterGlow,  ///< 控件外侧外扩发光（macOS 口径）。
    None,  ///< 不画焦点环（改由其它视觉提示表达焦点）。
};

/// @brief 布局与交互惯例组：一套设计语言里「不是颜色也不是尺寸」的那部分约定。
///
/// `Theme` 的纯值字段。之所以单独成组而不摊进 `ControlMetrics`：本组字段是**枚举而非标量**，
/// 消费方是排布与绘制分支选择（对话框按钮序、菜单栏位置、滚动条模式、焦点环形状），
/// 与度量数值的读取路径不同；也不同于 `LayoutConventions` 名字可能暗示的「强制规则」——
/// 它是**默认值**，控件与应用的显式设值一律优先（两态语义不变）。
///
/// 初值逐位复现令牌层落地前的行为（对话框「接受在前 + 标题左对齐」、窗口内自绘菜单栏、
/// 经典占位滚动条、描边焦点环、动效倍率 1.0），故 Material 档在接线批前后观感零变化。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct LayoutConventions {
    DialogButtonOrder dialog_button_order = DialogButtonOrder::AcceptFirst;  ///< 对话框动作按钮排布。
    DialogTitleAlignment dialog_title_alignment = DialogTitleAlignment::Leading;  ///< 对话框标题对齐。
    MenuBarPlacement menu_bar_placement = MenuBarPlacement::InWindow;  ///< 应用级菜单栏位置。
    ScrollbarMode scrollbar_mode = ScrollbarMode::Classic;  ///< 滚动条占位模式。
    FocusRingStyle focus_ring_style = FocusRingStyle::Outline;  ///< 焦点环呈现方式。
    float motion_scale = 1.0F;  ///< 动效时长倍率（只缩放既有曲线，与引擎级倍率相乘消费）。

    /// @brief 逐字段相等比较（六项全等）；供主题解析与测试断言。
    /// @param other 比较对象。
    /// @return 全部字段相等时为 true。
    [[nodiscard]] auto operator==(const LayoutConventions &other) const -> bool = default;
};

}  // namespace aurora

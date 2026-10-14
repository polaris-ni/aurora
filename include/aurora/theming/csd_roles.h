#pragma once

#include "aurora/core/color.h"

namespace aurora {

/// @brief 窗口装饰（CSD）配色角色组：标题栏与窗框投影的明暗两列取值。
///
/// 它是 `Theme` 的库自有字段（与通用色角色 `ColorRoles` 同层），专门承载窗口装饰这套
/// **组件级覆写面**。之所以不从通用 `ColorRoles` 派生：CSD 的既有取色与各视觉语言预设
/// （`TitleBarStyle::from_preset`）逐值绑定，且 Windows / macOS 预设与 Adwaita 不同，
/// 挂靠通用角色会污染所有读 `surface` / `on_surface` 的控件。本组只放**配色**字段——
/// 栏高、圆角、显隐、按钮语言等几何与行为字段属定制面，不经主题派生。
///
/// 契约不变量：`Theme::light()` 播种的本组值 ≡ `TitleBarStyle::from_preset(AdwaitaLight)`
/// 的对应配色字段；`Theme::dark()` ≡ `AdwaitaDark`。由 `utest_theme_csd_roles` 钉死。
///
/// @note Thread: thread-safe (pure value type)
/// @note Side-effects: none
struct CsdRoles {
    Color title_bar_bg_active;  ///< 标题栏激活态背景。
    Color title_bar_bg_inactive;  ///< 标题栏失焦背景。
    Color title_bar_fg_active;  ///< 标题栏激活态前景（标题文字 / 符号）。
    Color title_bar_fg_inactive;  ///< 标题栏失焦前景。
    Color title_bar_hover_tint;  ///< 标题栏非关闭按钮悬停叠色（含 alpha）。
    Color title_bar_pressed_tint;  ///< 标题栏非关闭按钮按压叠色（含 alpha）。
    Color title_bar_close_hover;  ///< 关闭按钮悬停底色（各视觉语言特征色）。
    Color shadow_color;  ///< 窗框投影基色（含 alpha）。

    /// @brief 逐字段相等比较（八个配色字段全等）；供主题解析与测试断言。
    /// @param other 比较对象。
    /// @return 全部字段相等时为 true。
    [[nodiscard]] auto operator==(const CsdRoles &other) const -> bool = default;

    /// @brief 亮色档 CSD 配色（基准 = `TitleBarStyle::from_preset(CsdPreset::AdwaitaLight)`）。
    /// @return 与 Adwaita 亮色预设逐值相同的组件角色组。
    [[nodiscard]] static constexpr auto light() noexcept -> CsdRoles {
        return CsdRoles{
            .title_bar_bg_active = Color{0xFF, 0xFF, 0xFF},
            .title_bar_bg_inactive = Color{0xFA, 0xFA, 0xFB},
            .title_bar_fg_active = Color{0x33, 0x33, 0x34},
            .title_bar_fg_inactive = Color{0x7D, 0x7D, 0x80},
            .title_bar_hover_tint = Color{0, 0, 0, 38},
            .title_bar_pressed_tint = Color{0, 0, 0, 77},
            .title_bar_close_hover = Color{0xE0, 0x1B, 0x24},
            .shadow_color = Color{0, 0, 0, 70},
        };
    }

    /// @brief 暗色档 CSD 配色（基准 = `TitleBarStyle::from_preset(CsdPreset::AdwaitaDark)`）。
    /// @return 与 Adwaita 暗色预设逐值相同的组件角色组。
    [[nodiscard]] static constexpr auto dark() noexcept -> CsdRoles {
        return CsdRoles{
            .title_bar_bg_active = Color{0x2E, 0x2E, 0x32},
            .title_bar_bg_inactive = Color{0x22, 0x22, 0x26},
            .title_bar_fg_active = Color{255, 255, 255},
            .title_bar_fg_inactive = Color{0x91, 0x91, 0x93},
            .title_bar_hover_tint = Color{255, 255, 255, 38},
            .title_bar_pressed_tint = Color{255, 255, 255, 77},
            .title_bar_close_hover = Color{0xE0, 0x1B, 0x24},
            .shadow_color = Color{0, 0, 0, 70},
        };
    }
};

}  // namespace aurora

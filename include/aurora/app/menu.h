#pragma once

#include <functional>
#include <string>
#include <vector>

namespace aurora {

/**
 * @brief 菜单项（声明式）：MenuBar / ContextMenu / SystemTray 共用的菜单数据模型。
 *
 * 支持：普通项、分隔符、子菜单、checkable 项、快捷键文本。
 * 对标 Qt `QAction`/`QMenu`、WPF `MenuItem`、SwiftUI `Button`/`Toggle` in `Menu`。
 *
 * @note Thread: main-thread only
 * @note Side-effects: none
 * @note Rebuildable: no
 */
struct MenuItem {
    std::string label;              ///< 显示文本（分隔符时为空）
    std::function<void()> on_click; ///< 点击回调
    std::vector<MenuItem> children; ///< 子菜单（非空时渲染为子菜单箭头）
    bool separator = false;         ///< 是否为分隔符
    bool checkable = false;         ///< 是否可勾选
    bool checked = false;           ///< 当前勾选状态（checkable 时有效）
    bool enabled = true;            ///< 是否可用（false 时灰显、不响应点击）
    std::string shortcut_text;      ///< 快捷键提示文本（如 "Ctrl+O"，仅显示）
    std::string icon;               ///< 图标标识（预留，当前未渲染）

    /// @brief 构造普通菜单项。
    MenuItem() = default;
    explicit MenuItem(std::string text, std::function<void()> action = {})
        : label(std::move(text)), on_click(std::move(action)) {}

    /// @brief 构造分隔符。
    [[nodiscard]] static auto separator_item() -> MenuItem {
        MenuItem item;
        item.separator = true;
        return item;
    }

    /// @brief 是否为子菜单（含 children）。
    [[nodiscard]] auto is_submenu() const -> bool { return !children.empty(); }
};

} // namespace aurora

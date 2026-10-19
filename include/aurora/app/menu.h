#pragma once

#include <functional>
#include <string>
#include <vector>

namespace aurora {

struct Menu;  // 顶级菜单定义在 widget 层的 menu_bar.h，此处只以不完整类型声明投影重载的形参。

/// @brief 菜单项（声明式）：MenuBar / ContextMenu / SystemTray 共用的菜单数据模型。
///
/// 支持：普通项、分隔符、子菜单、checkable 项、快捷键文本。
/// 对标 Qt `QAction`/`QMenu`、WPF `MenuItem`、SwiftUI `Button`/`Toggle` in `Menu`。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct MenuItem {
    std::string label;  ///< 显示文本（分隔符时为空）
    std::function<void()> on_click;  ///< 点击回调
    std::vector<MenuItem> children;  ///< 子菜单（非空时渲染为子菜单箭头）
    bool separator = false;  ///< 是否为分隔符
    bool checkable = false;  ///< 是否可勾选
    bool checked = false;  ///< 当前勾选状态（checkable 时有效）
    bool enabled = true;  ///< 是否可用（false 时灰显、不响应点击）
    std::string shortcut_text;  ///< 快捷键提示文本（如 "Ctrl+O"，仅显示）
    std::string icon;  ///< 图标标识（预留，当前未渲染）
    std::string id;  ///< 稳定标识（可空；自绘路径忽略，供原生菜单桥按标识路由点击）

    /// @brief 构造普通菜单项。
    MenuItem() = default;
    /// @brief 按显示文本构造菜单项：action 为空时仅展示、不响应点击。
    /// @param text 显示文本（label）。
    /// @param action 点击回调（可空）。
    explicit MenuItem(std::string text, std::function<void()> action = {})
        : label(std::move(text)), on_click(std::move(action)) {}

    /// @brief 构造分隔符。
    /// @return separator 标志已置位、label 与回调保持默认的菜单项。
    [[nodiscard]] static auto separator_item() -> MenuItem {
        MenuItem item;  // 新建的裸菜单项，随后仅置 separator 标志返回
        item.separator = true;
        return item;
    }

    /// @brief 是否为子菜单（含 children）。
    /// @return children 非空时为 `true`（渲染为带箭头的子菜单）。
    [[nodiscard]] auto is_submenu() const -> bool { return !children.empty(); }
};

/// @brief 菜单数据模型（纯数据）：菜单项与顶级菜单共用的单一真源，自绘控件与原生菜单桥同源于此。
///
/// 只承载数据：点击行为留在 `MenuItem::on_click`、图标提示留在 `MenuItem::icon`，二者都不进本结构。
/// 顶级菜单与菜单项共用一套模型：顶级菜单标题落在 `label`，其下拉项落在 `children`。
/// `children` 非空即子菜单，与 `MenuItem::is_submenu()` 同口径；分隔符同时携带 `children` 的组合语义未定义，
/// 投影只保证保序与字段保真。
///
/// @note Thread: main-thread only
/// @note Side-effects: none
/// @note Rebuildable: no
struct MenuModel {
    std::string id;  ///< 稳定标识（可空；菜单重建后据此路由点击）
    std::string label;  ///< 显示文本（分隔符时为空；顶级菜单时为菜单标题）
    bool enabled = true;  ///< 是否可用（false 时灰显、不响应点击）
    bool checkable = false;  ///< 是否可勾选
    bool checked = false;  ///< 当前勾选状态（checkable 时有效）
    std::string shortcut_text;  ///< 快捷键提示文本（如 "Ctrl+O"，仅显示）
    bool separator = false;  ///< 是否为分隔符
    std::vector<MenuModel> children;  ///< 子项（非空即为子菜单 / 顶级菜单的下拉项）
};

/// @brief 把菜单项投影为纯数据模型（递归，`id` 透传）。
///
/// 行为（`on_click`）与渲染提示（`icon`）不进投影，需行为的场景请继续持有原 `MenuItem`。
/// @param item [in] 待投影的菜单项；其 `children` 按原顺序逐位递归投影。
/// @return 与 item 的数据字段逐位对应的 MenuModel。
/// @note Thread: main-thread only
/// @note Side-effects: pure
/// @note Rebuildable: no
[[nodiscard]] auto to_menu_model(const MenuItem &item) -> MenuModel;

/// @brief 把顶级菜单投影为纯数据模型：`title` 映射 `label`、`items` 映射 `children`、`id` 透传。
/// @param menu [in] 待投影的顶级菜单。
/// @return label 为菜单标题、children 为其下拉项投影的 MenuModel（enabled 取默认 true）。
/// @note Thread: main-thread only
/// @note Side-effects: pure
/// @note Rebuildable: no
[[nodiscard]] auto to_menu_model(const Menu &menu) -> MenuModel;

/// @brief 把菜单栏整体投影为纯数据模型列表（顺序逐位保真）。
/// @param menus [in] 顶级菜单列表；列表顺序即投影结果的顺序（菜单栏从左到右）。
/// @return 与 menus 等长、逐位对应的 MenuModel 列表。
/// @note Thread: main-thread only
/// @note Side-effects: pure
/// @note Rebuildable: no
[[nodiscard]] auto to_menu_models(const std::vector<Menu> &menus) -> std::vector<MenuModel>;

/// @brief 把纯数据模型回转成菜单项（递归，`id` 透传）。
///
/// 回转结果只含数据：`on_click` 与 `icon` 均为空——供原生菜单桥按标识反查命令后自行接线行为。
/// @param model [in] 待回转的菜单数据模型；其 `children` 按原顺序逐位递归回转。
/// @return 与 model 的数据字段逐位对应的 MenuItem。
/// @note Thread: main-thread only
/// @note Side-effects: pure
/// @note Rebuildable: no
[[nodiscard]] auto from_menu_model(const MenuModel &model) -> MenuItem;

}  // namespace aurora

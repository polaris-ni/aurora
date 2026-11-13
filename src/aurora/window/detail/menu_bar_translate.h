#pragma once

// 原生菜单栏折算层：**内部头**（与 `system_drag_translate.h` / `ime_composition.h` / `recent_documents_translate.h`
// 同列于 src/）。
//
// 为何单列且零平台依赖：OS 侧菜单在 Win32（`HMENU` / `CreateMenu` / `AppendMenu`）与 macOS
// （`NSMenu` / `NSMenuItem`）上是两套完全不同的对象模型，但二者吃的都是同一份 `MenuModel` 树。
// 「树 → 平台可投递的扁平项表」这层折算若写在各座桥里，就会出现两套次序与两套字段约定（谁先谁后、
// 分隔符/勾选/禁用怎么标注各处不同），而次序与字段保真恰恰是菜单的语义。故折算收敛为**纯函数**（摊平
// 树、保留深度/父索引/各项属性）+ **可无头逐值单测**，两座（本次一座 Win32）平台桥共用同一份换算，
// 不存在两套次序。
//
// 本头不引任何 `<windows.h>` / Cocoa 头：只有 `<cstddef>` / `<functional>` / `<string>` / `<vector>`
// 与 `core/result.h`，因此在任何 CI 作业（含无头 Linux）上都能编译并被单测覆盖。
//
// 不做的事：**点击路由**。折算层只把 `MenuModel::id` 透传给扁平项，不替任何桥决定「点了之后干什么」——
// 那是桥与 `CommandRegistry` 之间的事（见 `native_menu_bar.h` 的登记口与 `win32_menu_bar.cpp`）。
//
// 门控：无。本头在**所有**平台编译（含 wasm）。

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "aurora/app/menu.h"  // MenuModel（纯数据模型，自绘与原生桥共源）
#include "aurora/core/result.h"

namespace aurora::menu_bar_translate {

/// @brief 父索引哨兵：顶层菜单项（深度 0）的父索引取此值（没有父）。
inline constexpr std::size_t AURORA_MENU_BAR_NO_PARENT = static_cast<std::size_t>(-1);

/// @brief 摊平后的单个菜单项（平台无关；桥据此重建原生菜单树）。
///
/// 整张表是 `MenuModel` 树的前序遍历（pre-order）：某个子菜单项一定紧邻其父项之后出现，直到
/// 深度回落到父项深度。桥据此用「深度 + 父索引」重建层级，无需额外携带树结构。
struct FlatMenuItem {
    std::size_t depth = 0;  ///< 层级深度：顶层菜单（File/Edit/...）为 0，其下拉项为 1，递归加深。
    std::size_t parent_index =
        AURORA_MENU_BAR_NO_PARENT;  ///< 父项在扁平表中的下标；顶层项取 `AURORA_MENU_BAR_NO_PARENT`。
    bool separator = false;  ///< 是否为分隔符（label/children/id 等均无意义）。
    bool checkable = false;  ///< 是否可勾选（勾选态语义由应用侧命令状态决定，不进本表之外）。
    bool checked = false;  ///< 当前勾选态（checkable 时有效）。
    bool enabled = true;  ///< 是否可用（false 时灰显、不响应点击）。
    std::string label;  ///< 显示文本（分隔符时为空）。
    std::string shortcut_text;  ///< 快捷键提示文本（如 "Ctrl+O"，仅显示，照原样追加到 label 后）。
    std::string id;  ///< 稳定标识（可空；原生路径跨菜单重建只能靠它路由点击，见 `native_menu_bar.h`）。
};

/// @brief 把 `MenuModel` 树摊平为平台无关的项表（前序遍历，深度/父索引/各项属性逐值保真）。
///
/// 折算规则：
/// - **保序**：树的前序遍历顺序即输出顺序，子项紧跟父项，不重排；
/// - **字段保真**：`separator` / `checkable` / `checked` / `enabled` / `label` / `shortcut_text` /
///   `id` 逐项透传，不丢不补；
/// - **空 id 照常建出**：`id` 为空的项仍出现在表里（不丢弃），其点击路由由桥侧留 WARN 处置
///   （不静默丢弃、不伪造 id），故本函数不对 `id` 做任何特殊处理。
///
/// @param models 顶层菜单模型列表（顺序即菜单栏从左到右）；空列表产出空表（合法结果而非错误）。
/// @return 摊平后的项表（可为空）。
[[nodiscard]] auto flatten_menu_models(const std::vector<MenuModel> &models) -> std::vector<FlatMenuItem>;

/// @brief 平台桥登记到折算层的**渲染通路**（进程级单槽）。
///
/// 存在的理由：公共入口 `NativeMenuBar::install()` 是进程级的、且必须在**无头 CI** 里被证到
/// 「显式安装 / 替换旧树 / 卸载 / 点击路由」——但原生菜单对象（HMENU / NSMenu）在无头环境建不出来。
/// 故折算层持一个可注入的函数表：真机上由平台桥登记，单测里由伪造后端登记，两侧走同一条代码路径
/// ——被测的就是产品代码本身，不是它的复刻。
struct MenuBarBackend {
    /// @brief 安装菜单栏：吃折算后的扁平项表 + 一个点击路由回调（桥在收到原生点击时调它，按 `id` 路由）。
    /// @param items 折算层产出的扁平项表（前序遍历，深度/父索引齐备）。
    /// @param on_click 点击路由回调：原生菜单项被点击时，桥以该项的 `id` 调它（空 id 的项桥侧留 WARN 不调）。
    /// @return 成功（含空表，即清空菜单栏）或结构化错误（平台无机制 / 装不上）。
    std::function<Result<void>(const std::vector<FlatMenuItem> &items, std::function<void(std::string)>)> install;
    /// @brief 卸载菜单栏（销毁原生菜单对象、断开窗口关联）；幂等。
    std::function<void()> uninstall;
    /// @brief 当前是否已安装（即最近一次 `install` 成功且尚未 `uninstall`）。
    std::function<bool()> is_installed;
};

/// @brief 登记渲染通路（覆盖式；进程级单槽）。
/// @param backend 待登记的通路（`install` / `uninstall` / `is_installed` 由桥填）。
auto register_menu_bar_backend(MenuBarBackend backend) -> void;

/// @brief 注销渲染通路（清空单槽；幂等，未登记时为空操作）。
auto unregister_menu_bar_backend() -> void;

/// @brief 当前登记的渲染通路。
/// @return 已登记时指向该后端（进程生命周期内稳定）；未登记时为 nullptr。
[[nodiscard]] auto menu_bar_backend() -> const MenuBarBackend *;

}  // namespace aurora::menu_bar_translate

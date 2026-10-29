#pragma once

/// @brief macOS 系统托盘桥（`NSStatusItem`）的 C++ 接线面。
///
/// `SystemTray` 的公共方法定义在 `system_tray.cpp`（跨平台同一份），AppKit 细节落在
/// 同目录的 `macos_tray.cpp`（ObjC++）。本头只暴露 C++ 可调用成员；`state_` 是 AppKit 对象的
/// 不透明句柄（`void*`），避免把 AppKit 类型拖进 C++ 头。
///
/// 语义映射：`set_title` → 状态按钮的 tooltip；`set_icon` → 状态栏图像（空路径回退应用图标）；
/// `set_context_menu` → `NSMenu`（子菜单 / 勾选 / 禁用逐项映射，点击回调按扁平索引路由）；
/// `show_balloon` → 复用本库通知中心（macOS 没有「托盘气泡」概念，用系统通知等价表达）。
///
/// @note Thread: main-thread only（NSStatusItem / NSMenu 均为主线程 AppKit 对象）
/// @file macos_tray.h

#include "aurora/core/platform.h"

#ifdef AURORA_PLATFORM_MACOS

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "aurora/app/menu.h"

namespace aurora::detail {

/// @brief macOS 状态栏托盘桥（RAII：构造创建状态项，析构移除）。
class MacOSTrayBridge {
  public:
    /// @brief 创建状态项并应用初始标题 / 图标。
    /// @param title 悬浮提示文字（状态按钮 tooltip）。
    /// @param icon_path 图标文件路径；空串回退应用图标。
    MacOSTrayBridge(const std::string &title, const std::string &icon_path);
    /// @brief 移除状态项并释放 AppKit 对象。
    ~MacOSTrayBridge();
    /// @brief 禁止拷贝（状态项为唯一所有权资源）。
    MacOSTrayBridge(const MacOSTrayBridge &) = delete;
    /// @brief 禁止拷贝赋值（同拷贝构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const MacOSTrayBridge &) -> MacOSTrayBridge & = delete;
    /// @brief 禁止移动（`state_` 裸句柄的唯一性由存活登记表保证，移走会让 target 指空）。
    MacOSTrayBridge(MacOSTrayBridge &&) = delete;
    /// @brief 禁止移动赋值（同移动构造）。
    /// @return 恒不返回（已 delete）。
    auto operator=(MacOSTrayBridge &&) -> MacOSTrayBridge & = delete;

    /// @brief 更新悬浮提示文字。
    /// @param title 新文字。
    void set_title(const std::string &title);
    /// @brief 更新状态栏图像。
    /// @param path 图标文件路径；空串回退应用图标。
    void set_icon(const std::string &path);
    /// @brief 显示状态项。
    void show();
    /// @brief 隐藏状态项。
    void hide();
    /// @brief 注册点击激活回调（无菜单时点击状态按钮触发）。
    /// @param callback 激活回调（可空）。
    void set_activate(std::function<void()> callback);
    /// @brief 设置右键 / 点击菜单（整体替换；空列表卸下菜单、恢复点击激活）。
    /// @param items 菜单项列表。
    void set_menu(const std::vector<MenuItem> &items);
    /// @brief 用系统通知表达「托盘气泡」（标题 + 正文），失败只记日志（接口无返回值）。
    /// @param title 通知标题。
    /// @param body 通知正文。
    void show_notification(const std::string &title, const std::string &body);

    // ---- 验收探针只读观测面（内部头，不进公共 API）----
    /// @brief 状态项是否创建成功。
    /// @return 已创建为 true。
    [[nodiscard]] auto attached() const -> bool;
    /// @brief 当前可见性（库内状态，与 AppKit `NSStatusItem.visible` 同步）。
    /// @return 可见为 true。
    [[nodiscard]] auto visible() const -> bool;
    /// @brief 当前 tooltip 文本。
    /// @return tooltip 文本（UTF-8）。
    [[nodiscard]] auto tooltip() const -> std::string;
    /// @brief 当前菜单的条目总数（含分隔符与子菜单项，按构建序）。
    /// @return 条目数。
    [[nodiscard]] auto menu_item_count() const -> std::size_t;
    /// @brief 原生 `NSStatusItem` 指针（验收探针做独立取证用；调用方不得接管所有权）。
    /// @return `NSStatusItem *` 的不透明指针；未创建时为 nullptr。
    [[nodiscard]] auto native_status_item() const -> void *;

  private:
    void *state_ = nullptr;  ///< AppKit 状态（`TrayState`，定义在 .cpp）
};

/// @brief 当前存活的桥（验收探针取证用；按创建序，析构即移除）。
///
/// 供探针在 `SystemTray` 的公开方法之外独立观察状态项（`SystemTray::Impl` 不对外暴露）。
/// 返回的是非拥有指针快照，元素随时可能随对象销毁而失效。
/// @return 存活桥指针列表（无存活桥时为空）。
/// @note Thread: main-thread only
[[nodiscard]] auto live_tray_bridges() -> std::vector<MacOSTrayBridge *>;

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_MACOS

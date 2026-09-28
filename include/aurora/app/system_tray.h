#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/menu.h"

namespace aurora {

/// @brief 系统托盘图标（Windows 通知区域）。RAII：构造即尝试添加图标，析构移除。
/// 真实平台（AURORA_PLATFORM_WINDOWS）实现见 `src/aurora/app/system_tray_win32.cpp`
/// （`Shell_NotifyIcon` + 隐藏消息窗口 + 图标/气泡/激活回调 + 右键菜单），由 `AURORA_BACKEND_WIN32` 裁剪；
/// 非 Win32 / Headless 下所有方法为 no-op（仅记录 `last_balloon_message` 供测试）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none (interacts with OS tray)
/// @note Rebuildable: no
class SystemTray {
  public:
    /// @brief 构造：记录悬浮提示并（Windows 下）创建隐藏消息窗口、加载图标并尝试添加到托盘；非 Win32 为 no-op。
    /// @param title 悬浮提示文字。
    /// @param icon_path 图标文件路径（空串 = 默认应用图标）。
    explicit SystemTray(std::string title, const std::string &icon_path = "");
    /// @brief 禁止拷贝（托盘图标与隐藏窗口句柄为唯一所有权资源）。
    SystemTray(const SystemTray &) = delete;
    /// @brief 禁止拷贝赋值（托盘图标与隐藏窗口句柄为唯一所有权资源）。
    /// @return 恒不返回（已 delete）。
    auto operator=(const SystemTray &) -> SystemTray & = delete;
    /// @brief 移动构造：接管托盘实现体与状态，并把实现体的回指指针改到新对象。
    /// @param other 移动源（移动后其 impl_ 为空）。
    SystemTray(SystemTray &&other) noexcept;
    /// @brief 移动赋值：先按析构语义释放自身托盘资源，再接管来源的实现体与状态（自赋值跳过）。
    /// @param other 移动源。
    /// @return 移动赋值后的自身引用。
    auto operator=(SystemTray &&other) noexcept -> SystemTray &;
    /// @brief 析构：移除托盘图标、销毁隐藏消息窗口并释放图标句柄（非 Win32 下为空操作）。
    ~SystemTray();

    /// @brief 更新悬浮提示文字（同时刷新托盘图标 tip）。
    /// @param t 新的悬浮提示文字。
    void set_title(std::string t);
    /// @brief 设置托盘图标（从文件加载；空路径用默认应用图标）。
    /// @param path 图标文件路径（空串 = 默认应用图标）。
    void set_icon(const std::string &path) const;
    /// @brief 显示气泡通知（title 为标题，msg 为正文）。
    /// @param title 气泡标题。
    /// @param msg 气泡正文（记入 `last_balloon_message`，非 Win32 下仅记录不外发）。
    void show_balloon(const std::string &title, const std::string &msg);
    /// @brief 添加 / 显示托盘图标。
    void show() const;
    /// @brief 移除托盘图标（隐藏）。
    void hide() const;
    /// @brief 注册激活回调：用户左键单击 / 气泡点击 / 键盘激活图标时触发。
    /// @param cb 激活回调（可空；空则清除既有回调）。
    void on_activate(std::function<void()> cb);

    /// @brief 设置右键上下文菜单（Win32 经 TrackPopupMenu 弹出；非 Win32 存储但不渲染）。
    /// 菜单项模型与 MenuBar / ContextMenuNode 共用 MenuItem 声明式数据结构。
    /// @param items 菜单项列表（整体替换既有菜单）。
    void set_context_menu(std::vector<MenuItem> items);
    /// @brief 当前右键菜单项（只读访问）。
    /// @return 最近一次 `set_context_menu` 存入的菜单项列表引用。
    [[nodiscard]] auto context_menu_items() const -> const std::vector<MenuItem> & { return context_menu_items_; }

    /// @brief 最近一次 `show_balloon` 的正文（headless 下也可查询；随对象销毁而失效）。
    /// @return 气泡正文的引用（从未显示过气泡时为空串）。
    [[nodiscard]] auto last_balloon_message() const -> const std::string & { return tray_balloon_msg_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  ///< Windows 下的托盘实现体（隐藏消息窗口 + 图标句柄）；非 Win32 恒为空
    std::string tray_title_;  ///< 悬浮提示文字（`set_title` 更新）
    std::string tray_balloon_msg_;  ///< 最近一次 `show_balloon` 的正文
    std::function<void()> on_activate_cb_;  ///< 激活回调（左键单击 / 气泡点击 / 键盘激活时触发）
    std::vector<MenuItem> context_menu_items_;  ///< 右键上下文菜单数据模型

    /// @brief 供嵌套 `Impl` 在图标激活时回调（嵌套类可访问私有成员）。
    void fire_activate() const {
        if (on_activate_cb_) {
            on_activate_cb_();
        }
    }
};

}  // namespace aurora

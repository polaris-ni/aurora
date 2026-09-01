#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aurora/app/menu.h"
#include "aurora/core/platform.h"

namespace aurora {

/// @brief 系统托盘图标（Windows 通知区域）。RAII：构造即尝试添加图标，析构移除。
///
/// 真实平台（AURORA_PLATFORM_WINDOWS）实现见 `src/aurora/app/system_tray_win32.cpp`
/// （`Shell_NotifyIcon` + 隐藏消息窗口 + 图标/气泡/激活回调 + 右键菜单），由 `AURORA_BACKEND_WIN32` 裁剪；
/// 非 Win32 / Headless 下所有方法为 no-op（仅记录 `last_balloon_message` 供测试）。
///
/// @note Thread: main-thread only
/// @note Side-effects: none (interacts with OS tray)
/// @note Rebuildable: no
class SystemTray {
  public:
    explicit SystemTray(std::string title, const std::string &icon_path = "");
    SystemTray(const SystemTray &) = delete;
    auto operator=(const SystemTray &) -> SystemTray & = delete;
    SystemTray(SystemTray &&) noexcept;
    auto operator=(SystemTray &&) noexcept -> SystemTray &;
    ~SystemTray();

    /// @brief 更新悬浮提示文字（同时刷新托盘图标 tip）。
    void set_title(std::string t);
    /// @brief 设置托盘图标（从文件加载；空路径用默认应用图标）。
    void set_icon(const std::string &path) const;
    /// @brief 显示气泡通知（title 为标题，msg 为正文）。
    void show_balloon(const std::string &title, const std::string &msg);
    /// @brief 添加 / 显示托盘图标。
    void show() const;
    /// @brief 移除托盘图标（隐藏）。
    void hide() const;
    /// @brief 注册激活回调：用户左键单击 / 气泡点击 / 键盘激活图标时触发。
    void on_activate(std::function<void()> cb);

    /// @brief 设置右键上下文菜单（Win32 经 TrackPopupMenu 弹出；非 Win32 存储但不渲染）。
    /// 菜单项模型与 MenuBar / ContextMenuNode 共用 MenuItem 声明式数据结构。
    void set_context_menu(std::vector<MenuItem> items);
    /// @brief 当前右键菜单项（只读访问）。
    [[nodiscard]] auto context_menu_items() const -> const std::vector<MenuItem> & { return context_menu_items_; }

    /// @brief 最近一次 `show_balloon` 的正文（headless 下也可查询；随对象销毁而失效）。
    [[nodiscard]] auto last_balloon_message() const -> const std::string & { return tray_balloon_msg_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string tray_title_;
    std::string tray_balloon_msg_;
    std::function<void()> on_activate_cb_;
    std::vector<MenuItem> context_menu_items_; ///< 右键上下文菜单数据模型

    /// @brief 供嵌套 `Impl` 在图标激活时回调（嵌套类可访问私有成员）。
    void fire_activate() const {
        if (on_activate_cb_) {
            on_activate_cb_();
        }
    }
};

} // namespace aurora

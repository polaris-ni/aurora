#pragma once

// Win32 原生菜单栏桥：**内部头**（与 `win32_drag.h` / `win32_ime.h` / `win32_recent_documents.h` 同列于 src/）。
//
// 机制：`CreateMenu` / `CreatePopupMenu` / `AppendMenu` / `SetMenu` 建出菜单栏并挂到窗口；菜单项点击
// 经 `WM_COMMAND` 回到 `Win32Host` 现有消息泵，由泵转发到本桥的 `on_command`（按命令 id 反查模型 id
// 后调外部注入的路由回调）。`HMENU` 与窗口句柄绑定：窗口在 ⇒ 菜单在，窗口销毁期先拆桥再放窗口（与
// 无障碍桥 / 输入法桥 / 拖放桥 / 最近文档桥同款纪律）。
//
// 能力归属：本桥只在 Win32 / D3D11 / WgpuWin32 三类 Surface 上由宿主构造（三者共用同一宿主、同一句柄），
// 见 `window/platform.h` 的 `native_menu_bar` 能力位（上述三者为 true）。其余平台（含 macOS / Linux /
// Wasm / Headless）不构造本桥，能力位为 false，回落自绘 `MenuBar`。
//
// 门控与 `win32_drag.h` / `win32_recent_documents.h` 同款（平台宏 ∧ 后端宏析取）：
//   AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)
#include "aurora/core/platform.h"

#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "aurora/core/result.h"
#include "aurora/window/detail/menu_bar_translate.h"

namespace aurora::detail {

/// @brief Win32 菜单命令 id 基数。
///
/// `WM_COMMAND` 的 `LOWORD(wParam)` 是 16 位菜单项 id，故取值须 < 65536 且与常见控件 id 错开。
/// 本桥派发的命令 id 全部落在 `[kMenuCommandBase, kMenuCommandBase + id_map_.size())` 区间内，
/// 区间外即非本桥的 `WM_COMMAND`（控件等），`on_command` 返回 false 交 `DefWindowProc` 处理。
inline constexpr UINT AURORA_WIN32_MENU_COMMAND_BASE = 0x4000U;

/// @brief Win32 原生菜单栏桥。
///
/// 由 `Win32Host::Impl` 在窗口构造期持有（唯一持有方），负责把折算层摊平的 `FlatMenuItem` 表重建为
/// `HMENU` 并挂到本窗口；点击经消息泵转发 `on_command` 后按模型 `id` 调外部路由回调。重复 `install`
/// 替换旧树（先卸旧 `HMENU` 再挂新），`uninstall` 清空。
class Win32MenuBarBridge {
  public:
    /// @brief 构造桥（不立即建菜单，等 `install`）。
    /// @param hwnd 宿主窗口句柄；菜单将挂到它身上（`SetMenu(hwnd, ...)`）。
    explicit Win32MenuBarBridge(HWND hwnd);

    Win32MenuBarBridge(const Win32MenuBarBridge &) = delete;
    auto operator=(const Win32MenuBarBridge &) -> Win32MenuBarBridge & = delete;
    Win32MenuBarBridge(Win32MenuBarBridge &&) = delete;
    auto operator=(Win32MenuBarBridge &&) -> Win32MenuBarBridge & = delete;

    ~Win32MenuBarBridge();

    /// @brief 安装 / 替换菜单栏。
    ///
    /// 把折算层摊平的扁平项表重建为 `HMENU` 挂到窗口；重复调用替换旧树（先卸旧再装新）。空表等同于清空。
    /// @param items 折算层产出的扁平项表（前序遍历，深度/父索引齐备）。
    /// @param on_click 点击路由回调：原生菜单项被点击时以该项的模型 `id` 调它（空 id 的项桥侧留 WARN 不调）。
    /// @return 成功（含空表清空）或结构化错误（`native-menu-install-failed`：建菜单/挂窗口失败）。
    [[nodiscard]] auto install(const std::vector<menu_bar_translate::FlatMenuItem> &items,
                               std::function<void(std::string)> on_click) -> Result<void>;

    /// @brief 卸载菜单栏（销毁 `HMENU`、断开窗口关联）。幂等：未安装时 no-op。
    auto uninstall() -> void;

    /// @brief 当前是否已安装（最近一次 `install` 成功且尚未 `uninstall`）。
    /// @return 已安装为 true。
    [[nodiscard]] auto is_installed() const -> bool { return installed_; }

    /// @brief 消息泵在收到 `WM_COMMAND` 时转发至此；只认本桥派发的命令 id 区间。
    /// @param wp `WM_COMMAND` 的 `wParam`（低字为菜单项 id）。
    /// @return 由本桥消费（id 在区间内）为 true；非本桥消息为 false（交 `DefWindowProc`）。
    [[nodiscard]] auto on_command(WPARAM wp) const -> bool;

  private:
    /// @brief 由扁平项表重建 `HMENU` 并挂窗（替换语义）。
    [[nodiscard]] auto build_menu(const std::vector<menu_bar_translate::FlatMenuItem> &items) -> Result<void>;

    HWND hwnd_ = nullptr;  ///< 宿主窗口句柄（菜单挂在此窗）
    HMENU hmenu_ = nullptr;  ///< 当前挂着的菜单栏；nullptr = 未安装
    std::function<void(std::string)> on_click_;  ///< 点击路由回调（install 时注入）
    std::vector<std::string> id_map_;  ///< 命令 id 下标 → 模型 id（空串 = 空 id 项，点击留 WARN）
    bool installed_ = false;  ///< 最近一次 install/uninstall 的净结果
};

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)

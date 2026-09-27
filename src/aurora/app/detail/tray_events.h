#pragma once

// 托盘回调消息（`NOTIFYICONDATAW::uCallbackMessage`）的 lParam 语义分类：内部头。
//
// 为何单列：这条 lParam 有两种互不兼容的编码，判错即静默丢事件或双触发——
//   * 旧版（未升到 `NOTIFYICON_VERSION_4`）：lParam **就是**鼠标消息本身（`WM_LBUTTONUP` / `WM_RBUTTONUP` …）；
//   * 版本 4：lParam 是打包值，**低字**是事件（鼠标消息与 `NIN_*` 各发一条），**高字**是图标 `uID`。
//
// 版本 4 下真机一次左键实测连发四条回调（`build/manual-lib/tray4.err`，2026-09-28）：
// `NIN_POPUPOPEN` → `WM_LBUTTONDOWN` → `WM_LBUTTONUP` → `NIN_SELECT`，右键则发
// `WM_RBUTTONDOWN` → `WM_RBUTTONUP` → `WM_CONTEXTMENU`。因此只认鼠标消息会把激活判两遍、把菜单弹两遍
// （`TrackPopupMenu` 带 `TPM_RETURNCMD` 会阻塞到菜单关闭，第二条随即再弹一次）；只认整值 `switch(lp)`
// 则一条也认不出——这正是本函数要修的两个缺陷。规则收敛成纯函数放这里，是不引 `<windows.h>`、
// 让非 Windows 编译单元与单测也能覆盖这段真机 CI 证不了的逻辑。

#include <cstdint>

namespace aurora::internal {

// 与 Windows SDK 同值的常量副本（取值来源：winuser.h 的 WM_* 与 shellapi.h 的 NIN_* = WM_USER+n）。
inline constexpr std::uint32_t k_tray_wm_contextmenu = 0x007B;        ///< WM_CONTEXTMENU
inline constexpr std::uint32_t k_tray_wm_lbuttonup = 0x0202;          ///< WM_LBUTTONUP
inline constexpr std::uint32_t k_tray_wm_lbuttondblclk = 0x0203;      ///< WM_LBUTTONDBLCLK
inline constexpr std::uint32_t k_tray_wm_rbuttonup = 0x0205;          ///< WM_RBUTTONUP
inline constexpr std::uint32_t k_tray_nin_select = 0x0400;            ///< NIN_SELECT (WM_USER+0)
inline constexpr std::uint32_t k_tray_nin_keyselect = 0x0401;         ///< NIN_KEYSELECT (WM_USER+1)
inline constexpr std::uint32_t k_tray_nin_balloonuserclick = 0x0405;  ///< NIN_BALLOONUSERCLICK (WM_USER+5)

/// @brief 本库注册托盘图标时使用的 `uID`（版本 4 的回调把它放在 lParam 高字，用于筛掉别的图标）。
inline constexpr std::uint32_t k_tray_icon_id = 1;

/// @brief 一次托盘回调的归类结果。
struct TrayCallbackEvent {
    bool activate = false;      ///< 左键 / 键盘选中 / 点气泡 → 触发 `on_activate`
    bool context_menu = false;  ///< 右键 → 弹出托盘上下文菜单
};

/// @brief 按注册时选定的编码版本，把回调 lParam 的两个半字归类。
/// @param event    `LOWORD(lParam)`：两种版本下都是事件本身（鼠标消息或 `NIN_*`）。
/// @param icon_id  `HIWORD(lParam)`：版本 4 下是图标 `uID`，旧版恒为 0。
/// @param version4 `NIM_SETVERSION(NOTIFYICON_VERSION_4)` 是否成功。
/// @note 版本 4 下激活只认 `NIN_*`、菜单只认 `WM_CONTEXTMENU`：同一次点击附带的裸鼠标消息是重复投递。
[[nodiscard]] constexpr auto classify_tray_callback(std::uint32_t event, std::uint32_t icon_id, bool version4)
    -> TrayCallbackEvent {
    TrayCallbackEvent ev{};
    if (version4) {
        if (icon_id != k_tray_icon_id) {
            return ev;  // 同一消息窗挂了别的图标：不归本库管
        }
        ev.activate = ((event == k_tray_nin_select) || (event == k_tray_nin_keyselect) ||
                       (event == k_tray_nin_balloonuserclick));
        ev.context_menu = (event == k_tray_wm_contextmenu);
        return ev;
    }
    ev.activate = ((event == k_tray_wm_lbuttonup) || (event == k_tray_wm_lbuttondblclk));
    ev.context_menu = ((event == k_tray_wm_rbuttonup) || (event == k_tray_wm_contextmenu));
    return ev;
}

}  // namespace aurora::internal

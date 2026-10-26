// win32_menu_bar.cpp — 原生菜单栏的 Windows 桥（HMENU 建树 + WM_COMMAND 回投）。
//
// 折算层纯函数摊平出的扁平项表在此重建为原生菜单树；点击由 Win32Host 消息泵经 `on_command` 回投，
// 按命令 id 反查模型 id 后调注入的路由回调。原生对象与窗口句柄的绑定、销毁时序纪律见 `win32_menu_bar.h`
// 文件头；本文件只做该口径的落地，不另立口径。
#include "aurora/core/platform.h"
#if defined(AURORA_PLATFORM_WINDOWS) && \
    (defined(AURORA_BACKEND_WIN32) || defined(AURORA_BACKEND_D3D11) || defined(AURORA_BACKEND_GPU_WGPU))

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  // NOLINT(*-identifier-naming): Windows SDK 宏，不可改名
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "aurora/core/log.h"
#include "aurora/core/utf8.h"
#include "aurora/window/detail/menu_bar_translate.h"
#include "aurora/window/detail/win32_menu_bar.h"

namespace aurora::detail {
namespace {

/// @brief 计算某项的 `AppendMenu` 标志位（禁用 / 勾选；分隔与弹出的专属位在调用处另加）。
/// @param it 待建项。
/// @return 该项的通用标志位（`MF_DISABLED | MF_GRAYED` / `MF_CHECKED`）。
[[nodiscard]] auto item_flags(const menu_bar_translate::FlatMenuItem &it) -> UINT {
    UINT flags = 0;
    if (!it.enabled) {
        flags |= MF_DISABLED | MF_GRAYED;  // 灰显且不响应点击
    }
    if (it.checkable && it.checked) {
        flags |= MF_CHECKED;  // 勾选态只在 checkable 时才有意义
    }
    return flags;
}

/// @brief 组装显示文本（label + 可选的 `\t` 快捷键提示）。
///
/// Win32 菜单项文本里 `\t` 之后的部分右对齐显示为快捷键（与 `MenuItem::shortcut_text` 的「仅显示」
/// 语义一致）。分隔符不消费本函数。
/// @param it 待建项。
/// @return UTF-16 显示文本。
[[nodiscard]] auto display_text(const menu_bar_translate::FlatMenuItem &it) -> std::wstring {
    std::string text = it.label;
    if (!it.shortcut_text.empty()) {
        text += "\t" + it.shortcut_text;  // 分隔符不会走到这里（调用方已分流）
    }
    return internal::utf8_to_wstr(text);
}

}  // namespace

Win32MenuBarBridge::Win32MenuBarBridge(HWND hwnd) : hwnd_(hwnd) {}

Win32MenuBarBridge::~Win32MenuBarBridge() {
    // 窗口销毁期由 Impl 先散本桥（早于 DestroyWindow），故 hwnd_ 此刻仍有效：安全摘下菜单并销毁。
    if (hmenu_ != nullptr) {
        if (hwnd_ != nullptr) {
            SetMenu(hwnd_, nullptr);  // 先摘再毁：避免悬空 HMENU 挂在已销毁窗口上
        }
        DestroyMenu(hmenu_);
        hmenu_ = nullptr;
    }
    installed_ = false;
}

auto Win32MenuBarBridge::install(const std::vector<menu_bar_translate::FlatMenuItem> &items,
                                 std::function<void(std::string)> on_click) -> Result<void> {
    on_click_ = std::move(on_click);
    return build_menu(items);
}

auto Win32MenuBarBridge::build_menu(const std::vector<menu_bar_translate::FlatMenuItem> &items) -> Result<void> {
    const HMENU hmenubar = CreateMenu();
    if (hmenubar == nullptr) {
        return make_error(ErrorCode::NativeMenuInstallFailed, "CreateMenu failed");
    }
    // submenu_of[i]：项 i 若是子菜单，则记它的弹出 HMENU；其 children 据此挂入。
    std::vector<HMENU> submenu_of(items.size(), nullptr);
    id_map_.clear();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto &it = items[i];
        // 父项的弹出菜单：顶层项（parent_index == AURORA_MENU_BAR_NO_PARENT）挂到菜单栏，否则挂到父项的弹出菜单。
        const HMENU parent =
            (it.parent_index == menu_bar_translate::AURORA_MENU_BAR_NO_PARENT) ? hmenubar : submenu_of[it.parent_index];
        if (parent == nullptr) {
            // 数据不变量被违反（父项未建出子菜单）：健壮兜底跳过本项，不污染已建部分。
            continue;
        }
        // 前序遍历下，子项紧跟父项、深度 +1；据此判断本项是否还有 children。
        const bool has_children = (i + 1U < items.size()) && items[i + 1U].depth == it.depth + 1U;
        if (it.separator) {
            AppendMenuW(parent, MF_SEPARATOR, 0, nullptr);
        } else if (has_children) {
            const HMENU child = CreatePopupMenu();
            if (child == nullptr) {
                DestroyMenu(hmenubar);
                return make_error(ErrorCode::NativeMenuInstallFailed, "CreatePopupMenu failed");
            }
            // Win32 把子菜单句柄塞进菜单项 id 字段（MF_POPUP 语义），只能经
            // reinterpret_cast 传递，别无类型安全通路。
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            AppendMenuW(parent, item_flags(it) | MF_POPUP, reinterpret_cast<UINT_PTR>(child), display_text(it).c_str());
            submenu_of[i] = child;
        } else {
            // 终端项：派发命令 id，记 id 映射（空 id 也记，点击时留 WARN 不路由）。
            const UINT cmd = AURORA_WIN32_MENU_COMMAND_BASE + static_cast<UINT>(id_map_.size());
            id_map_.push_back(it.id);
            AppendMenuW(parent, item_flags(it) | MF_STRING, cmd, display_text(it).c_str());
        }
    }
    if (SetMenu(hwnd_, hmenubar) == 0) {
        DestroyMenu(hmenubar);
        return make_error(ErrorCode::NativeMenuInstallFailed, "SetMenu failed (invalid window handle?)");
    }
    // 替换语义：先挂新树，再销毁旧树（旧树已被新 SetMenu 摘离窗口，销毁安全）。
    if (hmenu_ != nullptr) {
        DestroyMenu(hmenu_);
    }
    hmenu_ = hmenubar;
    installed_ = true;
    DrawMenuBar(hwnd_);  // 立即按新菜单重绘标题栏下方的菜单条
    return Result<void>{};
}

auto Win32MenuBarBridge::uninstall() -> void {
    if (hmenu_ != nullptr) {
        if (hwnd_ != nullptr) {
            SetMenu(hwnd_, nullptr);
        }
        DestroyMenu(hmenu_);
        hmenu_ = nullptr;
    }
    id_map_.clear();
    on_click_ = nullptr;
    installed_ = false;
}

[[nodiscard]] auto Win32MenuBarBridge::on_command(WPARAM wp) const -> bool {
    const UINT id = LOWORD(wp);
    if (id < AURORA_WIN32_MENU_COMMAND_BASE ||
        id >= AURORA_WIN32_MENU_COMMAND_BASE + static_cast<UINT>(id_map_.size())) {
        return false;  // 不在本桥派发的 id 区间：非菜单消息，交 DefWindowProc
    }
    const std::string &model_id = id_map_[id - AURORA_WIN32_MENU_COMMAND_BASE];
    if (model_id.empty()) {
        // 空 id 项照常建出、照常显示，但原生路径跨重建只能靠稳定 id 路由点击——空 id 路由不了。
        // 不静默丢弃、不伪造 id，只留诊断（与门面侧 route_click 的口径一致）。
        AURORA_LOG_WARN("native-menu-bar", "menu item clicked but its id is empty; click ignored");
        return true;  // 已消费（菜单项确实存在并响应了），只是不路由
    }
    if (on_click_) {
        on_click_(model_id);  // 按模型 id 回投（桥不知命令语义，只透传标识）
    }
    return true;
}

}  // namespace aurora::detail

#endif  // AURORA_PLATFORM_WINDOWS && (WIN32 || D3D11 || GPU_WGPU)

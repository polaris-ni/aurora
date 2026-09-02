// system_tray_win32.cpp — 系统托盘实现。
// - AURORA_PLATFORM_WINDOWS：真实 Shell_NotifyIcon + 隐藏消息窗口（见 Impl 方法）。
// - 非 AURORA_PLATFORM_WINDOWS：SystemTray 所有方法为 no-op（impl_ 恒为 null），仅记录 last_balloon_message。
// 公共方法定义始终编译（避免非 Win32 链接失败）；Win32 专有代码用内部 #ifdef 隔离。
//
// 注意：_WIN32_WINNT / _WIN32_IE 必须在任何 aurora 头文件之前定义（见 file_dialog_win32.cpp 注释）。
#include "aurora/core/platform.h"
#ifdef AURORA_PLATFORM_WINDOWS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
#define WIN32_IE 0x0600 // NOLINT(cppcoreguidelines-macro-usage, readability-identifier-naming): Windows SDK 版本宏
#endif
#define WIN32_LEAN_AND_MEAN // NOLINT(readability-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif
#include <string>

#include "aurora/app/system_tray.h"
#include "aurora/core/log.h"
#include "aurora/core/utf8.h"

namespace aurora {

struct SystemTray::Impl {
    SystemTray *owner = nullptr;
#ifdef AURORA_PLATFORM_WINDOWS
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid{};
    HICON hicon = nullptr;
    bool visible = false;
    UINT taskbar_created = 0;

    static constexpr UINT m_aurora_callback_mag = WM_APP + 1;

    auto create_window() -> bool;
    void destroy_window();
    auto add_icon() -> bool;
    void remove_icon();
    void update_icon(const std::string &path);
    void update_tip();
    void show_balloon_impl(const std::string &title, const std::string &msg);
    // NOLINTNEXTLINE(modernize-use-trailing-return-type): CALLBACK 调用约定下尾返回类型会改变签名语义
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    /// @brief 递归构建 Win32 HMENU：从 MenuItem 声明式模型转换为原生菜单。
    /// 每个可点击项用 WM_COMMAND (WM_APP+2 + index) 标识，由 show_context_menu 分发。
    static auto build_hmenu(const std::vector<MenuItem> &items, UINT &cmd_id) -> HMENU;
    /// @brief 弹出右键菜单：构建 HMENU → TrackPopupMenu → 分发 WM_COMMAND。
    void show_context_menu() const;
#endif
};

// ---- SystemTray 公共方法（始终定义；非 Win32 下 impl_ 为 null → no-op）----

SystemTray::SystemTray(std::string title, const std::string &icon_path) : tray_title_(std::move(title)) {
#ifdef AURORA_PLATFORM_WINDOWS
    impl_ = std::make_unique<Impl>();
    impl_->owner = this;
    if (impl_->create_window()) {
        impl_->update_icon(icon_path);
        impl_->add_icon();
    }
#else
    (void)icon_path;
#endif
}

SystemTray::SystemTray(SystemTray &&other) noexcept
    : impl_(std::move(other.impl_)), tray_title_(std::move(other.tray_title_)),
      tray_balloon_msg_(std::move(other.tray_balloon_msg_)), on_activate_cb_(std::move(other.on_activate_cb_)) {
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->owner = this;
    }
#endif
}

auto SystemTray::operator=(SystemTray &&other) noexcept -> SystemTray & {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        tray_title_ = std::move(other.tray_title_);
        tray_balloon_msg_ = std::move(other.tray_balloon_msg_);
        on_activate_cb_ = std::move(other.on_activate_cb_);
#ifdef AURORA_PLATFORM_WINDOWS
        if (impl_) {
            impl_->owner = this;
        }
#endif
    }
    return *this;
}

SystemTray::~SystemTray() {
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->remove_icon();
        impl_->destroy_window();
        if (impl_->hicon != nullptr) {
            DestroyIcon(impl_->hicon);
        }
    }
#endif
}

void SystemTray::set_title(std::string t) {
    tray_title_ = std::move(t);
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->update_tip();
    }
#endif
}

void SystemTray::set_icon(const std::string &path) const {
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->update_icon(path);
    }
#else
    (void)path;
#endif
}

void SystemTray::show_balloon(const std::string &title, const std::string &msg) {
    tray_balloon_msg_ = msg;
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->show_balloon_impl(title, msg);
    }
#else
    (void)title;
#endif
}

void SystemTray::show() const {
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->add_icon();
    }
#endif
}

void SystemTray::hide() const {
#ifdef AURORA_PLATFORM_WINDOWS
    if (impl_) {
        impl_->remove_icon();
    }
#endif
}

void SystemTray::on_activate(std::function<void()> cb) { on_activate_cb_ = std::move(cb); }

void SystemTray::set_context_menu(std::vector<MenuItem> items) { context_menu_items_ = std::move(items); }

#ifdef AURORA_PLATFORM_WINDOWS
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic,
// cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-type-union-access,
// performance-no-int-to-ptr): Win32 Shell_NotifyIcon/HMENU 句柄与字节搬运不可避免

auto SystemTray::Impl::create_window() -> bool {
    static constexpr const wchar_t *k_class = L"AuroraSystemTrayClass";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SystemTray::Impl::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = k_class;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            AURORA_LOG_WARN("system_tray", "RegisterClassExW failed");
        }
        registered = true;
    }
    hwnd = CreateWindowExW(0, k_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr) {
        AURORA_LOG_WARN("system_tray", "CreateWindowExW(HWND_MESSAGE) failed");
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    return true;
}

void SystemTray::Impl::destroy_window() {
    if (hwnd != nullptr) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
}

auto SystemTray::Impl::add_icon() -> bool {
    if (hwnd == nullptr) {
        return false;
    }
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = m_aurora_callback_mag;
    nid.hIcon = (hicon != nullptr) ? hicon : LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    nid.uVersion = NOTIFYICON_VERSION_4;
    update_tip();
    const BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    if (ok != 0) {
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        visible = true;
    } else {
        AURORA_LOG_WARN("system_tray", "Shell_NotifyIconW(NIM_ADD) failed (possibly no shell session)");
    }
    return ok != FALSE;
}

void SystemTray::Impl::remove_icon() {
    if ((hwnd != nullptr) && visible) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        visible = false;
    }
}

void SystemTray::Impl::update_tip() {
    if (hwnd == nullptr) {
        return;
    }
    const std::wstring tip = aurora::internal::utf8_to_wstr((owner != nullptr) ? owner->tray_title_ : std::string{});
    constexpr size_t k_max = (sizeof(nid.szTip) / sizeof(wchar_t)) - 1;
    // 有界拷贝，避免 wcsncpy 的 MSVC 弃用告警；保证以 null 结尾。
    const wchar_t *src = tip.c_str();
    size_t i = 0;
    for (; i < k_max && src[i] != L'\0'; ++i) {
        nid.szTip[i] = src[i];
    }
    nid.szTip[i] = L'\0';
    if (visible) {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void SystemTray::Impl::update_icon(const std::string &path) {
    if (hicon != nullptr) {
        DestroyIcon(hicon);
        hicon = nullptr;
    }
    if (!path.empty()) {
        hicon = static_cast<HICON>(LoadImageW(nullptr, aurora::internal::utf8_to_wstr(path).c_str(), IMAGE_ICON, 0, 0,
                                              LR_LOADFROMFILE | LR_DEFAULTSIZE));
    }
    if (visible) {
        nid.hIcon = (hicon != nullptr) ? hicon : LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void SystemTray::Impl::show_balloon_impl(const std::string &title, const std::string &msg) {
    if (hwnd == nullptr) {
        return;
    }
    const std::wstring wtitle = aurora::internal::utf8_to_wstr(title);
    const std::wstring wmsg = aurora::internal::utf8_to_wstr(msg);
    constexpr size_t k_max_title = (sizeof(nid.szInfoTitle) / sizeof(wchar_t)) - 1;
    constexpr size_t k_max_msg = (sizeof(nid.szInfo) / sizeof(wchar_t)) - 1;
    // 有界拷贝，避免 wcsncpy 的 MSVC 弃用告警；保证以 null 结尾。
    const wchar_t *ts = wtitle.c_str();
    size_t it = 0;
    for (; it < k_max_title && ts[it] != L'\0'; ++it) {
        nid.szInfoTitle[it] = ts[it];
    }
    nid.szInfoTitle[it] = L'\0';
    const wchar_t *ms = wmsg.c_str();
    size_t im = 0;
    for (; im < k_max_msg && ms[im] != L'\0'; ++im) {
        nid.szInfo[im] = ms[im];
    }
    nid.szInfo[im] = L'\0';
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 0; // 0 → 系统默认超时
    if (visible) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else {
        add_icon();
    }
}

// NOLINTNEXTLINE(modernize-use-trailing-return-type): CALLBACK 调用约定下尾返回类型会改变签名语义
LRESULT CALLBACK SystemTray::Impl::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *impl = reinterpret_cast<SystemTray::Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == SystemTray::Impl::m_aurora_callback_mag) {
        if (impl != nullptr) {
            switch (lp) {
            case WM_LBUTTONUP:
            case NIN_SELECT:
            case NIN_BALLOONUSERCLICK:
            case NIN_KEYSELECT: impl->owner->fire_activate(); break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU: impl->show_context_menu(); break;
            default: break;
            }
        }
        return 0;
    }
    if ((impl != nullptr) && (impl->taskbar_created != 0u) && msg == impl->taskbar_created) {
        impl->add_icon(); // 资源管理器重启后重新添加图标
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

auto SystemTray::Impl::build_hmenu(const std::vector<MenuItem> &items, UINT &cmd_id) -> HMENU {
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return nullptr;
    }
    for (const auto &item : items) {
        if (item.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (!item.enabled) {
            flags |= MF_GRAYED;
        }
        if (item.checkable) {
            flags |= (item.checked ? MF_CHECKED : MF_UNCHECKED);
        }
        if (item.is_submenu()) {
            const HMENU sub = build_hmenu(item.children, cmd_id);
            const std::wstring wlabel = aurora::internal::utf8_to_wstr(item.label);
            AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(sub), wlabel.c_str());
        } else {
            const std::wstring wlabel = aurora::internal::utf8_to_wstr(item.label);
            AppendMenuW(menu, flags, cmd_id, wlabel.c_str());
        }
        ++cmd_id;
    }
    return menu;
}

void SystemTray::Impl::show_context_menu() const {
    if ((hwnd == nullptr) || (owner == nullptr) || owner->context_menu_items_.empty()) {
        return;
    }

    // 扁平化菜单项以便按 cmd_id 索引回调
    std::vector<const MenuItem *> flat;
    std::function<void(const std::vector<MenuItem> &)> flatten;
    flatten = [&](const std::vector<MenuItem> &items) -> void {
        for (const auto &item : items) {
            if (item.separator) {
                continue;
            }
            if (item.is_submenu()) {
                flatten(item.children);
            } else {
                flat.push_back(&item);
            }
        }
    };
    flatten(owner->context_menu_items_);

    UINT cmd_id = WM_APP + 2;
    const HMENU menu = build_hmenu(owner->context_menu_items_, cmd_id);
    if (menu == nullptr) {
        return;
    }

    // TrackPopupMenu 需要屏幕坐标
    POINT pt{};
    GetCursorPos(&pt);

    // 必要：让菜单在失去焦点后正确关闭
    SetForegroundWindow(hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0); // 修复已知 TrackPopupMenu 焦点问题

    DestroyMenu(menu);

    // 分发点击回调
    if (cmd >= WM_APP + 2) {
        const auto idx = static_cast<std::size_t>(cmd - WM_APP - 2);
        if (idx < flat.size() && flat[idx]->on_click) {
            flat[idx]->on_click();
        }
    }
}

#endif // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic,
       // cppcoreguidelines-pro-bounds-constant-array-index, cppcoreguidelines-pro-type-union-access,
       // performance-no-int-to-ptr)
       // AURORA_PLATFORM_WINDOWS

} // namespace aurora

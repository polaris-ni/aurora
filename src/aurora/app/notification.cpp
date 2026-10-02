// notification.cpp — 跨平台系统通知的实现。
//
// 平台作风与 `system_tray_win32.cpp` / `file_dialog_win32.cpp` 一致：本文件在**全部**目标上编译，
// 平台专有代码用 `AURORA_PLATFORM_*`（取自 `core/platform.h`，不是原生 `_WIN32` / `__linux__`）
// 在文件内部分支，公共 API 的定义无条件可见，非目标平台走「记录 + 失败/降级」路径。
//
// 各平台后端：
// - Windows：`Shell_NotifyIconW` 按需添加隐藏托盘图标 + `NIF_INFO` 气球。图标是**临时**的：
//   气球超时 / 被用户关闭 / 被点击，或兜底定时器到点，都会 `NIM_DELETE` 撤掉，不长期占位。
// - Linux：三层运行时降级链，全部 `dlopen` / `popen`，无构建期依赖，也不引任何桌面头：
//   ① `libnotify.so.4` ② `libdbus-1.so.3` 手写 `org.freedesktop.Notifications.Notify`
//   ③ `popen("notify-send")`。三层全败才返回 `NotificationPostFailed`。
// - Headless 回退：只记录 `last_notification()` 并返回成功。
// - macOS / 其它：本轮不实现，返回结构化错误。
//
// 注意：_WIN32_WINNT / _WIN32_IE 必须在任何 aurora 头文件之前定义（见 file_dialog_win32.cpp 注释）。
#include "aurora/core/platform.h"
#ifdef AURORA_PLATFORM_WINDOWS
#ifndef _WIN32_WINNT
// Windows SDK 版本旋钮，不可改名（见 platform.h 例外说明）
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming)
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
// Windows SDK 版本旋钮，不可改名
// NOLINTNEXTLINE(*-macro-usage, *-reserved-identifier, *-identifier-naming)
#define _WIN32_IE 0x0600
#endif
#define WIN32_LEAN_AND_MEAN  // NOLINT(*-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "aurora/app/detail/platform_shell_win32.h"
#include "aurora/app/notification.h"
#include "aurora/core/log.h"

#ifdef AURORA_PLATFORM_WINDOWS
#include <cstddef>
#include <cstring>

#include "aurora/core/utf8.h"
#endif

#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
#include <dlfcn.h>

#include <cstdio>
#include <vector>
#endif

namespace aurora {

namespace {

/// @brief 默认 tag 的占位动作键：通知结构是值语义，`tag` 允许为空，
/// 但 XDG `ActionInvoked` 与 libnotify 的 action key 都要求非空的标识串。
/// 用 `x-` 前缀（freedesktop 保留自定义前缀）避免与真实业务 tag 撞车。
inline constexpr const char *k_default_tag_key = "x-aurora-default-action";

/// @brief  Windows 气球的兜底清理时长（毫秒）与「气球结束后再留的余量」。
inline constexpr std::uint32_t k_default_cleanup_ms = 12000;
inline constexpr std::uint32_t k_cleanup_slack_ms = 2000;

/// @brief 通知中心的进程级状态（互斥保护：`last_notification` 允许跨线程读取）。
struct CenterState {
    std::mutex mutex;  ///< 保护下面三个成员
    std::optional<Notification> last;  ///< 最近一次试图投递的通知
    std::function<void(std::string)> on_activated;  ///< 激活回调
    bool recording_only = false;  ///< 是否处于「仅记录」模式（测试 / headless 注入）
};

/// @brief 进程唯一状态实例（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto center_state() -> CenterState & {
    static CenterState state;
    return state;
}

/// @brief 取通知的有效动作键（空 tag 用占位键兜底）。
/// @param notification 通知内容。
/// @return 用于向桌面服务注册「点击」动作的非空键。
[[nodiscard]] auto action_key(const Notification &notification) -> std::string {
    return notification.tag.empty() ? std::string{k_default_tag_key} : notification.tag;
}

/// @brief 把回调拿到的动作键还原成调用方语义下的 tag（占位键还原为空串）。
/// @param key 桌面服务回传的动作键。
/// @return 与 `Notification::tag` 同语义的 tag。
[[nodiscard]] auto tag_from_action_key(const std::string &key) -> std::string {
    return (key == k_default_tag_key) ? std::string{} : key;
}

/// @brief 转发一次激活事件给已注册的回调（先把回调拷出来再放行，避免在持锁状态下调用用户代码）。
/// @param tag 被点击通知的 tag。
auto dispatch_activated(const std::string &tag) -> void {
    std::function<void(std::string)> callback;
    {
        auto &s = center_state();
        const std::scoped_lock lock{s.mutex};
        callback = s.on_activated;
    }
    if (callback) {
        callback(tag);
    }
}

/// @brief 统一失败出口。
/// @param detail 失败原因（进 `NotificationPostFailed` 的 `{detail}` 占位符）。
/// @return 结构化错误。
[[nodiscard]] auto post_failed(const std::string &detail) -> Result<void> {
    AURORA_LOG_WARN("notification", "post failed: ", detail);
    return make_error(ErrorCode::NotificationPostFailed, ErrorParams{{"detail", detail}});
}

// ───────────────────────────── Windows：临时托盘图标 + 气球 ─────────────────────────────
#ifdef AURORA_PLATFORM_WINDOWS
// 【豁免说明】Win32 托盘接线：句柄须以 reinterpret_cast 在 ShellWindow/HWND 之间往返，
// 回调函数指针的类型擦除同样由平台 API 形状决定，逐点抑制不成比例，故按区间豁免。
// NOLINTBEGIN(*-pro-type-*)

/// @brief 通知专用回调消息（挂 `NOTIFYICONDATAW::uCallbackMessage`）。
/// 与 `system_tray` 的 `WM_APP+1` 拉开距离：两个图标可能挂在同一隐藏窗口上。
inline constexpr UINT k_notify_callback_msg = WM_APP + 0x40;
/// @brief 兜底清理定时器的事件 ID。
inline constexpr UINT_PTR k_notify_timer_id = 0xA0;
/// @brief 通知图标的 `uID`（版本 4 回调把它放进 lParam 高字，用于筛掉同一窗口上的别的图标）。
inline constexpr UINT k_notify_icon_id = 2;

/// @brief Windows 气球的状态：一个进程同时只驻留一条通知（后来的覆盖前一条）。
struct BalloonState {
    HWND hwnd = nullptr;  ///< 承载图标的隐藏消息窗口
    NOTIFYICONDATAW nid{};  ///< 图标/气球描述（`Shell_NotifyIconW` 的三态参数）
    bool added = false;  ///< 图标当前是否已添加到任务栏（决定用 NIM_MODIFY 还是 NIM_ADD）
    bool version4 = false;  ///< `NIM_SETVERSION` 是否生效：决定回调 lParam 的解读方式
    UINT_PTR timer = 0;  ///< 兜底清理定时器（非 0 = 已挂）
    std::uint32_t hook_id = 0;  ///< 挂在 shared hidden window 上的消息钩子 ID
    std::string tag;  ///< 本条通知的 tag（气球被点击时回传给激活回调）
};

/// @brief 进程唯一气球状态。
[[nodiscard]] auto balloon() -> BalloonState & {
    static BalloonState state;
    return state;
}

/// @brief 有界宽字符拷贝：保证以 L'\0' 收尾，且绝不过写目标定长数组的最后一个槽位。
/// @param dst 目标宽字符数组。
/// @param dst_chars `dst` 的槽位数（含结尾空字符）。
/// @param src 源串（UTF-16）。
auto copy_bounded(wchar_t *dst, std::size_t dst_chars, const std::wstring &src) -> void {
    const wchar_t *source = src.c_str();
    std::size_t i = 0;
    while (((i + 1U) < dst_chars) && (source[i] != L'\0')) {
        dst[i] = source[i];
        ++i;
    }
    dst[i] = L'\0';
}

/// @brief 撤掉临时图标与兜底定时器（幂等）。
///
/// 气球超时（`NIN_BALLOONTIMEOUT`）、被用户关闭（`NIN_BALLOONHIDE`）、被点击
/// （`NIN_BALLOONUSERCLICK`）以及定时器到点都会走到这里：本进程没有常驻托盘诉求，
/// 通知图标不允许长期占位。
auto balloon_cleanup() -> void {
    auto &b = balloon();
    if ((b.hwnd != nullptr) && b.added) {
        Shell_NotifyIconW(NIM_DELETE, &b.nid);
        b.added = false;
        b.version4 = false;
    }
    if ((b.hwnd != nullptr) && (b.timer != 0U)) {
        KillTimer(b.hwnd, b.timer);
        b.timer = 0;
    }
    b.tag.clear();
}

/// @brief 隐藏消息窗口的消息钩子：把回调消息里的 balloon 事件折成「清理 + 激活回调」。
/// @param message 原生消息编号。
/// @param payload 消息的 WPARAM / LPARAM。
/// @return 消息已被消费时为 `true`。
auto balloon_hook(std::uint32_t message, const internal::ShellMessage &payload) -> bool {
    auto &b = balloon();
    if (message == WM_TIMER) {
        if (static_cast<UINT_PTR>(payload.wparam) == k_notify_timer_id) {
            balloon_cleanup();  // 兜底：服务器没吐超时事件时也要把图标撤掉
            return true;
        }
        return false;
    }
    if (message != k_notify_callback_msg) {
        return false;
    }
    const auto raw = static_cast<DWORD_PTR>(payload.lparam);
    const UINT event = LOWORD(raw);
    const UINT icon_id = HIWORD(raw);
    if (b.version4 && (icon_id != k_notify_icon_id)) {
        return false;  // 同一窗口上别的图标发的回调，不归通知模块管
    }
    if (event == NIN_BALLOONUSERCLICK) {
        const std::string tag = b.tag;
        balloon_cleanup();  // 先撤图标：回调里若再发通知，可以从干净状态重新开始
        dispatch_activated(tag_from_action_key(tag));
        return true;
    }
    if ((event == NIN_BALLOONTIMEOUT) || (event == NIN_BALLOONHIDE)) {
        balloon_cleanup();
        return true;
    }
    return false;
}

/// @brief 把通知投递到 Windows 通知区域（按需添加临时图标 + 气球）。
/// @param notification 通知内容。
/// @return 成功为 `Ok`；失败为 `NotificationPostFailed`。
[[nodiscard]] auto win_post(const Notification &notification) -> Result<void> {
    auto &b = balloon();
    if (b.hook_id == 0U) {
        b.hook_id = internal::add_message_hook(&balloon_hook);
        if (b.hook_id == 0U) {
            return post_failed("the shared Win32 message window is unavailable (no hidden window to host the balloon)");
        }
    }
    if (b.hwnd == nullptr) {
        internal::ShellWindow window = internal::ensure_message_window();
        if (window == nullptr) {
            return post_failed("the shared Win32 message window could not be created");
        }
        b.hwnd = static_cast<HWND>(window);
    }

    NOTIFYICONDATAW &nid = b.nid;
    std::memset(&nid, 0, sizeof(nid));  // 上一次的长字符串不残留到本次
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = b.hwnd;
    nid.uID = k_notify_icon_id;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    nid.uCallbackMessage = k_notify_callback_msg;
    nid.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay): 定长数组成员无法取 Span
    copy_bounded(nid.szTip, (sizeof(nid.szTip) / sizeof(wchar_t)), internal::utf8_to_wstr("Aurora"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay): 定长数组成员无法取 Span
    copy_bounded(nid.szInfoTitle, (sizeof(nid.szInfoTitle) / sizeof(wchar_t)),
                 internal::utf8_to_wstr(notification.title));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay): 定长数组成员无法取 Span
    copy_bounded(nid.szInfo, (sizeof(nid.szInfo) / sizeof(wchar_t)), internal::utf8_to_wstr(notification.body));

    // 紧急度 → 提示旗标：Low 不出声；Critical 不加 `NIIF_RESPECT_QUIET_TIME`，即不受专注时段抑制。
    UINT info_flags = NIIF_INFO;
    if (notification.urgency == NotificationUrgency::Low) {
        info_flags |= NIIF_NOSOUND;
    }
    nid.dwInfoFlags = info_flags;
    nid.uTimeout = static_cast<UINT>(notification.timeout_ms);  // 0 → 系统默认驻留时长
    nid.uVersion = NOTIFYICON_VERSION_4;

    BOOL ok = FALSE;
    if (b.added) {
        ok = Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else {
        ok = Shell_NotifyIconW(NIM_ADD, &nid);
        if (ok != FALSE) {
            b.added = true;
            b.version4 = Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
        }
    }
    if (ok == FALSE) {
        return post_failed(std::string("Shell_NotifyIconW(NIM_ADD/NIM_MODIFY) failed, GetLastError=") +
                           std::to_string(GetLastError()));
    }

    b.tag = action_key(notification);
    if (b.timer != 0U) {
        KillTimer(b.hwnd, b.timer);  // 覆盖上一条通知：定时器按新的驻留时长重挂
        b.timer = 0;
    }
    const std::uint32_t wait_ms =
        (notification.timeout_ms == 0U) ? k_default_cleanup_ms : (notification.timeout_ms + k_cleanup_slack_ms);
    if (SetTimer(b.hwnd, k_notify_timer_id, static_cast<UINT>(wait_ms), nullptr) == 0) {
        AURORA_LOG_WARN("notification", "SetTimer failed; relying on NIN_BALLOONTIMEOUT for icon cleanup");
    } else {
        b.timer = k_notify_timer_id;
    }
    return Result<void>{};
}
// NOLINTEND(*-pro-type-*)
#endif

// ───────────────────────────── Linux：三层运行时降级链 ─────────────────────────────
// 本 TU **刻意不引** <libnotify/notify.h> 与 <dbus/dbus.h>：全功能都在运行时 `dlsym` 拿到，
// 这样仓库既不绑定桌面依赖，也不需要本机装这些开发包（CI 与 Windows 开发机同样能编译本文件）。
// 代价是要自己声明符号原型与 ABI 布局 —— 下面每处都标注了取值来源。
#if defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
// 【豁免说明】FFI 边界：`dlsym` 只能返回 `void *`，转函数指针必然是 reinterpret_cast；
// 同理 GError 的各个字段要在未提供 GLib 声明的情况下读取。逐点抑制不成比例，故按区间豁免。
// NOLINTBEGIN(*-pro-type-*)

// ---- 公共小工具 ----

/// @brief 把符号解析成函数指针。
/// @tparam Fn 目标函数指针类型。
/// @param handle `dlopen` 返回的库句柄。
/// @param name 符号名（C 名字，未改编）。
/// @return 符号地址；符号不存在时为 nullptr。
template <typename Fn>
[[nodiscard]] auto resolve(void *handle, const char *name) -> Fn {
    void *symbol = dlsym(handle, name);  // POSIX 规定返回 void*，无其它途径取函数指针
    return reinterpret_cast<Fn>(symbol);
}

/// @brief POSIX 单引号转义：把任意字节串安全地放进外壳命令行。
/// @param raw 原始字符串。
/// @return 已加单引号、内部单引号按 `'\''` 规则转义的片段。
[[nodiscard]] auto shell_quote(const std::string &raw) -> std::string {
    std::string out{"'"};
    for (const char c : raw) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

// ---- ① libnotify ----

// libnotify 的 ABI（取自 libnotify 0.7 的 notification.h / notify.h，长期稳定）：
//   gboolean notify_init(const char *app_name);
//   gboolean notify_is_initted(void);
//   NotifyNotification *notify_notification_new(const char *summary, const char *body, const char *icon);
//   void notify_notification_set_urgency(NotifyNotification *, NotifyUrgency);   // LOW=0 NORMAL=1 CRITICAL=2
//   void notify_notification_set_timeout(NotifyNotification *, gint timeout_ms);
//   void notify_notification_add_action(NotifyNotification *, const char *action, const char *label,
//                                       NotifyActionCallback cb, gpointer user_data, GDestroyNotify free_func);
//   gboolean notify_notification_show(NotifyNotification *, GError **error);
using FnNotifyInit = int (*)(const char *);
using FnNotifyIsInitted = int (*)(void);
using FnNotifyNew = void *(*)(const char *, const char *, const char *);
using FnNotifySetUrgency = void (*)(void *, int);
using FnNotifySetTimeout = void (*)(void *, int);
using FnNotifyAddAction = void (*)(void *, const char *, const char *, void (*)(void *, char *, void *), void *,
                                  void (*)(void *));
using FnNotifyShow = int (*)(void *, void **);

struct NotifyApi {
    void *handle = nullptr;  ///< `dlopen` 句柄（进程内长期持有）
    FnNotifyInit init = nullptr;  ///< `notify_init`
    FnNotifyIsInitted is_initted = nullptr;  ///< `notify_is_initted`
    FnNotifyNew create = nullptr;  ///< `notify_notification_new`
    FnNotifySetUrgency set_urgency = nullptr;  ///< `notify_notification_set_urgency`
    FnNotifySetTimeout set_timeout = nullptr;  ///< `notify_notification_set_timeout`
    FnNotifyAddAction add_action = nullptr;  ///< `notify_notification_add_action`
    FnNotifyShow show = nullptr;  ///< `notify_notification_show`
    bool tried = false;  ///< 是否已尝试过加载（不重复 dlopen）
};

/// @brief libnotify / GLib 侧的进程级状态。
struct LinuxNotifyState {
    NotifyApi notify;  ///< libnotify 符号表
    void *glib_handle = nullptr;  ///< `dlopen("libglib-2.0.so.0")` 句柄（取 GLib 工具函数用）
    void (*g_object_unref)(void *) = nullptr;  ///< `g_object_unref`：回收通知对象
    void (*g_error_free)(void *) = nullptr;  ///< `g_error_free`：回收 GError
    int (*main_context_iteration)(void *, int) = nullptr;  ///< `g_main_context_iteration`：主循环排空
    bool glib_tried = false;  ///< 是否已尝试过加载 GLib
    std::vector<void *> live;  ///< 已投递、仍需存活以接收动作回调的通知对象（有上限）
    std::vector<std::pair<std::uint32_t, std::string>> dbus_pending;  ///< libdbus 层：通知 id → tag
};

/// @brief 进程唯一 Linux 状态。
[[nodiscard]] auto linux_state() -> LinuxNotifyState & {
    static LinuxNotifyState state;
    return state;
}

/// @brief GLib 的 `GError`（GLib 公开 ABI：`{ GQuark domain; gint code; gchar *message; }`）。
/// 本 TU 只把它当不透明出参 + 读 `message`，故不依赖 GLib 头文件。
struct LibnotifyError {
    std::uint32_t domain = 0;  ///< 错误域（GQuark）
    std::int32_t code = 0;  ///< 错误码
    const char *message = nullptr;  ///< 人类可读描述（可为 null）
};

/// @brief `NotifyUrgency` → 整值（libnotify 与 XDG `urgency` 提示同取值，可共用一个映射）。
/// @param urgency 抽象紧急度。
/// @return 0 / 1 / 2。
[[nodiscard]] auto urgency_value(NotificationUrgency urgency) -> std::uint8_t {
    if (urgency == NotificationUrgency::Low) {
        return 0;
    }
    if (urgency == NotificationUrgency::Critical) {
        return 2;
    }
    return 1;
}

/// @brief 供 libnotify 在用户点通知时回调：把动作键折回 tag 并转发给上层回调。
/// @param notification 被点击的通知对象（未使用）。
/// @param key libnotify 回传的动作键（即 `add_action` 时登记的 action）。
/// @param user_data 注册时携带的数据（未使用）。
auto libnotify_action_callback(void *notification, char *key, void *user_data) -> void {
    (void)notification;
    (void)user_data;
    if (key != nullptr) {
        dispatch_activated(tag_from_action_key(std::string{key}));
    }
}

/// @brief 登记一个仍需存活的通知对象；超出上限时按 FIFO 回收最老的（回调失效但不泄漏）。
/// @param handle `notify_notification_new` 返回的对象指针。
auto remember_live(void *handle) -> void {
    LinuxNotifyState &s = linux_state();
    constexpr std::size_t k_max_live = 64;
    // 取舍说明：无法确定桌面服务器何时把通知收回（`closed` 信号也要靠主循环才到），
    // 故用有上限的 FIFO 兜底；超限时释放最老对象的引用，宁可它收不到点击也不无限堆积。
    s.live.push_back(handle);
    if (s.live.size() > k_max_live) {
        void *oldest = s.live.front();
        s.live.erase(s.live.begin());
        if ((s.g_object_unref != nullptr) && (oldest != nullptr)) {
            s.g_object_unref(oldest);
        }
    }
}

/// @brief 取 GLib 的辅助符号（失败全都留在 null，调用方据此降级）。
auto load_glib() -> void {
    LinuxNotifyState &s = linux_state();
    if (s.glib_tried) {
        return;
    }
    s.glib_tried = true;
    s.glib_handle = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (s.glib_handle == nullptr) {
        return;
    }
    s.g_object_unref = resolve<void (*)(void *)>(s.glib_handle, "g_object_unref");
    s.g_error_free = resolve<void (*)(void *)>(s.glib_handle, "g_error_free");
    s.main_context_iteration = resolve<int (*)(void *, int)>(s.glib_handle, "g_main_context_iteration");
}

/// @brief 加载 libnotify 符号表（幂等；失败后不再重试）。
/// @return 可用时为符号表地址；库缺失或关键符号缺失时为 nullptr。
[[nodiscard]] auto load_libnotify() -> NotifyApi * {
    LinuxNotifyState &s = linux_state();
    NotifyApi &api = s.notify;
    if (api.tried) {
        return api.show != nullptr ? &api : nullptr;
    }
    api.tried = true;
    api.handle = dlopen("libnotify.so.4", RTLD_LAZY | RTLD_LOCAL);
    if (api.handle == nullptr) {
        return nullptr;
    }
    api.init = resolve<FnNotifyInit>(api.handle, "notify_init");
    api.is_initted = resolve<FnNotifyIsInitted>(api.handle, "notify_is_initted");
    api.create = resolve<FnNotifyNew>(api.handle, "notify_notification_new");
    api.set_urgency = resolve<FnNotifySetUrgency>(api.handle, "notify_notification_set_urgency");
    api.set_timeout = resolve<FnNotifySetTimeout>(api.handle, "notify_notification_set_timeout");
    api.add_action = resolve<FnNotifyAddAction>(api.handle, "notify_notification_add_action");
    api.show = resolve<FnNotifyShow>(api.handle, "notify_notification_show");
    if ((api.init == nullptr) || (api.create == nullptr) || (api.show == nullptr)) {
        AURORA_LOG_WARN("notification", "libnotify.so.4 loaded but a required symbol is missing");
        return nullptr;
    }
    return &api;
}

/// @brief 经 libnotify 投递一条通知。
/// @param notification 通知内容。
/// @return 成功为 `Ok`；失败为 `NotificationPostFailed`（detail 含 libnotify 给出的原因）。
[[nodiscard]] auto post_via_libnotify(const Notification &notification) -> Result<void> {
    NotifyApi *api = load_libnotify();
    if (api == nullptr) {
        return post_failed("libnotify.so.4 unavailable");
    }
    if ((api->is_initted == nullptr) || (api->is_initted() == 0)) {
        if (api->init("Aurora") == 0) {
            return post_failed("notify_init failed");
        }
    }
    void *handle = api->create(notification.title.c_str(), notification.body.c_str(), nullptr);
    if (handle == nullptr) {
        return post_failed("notify_notification_new returned null");
    }
    remember_live(handle);  // 动作回调是异步的：对象必须活到被点击为止
    if (api->set_urgency != nullptr) {
        api->set_urgency(handle, static_cast<int>(urgency_value(notification.urgency)));
    }
    if ((api->set_timeout != nullptr) && (notification.timeout_ms != 0U)) {
        api->set_timeout(handle, static_cast<int>(notification.timeout_ms));
    }
    const std::string key = action_key(notification);
    if (api->add_action != nullptr) {
        // user_data / free_func 都留空：动作键本身已携带 tag，无需额外分配，也就没有悬垂数据风险。
        api->add_action(handle, key.c_str(), "default", &libnotify_action_callback, nullptr, nullptr);
    }

    LibnotifyError *error = nullptr;
    const int shown = api->show(handle, reinterpret_cast<void **>(&error));
    if (shown == 0) {
        std::string detail = "notify_notification_show failed";
        if ((error != nullptr) && (error->message != nullptr)) {
            detail += std::string(": ") + error->message;
        }
        LinuxNotifyState &s = linux_state();
        if ((s.g_error_free != nullptr) && (error != nullptr)) {
            s.g_error_free(error);
        }
        return post_failed(detail);
    }
    return Result<void>{};
}

// ---- ② libdbus-1：手写 org.freedesktop.Notifications.Notify ----

// libdbus 的 ABI（取自 dbus 1.x 的 dbus-bus.h / dbus-message.h / dbus-connection.h，长期稳定）。
// `DBusConnection` / `DBusMessage` / `DBusPendingCall` 是不透明类型：用不完整的 struct 前向声明即可
// （地址对齐，永不解引用）。而 `DBusMessageIter` / `DBusError` 是**按值**传递的公开结构，
// 故用足够大的对齐块替代 —— 只作栈上暂存区经 API 往返，绝不读其内部字段。
struct LibdbusConnection;  ///< 不透明：`DBusConnection`
struct LibdbusMessage;  ///< 不透明：`DBusMessage`

/// @brief 替代 `DBusMessageIter`：真实结构 64 位下约 72 字节，128 字节留足余量。
struct LibdbusMessageIter {
    std::uint64_t opaque[16];  ///< 不透明暂存区（必须零初始化前 72 字节，见下方 `LibdbusMessageIter{}`）
};

/// @brief 替代 `DBusError`：真实结构 64 位下约 24 字节，64 字节留足余量。
struct LibdbusError {
    std::uint64_t opaque[8];  ///< 不透明暂存区
};

inline constexpr int k_dbus_bus_session = 0;  ///< `DBusBusType::DBUS_BUS_SESSION`
inline constexpr int k_dbus_type_byte = 'y';  ///< DBUS_TYPE_BYTE
inline constexpr int k_dbus_type_boolean = 'b';  ///< DBUS_TYPE_BOOLEAN
inline constexpr int k_dbus_type_int32 = 'i';  ///< DBUS_TYPE_INT32
inline constexpr int k_dbus_type_uint32 = 'u';  ///< DBUS_TYPE_UINT32
inline constexpr int k_dbus_type_string = 's';  ///< DBUS_TYPE_STRING
inline constexpr int k_dbus_type_array = 'a';  ///< DBUS_TYPE_ARRAY
inline constexpr int k_dbus_type_dict_entry = 'e';  ///< DBUS_TYPE_DICT_ENTRY
inline constexpr int k_dbus_type_variant = 'v';  ///< DBUS_TYPE_VARIANT
inline constexpr int k_dbus_message_type_error = 3;  ///< DBUS_MESSAGE_TYPE_ERROR
inline constexpr int k_dbus_message_type_method_return = 2;  ///< DBUS_MESSAGE_TYPE_METHOD_RETURN

using FnDbusErrorInit = void (*)(void *);
using FnDbusErrorFree = void (*)(void *);
using FnDbusBusGet = void *(*)(int, void *);
using FnDbusBusAddMatch = int (*)(void *, const char *, void *);
using FnDbusMessageNewMethodCall = void *(*)(const char *, const char *, const char *, const char *);
using FnDbusMessageUnref = void (*)(void *);
using FnDbusMessageIterInitAppend = void (*)(void *, void *);
using FnDbusMessageIterAppendBasic = int (*)(void *, int, const void *);
using FnDbusMessageIterOpenContainer = int (*)(void *, int, const char *, void *);
using FnDbusMessageIterCloseContainer = int (*)(void *, void *);
using FnDbusConnectionSendReplyBlock = void *(*)(void *, void *, int, void *);
using FnDbusMessageGetType = int (*)(void *);
using FnDbusMessageIterInit = int (*)(void *, void *);
using FnDbusMessageIterGetArgType = int (*)(void *);
using FnDbusMessageIterGetBasic = void (*)(void *, void *);
using FnDbusMessageIterNext = int (*)(void *);
using FnDbusMessageIsSignal = int (*)(void *, const char *, const char *);
using FnDbusConnectionReadWrite = int (*)(void *, int);
using FnDbusConnectionPopMessage = void *(*)(void *);

struct DbusApi {
    void *handle = nullptr;  ///< `dlopen` 句柄
    LibdbusConnection *connection = nullptr;  ///< 复用的会话总线连接
    bool tried = false;  ///< 是否已尝试过加载
    bool match_added = false;  ///< `ActionInvoked` / `NotificationClosed` 的匹配规则是否已登记
    FnDbusErrorInit error_init = nullptr;  ///< `dbus_error_init`
    FnDbusErrorFree error_free = nullptr;  ///< `dbus_error_free`
    FnDbusBusGet bus_get = nullptr;  ///< `dbus_bus_get`
    FnDbusBusAddMatch bus_add_match = nullptr;  ///< `dbus_bus_add_match`
    FnDbusMessageNewMethodCall message_new_call = nullptr;  ///< `dbus_message_new_method_call`
    FnDbusMessageUnref message_unref = nullptr;  ///< `dbus_message_unref`
    FnDbusMessageIterInitAppend iter_init_append = nullptr;  ///< `dbus_message_iter_init_append`
    FnDbusMessageIterAppendBasic iter_append_basic = nullptr;  ///< `dbus_message_iter_append_basic`
    FnDbusMessageIterOpenContainer iter_open = nullptr;  ///< `dbus_message_iter_open_container`
    FnDbusMessageIterCloseContainer iter_close = nullptr;  ///< `dbus_message_iter_close_container`
    FnDbusConnectionSendReplyBlock send_blocking = nullptr;  ///< `dbus_connection_send_with_reply_and_block`
    FnDbusMessageGetType message_get_type = nullptr;  ///< `dbus_message_get_type`
    FnDbusMessageIterInit iter_init = nullptr;  ///< `dbus_message_iter_init`
    FnDbusMessageIterGetArgType iter_arg_type = nullptr;  ///< `dbus_message_iter_get_arg_type`
    FnDbusMessageIterGetBasic iter_get_basic = nullptr;  ///< `dbus_message_iter_get_basic`
    FnDbusMessageIterNext iter_next = nullptr;  ///< `dbus_message_iter_next`
    FnDbusMessageIsSignal message_is_signal = nullptr;  ///< `dbus_message_is_signal`
    FnDbusConnectionReadWrite read_write = nullptr;  ///< `dbus_connection_read_write`
    FnDbusConnectionPopMessage pop_message = nullptr;  ///< `dbus_connection_pop_message`
};

/// @brief 进程唯一 libdbus 状态。
[[nodiscard]] auto dbus_api() -> DbusApi & {
    static DbusApi api;
    return api;
}

/// @brief `DBusError` 是否已被置位（等效 `dbus_error_is_set`：判 `name != NULL`）。
/// @param error 已由 `dbus_error_init` 初始化过的错误块。
/// @return 已置位为真。
[[nodiscard]] auto dbus_error_set(const LibdbusError &error) -> bool {
    // DBusError 的首个成员是 `const char *name`：libdbus 的错误一笔就是这个指针非空。
    const auto *words = reinterpret_cast<const void *const *>(&error);
    return words[0] != nullptr;
}

/// @brief 取出错误描述；拿不到（未置位 / 版本差异）时返回空串。
/// @param error 已由 libdbus 填充的错误块。
/// @return 描述串的副本；不可得时为空串。
[[nodiscard]] auto dbus_detail_or_unknown(const LibdbusError &error, std::string fallback) -> std::string {
    if (!dbus_error_set(error)) {
        return fallback;
    }
    const auto *fields = reinterpret_cast<const char *const *>(&error);
    return (fields[1] != nullptr) ? std::string{fields[1]} : fallback;
}

/// @brief 加载 libdbus 符号表（幂等；失败后不再重试）。
/// @return 可用时为符号表地址；库缺失或关键符号缺失时为 nullptr。
[[nodiscard]] auto load_libdbus() -> DbusApi * {
    DbusApi &api = dbus_api();
    if (api.tried) {
        return api.send_blocking != nullptr ? &api : nullptr;
    }
    api.tried = true;
    api.handle = dlopen("libdbus-1.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (api.handle == nullptr) {
        return nullptr;
    }
    api.error_init = resolve<FnDbusErrorInit>(api.handle, "dbus_error_init");
    api.error_free = resolve<FnDbusErrorFree>(api.handle, "dbus_error_free");
    api.bus_get = resolve<FnDbusBusGet>(api.handle, "dbus_bus_get");
    api.bus_add_match = resolve<FnDbusBusAddMatch>(api.handle, "dbus_bus_add_match");
    api.message_new_call = resolve<FnDbusMessageNewMethodCall>(api.handle, "dbus_message_new_method_call");
    api.message_unref = resolve<FnDbusMessageUnref>(api.handle, "dbus_message_unref");
    api.iter_init_append = resolve<FnDbusMessageIterInitAppend>(api.handle, "dbus_message_iter_init_append");
    api.iter_append_basic = resolve<FnDbusMessageIterAppendBasic>(api.handle, "dbus_message_iter_append_basic");
    api.iter_open = resolve<FnDbusMessageIterOpenContainer>(api.handle, "dbus_message_iter_open_container");
    api.iter_close = resolve<FnDbusMessageIterCloseContainer>(api.handle, "dbus_message_iter_close_container");
    api.send_blocking = resolve<FnDbusConnectionSendReplyBlock>(api.handle, "dbus_connection_send_with_reply_and_block");
    api.message_get_type = resolve<FnDbusMessageGetType>(api.handle, "dbus_message_get_type");
    api.iter_init = resolve<FnDbusMessageIterInit>(api.handle, "dbus_message_iter_init");
    api.iter_arg_type = resolve<FnDbusMessageIterGetArgType>(api.handle, "dbus_message_iter_get_arg_type");
    api.iter_get_basic = resolve<FnDbusMessageIterGetBasic>(api.handle, "dbus_message_iter_get_basic");
    api.iter_next = resolve<FnDbusMessageIterNext>(api.handle, "dbus_message_iter_next");
    api.message_is_signal = resolve<FnDbusMessageIsSignal>(api.handle, "dbus_message_is_signal");
    api.read_write = resolve<FnDbusConnectionReadWrite>(api.handle, "dbus_connection_read_write");
    api.pop_message = resolve<FnDbusConnectionPopMessage>(api.handle, "dbus_connection_pop_message");
    if ((api.error_init == nullptr) || (api.bus_get == nullptr) || (api.message_new_call == nullptr) ||
        (api.send_blocking == nullptr) || (api.iter_init_append == nullptr) || (api.iter_append_basic == nullptr) ||
        (api.iter_open == nullptr) || (api.iter_close == nullptr) || (api.message_unref == nullptr)) {
        AURORA_LOG_WARN("notification", "libdbus-1.so.3 loaded but a required symbol is missing");
        return nullptr;
    }
    return &api;
}

/// @brief 取（必要时建）复用的会话总线连接。
/// @return 连接指针；失败时为 nullptr。
[[nodiscard]] auto dbus_session() -> LibdbusConnection * {
    DbusApi *api = load_libdbus();
    if (api == nullptr) {
        return nullptr;
    }
    if (api->connection != nullptr) {
        return api->connection;
    }
    LibdbusError error{};
    api->error_init(&error);
    void *raw = api->bus_get(k_dbus_bus_session, &error);
    const std::string detail = dbus_detail_or_unknown(error, "unknown reason");
    if (api->error_free != nullptr) {
        api->error_free(&error);
    }
    if (raw == nullptr) {
        AURORA_LOG_WARN("notification", "dbus_bus_get(DBUS_BUS_SESSION) failed: ", detail);
        return nullptr;
    }
    // 刻意不 `dbus_connection_unref`：`dbus_bus_get` 返回的是 libdbus 侧缓存的共享连接，
    // 其引用归属在不同 libdbus 版本间口径不一；漏 ref 顶多泄漏一条连接，错 unref 则是堆破坏。
    api->connection = static_cast<LibdbusConnection *>(raw);
    return api->connection;
}

/// @brief 追加一条 `a{sv}` 提示。
/// @param hints 已打开的字典迭代器。
/// @param key 提示名。
/// @param signature 值的 DBus 类型签名（单个字符）。
/// @param value 值载荷的地址。
/// @return 全部追加成功为真。
auto append_hint(void *hints, const char *key, const char *signature, const void *value) -> bool {
    DbusApi &api = dbus_api();
    LibdbusMessageIter entry{};
    LibdbusMessageIter variant{};
    if (api.iter_open(hints, k_dbus_type_dict_entry, nullptr, &entry) == 0) {
        return false;
    }
    const char *key_ptr = key;
    if (api.iter_append_basic(&entry, k_dbus_type_string, &key_ptr) == 0) {
        return false;
    }
    if (api.iter_open(&entry, k_dbus_type_variant, signature, &variant) == 0) {
        return false;
    }
    const bool value_ok = api.iter_append_basic(&variant, static_cast<int>(signature[0]), value) != 0;
    (void)api.iter_close(&entry, &variant);
    (void)api.iter_close(hints, &entry);
    return value_ok;
}

/// @brief 经原始 libdbus 调用 `org.freedesktop.Notifications.Notify`。
/// @param notification 通知内容。
/// @return 成功为 `Ok`；失败为 `NotificationPostFailed`。
[[nodiscard]] auto post_via_libdbus(const Notification &notification) -> Result<void> {
    DbusApi *api = load_libdbus();
    if (api == nullptr) {
        return post_failed("libdbus-1.so.3 unavailable");
    }
    LibdbusConnection *connection = dbus_session();
    if (connection == nullptr) {
        return post_failed("no session bus connection");
    }

    void *message = api->message_new_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                          "org.freedesktop.Notifications", "Notify");
    if (message == nullptr) {
        return post_failed("dbus_message_new_method_call returned null");
    }

    // Notify(s app_name, u replaces_id, s app_icon, s summary, s body, as actions, a{sv} hints, i timeout)
    LibdbusMessageIter top{};
    LibdbusMessageIter actions{};
    LibdbusMessageIter hints{};
    api->iter_init_append(message, &top);

    const char *app_name = "Aurora";
    const char *app_icon = "";
    const char *summary = notification.title.c_str();
    const char *body = notification.body.c_str();
    std::uint32_t replaces_id = 0;
    bool shaped = true;
    shaped = (api->iter_append_basic(&top, k_dbus_type_string, &app_name) != 0) && shaped;
    shaped = (api->iter_append_basic(&top, k_dbus_type_uint32, &replaces_id) != 0) && shaped;
    shaped = (api->iter_append_basic(&top, k_dbus_type_string, &app_icon) != 0) && shaped;
    shaped = (api->iter_append_basic(&top, k_dbus_type_string, &summary) != 0) && shaped;
    shaped = (api->iter_append_basic(&top, k_dbus_type_string, &body) != 0) && shaped;

    // actions = ["default", <label>]：多数桌面把 `default` 当作「点通知本体」的动作，点击即触发。
    const char *action_key_cstr = "default";
    const char *action_label = "";
    if (api->iter_open(&top, k_dbus_type_array, "s", &actions) != 0) {
        (void)api->iter_append_basic(&actions, k_dbus_type_string, &action_key_cstr);
        (void)api->iter_append_basic(&actions, k_dbus_type_string, &action_label);
        shaped = (api->iter_close(&top, &actions) != 0) && shaped;
    } else {
        shaped = false;
    }

    // hints：urgency(0..2) / category / transient —— 都按 XDG 规范放进 `a{sv}`。
    const std::uint8_t urgency = urgency_value(notification.urgency);
    const char *category_value = "im.received";
    const int transient_value = (notification.urgency == NotificationUrgency::Low) ? 1 : 0;
    if (api->iter_open(&top, k_dbus_type_array, "{sv}", &hints) != 0) {
        shaped = append_hint(&hints, "urgency", "y", &urgency) && shaped;
        shaped = append_hint(&hints, "category", "s", &category_value) && shaped;
        shaped = append_hint(&hints, "transient", "b", &transient_value) && shaped;
        shaped = (api->iter_close(&top, &hints) != 0) && shaped;
    } else {
        shaped = false;
    }

    // timeout：-1 = 交给服务器策略（「平台默认」），0 = 永不超时（本 API 不产生这种取值）。
    const std::int32_t expire_timeout =
        (notification.timeout_ms == 0U) ? -1 : static_cast<std::int32_t>(notification.timeout_ms);
    shaped = (api->iter_append_basic(&top, k_dbus_type_int32, &expire_timeout) != 0) && shaped;
    if (!shaped) {
        api->message_unref(message);
        return post_failed("failed to marshal the Notify arguments");
    }

    LibdbusError error{};
    api->error_init(&error);
    void *reply = api->send_blocking(connection, message, 3000, &error);
    api->message_unref(message);
    if (reply == nullptr) {
        std::string detail = "Notify did not answer";
        if (dbus_error_set(error)) {
            detail = dbus_detail_or_unknown(error, detail);
        }
        if (api->error_free != nullptr) {
            api->error_free(&error);
        }
        return post_failed(detail);
    }
    if (api->error_free != nullptr) {
        api->error_free(&error);
    }

    std::uint32_t id = 0;
    const int kind = api->message_get_type(reply);
    LibdbusMessageIter response{};
    if ((kind == k_dbus_message_type_method_return) && (api->iter_init != nullptr) && (api->iter_arg_type != nullptr) &&
        (api->iter_get_basic != nullptr) && (api->iter_init(reply, &response) != 0) &&
        (api->iter_arg_type(&response) == k_dbus_type_uint32)) {
        api->iter_get_basic(&response, &id);
    }
    api->message_unref(reply);
    if ((kind == k_dbus_message_type_error) || (id == 0U)) {
        return post_failed("the notification server rejected the Notify call");
    }

    // 排队等待 `ActionInvoked(id, action_key)`：宿主 `pump_events()` 时会读到它。
    linux_state().dbus_pending.emplace_back(id, notification.tag);
    constexpr std::size_t k_max_pending = 64;
    if (linux_state().dbus_pending.size() > k_max_pending) {
        linux_state().dbus_pending.erase(linux_state().dbus_pending.begin());
    }
    if (!api->match_added && (api->bus_add_match != nullptr)) {
        LibdbusError match_error{};
        api->error_init(&match_error);
        api->bus_add_match(connection, "type='signal',interface='org.freedesktop.Notifications',member='ActionInvoked'",
                           &match_error);
        if (api->error_free != nullptr) {
            api->error_free(&match_error);
        }
        LibdbusError close_error{};
        api->error_init(&close_error);
        api->bus_add_match(connection,
                           "type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'",
                           &close_error);
        if (api->error_free != nullptr) {
            api->error_free(&close_error);
        }
        api->match_added = true;
    }
    return Result<void>{};
}

/// @brief 取出并注销一个待处理通知 id 的 tag。
/// @param id 桌面服务返回的通知 id。
/// @param out_tag 命中时写入的 tag。
/// @return 该 id 在等待表中为真。
[[nodiscard]] auto take_pending_tag(std::uint32_t id, std::string &out_tag) -> bool {
    std::vector<std::pair<std::uint32_t, std::string>> &pending = linux_state().dbus_pending;
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it->first == id) {
            out_tag = std::move(it->second);
            pending.erase(it);
            return true;
        }
    }
    return false;
}

/// @brief 处理一条已出队的 dbus 消息（只看 XDG 通知的 `ActionInvoked` / `NotificationClosed`）。
/// @param raw 消息指针（由本函数之外的调用方负责 unref）。
auto handle_dbus_signal(void *raw) -> void {
    DbusApi &api = dbus_api();
    if ((api.message_is_signal == nullptr) || (api.iter_init == nullptr) || (api.iter_arg_type == nullptr) ||
        (api.iter_get_basic == nullptr) || (api.iter_next == nullptr)) {
        return;
    }
    constexpr const char *k_iface = "org.freedesktop.Notifications";
    LibdbusMessageIter body{};
    if ((api.message_is_signal(raw, k_iface, "ActionInvoked") != 0) && (api.iter_init(raw, &body) != 0) &&
        (api.iter_arg_type(&body) == k_dbus_type_uint32)) {
        std::uint32_t id = 0;
        api.iter_get_basic(&body, &id);
        (void)api.iter_next(&body);
        std::string tag;
        if (take_pending_tag(id, tag)) {
            dispatch_activated(tag);
        }
        return;
    }
    if ((api.message_is_signal(raw, k_iface, "NotificationClosed") != 0) && (api.iter_init(raw, &body) != 0) &&
        (api.iter_arg_type(&body) == k_dbus_type_uint32)) {
        std::uint32_t id = 0;
        api.iter_get_basic(&body, &id);
        std::string ignored;
        (void)take_pending_tag(id, ignored);
    }
}

/// @brief 排空 libdbus 的入队消息（非阻塞）。
auto pump_dbus() -> void {
    DbusApi &api = dbus_api();
    if ((api.connection == nullptr) || (api.read_write == nullptr) || (api.pop_message == nullptr)) {
        return;
    }
    (void)api.read_write(api.connection, 0);
    for (;;) {
        void *raw = api.pop_message(api.connection);
        if (raw == nullptr) {
            break;
        }
        handle_dbus_signal(raw);
        api.message_unref(raw);
    }
}

// ---- ③ notify-send 兜底 ----

/// @brief 经 `notify-send` 外壳命令投递（即发即忘，无激活回调）。
/// @param notification 通知内容。
/// @return 成功为 `Ok`；命令不存在或退出码非零为 `NotificationPostFailed`。
[[nodiscard]] auto post_via_notify_send(const Notification &notification) -> Result<void> {
    std::string urgency_flag = "normal";
    if (notification.urgency == NotificationUrgency::Low) {
        urgency_flag = "low";
    } else if (notification.urgency == NotificationUrgency::Critical) {
        urgency_flag = "critical";
    }
    std::string command = "notify-send --app-name Aurora --urgency ";
    command += shell_quote(urgency_flag);
    if (notification.timeout_ms != 0U) {
        command += " --expire-time ";
        command += shell_quote(std::to_string(notification.timeout_ms));
    }
    command += " -- ";
    // 标题/正文是调用方数据：一律单引号转义后才拼进命令行，不假设它们是可信输入。
    command += shell_quote(notification.title);
    command += " ";
    command += shell_quote(notification.body);
    command += " 2>/dev/null";
    FILE *pipe = popen(command.c_str(), "r");  // NOLINT(bugprone-command-processor)
    if (pipe == nullptr) {
        return post_failed("popen(notify-send) failed");
    }
    const int exit_code = pclose(pipe);
    if (exit_code != 0) {
        return post_failed(std::string("notify-send exited with code ") + std::to_string(exit_code));
    }
    return Result<void>{};
}
// NOLINTEND(*-pro-type-*)
#endif

/// @brief 平台投递：把通知交给本平台的后端（record-only 模式在调用方已短路）。
/// @param notification 通知内容。
/// @return 成功为 `Ok`；失败为 `NotificationPostFailed`。
[[nodiscard]] auto post_notification(const Notification &notification) -> Result<void> {
#ifdef AURORA_PLATFORM_WINDOWS
    return win_post(notification);
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    std::string detail;
    Result<void> attempt = post_via_libnotify(notification);
    if (attempt.ok()) {
        return Result<void>{};
    }
    detail += "[libnotify] " + attempt.error().message;

    attempt = post_via_libdbus(notification);
    if (attempt.ok()) {
        return Result<void>{};
    }
    detail += "; [libdbus] " + attempt.error().message;

    attempt = post_via_notify_send(notification);
    if (attempt.ok()) {
        return Result<void>{};
    }
    detail += "; [notify-send] " + attempt.error().message;
    return post_failed("every desktop notification backend failed: " + detail);
#elif defined(AURORA_BACKEND_HEADLESS)
    (void)notification;
    // Headless：没有可投递的目标，但「请求了什么」已经如实记进 last_notification()，
    // 按契约返回成功 —— 无桌面不是调用方的错。
    return Result<void>{};
#else
    (void)notification;
    return post_failed("this platform has no notification implementation");
#endif
}

}  // namespace

// ───────────────────────────── 公共 API 定义 ─────────────────────────────

auto NotificationCenter::notify(const Notification &notification) -> Result<void> {
    {
        auto &s = center_state();
        const std::scoped_lock lock{s.mutex};
        s.last = notification;  // 投递成功与否都记录：这是调用方「请求了什么」的唯一真相
        if (s.recording_only) {
            return Result<void>{};
        }
    }
    return post_notification(notification);
}

auto NotificationCenter::set_on_notification_activated(std::function<void(std::string tag)> callback) -> void {
    auto &s = center_state();
    const std::scoped_lock lock{s.mutex};
    s.on_activated = std::move(callback);
}

auto NotificationCenter::last_notification() -> std::optional<Notification> {
    auto &s = center_state();
    const std::scoped_lock lock{s.mutex};
    return s.last;
}

auto NotificationCenter::clear_last_notification() -> void {
    auto &s = center_state();
    const std::scoped_lock lock{s.mutex};
    s.last.reset();
}

auto NotificationCenter::pump_events() -> void {
#ifdef AURORA_PLATFORM_WINDOWS
    // Windows 的气球事件由隐藏窗口的消息 pump 同步送达（宿主的 PeekMessage 即覆盖，
    // 这里若再抽一次会把宿主的窗口消息抢走），故这条就是 no-op。
    return;
#elif defined(AURORA_PLATFORM_LINUX) && !defined(AURORA_PLATFORM_ANDROID)
    linux_state();  // 保证状态先于下面两级访问而构造
    load_glib();
    LinuxNotifyState &s = linux_state();
    if (s.main_context_iteration != nullptr) {
        // GLib 主循环的非阻塞迭代：libnotify 的 action 回调由此派发。
        (void)s.main_context_iteration(nullptr, 0);
    }
    pump_dbus();
#endif
}

auto NotificationCenter::emit_activated(const std::string &tag) -> void { dispatch_activated(tag); }

auto NotificationCenter::install_recording_backend() -> bool {
    auto &s = center_state();
    const std::scoped_lock lock{s.mutex};
    s.recording_only = true;
    return true;
}

auto NotificationCenter::remove_recording_backend() -> bool {
    auto &s = center_state();
    const std::scoped_lock lock{s.mutex};
    const bool was_active = s.recording_only;
    s.recording_only = false;
    return was_active;
}

}  // namespace aurora

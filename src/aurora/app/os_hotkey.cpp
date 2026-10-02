// os_hotkey.cpp — OS 级全局热键注册表。
// - AURORA_PLATFORM_WINDOWS：`RegisterHotKey` / `UnregisterHotKey` + 隐藏消息窗口接 `WM_HOTKEY`。
// - 其它平台（Wayland / GLFW / macOS / WASM 等）：`enabled()` 恒 false，`add()` 恒返回
//   `OsHotkeyRegisterFailed`，`remove()` 恒 false——降级是显式失败，不是静默 no-op。
// 与 `system_tray_win32.cpp` 同口径：公共方法在所有目标上都定义，Win32 专有代码用内部
// #ifdef（只用 `AURORA_PLATFORM_WINDOWS`，不写原生平台宏，见 core/platform.h）隔离。
//
// 命中不就地回调：`WM_HOTKEY` 到达时只把 ID 推入队列，等帧循环调 `drain_pending()` 再执行动作。
// 消息泵内重入用户回调（重建页面 / 触发重排）会撞上半途的布局状态，故一律排队。
#include "aurora/app/os_hotkey.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurora/app/detail/platform_shell_win32.h"
#include "aurora/core/log.h"
#include "aurora/core/platform.h"
#include "aurora/core/result.h"

#ifdef AURORA_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN  // NOLINT(readability-identifier-naming): Windows SDK 宏，不可改名
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
// clang-format on
#endif

#if defined(AURORA_ENABLE_DEBUG) && defined(AURORA_ENABLE_TEST_HOOKS)
// NOLINTNEXTLINE(*-macro-usage)
#define AURORA_OS_HOTKEY_TEST_BACKEND 1
#endif

namespace aurora {

namespace {

/// @brief 进程内 inert 后端状态（test-only）：激活时 add/remove 全部走内存，不触碰 OS。
struct TestBackendState {
    bool active = false;  ///< 后端是否已安装
    bool supported = false;  ///< 模拟的平台支持位（`enabled()` 的返回值）
};

/// @brief 进程唯一后端实例（函数内 static，规避静态初始化顺序问题）。
[[nodiscard]] auto test_backend() -> TestBackendState & {
    static TestBackendState state;
    return state;
}

/// @brief 本平台的 OS 级全局热键后端是否可用（inert 后端激活时以其模拟值为准）。
[[nodiscard]] auto platform_supported() -> bool {
    if (test_backend().active) {
        return test_backend().supported;
    }
#ifdef AURORA_PLATFORM_WINDOWS
    return true;
#else
    return false;
#endif
}

/// @brief 组装注册失败错误（`detail` 说明具体成因，供诊断与日志）。
/// @param detail 人类可读的失败成因（ASCII）。
/// @return 带 `OsHotkeyRegisterFailed` 码的结构化错误。
[[nodiscard]] auto register_error(const std::string &detail) -> Error {
    return make_error(ErrorCode::OsHotkeyRegisterFailed, ErrorParams{{"detail", detail}});
}

/// @brief 分配下一个进程内唯一的热键 ID（自 1 起；0 保留为无效句柄）。
[[nodiscard]] auto next_hotkey_id() -> std::uint32_t {
    static std::uint32_t counter = 0;
    ++counter;
    return counter;
}

#ifdef AURORA_PLATFORM_WINDOWS
/// @brief `KeyCode` → Win32 虚拟键码（VK_*）的反向映射。
///
/// 反向表未收录在既有代码里（`window/win32_host.cpp` 只有 VK→KeyCode 的 `from_win32_vk`），
/// 故在此自建，并保持与其逐键互逆。
/// @param key 逻辑键码。
/// @return 对应的虚拟键码；无法映射（含 `Unknown` 与修饰键）时为 0（VK 0 不是合法键）。
[[nodiscard]] auto to_win32_vk(KeyCode key) -> int {
    if ((key >= KeyCode::A) && (key <= KeyCode::Z)) {
        return 'A' + (static_cast<int>(key) - static_cast<int>(KeyCode::A));
    }
    if ((key >= KeyCode::D0) && (key <= KeyCode::D9)) {
        return '0' + (static_cast<int>(key) - static_cast<int>(KeyCode::D0));
    }
    if ((key >= KeyCode::F1) && (key <= KeyCode::F12)) {
        return VK_F1 + (static_cast<int>(key) - static_cast<int>(KeyCode::F1));
    }
    switch (key) {
        case KeyCode::Enter:
            return VK_RETURN;
        case KeyCode::Escape:
            return VK_ESCAPE;
        case KeyCode::Tab:
            return VK_TAB;
        case KeyCode::Backspace:
            return VK_BACK;
        case KeyCode::Delete:
            return VK_DELETE;
        case KeyCode::Space:
            return VK_SPACE;
        case KeyCode::ArrowLeft:
            return VK_LEFT;
        case KeyCode::ArrowRight:
            return VK_RIGHT;
        case KeyCode::ArrowUp:
            return VK_UP;
        case KeyCode::ArrowDown:
            return VK_DOWN;
        case KeyCode::Home:
            return VK_HOME;
        case KeyCode::End:
            return VK_END;
        case KeyCode::PageUp:
            return VK_PRIOR;
        case KeyCode::PageDown:
            return VK_NEXT;
        case KeyCode::Minus:
            return VK_OEM_MINUS;
        case KeyCode::Equal:
            return VK_OEM_PLUS;
        case KeyCode::Semicolon:
            return VK_OEM_1;
        case KeyCode::Quote:
            return VK_OEM_7;
        case KeyCode::Comma:
            return VK_OEM_COMMA;
        case KeyCode::Period:
            return VK_OEM_PERIOD;
        case KeyCode::Slash:
            return VK_OEM_2;
        case KeyCode::Backquote:
            return VK_OEM_3;
        case KeyCode::LeftBracket:
            return VK_OEM_4;
        case KeyCode::RightBracket:
            return VK_OEM_6;
        case KeyCode::Backslash:
            return VK_OEM_5;
        default:
            // 修饰键（Shift/Control/Alt/Meta）不能作热键主键：OS 侧它们只作修饰位。
            return 0;
    }
}

/// @brief 修饰键位掩码 → `RegisterHotKey` 的 `fsModifiers`。
/// @param mods 修饰键位掩码。
/// @return MOD_* 位组合（无修饰键时为 0，但 OS 不允许纯主键热键，由调用方拦截）。
[[nodiscard]] auto to_win32_modifiers(ModifierKey mods) -> UINT {
    UINT out = 0;
    if ((mods & ModifierKey::Control) != 0) {  // NOLINT(*-redundant-parentheses)
        out |= MOD_CONTROL;
    }
    if ((mods & ModifierKey::Shift) != 0) {  // NOLINT(*-redundant-parentheses)
        out |= MOD_SHIFT;
    }
    if ((mods & ModifierKey::Alt) != 0) {  // NOLINT(*-redundant-parentheses)
        out |= MOD_ALT;
    }
    if ((mods & ModifierKey::Meta) != 0) {  // NOLINT(*-redundant-parentheses)
        out |= MOD_WIN;
    }
    return out;
}
#endif

}  // namespace

struct OsHotkeyRegistry::Impl {
    /// @brief 一条已注册热键：ID + 组合 + 动作。
    struct Entry {
        std::uint32_t id = 0;  ///< 热键 ID（进程内唯一，自 1 起）
        KeyCombo combo;  ///< 注册的键组合
        std::function<void()> action;  ///< 命中时执行的动作（可空）
    };

    std::vector<Entry> entries;  ///< 已注册热键（按注册序）
    std::vector<std::uint32_t> pending;  ///< 已命中、待帧循环排空的 ID（单线程，无锁）
    std::uint32_t hook_id = 0;  ///< 挂在隐藏消息窗口上的钩子 ID（0 = 未挂上）

    /// @brief 把热键注册到 OS（inert 后端激活时调用方不走到这里）。
    /// @param id 待注册的热键 ID（同时是 `WM_HOTKEY` 的 wParam）。
    /// @param combo 键组合。
    /// @return 注册成功为真。
    auto register_native(std::uint32_t id, const KeyCombo &combo) -> bool;
    /// @brief 从 OS 注销热键（与 `register_native` 成对）。
    /// @param id 热键 ID。
    auto unregister_native(std::uint32_t id) -> void;

    /// @brief 收到一条 `WM_HOTKEY`：只做「确认归属 + 入队」，不执行动作。
    /// @param id 命中 ID（`wParam`）。
    /// @return ID 属于本注册表（消息已消费）时为真。
    auto on_hotkey(std::uint32_t id) -> bool;
    /// @brief 查已注册项（供排空时取动作）。
    /// @param id 热键 ID。
    /// @return 命中的条目；未注册时为 nullptr（条目地址在动作执行期间可能被改，故只即时取值）。
    [[nodiscard]] auto find(std::uint32_t id) const -> const Entry *;
    /// @brief 键组合是否已被本注册表占用。
    /// @param combo 待查组合。
    /// @return 已占用为真。
    [[nodiscard]] auto has_combo(const KeyCombo &combo) const -> bool;
};

auto OsHotkeyRegistry::Impl::find(std::uint32_t id) const -> const OsHotkeyRegistry::Impl::Entry * {
    for (const auto &e : entries) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

auto OsHotkeyRegistry::Impl::has_combo(const KeyCombo &combo) const -> bool {
    for (const auto &e : entries) {
        if ((static_cast<std::uint8_t>(e.combo.modifiers) == static_cast<std::uint8_t>(combo.modifiers)) &&
            (e.combo.key == combo.key)) {
            return true;
        }
    }
    return false;
}

auto OsHotkeyRegistry::Impl::on_hotkey(std::uint32_t id) -> bool {
    if (find(id) == nullptr) {
        return false;  // 同一消息窗上别的注册表的热键：不归本实例管
    }
    pending.push_back(id);
    return true;
}

auto OsHotkeyRegistry::Impl::register_native(std::uint32_t id, const KeyCombo &combo) -> bool {
#ifdef AURORA_PLATFORM_WINDOWS
    auto *hwnd = static_cast<HWND>(internal::ensure_message_window());
    if (hwnd == nullptr) {
        return false;
    }
    const int vk = to_win32_vk(combo.key);
    if (vk == 0) {
        return false;
    }
    if (RegisterHotKey(hwnd, static_cast<int>(id), to_win32_modifiers(combo.modifiers), static_cast<UINT>(vk)) == 0) {
        AURORA_LOG_WARN("hotkey", "RegisterHotKey failed, id=", id, ", GetLastError=", GetLastError());
        return false;
    }
    return true;
#else
    (void)id;
    (void)combo;
    return false;
#endif
}

void OsHotkeyRegistry::Impl::unregister_native(std::uint32_t id) {
#ifdef AURORA_PLATFORM_WINDOWS
    auto *hwnd = static_cast<HWND>(internal::ensure_message_window());
    if (hwnd == nullptr) {
        return;
    }
    UnregisterHotKey(hwnd, static_cast<int>(id));
#else
    (void)id;
#endif
}

// ---- OsHotkeyRegistry 公共方法 ----

OsHotkeyRegistry::OsHotkeyRegistry() : impl_(std::make_unique<Impl>()) {
#ifdef AURORA_PLATFORM_WINDOWS
    // 挂消息钩子：本实例只认自己注册过的 ID，同一消息窗上可以有别的注册表实例。
    // 捕获 Impl 指针（而非 this）：Impl 由 unique_ptr 持有，地址在注册表移动后仍稳定。
    Impl *self = impl_.get();
    impl_->hook_id =
        internal::add_message_hook([self](std::uint32_t message, const internal::ShellMessage &payload) -> bool {
            if (message != WM_HOTKEY) {
                return false;
            }
            return self->on_hotkey(static_cast<std::uint32_t>(payload.wparam));
        });
#endif
}

OsHotkeyRegistry::~OsHotkeyRegistry() {
    if (impl_ == nullptr) {
        return;
    }
    clear();
#ifdef AURORA_PLATFORM_WINDOWS
    internal::remove_message_hook(impl_->hook_id);
#endif
}

auto OsHotkeyRegistry::add(const KeyCombo &combo, std::function<void()> action) -> Result<OsHotkeyHandle> {
    if (!platform_supported()) {
        return register_error("no OS global hotkey backend on this platform");
    }
    if (combo.key == KeyCode::Unknown) {
        return register_error("no primary key in the combination");
    }
    if (impl_->has_combo(combo)) {
        return register_error("combination already registered: " + combo.to_string());
    }
    const std::uint32_t id = next_hotkey_id();
    if (!test_backend().active && !impl_->register_native(id, combo)) {
        return register_error("OS rejected the combination: " + combo.to_string());
    }
    impl_->entries.push_back(Impl::Entry{.id = id, .combo = combo, .action = std::move(action)});
    return OsHotkeyHandle{id};
}

auto OsHotkeyRegistry::remove(OsHotkeyHandle h) -> bool {
    if ((impl_ == nullptr) || (h.id == 0U)) {
        return false;
    }
    for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
        if (it->id != h.id) {
            continue;
        }
        if (!test_backend().active) {
            impl_->unregister_native(it->id);
        }
        impl_->entries.erase(it);
        // 已命中但尚未排空的同 ID 一并丢弃：热键已不存在，再排空等于执行已撤销的动作。
        std::vector<std::uint32_t> kept;
        kept.reserve(impl_->pending.size());
        for (const std::uint32_t pending_id : impl_->pending) {
            if (pending_id != h.id) {
                kept.push_back(pending_id);
            }
        }
        impl_->pending = std::move(kept);
        return true;
    }
    return false;
}

auto OsHotkeyRegistry::clear() -> void {
    if (impl_ == nullptr) {
        return;
    }
    const bool inert = test_backend().active;
    for (const auto &e : impl_->entries) {
        if (!inert) {
            impl_->unregister_native(e.id);
        }
    }
    impl_->entries.clear();
    impl_->pending.clear();
}

auto OsHotkeyRegistry::drain_pending() -> std::size_t {
    if (impl_ == nullptr) {
        return 0;
    }
    // 先把队列整体搬走：动作里可能再次命中（入队）或注销热键，边遍历边改容器都不安全。
    std::vector<std::uint32_t> batch;
    batch.swap(impl_->pending);
    std::size_t fired = 0;
    for (const std::uint32_t id : batch) {
        // 动作在条目外执行：回调可能增删注册表，持引用跨调用会悬垂。
        std::function<void()> action;
        if (const Impl::Entry *e = impl_->find(id); e != nullptr) {
            action = e->action;
        }
        if (action) {
            action();
            ++fired;
        }
    }
    return fired;
}

auto OsHotkeyRegistry::enabled() const -> bool { return platform_supported(); }

auto OsHotkeyRegistry::count() const -> std::size_t { return (impl_ == nullptr) ? 0U : impl_->entries.size(); }

auto OsHotkeyRegistry::install_test_backend(bool simulated_platform_supported) -> bool {
#ifdef AURORA_OS_HOTKEY_TEST_BACKEND
    auto &backend = test_backend();
    backend.supported = simulated_platform_supported;
    backend.active = true;
    return true;
#else
    (void)simulated_platform_supported;
    return false;
#endif
}

auto OsHotkeyRegistry::remove_test_backend() -> bool {
#ifdef AURORA_OS_HOTKEY_TEST_BACKEND
    auto &backend = test_backend();
    if (!backend.active) {
        return false;
    }
    backend.active = false;
    backend.supported = false;
    return true;
#else
    return false;
#endif
}

}  // namespace aurora
